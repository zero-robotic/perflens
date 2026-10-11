#include "config.hpp"
#include "config_template.hpp"
#include <algorithm>
#include <stdexcept>

namespace perflens::cli {
uint32_t ParseNumber(const std::string& text, uint32_t min, uint32_t max) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    throw std::runtime_error("expected an unsigned integer: " + text);
  auto n = std::stoull(text);
  if (n < min || n > max) throw std::runtime_error("number out of range: " + text);
  return static_cast<uint32_t>(n);
}
std::vector<std::string> ParseCategories(const std::string& text) {
  std::vector<std::string> result;
  size_t start = 0;
  do {
    auto end = text.find(',', start);
    auto value = text.substr(start, end == std::string::npos ? end : end - start);
    if (value.empty() || value.size() > 128 || value.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.*?-/") != std::string::npos)
      throw std::runtime_error("invalid category: " + value);
    result.push_back(value);
    if (end == std::string::npos) break;
    start = end + 1;
  } while (true);
  return result;
}
static void Replace(std::string& s, const std::string& key, const std::string& value) {
  auto pos = s.find(key);
  if (pos == std::string::npos) throw std::runtime_error("missing config placeholder " + key);
  s.replace(pos, key.size(), value);
}
std::string MakeConfig(const Options& options, const std::set<std::string>& available) {
  for (const auto* required : {"sched/sched_switch", "sched/sched_waking"})
    if (!available.count(required)) throw std::runtime_error(std::string("required kernel event unavailable: ") + required);
  std::string optional;
  for (const auto* event : {"sched/sched_wakeup_new", "sched/sched_process_exit",
       "sched/sched_process_free", "task/task_newtask", "task/task_rename",
       "power/cpu_frequency", "power/cpu_idle"}) {
    if (available.count(event)) optional += std::string("      ftrace_events: \"") + event + "\"\n";
  }
  std::string track;
  if (!options.categories.empty()) {
    track = "data_sources { config { name: \"track_event\" track_event_config {\n"
            "  disabled_categories: \"*\"\n";
    for (const auto& category : options.categories) {
      ParseCategories(category);  // Validate values even when called outside the CLI.
      track += "  enabled_categories: \"" + category + "\"\n";
    }
    track += "} } }\n";
  }
  std::string config = kCpuConfigTemplate;
  Replace(config, "@DURATION_MS@", std::to_string(options.duration_ms));
  Replace(config, "@BUFFER_KB@", std::to_string(options.buffer_kb));
  Replace(config, "@FTRACE_BUFFER_KB@", std::to_string(options.ftrace_buffer_kb));
  Replace(config, "@OPTIONAL_EVENTS@", optional);
  Replace(config, "@TRACK_EVENT@", track);
  return config;
}
}
