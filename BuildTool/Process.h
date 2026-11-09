#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace duckbuild {
std::filesystem::path executable(const std::string& value, const std::filesystem::path& root);
// Echoes the command when asked and throws if it exits with a nonzero status.
void run(const std::vector<std::string>& arguments, bool echo = true);
// Collects everything the command prints into output, then throws if it failed.
// Safe to call from several threads at once.
void run(const std::vector<std::string>& arguments, std::string& output);
// The text run() echoes for a command.
std::string commandLine(const std::vector<std::string>& arguments);
// Runs a command with inherited output and returns its exit status.
int execute(const std::vector<std::string>& arguments);
// Runs a command, collects its standard output, and returns its exit status.
// mergeErrors collects standard error into the same text.
int capture(const std::vector<std::string>& arguments, std::string& output, bool mergeErrors = false);
// Sets an environment variable for later commands; empty removes it.
void setEnvironment(const std::string& name, const std::string& value);
void publish(const std::filesystem::path& source, const std::filesystem::path& destination);
}
