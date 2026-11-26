#include "Process.h"
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <algorithm>
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
// Every tool runs in one job object with memory limits. A compile whose memory
// runs away (clang has needed over 20 GB on a large module) then fails with an
// error instead of paging the whole machine into a freeze. Each process may
// commit DUCK_TOOL_MEMORY_MB, by default 60% of physical memory, and all tools
// together 80%; DUCK_TOOL_MEMORY_MB=0 lifts the limits. The job also ends the
// tools if the build tool itself is killed.
static size_t processLimit = 0;

static HANDLE toolJob() {
    static std::once_flag once;
    static HANDLE job = nullptr;
    std::call_once(once, [] {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (!GlobalMemoryStatusEx(&memory)) return;
        unsigned long long perProcess = memory.ullTotalPhys / 10 * 6, total = memory.ullTotalPhys / 10 * 8;
        if (const char* setting = std::getenv("DUCK_TOOL_MEMORY_MB")) {
            char* end = nullptr;
            const auto megabytes = std::strtoull(setting, &end, 10);
            if (end == setting || *end) throw std::runtime_error("DUCK_TOOL_MEMORY_MB must be a whole number of megabytes");
            perProcess = megabytes << 20;
            total = std::max(total, perProcess);
        }
        job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (perProcess) {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY;
            limits.ProcessMemoryLimit = static_cast<SIZE_T>(perProcess);
            limits.JobMemoryLimit = static_cast<SIZE_T>(total);
            processLimit = static_cast<size_t>(perProcess);
        }
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job);
            job = nullptr;
            processLimit = 0;
        }
    });
    return job;
}

// Starts a process created suspended inside the tool job. A process the job
// cannot take (unlikely on Windows 8 and later) still runs, without limits.
static void startInJob(const PROCESS_INFORMATION& process) {
    if (HANDLE job = toolJob()) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
}

// A tool that failed near its memory limit was most likely stopped by it.
static void explainMemoryLimit(HANDLE process, DWORD status, const std::string& tool) {
    if (status == 0 || processLimit == 0) return;
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(process, &counters, sizeof(counters))) return;
    if (counters.PeakPagefileUsage < processLimit / 10 * 9) return;
    throw std::runtime_error(fs::u8path(tool).filename().u8string() + " ran out of its memory limit (" +
                             std::to_string(processLimit >> 20) + " MB). Set DUCK_TOOL_MEMORY_MB to raise it, or 0 to lift it.");
}

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
                        CREATE_SUSPENDED, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("cannot launch tool: " + arguments.front() +
                                 " (Windows error " + std::to_string(GetLastError()) + ")");
    startInJob(process);
    CloseHandle(process.hThread);
    DWORD status = 1;
    bool finished = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &status);
    if (finished) {
        try { explainMemoryLimit(process.hProcess, status, arguments.front()); }
        catch (...) { CloseHandle(process.hProcess); throw; }
    }
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

// Held from creating a pipe until the parent closes its write end. A child
// started in that window would inherit the write end of another command's
// pipe, and that command's reader would not see end of file until the
// unrelated child exited.
static std::mutex launching;

static int captureRaw(const std::vector<std::string>& arguments, std::string& output, bool mergeErrors) {
    std::cout.flush();
    output.clear();
    char buffer[4096];
    std::unique_lock<std::mutex> launch(launching);
#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0) || !SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("cannot create pipe for: " + arguments.front());
    std::wstring command;
    for (const auto& argument : arguments) {
        if (!command.empty()) command += L' ';
        command += quote(fs::u8path(argument).wstring());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = mergeErrors ? writeEnd : GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const auto application = fs::u8path(arguments.front()).wstring();
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
        const auto error = GetLastError();
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        throw std::runtime_error("cannot launch tool: " + arguments.front() + " (Windows error " + std::to_string(error) + ")");
    }
    startInJob(process);
    CloseHandle(writeEnd);
    launch.unlock();
    CloseHandle(process.hThread);
    DWORD count = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &count, nullptr) && count) output.append(buffer, count);
    CloseHandle(readEnd);
    DWORD status = 1;
    bool finished = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &status);
    if (finished) {
        try { explainMemoryLimit(process.hProcess, status, arguments.front()); }
        catch (...) { CloseHandle(process.hProcess); throw; }
    }
    CloseHandle(process.hProcess);
    if (!finished) throw std::runtime_error("cannot wait for tool: " + arguments.front());
    return static_cast<int>(status);
#else
    int ends[2];
    if (pipe(ends) != 0) throw std::runtime_error("cannot create pipe for: " + arguments.front());
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    auto child = fork();
    if (child < 0) throw std::runtime_error("cannot launch tool: " + std::string(std::strerror(errno)));
    if (child == 0) {
        dup2(ends[1], 1);
        if (mergeErrors) dup2(ends[1], 2);
        close(ends[0]);
        close(ends[1]);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(ends[1]);
    launch.unlock();
    for (ssize_t count; (count = read(ends[0], buffer, sizeof(buffer))) != 0;) {
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        output.append(buffer, static_cast<size_t>(count));
    }
    close(ends[0]);
    int waitStatus = 0;
    while (waitpid(child, &waitStatus, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("cannot wait for tool");
    }
    return WIFEXITED(waitStatus) ? WEXITSTATUS(waitStatus) : 128 + WTERMSIG(waitStatus);
#endif
}

void setEnvironment(const std::string& name, const std::string& value) {
#ifdef _WIN32
    const auto wide = fs::u8path(name).wstring();
    if (value.empty()) SetEnvironmentVariableW(wide.c_str(), nullptr);
    else SetEnvironmentVariableW(wide.c_str(), fs::u8path(value).c_str());
#else
    if (value.empty()) unsetenv(name.c_str());
    else setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::string commandLine(const std::vector<std::string>& arguments) {
    std::string line;
    for (const auto& argument : arguments) line += '"' + argument + "\" ";
    return line;
}

static void requireSuccess(int status, const std::vector<std::string>& arguments) {
    if (status != 0) throw std::runtime_error("tool failed with exit code " +
        std::to_string(status) + ": " + arguments.front());
}

void run(const std::vector<std::string>& arguments, bool echo) {
    if (echo) std::cout << commandLine(arguments) << std::endl;
    requireSuccess(execute(arguments), arguments);
}

int capture(const std::vector<std::string>& arguments, std::string& output, bool mergeErrors) {
    const int status = captureRaw(arguments, output, mergeErrors);
    // Tools write CRLF into the pipe on Windows, and the stream this is
    // replayed through would turn each LF into CRLF again.
    size_t kept = 0;
    for (size_t i = 0; i < output.size(); ++i)
        if (output[i] != '\r' || i + 1 == output.size() || output[i + 1] != '\n') output[kept++] = output[i];
    output.resize(kept);
    return status;
}

void run(const std::vector<std::string>& arguments, std::string& output) {
    requireSuccess(capture(arguments, output, true), arguments);
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
