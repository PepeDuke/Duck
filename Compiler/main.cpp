#include "Pipeline.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: duckc [--library] [--dependency file.dk] <source.dk> "
                 "[more.dk ...] -o <output.ll>\nEmits "
                 "LLVM IR; "
                 "compile and link with clang output.ll -o program\n";
    return 0;
  }
  if (argc < 4 || std::string(argv[argc - 2]) != "-o") {
    std::cerr << "Usage: duckc <source.dk> [more.dk ...] -o <output.ll>\n";
    return 1;
  }
  try {
    const std::filesystem::path outputPath(argv[argc - 1]);
    std::vector<duck::Program> units, importedUnits;
    bool entry = true;
    for (int i = 1; i < argc - 2; ++i) {
      std::string path = argv[i];
      if (path == "--library") {
        entry = false;
        continue;
      }
      bool dependency = path == "--dependency";
      if (dependency) {
        if (++i >= argc - 2)
          throw std::runtime_error("--dependency requires a source path");
        path = argv[i];
      }
      if (path == "-o")
        throw std::runtime_error("-o must occur once, after all input files");
      std::ifstream input(path, std::ios::binary);
      if (!input)
        throw std::runtime_error(path + ": cannot open input file");
      std::error_code error;
      if (std::filesystem::equivalent(std::filesystem::path(path), outputPath,
                                      error))
        throw std::runtime_error(path +
                                 ": output must not overwrite an input file");
      std::ostringstream source;
      source << input.rdbuf();
      if (input.bad())
        throw std::runtime_error(path + ": cannot read input file");
      auto unit = duck::parse(duck::lex(source.str(), path));
      if (dependency)
        importedUnits.push_back(std::move(unit));
      else
        units.push_back(std::move(unit));
    }
    if (units.empty())
      throw std::runtime_error("at least one source file is required");
    auto program = duck::combineUnits(std::move(units));
    auto imports = duck::combineUnits(std::move(importedUnits));
    auto ir = duck::compileModule(program, imports, entry);
    std::ofstream output(outputPath, std::ios::binary);
    if (!output || !(output << ir) || !output.flush())
      throw std::runtime_error(outputPath.string() +
                               ": cannot write output file");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
