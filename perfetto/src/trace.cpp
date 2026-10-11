#include "perflens/trace.h"
#include <perfetto.h>
#include <atomic>
#include <mutex>
#include <stdexcept>

PERFETTO_DEFINE_CATEGORIES(perfetto::Category("perflens"));
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace {
std::once_flag initialized;
std::atomic<bool> ready{false};
std::atomic<uint64_t> generation{0};
class Observer final : public perfetto::TrackEventSessionObserver {
  void OnStart(const perfetto::DataSourceBase::StartArgs&) override {
    generation.fetch_add(1, std::memory_order_release);
  }
  void OnStop(const perfetto::DataSourceBase::StopArgs&) override {
    generation.fetch_add(1, std::memory_order_release);
  }
};
bool Enabled(const char* category) {
  return category && *category && ready.load(std::memory_order_acquire) &&
         (generation.load(std::memory_order_acquire) & 1) &&
         perfetto::TrackEvent::IsEnabled() &&
         perfetto::TrackEvent::IsDynamicCategoryEnabled(perfetto::DynamicCategory(category));
}
}  // namespace

extern "C" {
int pl_trace_init(void) {
  try {
    std::call_once(initialized, [] {
      perfetto::TracingInitArgs args;
      args.backends = perfetto::kSystemBackend;
      args.enable_system_consumer = false;
      args.supports_multiple_data_source_instances = false;
      perfetto::Tracing::Initialize(args);
      // Intentionally process-lifetime, as SDK callbacks can run during exit.
      static auto* observer = new Observer;
      perfetto::TrackEvent::AddSessionObserver(observer);
      if (!perfetto::TrackEvent::Register()) throw std::runtime_error("TrackEvent registration failed");
      ready.store(true, std::memory_order_release);
    });
    return 0;
  } catch (...) { return -1; }
}
int pl_trace_enabled(const char* category) {
  try { return Enabled(category); } catch (...) { return 0; }
}
uint64_t pl_trace_begin(const char* category, const char* name) {
  try {
    const auto token = generation.load(std::memory_order_acquire);
    if (!(token & 1) || !name || !Enabled(category)) return 0;
    const perfetto::DynamicCategory dynamic(category);
    TRACE_EVENT_BEGIN(dynamic, perfetto::DynamicString(name));
    return token;
  } catch (...) { return 0; }
}
void pl_trace_end(const char* category, uint64_t token) {
  try {
    if (!token || token != generation.load(std::memory_order_acquire) || !Enabled(category)) return;
    const perfetto::DynamicCategory dynamic(category);
    TRACE_EVENT_END(dynamic);
  } catch (...) {}
}
void pl_trace_instant(const char* category, const char* name) {
  try {
    if (name && Enabled(category)) {
      const perfetto::DynamicCategory dynamic(category);
      TRACE_EVENT_INSTANT(dynamic, perfetto::DynamicString(name));
    }
  } catch (...) {}
}
void pl_trace_counter(const char* category, const char* name, int64_t value) {
  try {
    if (name && Enabled(category)) {
      // Include the category in the name to avoid collisions between counters.
      std::string full_name = std::string(category) + "/" + name;
      const perfetto::DynamicCategory dynamic(category);
      TRACE_COUNTER(dynamic,
                    perfetto::CounterTrack(perfetto::DynamicString(full_name)), value);
    }
  } catch (...) {}
}
void pl_trace_flow_begin(const char* category, const char* name, uint64_t id) {
  try {
    if (name && Enabled(category)) {
      const perfetto::DynamicCategory dynamic(category);
      TRACE_EVENT_INSTANT(dynamic, perfetto::DynamicString(name),
                          perfetto::Flow::ProcessScoped(id));
    }
  } catch (...) {}
}
void pl_trace_flow_end(const char* category, const char* name, uint64_t id) {
  try {
    if (name && Enabled(category)) {
      const perfetto::DynamicCategory dynamic(category);
      TRACE_EVENT_INSTANT(dynamic, perfetto::DynamicString(name),
                          perfetto::TerminatingFlow::ProcessScoped(id));
    }
  } catch (...) {}
}
void pl_trace_flush(void) {
  try { if (ready.load(std::memory_order_acquire)) perfetto::TrackEvent::Flush(); } catch (...) {}
}
}
