#include "Process.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace duckbuild {
fs::path executable(const std::string& value, const fs::path& root) {
    auto path = fs::u8path(value);
    if (path.is_absolute() || value.find_first_of("/\\") != std::string::npos) {
        path = fs::weakly_canonical(root / path);
        if (fs::is_regular_file(path)) return path;
    } else {
#ifdef _WIN32
        DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        std::wstring environment(length, L'\0');
        if (length) environment.resize(GetEnvironmentVariableW(L"PATH", environment.data(), length));
        std::string paths = fs::path(environment).u8string();
        constexpr char separator = ';';
#else
        const char* environment = std::getenv("PATH");
        std::string paths = environment ? environment : "";
        constexpr char separator = ':';
#endif
        size_t start = 0;
        do {
            auto end = paths.find(separator, start);
            auto directory = paths.substr(start, end - start);
            if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"')
                directory = directory.substr(1, directory.size() - 2);
            auto candidate = fs::u8path(directory) / path;
#ifdef _WIN32
            if (!candidate.has_extension()) candidate += ".exe";
            if (fs::is_regular_file(candidate)) return fs::absolute(candidate);
#else
            if (fs::is_regular_file(candidate) && access(candidate.c_str(), X_OK) == 0)
                return fs::absolute(candidate);
#endif
            if (end == std::string::npos) break;
            start = end + 1;
        } while (true);
    }
    throw std::runtime_error("tool not found: " + value);
}

#ifdef _WIN32
// Encode each argument using the Windows CRT command-line rules, including
// trailing backslashes and embedded quotes. No shell interprets the command.
static std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result += L'\\';
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#endif

int execute(const std::vector<std::string>& arguments) {
    std::cout.flush();
#ifdef _WIN32
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += quote(fs::u8path(argument).wstring());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto application = fs::u8path(arguments.front()).wstring();
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("cannot launch tool: " + arguments.front() +
                                 " (Windows error " + std::to_string(GetLastError()) + ")");
    CloseHandle(process.hThread);
    DWORD status = 1;
    bool finished = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hProcess);
    if (!finished) throw std::runtime_error("cannot wait for tool: " + arguments.front());
#else
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    auto child = fork();
    if (child < 0) throw std::runtime_error("cannot launch tool: " + std::string(std::strerror(errno)));
    if (child == 0) { execv(argv[0], argv.data()); _exit(127); }
    int waitStatus = 0;
    while (waitpid(child, &waitStatus, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("cannot wait for tool");
    }
    int status = WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : 128 + WTERMSIG(waitStatus);
#endif
    return static_cast<int>(status);
}

void run(const std::vector<std::string>& arguments, bool echo) {
    if (echo) {
        for (const auto& argument : arguments) std::cout << '"' << argument << "\" ";
        std::cout << std::endl;
    }
    const int status = execute(arguments);
    if (status != 0) throw std::runtime_error("tool failed with exit code " +
        std::to_string(status) + ": " + arguments.front());
}

void publish(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("cannot publish " + destination.u8string() +
                                 " (Windows error " + std::to_string(GetLastError()) + ")");
#else
    fs::rename(source, destination);
#endif
}
}
