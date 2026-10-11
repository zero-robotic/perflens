#include <perflens/trace.h>
int main(void) {
  if (pl_trace_enabled("test") || pl_trace_enabled(0)) return 1;
  pl_trace_begin("test", "before-init");
  pl_trace_end("test");
  pl_trace_begin(0, 0);
  pl_trace_end(0);
  pl_trace_instant(0, 0);
  pl_trace_counter(0, 0, 0);
  pl_trace_flow_begin(0, 0, 1);
  pl_trace_flow_end(0, 0, 1);
  pl_trace_flush();
  if (pl_trace_init() || pl_trace_init()) return 2;
  pl_trace_begin("test", "after-init");
  pl_trace_end("test");
  return 0;
}
