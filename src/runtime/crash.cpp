#include "crash.h"
#include "log.h"
#include "kernel/thread.h"

#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

struct Sym {
    uint32_t addr;
    std::string name;
};
static std::vector<Sym> g_syms;
static std::mutex g_dbghelp_mutex;
static bool g_dbghelp_ok;

void symbols_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_WARN("crash", "symbol map %s not found; crash reports will show raw addresses", path);
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned a;
        char name[400];
        if (sscanf(line, "%x %399s", &a, name) == 2)
            g_syms.push_back({a, name});
    }
    fclose(f);
    std::sort(g_syms.begin(), g_syms.end(), [](const Sym &a, const Sym &b) { return a.addr < b.addr; });
    LOG_INFO("crash", "loaded %zu XBE symbols", g_syms.size());
}

std::string symbolize(uint32_t addr)
{
    char buf[600];
    if (addr >= 0x10000 && addr < 0x00D74000) {
        auto it = std::upper_bound(g_syms.begin(), g_syms.end(), addr,
                                   [](uint32_t a, const Sym &s) { return a < s.addr; });
        if (it != g_syms.begin()) {
            --it;
            snprintf(buf, sizeof buf, "%08X xbe!%s+0x%X", addr, it->name.c_str(), addr - it->addr);
            return buf;
        }
        snprintf(buf, sizeof buf, "%08X xbe", addr);
        return buf;
    }
    if (g_dbghelp_ok) {
        std::lock_guard<std::mutex> lock(g_dbghelp_mutex);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = reinterpret_cast<SYMBOL_INFO *>(storage);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), addr, &disp, si)) {
            snprintf(buf, sizeof buf, "%08X %s+0x%llX", addr, si->Name, disp);
            return buf;
        }
    }
    snprintf(buf, sizeof buf, "%08X", addr);
    return buf;
}

static bool readable(uint32_t a, uint32_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void *>(uintptr_t(a)), &mbi, sizeof mbi))
        return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return a + n <= uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
}

// A stack slot "looks like" a return address if the bytes just before it
// encode a call instruction.
static bool looks_like_return(uint32_t a)
{
    bool in_xbe = a >= 0x11000 && a < 0x00446000;
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&looks_like_return), &self);
    HMODULE owner = nullptr;
    bool in_rt = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(uintptr_t(a)), &owner) && owner == self;
    if (!in_xbe && !in_rt)
        return false;
    if (!readable(a - 7, 7))
        return false;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(uintptr_t(a));
    if (p[-5] == 0xE8) return true;                                  // call rel32
    if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) return true;       // call r/m (reg)
    if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) return true;       // call [r+d8]
    if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) return true;       // call [d32]/[r+d32]
    if (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2) return true;       // call [sib+d32]
    return false;
}

void dump_context(const char *title, const CONTEXT *c)
{
    LOG_ERROR("crash", "==== %s ====", title);
    LOG_ERROR("crash", "EIP %s", symbolize(c->Eip).c_str());
    LOG_ERROR("crash", "EAX %08lX EBX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX", c->Eax, c->Ebx, c->Ecx,
              c->Edx, c->Esi, c->Edi);
    LOG_ERROR("crash", "EBP %08lX ESP %08lX EFL %08lX", c->Ebp, c->Esp, c->EFlags);
    if (readable(c->Eip, 16)) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(uintptr_t(c->Eip));
        LOG_ERROR("crash", "code: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", p[0], p[1], p[2],
                  p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
    }
    int found = 0;
    for (uint32_t sp = c->Esp; found < 40 && sp < c->Esp + 0x4000; sp += 4) {
        if (!readable(sp, 4))
            break;
        uint32_t v = *reinterpret_cast<const uint32_t *>(uintptr_t(sp));
        if (looks_like_return(v)) {
            LOG_ERROR("crash", "  [esp+%04X] %s", sp - c->Esp, symbolize(v).c_str());
            found++;
        }
    }
    log_flush();
}

static thread_local int t_first_chance_depth;

void crash_note_first_chance(EXCEPTION_POINTERS *ep)
{
    if (t_first_chance_depth)
        return;
    t_first_chance_depth++;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_STACK_OVERFLOW) {
        LOG_DEBUG("crash", "first-chance exception %08lX at %s (addr %08lX)", code,
                  symbolize(ep->ContextRecord->Eip).c_str(),
                  ep->ExceptionRecord->NumberParameters > 1 ? DWORD(ep->ExceptionRecord->ExceptionInformation[1]) : 0);
    }
    t_first_chance_depth--;
}

long crash_filter(EXCEPTION_POINTERS *ep)
{
    char title[200];
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    snprintf(title, sizeof title, "unhandled exception %08lX (%s 0x%08lX)", er->ExceptionCode,
             er->NumberParameters > 0 && er->ExceptionInformation[0] ? "write" : "read",
             er->NumberParameters > 1 ? DWORD(er->ExceptionInformation[1]) : 0);
    dump_context(title, ep->ContextRecord);
    dump_all_threads("crash");
    fatal("%s at %s\nSee swbf2.log for details.", title, symbolize(ep->ContextRecord->Eip).c_str());
}

static LONG WINAPI top_level_filter(EXCEPTION_POINTERS *ep)
{
    return crash_filter(ep);
}

void dump_all_threads(const char *reason)
{
    LOG_ERROR("crash", "---- thread dump (%s) ----", reason);
    kthread_for_each([](HostThreadInfo &ti) {
        if (ti.host_id == GetCurrentThreadId())
            return;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ti.host_id);
        if (!h)
            return;
        if (SuspendThread(h) != DWORD(-1)) {
            CONTEXT ctx = {};
            ctx.ContextFlags = CONTEXT_FULL;
            BOOL ok = GetThreadContext(h, &ctx);
            ResumeThread(h); // resume before logging: the thread may hold the heap/log locks
            if (ok) {
                char title[128];
                snprintf(title, sizeof title, "thread %s (host %lu)", ti.name, ti.host_id);
                dump_context(title, &ctx);
            }
        }
        CloseHandle(h);
    });
}

void profile_threads(uint32_t ms)
{
    // Only GetThreadContext runs while a thread is suspended: it may hold the
    // heap or log lock, so anything that allocates must wait until resume.
    struct T {
        DWORD id;
        std::string name;
        HANDLE h;
        std::vector<uint32_t> eips;
    };
    static std::vector<HostThreadInfo> *list;
    std::vector<HostThreadInfo> l;
    list = &l;
    kthread_for_each([](HostThreadInfo &ti) { list->push_back(ti); });
    std::vector<T> ts;
    for (auto &i : l) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, i.host_id);
        if (h) {
            ts.push_back({i.host_id, i.name, h, {}});
            ts.back().eips.reserve(ms + 16);
        }
    }
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        for (T &t : ts) {
            if (t.eips.size() == t.eips.capacity()) continue;
            if (SuspendThread(t.h) == DWORD(-1)) continue;
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            BOOL ok = GetThreadContext(t.h, &c);
            ResumeThread(t.h);
            if (ok) t.eips.push_back(c.Eip);
        }
        Sleep(1);
    }
    for (T &t : ts) {
        std::map<std::string, int> hits;
        for (uint32_t e : t.eips) {
            std::string s = symbolize(e);
            size_t plus = s.find('+');
            hits[plus != std::string::npos && s.size() > 9 ? s.substr(9, plus - 9) : s]++;
        }
        std::vector<std::pair<int, std::string>> v;
        for (auto &kv : hits) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        std::string line;
        for (size_t i = 0; i < v.size() && i < 6; i++)
            line += v[i].second + " " + std::to_string(v[i].first * 100 / int(t.eips.empty() ? 1 : t.eips.size())) + "%  ";
        LOG_INFO("prof", "%-12s %s", t.name.c_str(), line.c_str());
        CloseHandle(t.h);
    }
}

static BOOL WINAPI console_handler(DWORD type)
{
    if (type == CTRL_BREAK_EVENT) {
        dump_all_threads("Ctrl+Break");
        return TRUE;
    }
    return FALSE;
}

void crash_init()
{
    SetUnhandledExceptionFilter(top_level_filter);
    SetConsoleCtrlHandler(console_handler, TRUE);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    g_dbghelp_ok = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
}

} // namespace rt
