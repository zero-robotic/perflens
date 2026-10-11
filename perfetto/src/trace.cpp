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
bool Enabled(const char* category) {
  return category && *category && ready.load(std::memory_order_acquire) &&
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
      perfetto::Tracing::Initialize(args);
      if (!perfetto::TrackEvent::Register()) throw std::runtime_error("TrackEvent registration failed");
      ready.store(true, std::memory_order_release);
    });
    return 0;
  } catch (...) { return -1; }
}
int pl_trace_enabled(const char* category) {
  try { return Enabled(category); } catch (...) { return 0; }
}
void pl_trace_begin(const char* category, const char* name) {
  try {
    if (!name || !Enabled(category)) return;
    const perfetto::DynamicCategory dynamic(category);
    TRACE_EVENT_BEGIN(dynamic, perfetto::DynamicString(name));
  } catch (...) {}
}
void pl_trace_end(const char* category) {
  try {
    if (!Enabled(category)) return;
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
