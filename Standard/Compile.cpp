#include "Pipeline.h"
#include <fstream>
#include <sstream>
#include <iostream>

// Build the standard module with the same parser, interface writer and code
// generator as application modules, before linking the user-facing compiler.
int main(int argc, char **argv) {
    if (argc != 4) return 1;
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) return 1;
        std::ostringstream source;
        source << input.rdbuf();
        auto tokens = duck::preprocess(duck::lex(source.str(), argv[1]));
        std::set<std::string> names;
        duck::collectContainerNames(tokens, names);
        auto unit = duck::parse(tokens, names, {});
        auto ir = duck::compileModule(unit, {}, false, true);
        auto interface = duck::writeInterface("", {duck::interfaceTokens(tokens, unit)});
        std::ofstream module(argv[2], std::ios::binary), header(argv[3], std::ios::binary);
        return !(module << ir).flush() || !(header << interface).flush();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
