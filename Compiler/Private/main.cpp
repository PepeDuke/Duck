#include "Pipeline.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

namespace fs = std::filesystem;

struct Options {
  bool entry = true;
  bool parallel = true;
  bool test = false;
};

struct SourceFile {
  bool dependency;
  std::vector<duck::Token> tokens;
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
  const auto source = sources.find(token.source);

  if (source == sources.end() || token.text.empty() || token.line < 1 || token.column < 1) return;

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
  std::cout << "Usage: duckc [--library | --test] [--sequential] [--dependency file.dk] <source.dk> "
               "[more.dk ...] -o <output.ll>\nEmits "
               "LLVM IR; "
               "compile and link with clang output.ll -o program\n"
               "--test builds a test runner from the T{ } blocks instead of the "
               "program;\nits executable takes --list and name filters.\n"
               "--sequential keeps every call on the calling thread. At run "
               "time,\nDUCK_WORKERS=n limits threads running Duck code "
               "(default: CPU count).\n";
}

std::string readSource(const std::string &path, const fs::path &outputPath) {
  if (path == "-o") throw std::runtime_error("-o must occur once, after all input files");

  std::ifstream input(path, std::ios::binary);

  if (!input) throw std::runtime_error(path + ": cannot open input file");

  std::error_code error;

  if (fs::equivalent(fs::path(path), outputPath, error))
    throw std::runtime_error(path + ": output must not overwrite an input file");

  std::ostringstream source;
  source << input.rdbuf();

  if (input.bad()) throw std::runtime_error(path + ": cannot read input file");

  return source.str();
}

void addSource(Inputs &inputs, SourceTexts &texts, const std::string &path, bool dependency, const fs::path &outputPath) {
  const auto &text = texts[path] = readSource(path, outputPath);
  auto tokens = duck::preprocess(duck::lex(text, path));
  duck::collectContainerNames(tokens, inputs.containerNames);
  duck::collectMacros(tokens, inputs.macros);
  inputs.sources.push_back({dependency, std::move(tokens)});
}

// Flags and inputs may mix freely; the last two arguments are always -o <path>.
Inputs readArguments(int argc, char **argv, const fs::path &outputPath, Options &options, SourceTexts &texts) {
  Inputs inputs;

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

    if (path == "--sequential") {
      options.parallel = false;
      continue;
    }

    const bool dependency = path == "--dependency";

    if (dependency) {
      if (++i >= argc - 2) throw std::runtime_error("--dependency requires a source path");

      path = argv[i];
    }

    addSource(inputs, texts, path, dependency, outputPath);
  }

  return inputs;
}

// A dependency's statements are private to the module defining them, and its
// tests belong to its own test build.
void stripDependencyBody(duck::Program &unit) {
  unit.erase(std::remove_if(unit.begin(), unit.end(), [](const duck::Function &f) { return f.script; }), unit.end());
  unit.tests.clear();
}

std::string compileInputs(Inputs inputs, const Options &options) {
  std::vector<duck::Program> units, importedUnits;

  for (auto &[dependency, tokens] : inputs.sources) {
    auto unit = duck::parse(std::move(tokens), inputs.containerNames, inputs.macros);

    if (dependency) {
      stripDependencyBody(unit);
      importedUnits.push_back(std::move(unit));
    } else
      units.push_back(std::move(unit));
  }

  if (units.empty()) throw std::runtime_error("at least one source file is required");

  if (options.test && !options.entry) throw std::runtime_error("--test and --library cannot be combined");

  auto program = duck::combineUnits(std::move(units));
  auto imports = duck::combineUnits(std::move(importedUnits));

  if (options.test) duck::enableTests(program);

  return duck::compileModule(program, imports, options.entry && !options.test, options.parallel, options.test);
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
    auto inputs = readArguments(argc, argv, outputPath, options, texts);
    writeOutput(outputPath, compileInputs(std::move(inputs), options));
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
