#include <perflens/trace.hpp>
int main() {
  int evaluated = 0;
  PL_TRACE_SCOPE((++evaluated, "demo"), "scope");
  PL_TRACE_INSTANT("demo", (++evaluated, "event"));
  PL_TRACE_COUNTER("demo", "value", ++evaluated);
  PL_TRACE_FLOW_BEGIN("demo", "flow", ++evaluated);
  PL_TRACE_FLOW_END("demo", "flow", ++evaluated);
  return !perflens::trace::Initialize() || evaluated != 0;
}
