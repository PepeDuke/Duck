#include "Pipeline.h"
#include "Timing.h"
#include "WebInterface.h"
#ifdef DUCK_NET_LIBRARY
#define NOMINMAX
#include <windows.h>
#undef interface
#include "NetInterface.h"
#endif
#include <algorithm>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

namespace fs = std::filesystem;

struct Options {
  duck::Target target = duck::Target::Native;
  bool entry = true;
  bool parallel = true;
  bool test = false;
  // Where to write this module's interface file, and the key to record in it.
  std::string interface, interfaceKey;
};

// One input from the command line: a source file, or an interface file given with --import.
struct InputFile {
  std::string path;
  bool import = false;
  bool dependency = false;
  duck::SourceId source = 0; // Registered before the workers start; unused for an interface.
};

struct SourceFile {
  bool dependency;
  std::vector<duck::Token> tokens;
  // Position on the command line, then the part of an interface file. Workers
  // finish in any order; sorting on this restores the order of the arguments.
  std::pair<size_t, size_t> order{};
  // A part of an interface file read by a worker: its path, registered and
  // stamped onto the tokens once the workers stop.
  std::string unregistered;
};

struct ParsedSource {
  duck::Program unit;
  std::vector<duck::Token> interface; // What importers read, when writing an interface file.
  std::exception_ptr failure;
};

struct Inputs {
  std::vector<SourceFile> sources;
  std::set<std::string> containerNames;
  duck::MacroTable macros;
};

// File contents by the path given on the command line, for diagnostics.
using SourceTexts = std::map<std::string, std::string>;

const size_t maxReportedErrors = 20;

std::string lineAt(const std::string &text, int number, bool &found) {
  size_t start = 0;

  for (int line = 1; line < number && start != std::string::npos; ++line) {
    start = text.find('\n', start);

    if (start != std::string::npos) ++start;
  }

  found = start != std::string::npos;

  if (!found) return "";

  auto line = text.substr(start, text.find('\n', start) - start);

  if (!line.empty() && line.back() == '\r') line.pop_back();

  return line;
}

// Display columns of line[from, to): tabs print as four spaces, and UTF-8
// continuation bytes take no column of their own.
size_t displayWidth(const std::string &line, size_t from, size_t to) {
  size_t width = 0;

  for (size_t i = from; i < to && i < line.size(); ++i)
    width += line[i] == '\t' ? 4 : (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80 ? 0 : 1;

  return width;
}

// How many bytes of line the token covers, starting at index start.
size_t tokenLength(const duck::Token &token, const std::string &line, size_t start) {
  if (start >= line.size()) return 1;

  if (token.string) {
    const char quote = line[start];
    size_t i = start + 1;

    while (i < line.size() && line[i] != quote) i += line[i] == '\\' ? 2 : 1;

    return std::min(i + 1, line.size()) - start;
  }

  if (line.compare(start, token.text.size(), token.text) == 0) return token.text.size();

  // A call token holds its qualified name but sits on the member, as in p.Get.
  const auto member = token.text.substr(token.text.rfind('.') + 1);
  return line.compare(start, member.size(), member) == 0 ? member.size() : 1;
}

// Prints the token's source line with a caret under the token. A location
// with no source text behind it, or on a blank line, prints nothing.
void printSourceLine(const duck::Token &token, const SourceTexts &sources) {
  if (token.text.empty() || token.line < 1 || token.column < 1) return;

  // A dependency read from its interface file was never loaded as text.
  static SourceTexts imported;
  const auto &path = duck::sourcePath(token.source);
  auto source = sources.find(path);

  if (source == sources.end()) {
    source = imported.find(path);

    if (source == imported.end()) {
      std::ifstream input(path, std::ios::binary);

      if (!input) return;

      std::ostringstream text;
      text << input.rdbuf();
      source = imported.emplace(path, text.str()).first;
    }
  }

  bool found = false;
  const auto line = lineAt(source->second, token.line, found);

  if (!found || line.find_first_not_of(" \t") == std::string::npos) return;

  std::string shown;

  for (char c : line) shown += c == '\t' ? std::string(4, ' ') : std::string(1, c);

  const size_t start = size_t(token.column) - 1;
  const size_t length = tokenLength(token, line, start);
  const size_t pad = displayWidth(line, 0, start) + (start > line.size() ? start - line.size() : 0);
  const size_t width = std::max<size_t>(1, displayWidth(line, start, start + length));
  auto number = std::to_string(token.line);
  number.insert(0, number.size() < 5 ? 5 - number.size() : 0, ' ');
  std::cerr << number << " | " << shown << '\n'
            << std::string(number.size(), ' ') << " | " << std::string(pad, ' ') << '^' << std::string(width - 1, '~')
            << '\n';
}

// The first line of what() is the error; the rest are its notes.
void printError(const duck::CompileError &error, const SourceTexts &sources) {
  const std::string text = error.what();
  const auto newline = text.find('\n');
  std::cerr << text.substr(0, newline) << '\n';
  printSourceLine(error.token, sources);

  if (newline != std::string::npos) std::cerr << text.substr(newline + 1) << '\n';
}

void printErrors(const std::vector<duck::CompileError> &errors, const SourceTexts &sources) {
  for (size_t i = 0; i < errors.size() && i < maxReportedErrors; ++i) printError(errors[i], sources);

  if (errors.size() > 1)
    std::cerr << errors.size() << " errors"
              << (errors.size() > maxReportedErrors ? " (showing the first " + std::to_string(maxReportedErrors) + ")" : "")
              << '\n';
}

void printHelp() {
  std::cout << "Usage: duckc [--target native|wasm64] [--library | --test] [--sequential] [--dependency file.dk] [--import module.dki]\n"
               "             [--interface module.dki [--interface-key KEY]] <source.dk> [more.dk ...] -o <output.ll>\n"
               "Emits LLVM IR; compile and link with clang output.ll -o program\n"
               "--import reads a dependency from its interface file instead of its sources.\n"
               "--interface also writes this module's interface file, recording KEY in it.\n"
               "--test builds a test runner from the T{ } blocks instead of the "
               "program;\nits executable takes --list and name filters.\n"
               "--sequential keeps every call on the calling thread. At run "
               "time,\nDUCK_WORKERS=n limits threads running Duck code "
               "(default: CPU count).\n";
}

// Reads the whole stream into buffer, reusing its capacity from earlier files.
void readAll(std::ifstream &input, std::string &buffer) {
  buffer.clear();
  char chunk[16384];

  while (input.read(chunk, sizeof chunk), input.gcount() > 0) buffer.append(chunk, size_t(input.gcount()));
}

void readSource(std::string &buffer, const std::string &path, const fs::path &outputPath) {
  if (path == "-o") throw std::runtime_error("-o must occur once, after all input files");

  std::ifstream input(path, std::ios::binary);

  if (!input) throw std::runtime_error(path + ": cannot open input file");

  std::error_code error;

  if (fs::equivalent(fs::path(path), outputPath, error))
    throw std::runtime_error(path + ": output must not overwrite an input file");

  readAll(input, buffer);

  if (input.bad()) throw std::runtime_error(path + ": cannot read input file");
}

// A dependency read from its interface file: tokens already lexed and
// preprocessed, with the bodies importers never read left empty.
void addInterface(Inputs &inputs, const std::string &text, const std::string &path) {
  std::string key;
  for (auto &file : duck::readInterface(text, path, key)) {
    duck::stampSource(file.tokens, duck::registerSource(file.source));
    duck::collectContainerNames(file.tokens, inputs.containerNames);
    duck::collectMacros(file.tokens, inputs.macros);
    inputs.sources.push_back({true, std::move(file.tokens)});
  }
}

// Flags and inputs may mix freely; the last two arguments are always -o <path>.
std::vector<InputFile> readArguments(int argc, char **argv, Options &options) {
  std::vector<InputFile> inputs;

  for (int i = 1; i < argc - 2; ++i) {
    std::string path = argv[i];

    if (path == "--library") {
      options.entry = false;
      continue;
    }

    if (path == "--test") {
      options.test = true;
      continue;
    }

    if (path == "--target") {
      if (++i >= argc - 2) throw std::runtime_error("--target requires native or wasm64");
      const std::string value = argv[i];
      if (value == "wasm64") options.target = duck::Target::Wasm64;
      else if (value == "native") options.target = duck::Target::Native;
      else throw std::runtime_error("unknown target: " + value);
      continue;
    }

    if (path == "--sequential") {
      options.parallel = false;
      continue;
    }

    if (path == "--import" || path == "--interface" || path == "--interface-key") {
      if (++i >= argc - 2) throw std::runtime_error(path + " requires a value");

      if (path == "--import") inputs.push_back({argv[i], true, true});
      else if (path == "--interface") options.interface = argv[i];
      else options.interfaceKey = argv[i];

      continue;
    }

    const bool dependency = path == "--dependency";

    if (dependency) {
      if (++i >= argc - 2) throw std::runtime_error("--dependency requires a source path");

      path = argv[i];
    }

    inputs.push_back({path, false, dependency});
  }

  return inputs;
}

// A dependency's statements are private to the module defining them, and its
// tests belong to its own test build.
void stripDependencyBody(duck::Program &unit) {
  unit.erase(std::remove_if(unit.begin(), unit.end(), [](const duck::Function &f) { return f.script; }), unit.end());
  unit.tests.clear();
}

// Loads the inputs on a few worker threads in two rounds. In the first, each
// worker takes the next file from the command line, reads and lexes it, and
// adds its tokens to the second queue. Parsing needs every file's container
// names and macros, so the second round starts only once all files are lexed:
// then the same workers take lexed files from the second queue and parse them.
// Results keep command-line order, so the output and the first reported error
// never depend on which worker finished first.
class Loader {
public:
  static constexpr size_t Workers = 4;
  // A guess at a source file's length. Each worker reserves it once and
  // reuses the buffer, so most files are read without growing it.
  static constexpr size_t ExpectedFileSize = 10000;

  Inputs inputs; // Its sources are the second queue.

  Loader(std::vector<InputFile> files, fs::path outputPath)
      : files(std::move(files)), outputPath(std::move(outputPath)), texts(this->files.size()),
        failures(this->files.size()) {
    // Register the source paths in command-line order, so a file gets the same
    // index on every run, then freeze the table for the workers to read.
    for (auto &file : this->files)
      if (!file.import) file.source = duck::registerSource(file.path);

    duck::freezeSources(true);
    const auto count = std::min(Workers, std::max<size_t>(this->files.size(), 1));

    for (size_t i = 0; i < count; ++i) workers.emplace_back([this] { work(); });
  }
  ~Loader() {
    {
      std::lock_guard<std::mutex> guard(lock);
      stopping = true;
    }

    wake.notify_all();

    for (auto &worker : workers) worker.join();

    duck::freezeSources(false);
  }
  // Waits for the first round, then collects names and macros in command-line
  // order. Diagnostics need the text of every file that was read.
  void lexAll(SourceTexts &sourceTexts) {
    {
      std::unique_lock<std::mutex> guard(lock);
      settled.wait(guard, [&] { return lexedFiles == files.size(); });
    }

    // The workers are idle, so the table may grow again.
    duck::freezeSources(false);
    std::stable_sort(inputs.sources.begin(), inputs.sources.end(),
                     [](const SourceFile &a, const SourceFile &b) { return a.order < b.order; });

    for (auto &source : inputs.sources)
      if (!source.unregistered.empty()) {
        duck::stampSource(source.tokens, duck::registerSource(source.unregistered));
        source.unregistered.clear();
      }

    for (size_t i = 0; i < files.size(); ++i)
      if (!files[i].import && read[i]) sourceTexts[files[i].path] = std::move(texts[i]);

    for (size_t i = 0; i < files.size(); ++i)
      if (failures[i]) std::rethrow_exception(failures[i]);

    for (const auto &source : inputs.sources) {
      duck::collectContainerNames(source.tokens, inputs.containerNames);
      duck::collectMacros(source.tokens, inputs.macros);
    }
  }
  // The second round, over every source including ones added after lexing.
  std::vector<ParsedSource> parseAll(bool writeInterface) {
    {
      std::lock_guard<std::mutex> guard(lock);
      results.resize(inputs.sources.size());
      wantInterface = writeInterface;
      parsing = true;
      duck::freezeSources(true);
    }

    wake.notify_all();
    std::unique_lock<std::mutex> guard(lock);
    settled.wait(guard, [&] { return parsedSources == inputs.sources.size(); });
    duck::freezeSources(false);
    return std::move(results);
  }

private:
  std::vector<InputFile> files;
  fs::path outputPath;
  std::vector<std::string> texts;           // By file, for diagnostics.
  std::vector<char> read = std::vector<char>(files.size()); // By file: its text was read.
  std::vector<std::exception_ptr> failures; // By file, from the first round.
  std::vector<ParsedSource> results;        // By source, from the second round.
  std::vector<std::thread> workers;
  std::mutex lock;
  std::condition_variable wake, settled;
  size_t nextFile = 0, lexedFiles = 0, nextSource = 0, parsedSources = 0;
  bool parsing = false, wantInterface = false, stopping = false;

  void work() {
    std::string buffer;
    buffer.reserve(ExpectedFileSize);
    std::unique_lock<std::mutex> guard(lock);

    for (;;) {
      wake.wait(guard, [&] {
        return stopping || nextFile < files.size() || (parsing && nextSource < inputs.sources.size());
      });

      if (stopping) return;

      if (nextFile < files.size()) {
        const auto i = nextFile++;
        guard.unlock();
        auto lexed = lex(i, buffer);
        guard.lock();

        for (auto &source : lexed) inputs.sources.push_back(std::move(source));

        if (++lexedFiles == files.size()) settled.notify_all();
      } else {
        const auto i = nextSource++;
        guard.unlock();
        parse(inputs.sources[i], results[i]);
        guard.lock();

        if (++parsedSources == inputs.sources.size()) settled.notify_all();
      }
    }
  }
  std::vector<SourceFile> lex(size_t i, std::string &buffer) {
    const auto &file = files[i];
    std::vector<SourceFile> lexed;

    try {
      if (file.import) {
        std::ifstream input(file.path, std::ios::binary);

        if (!input) throw std::runtime_error(file.path + ": cannot open interface file");

        readAll(input, buffer);
        std::string key;
        size_t index = 0;

        for (auto &part : duck::readInterface(buffer, file.path, key))
          lexed.push_back({true, std::move(part.tokens), {i, index++}, std::move(part.source)});
      } else {
        readSource(buffer, file.path, outputPath);
        texts[i] = buffer;
        read[i] = true;
        lexed.push_back({file.dependency, duck::preprocess(duck::lex(buffer, file.source)), {i, 0}});
      }
    } catch (...) {
      failures[i] = std::current_exception();
      lexed.clear();
    }

    return lexed;
  }
  // The interface is made from the tokens the parser sees, before parsing
  // consumes them, and the parse that tells which bodies importers read.
  void parse(SourceFile &source, ParsedSource &result) {
    try {
      if (!source.dependency && wantInterface) {
        result.unit = duck::parseKeeping(source.tokens, inputs.containerNames, inputs.macros);
        result.interface = duck::interfaceTokens(std::move(source.tokens), result.unit);
      } else {
        result.unit = duck::parse(std::move(source.tokens), inputs.containerNames, inputs.macros);

        if (source.dependency) stripDependencyBody(result.unit);
      }
    } catch (...) {
      result.failure = std::current_exception();
    }
  }
};

void writeOutput(const fs::path &outputPath, const std::string &ir);

std::string compileInputs(Loader &loader, const Options &options) {
  auto &inputs = loader.inputs;
  std::vector<duck::Program> units, importedUnits;
  std::vector<std::vector<duck::Token>> interface;

  const bool webNet = options.target == duck::Target::Wasm64 && std::any_of(inputs.sources.begin(), inputs.sources.end(), [&](const SourceFile &file) {
    return std::any_of(file.tokens.begin(), file.tokens.end(), [&](const duck::Token &t) {
      return !t.string && (t.text == duck::Tok::Net || t.text == duck::Tok::NetReceiveEvent || t.text == duck::Tok::WebTransportSession ||
                           t.text == duck::Tok::WebTransportStream) && !inputs.containerNames.count(t.text);
    });
  });
  if (webNet) addInterface(inputs, webInterface, "<standard WebTransport>");
#ifdef DUCK_NET_LIBRARY
  const bool needsNet = options.target == duck::Target::Native && std::any_of(inputs.sources.begin(), inputs.sources.end(), [&](const SourceFile &file) {
    return std::any_of(file.tokens.begin(), file.tokens.end(), [&](const duck::Token &t) {
      return !t.string && (t.text == duck::Tok::Net || t.text == duck::Tok::TcpListener || t.text == duck::Tok::TcpConnection ||
                           t.text == duck::Tok::UdpSocket || t.text == duck::Tok::UdpEndpoint || t.text == duck::Tok::NetReceiveSettings ||
                           t.text == duck::Tok::NetReceiveEvent || t.text == duck::Tok::NetIncoming) &&
             !inputs.containerNames.count(t.text);
    });
  });
  if (needsNet) addInterface(inputs, netInterface, "<standard Net>");
#endif
  auto parsed = loader.parseAll(!options.interface.empty());

  for (size_t i = 0; i < parsed.size(); ++i) {
    if (parsed[i].failure) std::rethrow_exception(parsed[i].failure);

    if (inputs.sources[i].dependency) {
      importedUnits.push_back(std::move(parsed[i].unit));
    } else {
      if (!options.interface.empty()) interface.push_back(std::move(parsed[i].interface));

      units.push_back(std::move(parsed[i].unit));
    }
  }

  if (units.empty()) throw std::runtime_error("at least one source file is required");

  if (options.test && !options.entry) throw std::runtime_error("--test and --library cannot be combined");

  duck::phase("parse");
  auto program = duck::combineUnits(std::move(units));
  auto imports = duck::combineUnits(std::move(importedUnits));

  if (options.test) duck::enableTests(program);

  duck::phase("combine units");
  auto ir = duck::compileModule(program, imports, options.entry && !options.test, options.parallel, options.test, options.target);

#ifdef DUCK_NET_LIBRARY
  if (needsNet) {
    // COFF carries these dependencies through separately compiled Duck modules.
    std::wstring executable(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) throw std::runtime_error("cannot locate the Net runtime");
    executable.resize(length);
    auto library = fs::path(executable).parent_path() / DUCK_NET_LIBRARY;
    ir += "\n; Duck Net runtime " + std::string(netRuntimeHash) +
          "\n!llvm.linker.options = !{!0, !1}\n!0 = !{!\"/DEFAULTLIB:\\22" + library.generic_u8string() +
          "\\22\"}\n!1 = !{!\"/DEFAULTLIB:Ws2_32.lib\"}\n";
  }
#endif
  if (webNet) ir.insert(0, "; Duck WebTransport\n");
  // Written only once the module compiles, so a dependent never imports one
  // that would not.
  if (!options.interface.empty()) {
    writeOutput(fs::u8path(options.interface), duck::writeInterface(options.interfaceKey, interface));
    duck::phase("write interface");
  }

  return ir;
}

void writeOutput(const fs::path &outputPath, const std::string &ir) {
  std::ofstream output(outputPath, std::ios::binary);

  if (!output || !(output << ir) || !output.flush())
    throw std::runtime_error(outputPath.string() + ": cannot write output file");
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    printHelp();
    return 0;
  }

  if (argc < 4 || std::string(argv[argc - 2]) != "-o") {
    std::cerr << "Usage: duckc <source.dk> [more.dk ...] -o <output.ll>\n";
    return 1;
  }

  SourceTexts texts;

  try {
    const fs::path outputPath(argv[argc - 1]);
    Options options;
    Loader loader(readArguments(argc, argv, options), outputPath);
    loader.lexAll(texts);
    duck::phase("read, lex, preprocess");
    const auto ir = compileInputs(loader, options);
    writeOutput(outputPath, ir);
    duck::phase("write output");
    return 0;
  } catch (const duck::CompileErrors &e) {
    printErrors(e.errors, texts);
    return 1;
  } catch (const duck::CompileError &e) {
    printError(e, texts);
    return 1;
  } catch (const std::logic_error &e) {
    // map::at, vector::at, and the like: a compiler bug, not a user mistake.
    std::cerr << "duckc: internal compiler error: " << e.what() << '\n';
    return 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
