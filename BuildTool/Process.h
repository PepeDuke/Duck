#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace duckbuild {
std::filesystem::path executable(const std::string& value, const std::filesystem::path& root);
void run(const std::vector<std::string>& arguments);
void publish(const std::filesystem::path& source, const std::filesystem::path& destination);
}
