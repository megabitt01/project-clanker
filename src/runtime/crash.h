#pragma once
#include <cstdint>
#include <string>

struct _EXCEPTION_POINTERS;
struct _CONTEXT;

namespace rt {

void crash_init();
void crash_note_first_chance(_EXCEPTION_POINTERS *ep);
// Exception filter used around every Xbox thread: logs and terminates.
long crash_filter(_EXCEPTION_POINTERS *ep);

// Symbolize an address in either the XBE (from swbf2.sym) or the runtime DLL.
std::string symbolize(uint32_t addr);
void symbols_load(const char *path);

// Logs registers and a heuristic stack trace for a thread context.
void dump_context(const char *title, const _CONTEXT *ctx);
// Suspends and dumps every Xbox thread (bound to Ctrl+Break / F12).
void dump_all_threads(const char *reason);
// Samples every Xbox thread's EIP for `ms` milliseconds and logs the hottest
// functions per thread (debug aid).
void profile_threads(uint32_t ms);

} // namespace rt
