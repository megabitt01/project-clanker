#include "object.h"
#include "thread.h"
#include "../log.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace rt {

uint32_t g_ExEventObjectType[8] = {'vevE'};
uint32_t g_PsThreadObjectType[8] = {'erhT'};
uint32_t g_IoFileObjectType[8] = {'eliF'};

void *ThreadObj::body() { return &thread->kthread; }
XDISPATCHER_HEADER *ThreadObj::dispatcher() { return &thread->kthread.Header; }

static std::mutex g_ob_mutex;
static std::unordered_map<XHANDLE, XObject *> g_handles;
static std::unordered_map<XObject *, int> g_handle_refs;   // open handles per object
static std::unordered_map<std::string, XObject *> g_names;
static XHANDLE g_next_handle = 0x1000;

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

std::string ansi_to_string(const XANSI_STRING *s)
{
    if (!s || !s->Buffer)
        return {};
    return std::string(s->Buffer, s->Length);
}

XHANDLE ob_insert(XObject *obj)
{
    std::lock_guard<std::mutex> lock(g_ob_mutex);
    XHANDLE h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = obj;
    g_handle_refs[obj]++;
    return h;
}

XObject *ob_lookup(XHANDLE h)
{
    std::lock_guard<std::mutex> lock(g_ob_mutex);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}

XHANDLE ob_duplicate(XHANDLE h)
{
    XObject *o = ob_lookup(h);
    return o ? ob_insert(o) : 0;
}

bool ob_close(XHANDLE h)
{
    XObject *del = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_ob_mutex);
        auto it = g_handles.find(h);
        if (it == g_handles.end())
            return false;
        XObject *o = it->second;
        g_handles.erase(it);
        if (--g_handle_refs[o] == 0) {
            g_handle_refs.erase(o);
            // Dispatcher objects may still be referenced by pointer (waits,
            // ObReferenceObjectByHandle); only file-like objects are freed.
            if (o->kind == ObjKind::File)
                del = o;
        }
    }
    delete del;
    return true;
}

XObject *ob_find_named(const std::string &name)
{
    std::lock_guard<std::mutex> lock(g_ob_mutex);
    auto it = g_names.find(lower(name));
    return it == g_names.end() ? nullptr : it->second;
}

void ob_add_named(XObject *obj)
{
    std::lock_guard<std::mutex> lock(g_ob_mutex);
    g_names[lower(obj->name)] = obj;
}

void ob_remove_named(const std::string &name)
{
    std::lock_guard<std::mutex> lock(g_ob_mutex);
    g_names.erase(lower(name));
}

} // namespace rt
