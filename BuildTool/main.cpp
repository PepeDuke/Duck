#include "Process.h"
#include "vendor/tomlplusplus/toml.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
using Strings = std::vector<std::string>;
using Paths = std::vector<fs::path>;
using duckbuild::executable;
using duckbuild::run;

static void fail(const std::string& message) { throw std::runtime_error(message); }
static std::string lower(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
static const toml::table& table(const toml::node* node, const Strings& allowed, const std::string& location) {
    if (!node || !node->is_table()) fail(location + " must be a table");
    const auto& result = *node->as_table();
    for (const auto& [key, value] : result) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end())
            fail(location + ": unknown keys: " + std::string(key.str()));
    }
    return result;
}
static std::string string(const toml::node* node, const std::string& location) {
    auto result = node ? node->value<std::string>() : std::nullopt;
    if (!result || result->empty()) fail(location + " must be a nonempty string");
    return *result;
}
static Strings strings(const toml::node* node, const std::string& location, bool required = false) {
    if (!node && !required) return {};
    if (!node || !node->is_array()) fail(location + " must be an array of nonempty strings");
    Strings result;
    std::set<std::string> unique;
    for (const auto& item : *node->as_array()) {
        auto value = string(&item, location);
        if (!unique.insert(value).second) fail(location + " contains duplicates");
        result.push_back(value);
    }
    return result;
}
static std::string name(const std::string& value, const std::string& location) {
    if (!std::regex_match(value, std::regex("[A-Za-z][A-Za-z0-9_-]*")))
        fail(location + " must start with a letter and contain only letters, digits, _ or -");
    if (std::regex_match(lower(value), std::regex("con|prn|aux|nul|com[1-9]|lpt[1-9]")))
        fail(location + " is a reserved filename");
    return value;
}

// Match one path component. Recursive ** is handled separately so **/*.dk
// includes files directly in the directory as well as in its descendants.
static bool match(const std::string& pattern, const std::string& filename) {
    if (!filename.empty() && filename[0] == '.' && (pattern.empty() || pattern[0] != '.')) return false;
    std::string expression = "^";
    for (size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '*') expression += ".*";
        else if (c == '?') expression += '.';
        else if (c == '[') {
            auto end = pattern.find(']', i + (i + 1 < pattern.size() && pattern[i + 1] == '!' ? 2 : 1));
            if (end == std::string::npos) expression += "\\[";
            else {
                expression += '[';
                size_t start = i + 1;
                if (pattern[start] == '!') { expression += '^'; ++start; }
                else if (pattern[start] == '^') expression += '\\';
                for (size_t j = start; j < end; ++j) {
                    if (pattern[j] == '\\') expression += '\\';
                    expression += pattern[j];
                }
                expression += ']';
                i = end;
            }
        } else {
            if (std::string(".\\+(){}^$|").find(c) != std::string::npos) expression += '\\';
            expression += c;
        }
    }
    expression += '$';
#ifdef _WIN32
    return std::regex_match(filename, std::regex(expression, std::regex::icase));
#else
    return std::regex_match(filename, std::regex(expression));
#endif
}

static Paths glob(const fs::path& root, const std::string& pattern) {
    auto full = fs::absolute(root / fs::u8path(pattern));
    Paths components;
    for (const auto& component : full.relative_path()) components.push_back(component);
    Paths result;
    std::set<fs::path> active;
    std::function<void(const fs::path&, size_t)> walk = [&](const fs::path& current, size_t index) {
        if (index == components.size()) {
            if (fs::is_regular_file(current)) result.push_back(fs::canonical(current));
            return;
        }
        auto part = components[index].u8string();
        if (part.find_first_of("*?[") == std::string::npos) {
            walk(current / components[index], index + 1);
            return;
        }
        if (part == "**") walk(current, index + 1);
        if (!fs::is_directory(current)) return;
        auto real = fs::canonical(current);
        if (!active.insert(real).second) return; // Stop recursive symlink loops.
        for (const auto& item : fs::directory_iterator(current)) {
            auto filename = item.path().filename().u8string();
            if (part == "**") {
                if (!filename.empty() && filename[0] != '.') walk(item.path(), index);
            } else if (match(part, filename)) walk(item.path(), index + 1);
        }
        active.erase(real);
    };
    walk(full.root_path(), 0);
    return result;
}

struct Options {
    fs::path project;
    std::string module, profile = "debug", compiler, clang;
    Strings enable, disable, filters;
    bool list = false;
};
struct Link {
    Strings libraries, directories;
    explicit Link(const toml::table& config, const std::string& location)
        : libraries(strings(config.get("libraries"), location + ".libraries")),
          directories(strings(config.get("library_paths"), location + ".library_paths")) {}
};
struct Module : Link {
    Strings patterns, depends;
    bool enabled = true;
    Module(const toml::table& config, const std::string& id) : Link(config, id),
        patterns(strings(config.get("sources"), id + ".sources", true)),
        depends(strings(config.get("depends"), id + ".depends")) {
        if (patterns.empty()) fail(id + ".sources cannot be empty");
        if (auto node = config.get("enabled")) {
            if (!node->is_boolean()) fail(id + ".enabled must be true or false");
            enabled = *node->value<bool>();
        }
    }
};

class Project {
public:
    fs::path root, buildDir;
    std::string projectName, entry;
    std::map<std::string, std::string> toolchain;
    std::map<std::string, Module> modules;
    Strings order;
    Link link{toml::table{}, "link"};

    explicit Project(const Options& options, std::set<fs::path> activeProjects = {}) {
        auto path = fs::weakly_canonical(fs::absolute(options.project));
        if (lower(path.extension().u8string()) != ".dkp") fail("project file must have the .dkp extension");
        if (!activeProjects.insert(path).second) fail("project reference cycle: " + path.u8string());
        root = path.parent_path();
        auto data = toml::parse_file(path.u8string());
        table(&data, {"version", "project", "modules", "toolchain", "link", "references"}, "project file");
        auto version = data.get("version");
        if (!version || !version->is_integer() || version->value<int64_t>() != 1)
            fail("project file requires version = 1");
        const auto& config = table(data.get("project"), {"name", "entry", "build_dir", "kind"}, "project");
        projectName = name(string(config.get("name"), "project.name"), "project.name");
        auto kind = config.contains("kind") ? string(config.get("kind"), "project.kind") : "executable";
        if (kind != "executable" && kind != "library") fail("project.kind must be executable or library");
        if (kind == "library") {
            if (config.contains("entry")) fail("library project cannot have an entry module");
        } else entry = name(string(config.get("entry"), "project.entry"), "project.entry");
        buildDir = fs::weakly_canonical(root / fs::u8path(config.contains("build_dir") ?
            string(config.get("build_dir"), "project.build_dir") : ".duck/build"));
        if (auto tools = data.get("toolchain")) {
            for (const auto& [key, value] : table(tools, {"compiler", "clang"}, "toolchain"))
                toolchain[std::string(key.str())] = string(&value, "toolchain." + std::string(key.str()));
        }
        if (auto node = data.get("link")) link = Link(table(node, {"libraries", "library_paths"}, "link"), "link");
        auto node = data.get("modules");
        if (!node || !node->is_table() || node->as_table()->empty())
            fail("modules must contain at least one module table");
        std::vector<std::pair<toml::source_position, std::string>> declarations;
        for (const auto& [key, value] : *node->as_table()) {
            auto id = name(std::string(key.str()), "module name");
            const auto& module = table(&value, {"sources", "depends", "enabled", "libraries", "library_paths"}, "modules." + id);
            modules.emplace(id, Module(module, id));
            declarations.emplace_back(value.source().begin, id);
        }
        if (!entry.empty() && !modules.count(entry)) fail("entry module '" + entry + "' does not exist");
        // References contribute separately compiled modules, never an executable entry.
        for (const auto& reference : strings(data.get("references"), "references")) {
            Options childOptions;
            childOptions.project = root / fs::u8path(reference);
            Project child(childOptions, activeProjects);
            if (!child.entry.empty()) fail("referenced project must be a library: " + reference);
            auto rebaseLink = [&](Link& value) {
                for (auto& directory : value.directories)
                    directory = (child.root / fs::u8path(directory)).u8string();
                for (auto& library : value.libraries) {
                    auto ext = lower(fs::u8path(library).extension().u8string());
                    if (library.find_first_of("/\\") != std::string::npos ||
                        ext == ".lib" || ext == ".a" || ext == ".so" || ext == ".dylib" || ext == ".o" || ext == ".obj")
                        library = (child.root / fs::u8path(library)).u8string();
                }
            };
            for (const auto& id : child.order) {
                auto module = child.modules.at(id);
                for (auto& pattern : module.patterns) pattern = (child.root / fs::u8path(pattern)).u8string();
                rebaseLink(module);
                if (!modules.emplace(id, std::move(module)).second)
                    fail("duplicate module from project reference: " + id);
                declarations.emplace_back(toml::source_position{}, id);
            }
            rebaseLink(child.link);
            link.libraries.insert(link.libraries.end(), child.link.libraries.begin(), child.link.libraries.end());
            link.directories.insert(link.directories.end(), child.link.directories.begin(), child.link.directories.end());
        }
        for (const auto& id : options.enable)
            if (std::find(options.disable.begin(), options.disable.end(), id) != options.disable.end())
                fail("a module cannot be both enabled and disabled");
        for (bool enabled : {true, false}) {
            for (const auto& id : enabled ? options.enable : options.disable) {
                if (!modules.count(id)) fail("unknown module '" + id + "'");
                modules.at(id).enabled = enabled;
            }
        }
        for (const auto& [id, module] : modules) {
            for (const auto& dependency : module.depends) {
                if (!modules.count(dependency)) fail("module '" + id + "' depends on missing module '" + dependency + "'");
                if (dependency == entry) fail("module '" + id + "' cannot depend on entry module '" + dependency + "'");
            }
        }
        Strings active;
        std::set<std::string> done;
        std::function<void(const std::string&)> visit = [&](const std::string& id) {
            if (std::find(active.begin(), active.end(), id) != active.end()) {
                std::string cycle;
                for (const auto& step : active) cycle += step + " -> ";
                fail("module dependency cycle: " + cycle + id);
            }
            if (done.count(id)) return;
            active.push_back(id);
            for (const auto& dependency : modules.at(id).depends) visit(dependency);
            active.pop_back();
            done.insert(id);
            order.push_back(id);
        };
        std::sort(declarations.begin(), declarations.end());
        for (const auto& declaration : declarations) visit(declaration.second);
    }

    std::vector<std::pair<std::string, Paths>> plan(const std::string& target) const {
        if (!target.empty() && !modules.count(target)) fail("unknown module '" + target + "'");
        if (target.empty() && !entry.empty() && !modules.at(entry).enabled) fail("entry module cannot be disabled for an executable build");
        std::set<std::string> selected;
        std::function<void(const std::string&)> select = [&](const std::string& id) {
            const auto& module = modules.at(id);
            if (!module.enabled) fail("required module '" + id + "' is disabled; enable it or disable its dependents");
            if (!selected.insert(id).second) return;
            for (const auto& dependency : module.depends) select(dependency);
        };
        if (!target.empty()) select(target);
        else for (const auto& id : order) if (modules.at(id).enabled) select(id);
        std::map<fs::path, std::string> owners;
        std::vector<std::pair<std::string, Paths>> result;
        for (const auto& id : order) {
            if (!selected.count(id)) continue;
            std::set<fs::path> files;
            for (const auto& pattern : modules.at(id).patterns) {
                auto matches = glob(root, pattern);
                if (matches.empty()) fail("module '" + id + "': source pattern matches no files: " + pattern);
                for (const auto& path : matches) {
                    if (lower(path.extension().u8string()) != ".dk") fail("module '" + id + "': source is not a .dk file: " + path.u8string());
                    auto [owner, inserted] = owners.emplace(path, id);
                    if (!inserted && owner->second != id) fail("source '" + path.u8string() + "' belongs to both '" + owner->second + "' and '" + id + "'");
                    files.insert(path);
                }
            }
            result.emplace_back(id, Paths(files.begin(), files.end()));
        }
        return result;
    }

    std::string tool(const std::string& key, const std::string& override, const fs::path& bin) const {
        if (!override.empty()) return executable(override, fs::current_path()).u8string();
        auto configured = toolchain.find(key);
        if (configured != toolchain.end()) return executable(configured->second, root).u8string();
        const auto repo = fs::u8path(DUCK_SOURCE_DIR);
        Strings candidates = key == "compiler" ? Strings{"duckc", (bin / "duckc.exe").u8string(),
            (bin / "duckc").u8string(), (repo / "build/Debug/duckc.exe").u8string(),
            (repo / "build/Release/duckc.exe").u8string(), (repo / "build/duckc").u8string()} :
            Strings{"clang", "C:/Program Files/LLVM/bin/clang.exe"};
        for (const auto& candidate : candidates) {
            try { return executable(candidate, root).u8string(); }
            catch (const std::runtime_error&) {}
        }
        fail("cannot find " + key + "; set --" + key + " or toolchain." + key);
        return {};
    }

    void linkFlags(const Link& config, Strings& flags) const {
        for (const auto& directory : config.directories) {
            auto path = fs::weakly_canonical(root / fs::u8path(directory));
            if (!fs::is_directory(path)) fail("library directory not found: " + path.u8string());
            flags.insert(flags.end(), {"-L", path.u8string()});
        }
        for (const auto& library : config.libraries) {
            auto extension = lower(fs::u8path(library).extension().u8string());
            const std::set<std::string> extensions{".a", ".lib", ".so", ".dylib", ".o", ".obj"};
            if (library.find_first_of("/\\") != std::string::npos || extensions.count(extension)) {
                auto path = fs::weakly_canonical(root / fs::u8path(library));
                if (!fs::is_regular_file(path)) fail("library file not found: " + path.u8string());
                flags.push_back(path.u8string());
            } else {
                if (library.front() == '-') fail("library names must not be linker options");
                flags.push_back("-l" + library);
            }
        }
    }
};

struct WorkDirectory {
    fs::path path;
    explicit WorkDirectory(const fs::path& parent) {
        fs::create_directories(parent);
        std::mt19937_64 random(std::random_device{}());
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = parent / (".work-" + std::to_string(random()));
            if (fs::create_directory(path)) return;
        }
        fail("cannot create temporary build directory");
    }
    ~WorkDirectory() { std::error_code error; fs::remove_all(path, error); }
};

using Plan = std::vector<std::pair<std::string, Paths>>;
// mode is "--library", "--test", or empty for an entry module.
static Strings compileCommand(const Project& project, const Plan& sources, const std::string& compiler,
                              const std::string& id, const std::string& mode, const fs::path& ir) {
    Strings command{compiler};
    if (!mode.empty()) command.push_back(mode);
    for (const auto& dependency : project.modules.at(id).depends) {
        auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& item) { return item.first == dependency; });
        for (const auto& source : found->second) command.insert(command.end(), {"--dependency", source.u8string()});
    }
    const auto own = std::find_if(sources.begin(), sources.end(), [&](const auto& item) { return item.first == id; });
    for (const auto& file : own->second) command.push_back(file.u8string());
    command.insert(command.end(), {"-o", ir.u8string()});
    return command;
}

static bool definesTests(const fs::path& ir) {
    std::ifstream input(ir, std::ios::binary);
    std::stringstream text;
    text << input.rdbuf();
    return text.str().find("; duck-tests: 0\n") == std::string::npos;
}

// Each module's tests become their own executable under <build>/<profile>/tests:
// the module's test build linked with the ordinary objects of its dependencies.
// Nothing in the program's own build output is read or replaced.
static int test(const Options& options, const fs::path& bin) {
    Project project(options);
    const auto selected = project.plan(options.module);
    auto compiler = project.tool("compiler", options.compiler, bin);
    auto clang = project.tool("clang", options.clang, bin);
    const auto destination = project.buildDir / options.profile / "tests";
    WorkDirectory work(destination);
    const std::string optimize = options.profile == "debug" ? "-O0" : "-O2";
    std::map<std::string, fs::path> dependencies;
    int passed = 0, failed = 0, unmatched = 0;
    for (const auto& [id, files] : selected) {
        (void)files;
        if (!options.module.empty() && id != options.module) continue;
        const auto sources = project.plan(id);
        const auto ir = work.path / (id + ".test.ll"), object = work.path / (id + ".test.o");
        run(compileCommand(project, sources, compiler, id, "--test", ir), false);
        if (!definesTests(ir)) continue;
        run({clang, "-c", optimize, ir.u8string(), "-o", object.u8string()}, false);
        Strings link{clang, object.u8string()}, flags;
        for (const auto& [dependency, unused] : sources) {
            (void)unused;
            if (dependency == id) continue;
            auto known = dependencies.find(dependency);
            if (known == dependencies.end()) {
                const auto library = work.path / (dependency + ".ll"), output = work.path / (dependency + ".o");
                run(compileCommand(project, sources, compiler, dependency, "--library", library), false);
                run({clang, "-c", optimize, library.u8string(), "-o", output.u8string()}, false);
                known = dependencies.emplace(dependency, output).first;
            }
            link.push_back(known->second.u8string());
        }
        for (auto it = sources.rbegin(); it != sources.rend(); ++it) project.linkFlags(project.modules.at(it->first), flags);
        project.linkFlags(project.link, flags);
        auto output = work.path / (project.projectName + "." + id + ".test");
#ifdef _WIN32
        output += ".exe";
#endif
        link.insert(link.end(), flags.begin(), flags.end());
        link.insert(link.end(), {"-o", output.u8string()});
        run(link, false);
        const auto executablePath = destination / output.filename();
        duckbuild::publish(output, executablePath);
        std::cout << "== module " << id << std::endl;
        Strings command{executablePath.u8string()};
        if (options.list) command.push_back("--list");
        command.insert(command.end(), options.filters.begin(), options.filters.end());
        const int status = duckbuild::execute(command);
        if (status == 0) ++passed;
        else if (status == 2 && !options.filters.empty()) ++unmatched;
        else {
            ++failed;
            if (status != 1) std::cout << "module " << id << " stopped with exit code " << status
                                       << "; the last RUN line above names the test" << std::endl;
        }
    }
    if (failed) {
        std::cout << "duck test: " << failed << " module(s) failed, " << passed << " passed" << std::endl;
        return 1;
    }
    if (!passed && unmatched) {
        std::cerr << "duck test: error: no test matches the filter\n";
        return 1;
    }
    if (!passed) std::cout << "duck test: no tests found" << std::endl;
    return 0;
}

static void build(const Options& options, const fs::path& bin) {
    Project project(options);
    auto sources = project.plan(options.module);
    auto compiler = project.tool("compiler", options.compiler, bin);
    auto clang = project.tool("clang", options.clang, bin);
    Strings flags;
    if (options.module.empty() && !project.entry.empty()) {
        for (auto it = sources.rbegin(); it != sources.rend(); ++it) project.linkFlags(project.modules.at(it->first), flags);
        project.linkFlags(project.link, flags);
    }
    const auto destination = project.buildDir / options.profile;
    WorkDirectory work(destination);
    Paths artifacts;
    Strings objects;
    for (const auto& [id, files] : sources) {
        auto ir = work.path / (id + ".ll"), object = work.path / (id + ".o");
        (void)files;
        run(compileCommand(project, sources, compiler, id, id != project.entry ? "--library" : "", ir));
        run({clang, "-c", options.profile == "debug" ? "-O0" : "-O2", ir.u8string(), "-o", object.u8string()});
        objects.push_back(object.u8string());
        artifacts.insert(artifacts.end(), {ir, object});
    }
    if (options.module.empty() && !project.entry.empty()) {
        auto output = work.path / project.projectName;
#ifdef _WIN32
        output += ".exe";
#endif
        Strings command{clang};
        command.insert(command.end(), objects.begin(), objects.end());
        command.insert(command.end(), flags.begin(), flags.end());
        command.insert(command.end(), {"-o", output.u8string()});
        run(command);
        artifacts.push_back(output);
    }
    for (const auto& artifact : artifacts) duckbuild::publish(artifact, destination / artifact.filename());
    std::cout << "Built " << (options.module.empty() ? project.projectName : "module " + options.module)
              << ": " << destination.u8string() << '\n';
}

static void help() {
    std::cout << "Build modular Duck projects (.dkp, TOML syntax)\n"
        "Usage: duck build PROJECT [--module MODULE] [--enable MODULE] [--disable MODULE]\n"
        "                        [--profile debug|release] [--compiler PATH] [--clang PATH]\n"
        "--module builds objects for the module and its dependencies without linking.\n"
        "--enable and --disable may be repeated. Tool overrides resolve from the working directory.\n"
        "\n"
        "Usage: duck test PROJECT [--module MODULE] [--filter TEXT] [--list] [build options above]\n"
        "Builds each module's T{ } tests into its own executable, separate from the program, and runs them.\n"
        "--module tests one module. --filter runs tests whose name contains TEXT and may be repeated.\n"
        "--list prints test names without running them.\n";
}
static int mainImpl(const Strings& arguments) {
    try {
        if (std::find(arguments.begin() + 1, arguments.end(), "--help") != arguments.end() ||
            std::find(arguments.begin() + 1, arguments.end(), "-h") != arguments.end()) { help(); return 0; }
        if (arguments.size() < 3 || (arguments[1] != "build" && arguments[1] != "test"))
            fail("expected: duck build PROJECT or duck test PROJECT (see --help)");
        const bool testing = arguments[1] == "test";
        Options options;
        for (size_t i = 2; i < arguments.size(); ++i) {
            auto argument = arguments[i];
            if (argument.rfind("--", 0) != 0) {
                if (!options.project.empty()) fail("unexpected argument: " + argument);
                options.project = fs::u8path(argument);
                continue;
            }
            if (testing && argument == "--list") {
                options.list = true;
                continue;
            }
            auto equals = argument.find('=');
            auto key = argument.substr(0, equals);
            if (key != "--module" && key != "--enable" && key != "--disable" && key != "--profile" && key != "--compiler" && key != "--clang" &&
                !(testing && key == "--filter"))
                fail("unknown option: " + key);
            std::string value;
            if (equals != std::string::npos) value = argument.substr(equals + 1);
            else {
                if (i + 1 == arguments.size() || arguments[i + 1].rfind("--", 0) == 0) fail("missing value for " + key);
                value = arguments[++i];
            }
            if (value.empty()) fail("missing value for " + key);
            if (key == "--enable") options.enable.push_back(value);
            else if (key == "--disable") options.disable.push_back(value);
            else if (key == "--filter") {
                if (value.front() == '-') fail("a test filter cannot start with '-': " + value);
                options.filters.push_back(value);
            }
            else if (key == "--module") options.module = value;
            else if (key == "--profile") options.profile = value;
            else if (key == "--compiler") options.compiler = value;
            else options.clang = value;
        }
        if (options.project.empty()) fail("a .dkp project is required");
        if (options.profile != "debug" && options.profile != "release") fail("profile must be debug or release");
#ifdef _WIN32
        std::wstring executablePath(32768, L'\0');
        auto length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
        if (!length || length >= executablePath.size()) fail("cannot locate build-tool executable");
        executablePath.resize(length);
        auto bin = fs::path(executablePath).parent_path();
#else
        auto bin = executable(arguments.front(), fs::current_path()).parent_path();
#endif
        if (testing) return test(options, bin);
        build(options, bin);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "duck " << arguments[1] << ": error: " << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    Strings arguments;
    for (int i = 0; i < argc; ++i) arguments.push_back(fs::path(argv[i]).u8string());
    return mainImpl(arguments);
}
#else
int main(int argc, char** argv) { return mainImpl(Strings(argv, argv + argc)); }
#endif
