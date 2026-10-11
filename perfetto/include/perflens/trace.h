#ifndef PERFLENS_TRACE_H
#define PERFLENS_TRACE_H

#include <stdint.h>
#if defined(__GNUC__)
#define PL_TRACE_API __attribute__((visibility("default")))
#else
#define PL_TRACE_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* Initialize once per process, before starting application threads. Idempotent.
 * Uses PERFETTO_PRODUCER_SOCK_NAME; configure it before initialization.
 * Returns 0 on success, -1 on error. Success means initialized, not connected.
 * Do not fork after initialization without immediately exec'ing in the child.
 */
PL_TRACE_API int pl_trace_init(void);
PL_TRACE_API int pl_trace_enabled(const char* category);

/* Strings are copied as needed by the SDK; dynamic names are supported.
 * Begin/end must be properly nested on the same OS thread. A zero token means
 * inactive. Pass the original category and returned token to end. Tokens guard
 * against an old scope ending in a later recording session.
 */
PL_TRACE_API uint64_t pl_trace_begin(const char* category, const char* name);
PL_TRACE_API void pl_trace_end(const char* category, uint64_t token);
PL_TRACE_API void pl_trace_instant(const char* category, const char* name);
PL_TRACE_API void pl_trace_counter(const char* category, const char* name, int64_t value);
/* Process-scoped flow IDs connect events across threads. Reuse an ID only after
 * its previous flow has ended. These markers do not measure task CPU time.
 */
PL_TRACE_API void pl_trace_flow_begin(const char* category, const char* name, uint64_t id);
PL_TRACE_API void pl_trace_flow_end(const char* category, const char* name, uint64_t id);
/* Flush pending packets on the calling thread; not a service-wide barrier. */
PL_TRACE_API void pl_trace_flush(void);

#ifdef __cplusplus
}
#endif
#endif
