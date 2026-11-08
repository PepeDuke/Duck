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

void addSource(Inputs &inputs, const std::string &path, bool dependency, const fs::path &outputPath) {
  auto tokens = duck::preprocess(duck::lex(readSource(path, outputPath), path));
  duck::collectContainerNames(tokens, inputs.containerNames);
  duck::collectMacros(tokens, inputs.macros);
  inputs.sources.push_back({dependency, std::move(tokens)});
}

// Flags and inputs may mix freely; the last two arguments are always -o <path>.
Inputs readArguments(int argc, char **argv, const fs::path &outputPath, Options &options) {
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

    addSource(inputs, path, dependency, outputPath);
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

  try {
    const fs::path outputPath(argv[argc - 1]);
    Options options;
    auto inputs = readArguments(argc, argv, outputPath, options);
    writeOutput(outputPath, compileInputs(std::move(inputs), options));
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
