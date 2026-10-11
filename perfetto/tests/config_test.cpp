#include "config.hpp"
#include <iostream>
#include <stdexcept>
using namespace perflens::cli;
void Check(bool ok) { if (!ok) throw std::runtime_error("configuration contract failed"); }
template <typename Fn> void Reject(Fn fn) {
  bool threw = false;
  try { fn(); } catch (const std::exception&) { threw = true; }
  Check(threw);
}
int main() {
  try {
    Options options;
    std::set<std::string> events{"sched/sched_switch", "sched/sched_waking"};
    auto system = MakeConfig(options, events);
    Check(system.find("track_event") == std::string::npos);
    Check(system.find("power/cpu_idle") == std::string::npos);
    Check(system.find("scan_all_processes_on_start: true") != std::string::npos);
    Check(system.find("record_thread_names: true") != std::string::npos);
    Check(system.find("duration_ms: 10000") != std::string::npos);
    Check(system.find('@') == std::string::npos);
    options.categories = ParseCategories("demo,request.*");
    events.insert("power/cpu_idle");
    auto combined = MakeConfig(options, events);
    Check(combined.find("enabled_categories: \"request.*\"") != std::string::npos);
    Check(combined.find("disabled_categories: \"*\"") != std::string::npos);
    Check(combined.find("power/cpu_idle") != std::string::npos);
    Reject([&] { MakeConfig(options, {}); });
    for (const char* input : {"", "demo,", ",demo", "demo\" }", "demo\n", "demo,,other"})
      Reject([&] { ParseCategories(input); });
    for (const char* input : {"-1", "0", "1.5", "12s", "4294967296", "9999999999999999999999999999"})
      Reject([&] { ParseNumber(input, 1, 100); });
    Check(ParseNumber("10", 1, 100) == 10);
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
