#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace perflens::cli {
struct Options {
  uint32_t duration_ms = 10000;
  uint32_t buffer_kb = 65536;
  uint32_t ftrace_buffer_kb = 2048;
  std::vector<std::string> categories;
};
std::string MakeConfig(const Options&, const std::set<std::string>& available_events);
uint32_t ParseNumber(const std::string&, uint32_t min, uint32_t max);
std::vector<std::string> ParseCategories(const std::string&);
}
