#include "Process.h"
#include "vendor/tomlplusplus/toml.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
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
    bool list = false, compareSchedules = false, verbose = false;
    int samples = 5;
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
#ifndef _WIN32
        flags.push_back("-lm"); // Math.Sin and friends call the C library.
#endif
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

// Interface files (.dki): compiling a library module also writes one, and a
// dependent reads it in place of the module's sources while its key, which
// covers everything that shapes it, still matches. A stale one is not waited
// for: the dependent reads the sources as before, and the module's own compile
// writes a fresh interface for the next build.
struct Interfaces {
    std::map<std::string, std::string> keys; // Every planned module's current key.
    std::map<std::string, fs::path> fresh;   // Published interfaces whose key matches.
};

// mode is "--library", "--test", or empty for an entry module. A dependency
// with a fresh interface is imported from it instead of from its sources.
static Strings compileCommand(const Project& project, const Plan& sources, const std::string& compiler,
                              const std::string& id, const std::string& mode, const fs::path& ir,
                              bool sequential = false, const Interfaces* interfaces = nullptr) {
    Strings command{compiler};
    if (!mode.empty()) command.push_back(mode);
    if (sequential) command.push_back("--sequential");
    for (const auto& dependency : project.modules.at(id).depends) {
        if (interfaces) {
            if (auto found = interfaces->fresh.find(dependency); found != interfaces->fresh.end()) {
                command.insert(command.end(), {"--import", found->second.u8string()});
                continue;
            }
        }
        auto found = std::find_if(sources.begin(), sources.end(), [&](const auto& item) { return item.first == dependency; });
        for (const auto& source : found->second) command.insert(command.end(), {"--dependency", source.u8string()});
    }
    const auto own = std::find_if(sources.begin(), sources.end(), [&](const auto& item) { return item.first == id; });
    for (const auto& file : own->second) command.push_back(file.u8string());
    command.insert(command.end(), {"-o", ir.u8string()});
    return command;
}

static std::string readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::stringstream text;
    text << input.rdbuf();
    return text.str();
}

// FNV-1a over each part, with each part's length mixed in so parts cannot
// run together.
static void mix(uint64_t& hash, const std::string& bytes) {
    for (unsigned char c : bytes + '\0' + std::to_string(bytes.size())) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
}

static std::string hashOf(const Strings& parts) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto& part : parts) mix(hash, part);
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// A tool binary's identity for keys: rebuilding or replacing it changes these.
static Strings toolIdentity(const std::string& tool) {
    std::error_code error;
    const auto path = fs::u8path(tool);
    const auto stamp = fs::last_write_time(path, error).time_since_epoch().count();
    const auto size = error ? 0 : fs::file_size(path, error);
    return {tool, std::to_string(size), std::to_string(stamp)};
}

// The key in a stamp file, which records what an output was built from.
static std::string stampKey(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string key;
    std::getline(input, key);
    return key;
}

static void writeStamp(const fs::path& path, const std::string& key) {
    std::ofstream output(path, std::ios::binary);
    if (!(output << key << '\n') || !output.flush()) fail("cannot write " + path.u8string());
}

// The key recorded in an interface file, or empty if there is none.
static std::string storedKey(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string header, keyLine;
    if (!std::getline(input, header) || header != "duck-interface 1" || !std::getline(input, keyLine)) return "";
    std::istringstream fields(keyLine);
    std::string word, key;
    size_t length = 0;
    if (!(fields >> word >> length >> key) || word != "key" || key.size() != length) return "";
    return key;
}

// A module's key covers the compiler binary, its own sources' paths and text,
// and its dependencies' keys, which cover theirs: a dependency's container
// names change how the module's own files parse.
static Interfaces interfaces(const Project& project, const Plan& sources, const std::string& compiler,
                             const fs::path& destination) {
    Interfaces result;
    const auto tool = toolIdentity(compiler);
    std::function<const std::string&(const std::string&)> key = [&](const std::string& id) -> const std::string& {
        if (auto found = result.keys.find(id); found != result.keys.end()) return found->second;
        Strings parts{"duck-interface 1", id};
        parts.insert(parts.end(), tool.begin(), tool.end());
        const auto own = std::find_if(sources.begin(), sources.end(), [&](const auto& item) { return item.first == id; });
        for (const auto& file : own->second) parts.insert(parts.end(), {file.u8string(), readFile(file)});
        for (const auto& dependency : project.modules.at(id).depends) parts.push_back(key(dependency));
        return result.keys[id] = hashOf(parts);
    };
    for (const auto& [id, files] : sources) {
        (void)files;
        const auto path = destination / (id + ".dki");
        const auto& current = key(id); // Every module needs one; only libraries have interfaces.
        if (id != project.entry && storedKey(path) == current) result.fresh[id] = path;
    }
    return result;
}

// Builds each module's tests into its own executable under <build>/<profile>/tests:
// the module's test build linked with the ordinary objects of its dependencies.
// Nothing in the program's own build output is read or replaced.
class TestBuilder {
    const Project& project;
    std::string compiler, clang, optimize;
    fs::path destination;
    WorkDirectory work;
    std::map<std::pair<std::string, bool>, fs::path> dependencies;

public:
    TestBuilder(const Project& p, std::string compilerPath, std::string clangPath, const Options& options)
        : project(p), compiler(std::move(compilerPath)), clang(std::move(clangPath)),
          optimize(options.profile == "debug" ? "-O0" : "-O2"), destination(p.buildDir / options.profile / "tests"),
          work(destination) {}

    struct Result {
        fs::path executable;
        bool tests = false;
        bool scheduledWork = false; // Dependency analysis accepted at least one task.
    };
    Result build(const std::string& id, bool sequential) {
        const auto sources = project.plan(id);
        const std::string tag = sequential ? ".sequential" : "";
        Result result;
        const auto ir = work.path / (id + tag + ".test.ll"), object = work.path / (id + tag + ".test.o");
        run(compileCommand(project, sources, compiler, id, "--test", ir, sequential), false);
        const auto text = readFile(ir);
        if (text.find("; duck-tests: 0\n") != std::string::npos) return result;
        result.tests = true;
        result.scheduledWork = text.find("@duck.task.limit") != std::string::npos;
        run({clang, "-c", optimize, ir.u8string(), "-o", object.u8string()}, false);
        Strings link{clang, object.u8string()}, flags;
        for (const auto& [dependency, unused] : sources) {
            (void)unused;
            if (dependency == id) continue;
            const auto key = std::make_pair(dependency, sequential);
            auto known = dependencies.find(key);
            if (known == dependencies.end()) {
                const auto library = work.path / (dependency + tag + ".ll"), output = work.path / (dependency + tag + ".o");
                run(compileCommand(project, sources, compiler, dependency, "--library", library, sequential), false);
                run({clang, "-c", optimize, library.u8string(), "-o", output.u8string()}, false);
                known = dependencies.emplace(key, output).first;
            }
            result.scheduledWork = result.scheduledWork ||
                readFile(known->second.parent_path() / (dependency + tag + ".ll")).find("@duck.task.limit") != std::string::npos;
            link.push_back(known->second.u8string());
        }
        for (auto it = sources.rbegin(); it != sources.rend(); ++it) project.linkFlags(project.modules.at(it->first), flags);
        project.linkFlags(project.link, flags);
        auto output = work.path / (project.projectName + "." + id + tag + ".test");
#ifdef _WIN32
        output += ".exe";
#endif
        link.insert(link.end(), flags.begin(), flags.end());
        link.insert(link.end(), {"-o", output.u8string()});
        run(link, false);
        result.executable = destination / output.filename();
        duckbuild::publish(output, result.executable);
        return result;
    }
};

// The nearest-rank percentile of samples, which must be sorted.
static long long percentile(const std::vector<long long>& sorted, size_t percent) {
    const auto rank = (percent * sorted.size() + 99) / 100;
    return sorted[rank ? rank - 1 : 0];
}

static std::string milliseconds(long long nanos) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(nanos) / 1e6);
    return text;
}

// One schedule under evaluation. Every candidate only varies how much of the
// parallelism that dependency analysis already accepted is used, so a result
// here can reject a schedule or prefer one, but never licenses a new one.
struct Candidate {
    std::string name, workers;
    const fs::path* executable;
    std::vector<long long> totals;
    std::string discarded;
};

// Runs one sample: true with the summed test time, or false with the reason
// the run does not count as a pass.
static bool sample(const Candidate& candidate, const Options& options, long long& total, std::string& reason) {
    Strings command{candidate.executable->u8string(), "--timing"};
    command.insert(command.end(), options.filters.begin(), options.filters.end());
    duckbuild::setEnvironment("DUCK_WORKERS", candidate.workers);
    std::string output;
    const int status = duckbuild::capture(command, output);
    duckbuild::setEnvironment("DUCK_WORKERS", "");
    total = 0;
    std::istringstream lines(output);
    std::string failed;
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("FAIL ", 0) == 0 && failed.empty()) failed = line.substr(5);
        if (line.rfind("TIME ", 0) == 0) total += std::atoll(line.c_str() + 5);
    }
    if (status == 0) return true;
    if (status == 2) reason = "no test matches the filter";
    else if (status == 1) reason = "test failed: " + failed;
    else reason = "stopped with exit code " + std::to_string(status);
    return false;
}

// Compares one module's schedules. Returns false when the sequential baseline
// itself does not pass, which leaves nothing to compare against.
static bool compareModule(const std::string& id, const TestBuilder::Result& sequential, const fs::path& parallel,
                          const Options& options, bool& matched) {
    std::vector<Candidate> candidates{{"sequential", "", &sequential.executable, {}, {}},
                                      {"workers=2", "2", &parallel, {}, {}},
                                      {"workers=4", "4", &parallel, {}, {}},
                                      {"workers=auto", "", &parallel, {}, {}}};
    // Interleaved, with a rotating start, so drift during the run is spread
    // over the candidates instead of favoring whichever always ran first.
    for (int round = 0; round < options.samples; ++round)
        for (size_t step = 0; step < candidates.size(); ++step) {
            auto& candidate = candidates[(step + static_cast<size_t>(round)) % candidates.size()];
            if (!candidate.discarded.empty()) continue;
            long long total = 0;
            std::string reason;
            if (sample(candidate, options, total, reason)) candidate.totals.push_back(total);
            else candidate.discarded = reason;
        }
    const auto& baseline = candidates.front();
    if (!baseline.discarded.empty()) {
        if (baseline.discarded == "no test matches the filter") return true;
        matched = true;
        std::cout << "module " << id << ": the sequential baseline did not pass (" << baseline.discarded
                  << "); fix the tests before comparing schedules" << std::endl;
        return false;
    }
    matched = true;
    auto base = baseline.totals;
    std::sort(base.begin(), base.end());
    const auto baseMedian = percentile(base, 50), baseLow = percentile(base, 25), baseHigh = percentile(base, 75);
    std::cout << "== module " << id << ": " << options.samples << " sample(s) per schedule, total test time in ms" << std::endl;
    std::string best = baseline.name;
    long long bestMedian = baseMedian;
    for (auto& candidate : candidates) {
        if (!candidate.discarded.empty()) {
            std::cout << "  " << candidate.name << ": DISCARDED (" << candidate.discarded << ")" << std::endl;
            continue;
        }
        std::sort(candidate.totals.begin(), candidate.totals.end());
        const auto median = percentile(candidate.totals, 50), low = percentile(candidate.totals, 25), high = percentile(candidate.totals, 75);
        std::string verdict = "baseline";
        if (&candidate != &baseline) {
            // A difference counts only when the middle halves do not overlap.
            verdict = high < baseLow ? "faster" : low > baseHigh ? "slower" : "no clear difference";
            if (verdict == "faster" && median < bestMedian) {
                best = candidate.name;
                bestMedian = median;
            }
        }
        std::cout << "  " << candidate.name << ": median " << milliseconds(median) << " (min " << milliseconds(candidate.totals.front())
                  << ", max " << milliseconds(candidate.totals.back()) << ") " << verdict << std::endl;
    }
    std::cout << "  preferred: " << best << std::endl;
    return true;
}

static int test(const Options& options, const fs::path& bin) {
    Project project(options);
    const auto selected = project.plan(options.module);
    auto compiler = project.tool("compiler", options.compiler, bin);
    auto clang = project.tool("clang", options.clang, bin);
    TestBuilder builder(project, compiler, clang, options);
    int passed = 0, failed = 0, unmatched = 0;
    bool compared = false;
    if (options.compareSchedules)
        std::cout << "Candidates vary only schedules that dependency analysis accepted; "
                     "passing tests do not show that a schedule is free of races." << std::endl;
    for (const auto& [id, files] : selected) {
        (void)files;
        if (!options.module.empty() && id != options.module) continue;
        if (options.compareSchedules) {
            const auto parallel = builder.build(id, false);
            if (!parallel.tests) continue;
            if (!parallel.scheduledWork) {
                compared = true;
                std::cout << "== module " << id << ": dependency analysis scheduled no tasks; nothing to compare" << std::endl;
                continue;
            }
            const auto sequential = builder.build(id, true);
            if (!compareModule(id, sequential, parallel.executable, options, compared)) ++failed;
            continue;
        }
        const auto built = builder.build(id, false);
        if (!built.tests) continue;
        std::cout << "== module " << id << std::endl;
        Strings command{built.executable.u8string()};
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
    if (options.compareSchedules) {
        if (compared) return 0;
        if (!options.filters.empty()) {
            std::cerr << "duck test: error: no test matches the filter\n";
            return 1;
        }
        std::cout << "duck test: no tests found" << std::endl;
        return 0;
    }
    if (!passed && unmatched) {
        std::cerr << "duck test: error: no test matches the filter\n";
        return 1;
    }
    if (!passed) std::cout << "duck test: no tests found" << std::endl;
    return 0;
}

using Clock = std::chrono::steady_clock;

static std::string seconds(Clock::duration took) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1fs", std::chrono::duration<double>(took).count());
    return text;
}

// Prints a "[N/X] tool subject time" line as each build step finishes, so a
// long build shows how far along it is. Steps finish in any order, so these
// lines are live; diagnostics are still replayed in build order. The lock is
// shared by every build print so lines from worker threads never interleave.
struct Progress {
    size_t total, done = 0, width;
    std::mutex print;

    Progress(size_t total, size_t width) : total(total), width(width) {}

    void finished(const std::string& tool, const std::string& subject, Clock::duration took) {
        std::lock_guard<std::mutex> lock(print);
        const auto digits = std::to_string(total).size();
        auto count = std::to_string(++done);
        count.insert(0, digits - count.size(), ' ');
        auto name = subject;
        if (name.size() < width) name.append(width - name.size(), ' ');
        std::cout << '[' << count << '/' << total << "] " << tool << ' ' << name << "  " << seconds(took) << std::endl;
    }
};

struct ModuleStep { std::string command, output; };
struct ModuleBuild {
    std::vector<ModuleStep> steps;
    std::string error, running;
    Clock::time_point since;
    bool finished = false;
};

// Compiles the modules not up to date concurrently, which is safe because a
// module's compile reads only source files and interfaces published by an
// earlier build, never another module's output from this one. Results are printed
// in build order and stop at the first failure, so output matches a serial
// build and an error in a dependency is not repeated by each of its dependents.
static void compileModules(const Project& project, const Plan& sources, const std::string& compiler,
                           const std::string& clang, const std::string& optimize, const fs::path& directory,
                           Progress& progress, bool verbose, const Interfaces& interfaces,
                           const std::set<std::string>& upToDate) {
    std::vector<ModuleBuild> builds(sources.size());
    std::mutex mutex;
    std::condition_variable finished;
    // Modules are claimed in order, so every module before a failed one has
    // been claimed and will finish.
    std::atomic<size_t> next{0};
    std::atomic<bool> stop{false};
    const auto worker = [&] {
        while (!stop) {
            const size_t index = next++;
            if (index >= sources.size()) return;
            auto& build = builds[index];
            const auto& id = sources[index].first;
            const auto step = [&](const char* tool, const Strings& command) {
                build.steps.push_back({duckbuild::commandLine(command), {}});
                const auto start = Clock::now();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    build.running = tool;
                    build.since = start;
                }
                run(command, build.steps.back().output);
                if (!stop) progress.finished(tool, id, Clock::now() - start);
            };
            const auto ir = directory / (id + ".ll"), object = directory / (id + ".o");
            // An up-to-date module's published outputs already match its
            // sources and tools, so it has no steps.
            if (!upToDate.count(id)) try {
                const bool library = id != project.entry;
                auto command = compileCommand(project, sources, compiler, id, library ? "--library" : "", ir, false, &interfaces);
                if (library) {
                    const auto dki = directory / (id + ".dki");
                    command.insert(command.end() - 2, {"--interface", dki.u8string(), "--interface-key", interfaces.keys.at(id)});
                }
                step("duckc", command);
                step("clang", {clang, "-c", optimize, ir.u8string(), "-o", object.u8string()});
            } catch (const std::exception& error) {
                build.error = error.what();
                stop = true;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                build.finished = true;
            }
            finished.notify_all();
        }
    };
    std::vector<std::thread> workers;
    const size_t count = std::min<size_t>(sources.size(), std::max(1u, std::thread::hardware_concurrency()));
    for (size_t i = 0; i < count; ++i) workers.emplace_back(worker);
    std::string failure;
    for (const auto& build : builds) {
        // While a step runs long, say which ones are still going, so a slow
        // or stuck tool is visible instead of a silent pause.
        for (;;) {
            std::string running;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (finished.wait_for(lock, std::chrono::seconds(15), [&] { return build.finished; })) break;
                const auto now = Clock::now();
                for (size_t i = 0; i < builds.size(); ++i) {
                    if (builds[i].finished || builds[i].running.empty()) continue;
                    if (!running.empty()) running += ", ";
                    running += builds[i].running + " " + sources[i].first + " (" + seconds(now - builds[i].since) + ")";
                }
            }
            if (!running.empty() && !stop) {
                std::lock_guard<std::mutex> lock(progress.print);
                std::cout << "      still running: " << running << std::endl;
            }
        }
        {
            std::lock_guard<std::mutex> lock(progress.print);
            for (const auto& step : build.steps) {
                if (verbose) std::cout << step.command << std::endl;
                std::cerr << step.output << std::flush;
            }
        }
        if (!build.error.empty()) {
            failure = build.error;
            break;
        }
    }
    // Commands still running finish before the work directory is removed.
    for (auto& thread : workers) thread.join();
    if (!failure.empty()) fail(failure);
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
    const bool linking = options.module.empty() && !project.entry.empty();
    const auto started = Clock::now();
    size_t width = 0;
    for (const auto& source : sources) width = std::max(width, source.first.size());
    const auto optimize = options.profile == "debug" ? "-O0" : "-O2";
    const auto known = interfaces(project, sources, compiler, destination);

    // A module is skipped when its published outputs carry the key of what
    // they would be built from now: its interface key (its sources, its
    // dependencies' keys and the compiler), clang, the optimization level,
    // and whether it is the entry module. A changed dependency changes the
    // key of every module above it, so those rebuild too.
    const auto clangTool = toolIdentity(clang);
    std::map<std::string, std::string> objectKeys;
    std::set<std::string> upToDate;
    for (const auto& [id, files] : sources) {
        (void)files;
        const bool library = id != project.entry;
        Strings parts{"duck-object 1", known.keys.at(id), optimize, library ? "library" : "entry"};
        parts.insert(parts.end(), clangTool.begin(), clangTool.end());
        objectKeys[id] = hashOf(parts);
        if (fs::exists(destination / (id + ".o")) && fs::exists(destination / (id + ".ll")) &&
            (!library || known.fresh.count(id)) && stampKey(destination / (id + ".stamp")) == objectKeys[id])
            upToDate.insert(id);
    }
    auto executable = fs::u8path(project.projectName);
#ifdef _WIN32
    executable += ".exe";
#endif
    std::string linkKey;
    if (linking) {
        Strings parts{"duck-link 1"};
        for (const auto& [id, files] : sources) {
            (void)files;
            parts.push_back(objectKeys[id]);
        }
        // A native library given by path relinks when the file changes.
        for (const auto& flag : flags) {
            std::error_code error;
            if (!fs::is_regular_file(fs::u8path(flag), error)) parts.push_back(flag);
            else for (auto& part : toolIdentity(flag)) parts.push_back(part);
        }
        parts.insert(parts.end(), clangTool.begin(), clangTool.end());
        linkKey = hashOf(parts);
    }
    const bool link = linking && (upToDate.size() < sources.size() || !fs::exists(destination / executable) ||
                                  stampKey(destination / (executable.u8string() + ".stamp")) != linkKey);

    Progress progress((sources.size() - upToDate.size()) * 2 + (link ? 1 : 0), width);
    std::cout << "duck build: " << sources.size() << " module(s), " << upToDate.size() << " up to date, "
              << progress.total << " steps (" << options.profile << "), " << known.fresh.size()
              << " interface(s) up to date" << std::endl;
    WorkDirectory work(destination);
    compileModules(project, sources, compiler, clang, optimize, work.path, progress, options.verbose, known, upToDate);
    Paths artifacts;
    Strings objects;
    for (const auto& [id, files] : sources) {
        (void)files;
        if (upToDate.count(id)) {
            objects.push_back((destination / (id + ".o")).u8string());
            continue;
        }
        auto ir = work.path / (id + ".ll"), object = work.path / (id + ".o"), stamp = work.path / (id + ".stamp");
        objects.push_back(object.u8string());
        artifacts.insert(artifacts.end(), {ir, object});
        if (id != project.entry) artifacts.push_back(work.path / (id + ".dki"));
        // Published last, so a stamp never describes outputs it did not replace.
        writeStamp(stamp, objectKeys[id]);
        artifacts.push_back(stamp);
    }
    if (link) {
        const auto output = work.path / executable;
        Strings command{clang};
        command.insert(command.end(), objects.begin(), objects.end());
        command.insert(command.end(), flags.begin(), flags.end());
        command.insert(command.end(), {"-o", output.u8string()});
        const auto start = Clock::now();
        run(command, options.verbose);
        progress.finished("link ", output.filename().u8string(), Clock::now() - start);
        const auto stamp = work.path / (executable.u8string() + ".stamp");
        writeStamp(stamp, linkKey);
        artifacts.insert(artifacts.end(), {output, stamp});
    }
    for (const auto& artifact : artifacts) duckbuild::publish(artifact, destination / artifact.filename());
    std::cout << "Built " << (options.module.empty() ? project.projectName : "module " + options.module)
              << " in " << seconds(Clock::now() - started) << ": " << destination.u8string() << '\n';
}

static void help() {
    std::cout << "Build modular Duck projects (.dkp, TOML syntax)\n"
        "Usage: duck build PROJECT [--module MODULE] [--enable MODULE] [--disable MODULE]\n"
        "                        [--profile debug|release] [--compiler PATH] [--clang PATH] [--verbose]\n"
        "--module builds objects for the module and its dependencies without linking.\n"
        "Each step prints [done/total] as it finishes; --verbose also prints every tool command.\n"
        "--enable and --disable may be repeated. Tool overrides resolve from the working directory.\n"
        "\n"
        "Usage: duck test PROJECT [--module MODULE] [--filter TEXT] [--list] [build options above]\n"
        "Builds each module's T{ } tests into its own executable, separate from the program, and runs them.\n"
        "--module tests one module. --filter runs tests whose name contains TEXT and may be repeated.\n"
        "--list prints test names without running them.\n"
        "--compare-schedules runs the tests repeatedly under the sequential schedule and each\n"
        "accepted parallel schedule, discards any that fail, and compares timings. --samples N\n"
        "sets the samples per schedule (default 5).\n";
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
            if (!testing && argument == "--verbose") {
                options.verbose = true;
                continue;
            }
            if (testing && argument == "--compare-schedules") {
                options.compareSchedules = true;
                continue;
            }
            auto equals = argument.find('=');
            auto key = argument.substr(0, equals);
            if (key != "--module" && key != "--enable" && key != "--disable" && key != "--profile" && key != "--compiler" && key != "--clang" &&
                !(testing && (key == "--filter" || key == "--samples")))
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
            else if (key == "--samples") {
                if (!std::regex_match(value, std::regex("[0-9]{1,5}")) || std::stoi(value) < 1)
                    fail("--samples requires a positive whole number");
                options.samples = std::stoi(value);
            }
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
        if (options.list && options.compareSchedules) fail("--list and --compare-schedules cannot be combined");
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
