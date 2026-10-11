#ifndef PERFLENS_TRACE_HPP
#define PERFLENS_TRACE_HPP
#include "perflens/trace.h"
#include <string>

namespace perflens::trace {
inline bool Initialize() {
#ifdef PERFLENS_TRACE_DISABLED
  return true;
#else
  return pl_trace_init() == 0;
#endif
}
class Scope final {
 public:
  Scope(const char* category, const char* name) {
#ifndef PERFLENS_TRACE_DISABLED
    // Keep dynamic categories alive until the corresponding end call, even if
    // recording is disabled when the scope starts.
    if (category && name) category_ = category;
    pl_trace_begin(category_.c_str(), name);
#else
    (void)category; (void)name;
#endif
  }
  ~Scope() {
#ifndef PERFLENS_TRACE_DISABLED
    pl_trace_end(category_.c_str());
#endif
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;
 private:
  std::string category_;
};
}  // namespace perflens::trace

#define PL_TRACE_JOIN_IMPL(a, b) a##b
#define PL_TRACE_JOIN(a, b) PL_TRACE_JOIN_IMPL(a, b)
#ifdef PERFLENS_TRACE_DISABLED
#define PL_TRACE_SCOPE(...) ((void)0)
#define PL_TRACE_INSTANT(...) ((void)0)
#define PL_TRACE_COUNTER(...) ((void)0)
#define PL_TRACE_FLOW_BEGIN(...) ((void)0)
#define PL_TRACE_FLOW_END(...) ((void)0)
#else
#define PL_TRACE_SCOPE(category, name) \
  ::perflens::trace::Scope PL_TRACE_JOIN(pl_trace_scope_, __COUNTER__){category, name}
#define PL_TRACE_INSTANT(category, name) pl_trace_instant(category, name)
#define PL_TRACE_COUNTER(category, name, value) pl_trace_counter(category, name, value)
#define PL_TRACE_FLOW_BEGIN(category, name, id) pl_trace_flow_begin(category, name, id)
#define PL_TRACE_FLOW_END(category, name, id) pl_trace_flow_end(category, name, id)
#endif
#endif
