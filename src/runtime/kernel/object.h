#pragma once
#include "xboxkrnl.h"

#include <string>

namespace rt {

struct XThread;

enum class ObjKind { Event, Semaphore, Mutant, Timer, Thread, File, SymbolicLink, Device };

struct XObject {
    ObjKind kind;
    std::string name;
    explicit XObject(ObjKind k) : kind(k) {}
    virtual ~XObject() = default;
    // Pointer handed to the title by ObReferenceObjectByHandle.
    virtual void *body() = 0;
    virtual XDISPATCHER_HEADER *dispatcher() { return nullptr; }
};

struct EventObj : XObject {
    XKEVENT ev{};
    EventObj() : XObject(ObjKind::Event) {}
    void *body() override { return &ev; }
    XDISPATCHER_HEADER *dispatcher() override { return &ev.Header; }
};

struct SemaphoreObj : XObject {
    XKSEMAPHORE sem{};
    SemaphoreObj() : XObject(ObjKind::Semaphore) {}
    void *body() override { return &sem; }
    XDISPATCHER_HEADER *dispatcher() override { return &sem.Header; }
};

struct ThreadObj : XObject {
    XThread *thread;
    explicit ThreadObj(XThread *t) : XObject(ObjKind::Thread), thread(t) {}
    void *body() override;
    XDISPATCHER_HEADER *dispatcher() override;
};

struct SymlinkObj : XObject {
    std::string target;
    SymlinkObj() : XObject(ObjKind::SymbolicLink) {}
    void *body() override { return this; }
};

constexpr XHANDLE X_NtCurrentThread = 0xFFFFFFFE;
constexpr XHANDLE X_NtCurrentProcess = 0xFFFFFFFF;

XHANDLE ob_insert(XObject *obj);
// Returns nullptr for invalid handles. The pointer stays valid until the
// handle is closed.
XObject *ob_lookup(XHANDLE h);
template <class T> T *ob_lookup_as(XHANDLE h, ObjKind kind)
{
    XObject *o = ob_lookup(h);
    return o && o->kind == kind ? static_cast<T *>(o) : nullptr;
}
bool ob_close(XHANDLE h);
XHANDLE ob_duplicate(XHANDLE h);

// Named objects ("\??\D:", named events, ...). Names are case-insensitive.
XObject *ob_find_named(const std::string &name);
void ob_add_named(XObject *obj);
void ob_remove_named(const std::string &name);

// Exported OBJECT_TYPE data (compared by address only).
extern uint32_t g_ExEventObjectType[8];
extern uint32_t g_PsThreadObjectType[8];
extern uint32_t g_IoFileObjectType[8];

std::string ansi_to_string(const XANSI_STRING *s);

} // namespace rt
