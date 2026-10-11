#include <perflens/trace.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
int main(int argc, char** argv) {
  int seconds = 10;
  int wait_ms = 30000;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg != "--seconds" && arg != "--wait-ms") || i + 1 == argc) {
      std::cerr << "Usage: perflens-demo [--seconds N] [--wait-ms N]\n"; return 1;
    }
    char* end = nullptr;
    auto value = std::strtol(argv[++i], &end, 10);
    if (!end || *end || value < 0 || value > 3600000) return 1;
    (arg == "--seconds" ? seconds : wait_ms) = static_cast<int>(value);
  }
  if (!perflens::trace::Initialize()) return 1;
  std::cout << "Waiting for tracing category 'demo'..." << std::endl;
  auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  while (!pl_trace_enabled("demo") && std::chrono::steady_clock::now() < wait_until)
    std::this_thread::sleep_for(10ms);
  if (!pl_trace_enabled("demo")) { std::cerr << "No recording enabled demo before timeout\n"; return 2; }
  std::atomic<uint64_t> checksum{0};
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  std::vector<std::thread> workers;
  for (int id = 0; id < 2; ++id) {
    PL_TRACE_FLOW_BEGIN("demo", "Dispatch", id + 1);
    workers.emplace_back([&, id] {
      PL_TRACE_FLOW_END("demo", "WorkerStart", id + 1);
      int iterations = 0;
      while (std::chrono::steady_clock::now() < deadline) {
        PL_TRACE_SCOPE("demo", "Iteration");
        {
          PL_TRACE_SCOPE("demo", "Compute");
          uint64_t value = static_cast<uint64_t>(id + iterations + 1);
          for (int i = 0; i < 400000; ++i) value = value * 6364136223846793005ULL + 1;
          checksum.fetch_xor(value, std::memory_order_relaxed);
        }
        {
          PL_TRACE_SCOPE("demo", "Sleep");
          std::this_thread::sleep_for(5ms);
        }
        PL_TRACE_INSTANT("demo", "IterationDone");
        PL_TRACE_INSTANT("excluded", "MustNotAppear");
        PL_TRACE_COUNTER("demo", "iterations", ++iterations);
      }
      pl_trace_flush();
    });
  }
  for (auto& worker : workers) worker.join();
  pl_trace_flush();
  std::cout << "Workload finished: " << checksum << '\n';
}
