#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace duckbuild {
std::filesystem::path executable(const std::string& value, const std::filesystem::path& root);
// Echoes the command when asked and throws if it exits with a nonzero status.
void run(const std::vector<std::string>& arguments, bool echo = true);
// Runs a command with inherited output and returns its exit status.
int execute(const std::vector<std::string>& arguments);
void publish(const std::filesystem::path& source, const std::filesystem::path& destination);
}
