// I/O manager: files, directories, volumes and symbolic links.
//
// Xbox paths are object-manager paths such as "\??\D:\Data\foo.lvl" or
// "\Device\Harddisk0\Partition1\TDATA\4c41001a\save.dat". Drive letters are
// symbolic links created by XAPI at startup; device paths map to host folders:
//   \Device\CdRom0                 -> the game files in src/rom (read-only)
//   \Device\Harddisk0\PartitionN   -> <userdata>/PartitionN
#include "kexports.h"
#include "object.h"
#include "sync.h"
#include "thread.h"
#include "../log.h"

#include <windows.h>
#include <algorithm>
#include <mutex>
#include <string>

namespace rt {

static std::string g_rom_dir;
static std::string g_user_dir;

struct FileObj : XObject {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE find = INVALID_HANDLE_VALUE;
    bool is_dir = false;
    bool read_only = false;
    bool delete_on_close = false;
    std::string host_path;
    std::string xbox_path;
    std::mutex m;
    FileObj() : XObject(ObjKind::File) {}
    void *body() override { return this; }
    ~FileObj() override
    {
        if (find != INVALID_HANDLE_VALUE)
            FindClose(find);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        if (delete_on_close) {
            if (is_dir)
                RemoveDirectoryA(host_path.c_str());
            else
                DeleteFileA(host_path.c_str());
        }
    }
};

static void make_dirs(const std::string &path)
{
    for (size_t i = 3; i < path.size(); i++) {
        if (path[i] == '\\') {
            std::string sub = path.substr(0, i);
            CreateDirectoryA(sub.c_str(), nullptr);
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
}

void io_init(const char *rom_dir, const char *userdata_dir)
{
    g_rom_dir = rom_dir;
    g_user_dir = userdata_dir;
    std::replace(g_rom_dir.begin(), g_rom_dir.end(), '/', '\\');
    std::replace(g_user_dir.begin(), g_user_dir.end(), '/', '\\');
    for (int p = 1; p <= 5; p++)
        make_dirs(g_user_dir + "\\Partition" + std::to_string(p));
    make_dirs(g_user_dir + "\\Partition1\\TDATA");
    make_dirs(g_user_dir + "\\Partition1\\UDATA");
    LOG_INFO("io", "D: -> %s, HDD -> %s", g_rom_dir.c_str(), g_user_dir.c_str());
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
    return s;
}

static bool starts_with_ci(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && _strnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// Resolves symbolic links ("\??\D:" etc.) until a device path remains.
static std::string resolve_links(std::string path)
{
    for (int depth = 0; depth < 8; depth++) {
        bool changed = false;
        // Try the longest prefix that is a registered symbolic link.
        size_t pos = path.size();
        while (pos != std::string::npos && pos > 0) {
            std::string prefix = path.substr(0, pos);
            if (XObject *o = ob_find_named(prefix)) {
                if (o->kind == ObjKind::SymbolicLink) {
                    path = static_cast<SymlinkObj *>(o)->target + path.substr(pos);
                    changed = true;
                    break;
                }
            }
            if (pos == 0)
                break;
            pos = path.rfind('\\', pos - 1);
        }
        if (!changed)
            break;
    }
    return path;
}

struct HostPath {
    std::string path;
    bool read_only = false;
    bool valid = false;
};

static HostPath to_host(const std::string &xbox_path)
{
    HostPath hp;
    std::string p = resolve_links(xbox_path);
    // Accept "D:\..." too (some code paths skip the \??\ prefix).
    if (p.size() >= 2 && p[1] == ':' && !starts_with_ci(p, "\\")) {
        p = resolve_links("\\??\\" + p);
    }
    auto tail = [&](size_t n) {
        std::string t = p.substr(n);
        std::replace(t.begin(), t.end(), '/', '\\');
        while (!t.empty() && t.back() == '\\')
            t.pop_back();
        if (!t.empty() && t[0] != '\\')
            t = "\\" + t;
        return t;
    };
    if (starts_with_ci(p, "\\Device\\CdRom0")) {
        hp.path = g_rom_dir + tail(14);
        hp.read_only = true;
        hp.valid = true;
    } else if (starts_with_ci(p, "\\Device\\Harddisk0\\Partition")) {
        size_t n = strlen("\\Device\\Harddisk0\\Partition");
        if (p.size() > n && isdigit(uint8_t(p[n])) && p[n] != '0') {
            hp.path = g_user_dir + "\\Partition" + p[n] + tail(n + 1);
            hp.valid = true;
        }
    }
    return hp;
}

static std::string object_path(const XOBJECT_ATTRIBUTES *oa)
{
    std::string name = ansi_to_string(oa->ObjectName);
    if (oa->RootDirectory) {
        if (FileObj *root = ob_lookup_as<FileObj>(oa->RootDirectory, ObjKind::File)) {
            std::string base = root->xbox_path;
            if (!name.empty() && name[0] != '\\' && (base.empty() || base.back() != '\\'))
                base += "\\";
            return base + name;
        }
    }
    return name;
}

static NTSTATUS win32_to_nt(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND: return X_STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND: return X_STATUS_OBJECT_PATH_NOT_FOUND;
    case ERROR_INVALID_NAME: return X_STATUS_OBJECT_NAME_INVALID;
    case ERROR_ACCESS_DENIED: return X_STATUS_ACCESS_DENIED;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return X_STATUS_OBJECT_NAME_COLLISION;
    case ERROR_SHARING_VIOLATION: return X_STATUS_SHARING_VIOLATION;
    case ERROR_DISK_FULL: return X_STATUS_DISK_FULL;
    case ERROR_DIR_NOT_EMPTY: return X_STATUS_DIRECTORY_NOT_EMPTY;
    case ERROR_DIRECTORY: return X_STATUS_NOT_A_DIRECTORY;
    case ERROR_HANDLE_EOF: return X_STATUS_END_OF_FILE;
    case ERROR_NO_MORE_FILES: return X_STATUS_NO_MORE_FILES;
    case ERROR_INVALID_HANDLE: return X_STATUS_INVALID_HANDLE;
    default: return X_STATUS_UNSUCCESSFUL;
    }
}

static void fill_times(const WIN32_FILE_ATTRIBUTE_DATA &a, int64_t *out4)
{
    auto ft = [](FILETIME f) { return (int64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    out4[0] = ft(a.ftCreationTime);
    out4[1] = ft(a.ftLastAccessTime);
    out4[2] = ft(a.ftLastWriteTime);
    out4[3] = ft(a.ftLastWriteTime);
}

#pragma pack(push, 4)
struct XFILE_NETWORK_OPEN_INFORMATION {
    int64_t CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    int64_t AllocationSize, EndOfFile;
    uint32_t FileAttributes;
    uint32_t Reserved;
};
struct XFILE_BASIC_INFORMATION {
    int64_t CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    uint32_t FileAttributes;
};
struct XFILE_STANDARD_INFORMATION {
    int64_t AllocationSize, EndOfFile;
    uint32_t NumberOfLinks;
    uint8_t DeletePending, Directory;
};
struct XFILE_DIRECTORY_INFORMATION {
    uint32_t NextEntryOffset, FileIndex;
    int64_t CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    int64_t EndOfFile, AllocationSize;
    uint32_t FileAttributes;
    uint32_t FileNameLength;
    char FileName[1];
};
#pragma pack(pop)

static bool query_attrs(const std::string &host, WIN32_FILE_ATTRIBUTE_DATA &a)
{
    return GetFileAttributesExA(host.c_str(), GetFileExInfoStandard, &a) != FALSE;
}

static void fill_network_info(const WIN32_FILE_ATTRIBUTE_DATA &a, XFILE_NETWORK_OPEN_INFORMATION *ni)
{
    int64_t t[4];
    fill_times(a, t);
    ni->CreationTime = t[0];
    ni->LastAccessTime = t[1];
    ni->LastWriteTime = t[2];
    ni->ChangeTime = t[3];
    ni->EndOfFile = (int64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    ni->AllocationSize = (ni->EndOfFile + 0xFFF) & ~int64_t(0xFFF);
    ni->FileAttributes = a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY |
                                               FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    if (!ni->FileAttributes)
        ni->FileAttributes = FILE_ATTRIBUTE_NORMAL;
    ni->Reserved = 0;
}

// NT create dispositions / options
enum { FILE_SUPERSEDE_ = 0, FILE_OPEN_ = 1, FILE_CREATE_ = 2, FILE_OPEN_IF_ = 3, FILE_OVERWRITE_ = 4, FILE_OVERWRITE_IF_ = 5 };
enum { FILE_DIRECTORY_FILE_ = 0x1, FILE_NON_DIRECTORY_FILE_ = 0x40, FILE_DELETE_ON_CLOSE_ = 0x1000 };

static NTSTATUS create_file(XHANDLE *out, uint32_t access, const XOBJECT_ATTRIBUTES *oa, XIO_STATUS_BLOCK *iosb,
                            uint32_t attributes, uint32_t share, uint32_t disposition, uint32_t options)
{
    std::string xp = object_path(oa);
    HostPath hp = to_host(xp);
    if (!hp.valid) {
        LOG_WARN("io", "open of unmapped path '%s' (resolved '%s')", xp.c_str(), resolve_links(xp).c_str());
        if (iosb) iosb->Status = X_STATUS_OBJECT_PATH_NOT_FOUND;
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    bool wants_write = (access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) != 0 ||
                       disposition != FILE_OPEN_;
    if (hp.read_only && wants_write && disposition != FILE_OPEN_IF_ && disposition != FILE_OPEN_) {
        if (iosb) iosb->Status = X_STATUS_ACCESS_DENIED;
        return X_STATUS_ACCESS_DENIED;
    }

    DWORD fa = GetFileAttributesA(hp.path.c_str());
    bool exists = fa != INVALID_FILE_ATTRIBUTES;
    bool is_dir = exists ? (fa & FILE_ATTRIBUTE_DIRECTORY) != 0 : (options & FILE_DIRECTORY_FILE_) != 0;
    uint32_t info = 1; // FILE_OPENED

    if (exists && is_dir && (options & FILE_NON_DIRECTORY_FILE_)) {
        if (iosb) iosb->Status = X_STATUS_FILE_IS_A_DIRECTORY;
        return X_STATUS_FILE_IS_A_DIRECTORY;
    }
    if (exists && !is_dir && (options & FILE_DIRECTORY_FILE_)) {
        if (iosb) iosb->Status = X_STATUS_NOT_A_DIRECTORY;
        return X_STATUS_NOT_A_DIRECTORY;
    }

    auto obj = new FileObj();
    obj->xbox_path = resolve_links(xp);
    obj->host_path = hp.path;
    obj->read_only = hp.read_only;
    obj->delete_on_close = (options & FILE_DELETE_ON_CLOSE_) != 0;

    if (is_dir) {
        if (!exists) {
            if (disposition == FILE_OPEN_ || disposition == FILE_OVERWRITE_) {
                delete obj;
                if (iosb) iosb->Status = X_STATUS_OBJECT_NAME_NOT_FOUND;
                return X_STATUS_OBJECT_NAME_NOT_FOUND;
            }
            if (!CreateDirectoryA(hp.path.c_str(), nullptr)) {
                NTSTATUS s = win32_to_nt(GetLastError());
                delete obj;
                if (iosb) iosb->Status = s;
                return s;
            }
            info = 2; // FILE_CREATED
        } else if (disposition == FILE_CREATE_) {
            delete obj;
            if (iosb) iosb->Status = X_STATUS_OBJECT_NAME_COLLISION;
            return X_STATUS_OBJECT_NAME_COLLISION;
        }
        obj->is_dir = true;
        obj->h = CreateFileA(hp.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    } else {
        DWORD cd;
        switch (disposition) {
        case FILE_SUPERSEDE_: cd = CREATE_ALWAYS; break;
        case FILE_CREATE_: cd = CREATE_NEW; break;
        case FILE_OPEN_IF_: cd = OPEN_ALWAYS; break;
        case FILE_OVERWRITE_: cd = TRUNCATE_EXISTING; break;
        case FILE_OVERWRITE_IF_: cd = CREATE_ALWAYS; break;
        default: cd = OPEN_EXISTING; break;
        }
        DWORD acc = GENERIC_READ;
        if (!hp.read_only && wants_write)
            acc |= GENERIC_WRITE;
        if (access & DELETE)
            acc |= DELETE;
        obj->h = CreateFileA(hp.path.c_str(), acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             cd, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (obj->h == INVALID_HANDLE_VALUE) {
            NTSTATUS s = win32_to_nt(GetLastError());
            LOG_DEBUG("io", "open '%s' -> %s failed (0x%08X)", xp.c_str(), hp.path.c_str(), s);
            delete obj;
            if (iosb) iosb->Status = s;
            return s;
        }
        if (exists) {
            if (disposition == FILE_SUPERSEDE_) info = 0;
            else if (disposition == FILE_OVERWRITE_ || disposition == FILE_OVERWRITE_IF_) info = 3;
        } else {
            info = 2;
        }
    }
    *out = ob_insert(obj);
    LOG_DEBUG("io", "open '%s' -> handle %X (%s)", xp.c_str(), *out, obj->is_dir ? "dir" : "file");
    if (iosb) {
        iosb->Status = X_STATUS_SUCCESS;
        iosb->Information = info;
    }
    return X_STATUS_SUCCESS;
}

// Signals completion the way the NT I/O manager would.
static void complete_io(XHANDLE event, void *apc_routine, void *apc_context, XIO_STATUS_BLOCK *iosb)
{
    if (event) {
        if (XObject *o = ob_lookup(event)) {
            if (o->kind == ObjKind::Event)
                sync_set_event(&static_cast<EventObj *>(o)->ev);
        }
    }
    if (apc_routine) {
        if (XThread *t = xt_current())
            sync_queue_user_apc(t, apc_routine, apc_context, iosb, nullptr);
    }
}

} // namespace rt

using namespace rt;

KEXPORT(NTSTATUS) kx_NtCreateFile(XHANDLE *FileHandle, uint32_t DesiredAccess, XOBJECT_ATTRIBUTES *ObjectAttributes,
                                  XIO_STATUS_BLOCK *IoStatusBlock, int64_t *AllocationSize, uint32_t FileAttributes,
                                  uint32_t ShareAccess, uint32_t CreateDisposition, uint32_t CreateOptions)
{
    return create_file(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, FileAttributes, ShareAccess,
                       CreateDisposition, CreateOptions);
}

KEXPORT(NTSTATUS) kx_NtOpenFile(XHANDLE *FileHandle, uint32_t DesiredAccess, XOBJECT_ATTRIBUTES *ObjectAttributes,
                                XIO_STATUS_BLOCK *IoStatusBlock, uint32_t ShareAccess, uint32_t OpenOptions)
{
    return create_file(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, 0, ShareAccess, FILE_OPEN_,
                       OpenOptions);
}

KEXPORT(NTSTATUS) kx_NtReadFile(XHANDLE FileHandle, XHANDLE Event, void *ApcRoutine, void *ApcContext,
                                XIO_STATUS_BLOCK *IoStatusBlock, void *Buffer, uint32_t Length, int64_t *ByteOffset)
{
    FileObj *f = ob_lookup_as<FileObj>(FileHandle, ObjKind::File);
    if (!f || f->is_dir)
        return X_STATUS_INVALID_HANDLE;
    if (Event || ApcRoutine)
        LOG_DEBUG("io", "async read h=%X len=%u off=%lld event=%X apc=%p ctx=%p", FileHandle, Length,
                  ByteOffset ? *ByteOffset : -1ll, Event, ApcRoutine, ApcContext);
    DWORD got = 0;
    BOOL ok;
    {
        std::lock_guard<std::mutex> lock(f->m);
        if (ByteOffset && *ByteOffset >= 0) {
            LARGE_INTEGER li;
            li.QuadPart = *ByteOffset;
            SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN);
        }
        ok = ReadFile(f->h, Buffer, Length, &got, nullptr);
    }
    NTSTATUS s = X_STATUS_SUCCESS;
    if (!ok)
        s = win32_to_nt(GetLastError());
    else if (got == 0 && Length)
        s = X_STATUS_END_OF_FILE;
    IoStatusBlock->Status = s;
    IoStatusBlock->Information = got;
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return s;
}

KEXPORT(NTSTATUS) kx_NtWriteFile(XHANDLE FileHandle, XHANDLE Event, void *ApcRoutine, void *ApcContext,
                                 XIO_STATUS_BLOCK *IoStatusBlock, const void *Buffer, uint32_t Length, int64_t *ByteOffset)
{
    FileObj *f = ob_lookup_as<FileObj>(FileHandle, ObjKind::File);
    if (!f || f->is_dir)
        return X_STATUS_INVALID_HANDLE;
    if (f->read_only)
        return X_STATUS_ACCESS_DENIED;
    DWORD put = 0;
    BOOL ok;
    {
        std::lock_guard<std::mutex> lock(f->m);
        if (ByteOffset && *ByteOffset >= 0) {
            LARGE_INTEGER li;
            li.QuadPart = *ByteOffset;
            SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN);
        }
        ok = WriteFile(f->h, Buffer, Length, &put, nullptr);
    }
    NTSTATUS s = ok ? X_STATUS_SUCCESS : win32_to_nt(GetLastError());
    IoStatusBlock->Status = s;
    IoStatusBlock->Information = put;
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return s;
}

KEXPORT(NTSTATUS) kx_NtQueryInformationFile(XHANDLE FileHandle, XIO_STATUS_BLOCK *IoStatusBlock, void *FileInformation,
                                            uint32_t Length, uint32_t FileInformationClass)
{
    FileObj *f = ob_lookup_as<FileObj>(FileHandle, ObjKind::File);
    if (!f)
        return X_STATUS_INVALID_HANDLE;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    query_attrs(f->host_path, a);
    XFILE_NETWORK_OPEN_INFORMATION ni;
    fill_network_info(a, &ni);
    NTSTATUS s = X_STATUS_SUCCESS;
    uint32_t info = 0;
    switch (FileInformationClass) {
    case 4: { // FileBasicInformation
        auto *b = static_cast<XFILE_BASIC_INFORMATION *>(FileInformation);
        b->CreationTime = ni.CreationTime;
        b->LastAccessTime = ni.LastAccessTime;
        b->LastWriteTime = ni.LastWriteTime;
        b->ChangeTime = ni.ChangeTime;
        b->FileAttributes = ni.FileAttributes;
        info = sizeof *b;
        break;
    }
    case 5: { // FileStandardInformation
        auto *st = static_cast<XFILE_STANDARD_INFORMATION *>(FileInformation);
        LARGE_INTEGER sz{};
        if (!f->is_dir)
            GetFileSizeEx(f->h, &sz);
        st->EndOfFile = sz.QuadPart;
        st->AllocationSize = (sz.QuadPart + 0xFFF) & ~int64_t(0xFFF);
        st->NumberOfLinks = 1;
        st->DeletePending = f->delete_on_close;
        st->Directory = f->is_dir;
        info = sizeof *st;
        break;
    }
    case 6: // FileInternalInformation
        *static_cast<int64_t *>(FileInformation) = int64_t(FileHandle);
        info = 8;
        break;
    case 14: { // FilePositionInformation
        LARGE_INTEGER zero{}, pos{};
        SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
        *static_cast<int64_t *>(FileInformation) = pos.QuadPart;
        info = 8;
        break;
    }
    case 16: // FileModeInformation
        *static_cast<uint32_t *>(FileInformation) = 0x20;
        info = 4;
        break;
    case 17: // FileAlignmentInformation
        *static_cast<uint32_t *>(FileInformation) = 0;
        info = 4;
        break;
    case 34: { // FileNetworkOpenInformation
        if (!f->is_dir) {
            LARGE_INTEGER sz{};
            GetFileSizeEx(f->h, &sz);
            ni.EndOfFile = sz.QuadPart;
            ni.AllocationSize = (sz.QuadPart + 0xFFF) & ~int64_t(0xFFF);
        }
        memcpy(FileInformation, &ni, std::min<uint32_t>(Length, sizeof ni));
        info = sizeof ni;
        break;
    }
    default:
        LOG_WARN("io", "NtQueryInformationFile class %u not implemented", FileInformationClass);
        s = X_STATUS_INVALID_PARAMETER;
    }
    IoStatusBlock->Status = s;
    IoStatusBlock->Information = info;
    return s;
}

KEXPORT(NTSTATUS) kx_NtSetInformationFile(XHANDLE FileHandle, XIO_STATUS_BLOCK *IoStatusBlock, void *FileInformation,
                                          uint32_t Length, uint32_t FileInformationClass)
{
    FileObj *f = ob_lookup_as<FileObj>(FileHandle, ObjKind::File);
    if (!f)
        return X_STATUS_INVALID_HANDLE;
    NTSTATUS s = X_STATUS_SUCCESS;
    std::lock_guard<std::mutex> lock(f->m);
    switch (FileInformationClass) {
    case 4: { // FileBasicInformation
        auto *b = static_cast<XFILE_BASIC_INFORMATION *>(FileInformation);
        if (b->FileAttributes && !f->read_only)
            SetFileAttributesA(f->host_path.c_str(), b->FileAttributes & ~FILE_ATTRIBUTE_DIRECTORY);
        break;
    }
    case 13: // FileDispositionInformation
        if (f->read_only) { s = X_STATUS_ACCESS_DENIED; break; }
        f->delete_on_close = *static_cast<uint8_t *>(FileInformation) != 0;
        break;
    case 14: { // FilePositionInformation
        LARGE_INTEGER li;
        li.QuadPart = *static_cast<int64_t *>(FileInformation);
        if (!SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN))
            s = win32_to_nt(GetLastError());
        break;
    }
    case 19: // FileAllocationInformation
    case 20: { // FileEndOfFileInformation
        if (f->read_only) { s = X_STATUS_ACCESS_DENIED; break; }
        if (FileInformationClass == 19) break;
        LARGE_INTEGER cur{}, zero{}, eof;
        SetFilePointerEx(f->h, zero, &cur, FILE_CURRENT);
        eof.QuadPart = *static_cast<int64_t *>(FileInformation);
        SetFilePointerEx(f->h, eof, nullptr, FILE_BEGIN);
        if (!SetEndOfFile(f->h))
            s = win32_to_nt(GetLastError());
        SetFilePointerEx(f->h, cur, nullptr, FILE_BEGIN);
        break;
    }
    case 10: { // FileRenameInformation {BOOLEAN ReplaceIfExists; HANDLE RootDirectory; OBJECT_STRING FileName;}
        if (f->read_only) { s = X_STATUS_ACCESS_DENIED; break; }
        uint8_t replace = *static_cast<uint8_t *>(FileInformation);
        auto *name = reinterpret_cast<XANSI_STRING *>(static_cast<uint8_t *>(FileInformation) + 8);
        XOBJECT_ATTRIBUTES oa{*reinterpret_cast<XHANDLE *>(static_cast<uint8_t *>(FileInformation) + 4), name, 0};
        HostPath hp = to_host(object_path(&oa));
        if (!hp.valid || hp.read_only) { s = X_STATUS_ACCESS_DENIED; break; }
        CloseHandle(f->h);
        f->h = INVALID_HANDLE_VALUE;
        if (!MoveFileExA(f->host_path.c_str(), hp.path.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0))
            s = win32_to_nt(GetLastError());
        else
            f->host_path = hp.path;
        f->h = CreateFileA(f->host_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           f->is_dir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
        break;
    }
    default:
        LOG_WARN("io", "NtSetInformationFile class %u not implemented", FileInformationClass);
        s = X_STATUS_INVALID_PARAMETER;
    }
    IoStatusBlock->Status = s;
    IoStatusBlock->Information = 0;
    return s;
}

KEXPORT(NTSTATUS) kx_NtQueryVolumeInformationFile(XHANDLE FileHandle, XIO_STATUS_BLOCK *IoStatusBlock, void *FsInformation,
                                                  uint32_t Length, uint32_t FsInformationClass)
{
    FileObj *f = ob_lookup_as<FileObj>(FileHandle, ObjKind::File);
    if (!f)
        return X_STATUS_INVALID_HANDLE;
    NTSTATUS s = X_STATUS_SUCCESS;
    uint32_t info = 0;
    uint8_t *out = static_cast<uint8_t *>(FsInformation);
    switch (FsInformationClass) {
    case 3:   // FileFsSizeInformation
    case 7: { // FileFsFullSizeInformation
        // Report a roomy but Xbox-plausible volume: 4 GiB total, 2 GiB free,
        // 16 KiB clusters (32 x 512-byte sectors).
        int64_t total = (4ll << 30) / 16384, avail = (2ll << 30) / 16384;
        memcpy(out, &total, 8);
        memcpy(out + 8, &avail, 8);
        if (FsInformationClass == 7) {
            memcpy(out + 16, &avail, 8);
            *reinterpret_cast<uint32_t *>(out + 24) = 32;
            *reinterpret_cast<uint32_t *>(out + 28) = 512;
            info = 32;
        } else {
            *reinterpret_cast<uint32_t *>(out + 16) = 32;
            *reinterpret_cast<uint32_t *>(out + 20) = 512;
            info = 24;
        }
        break;
    }
    case 1: { // FileFsVolumeInformation
        memset(out, 0, std::min<uint32_t>(Length, 24));
        *reinterpret_cast<uint32_t *>(out + 8) = 0x5357424Fu; // serial
        info = std::min<uint32_t>(Length, 18);
        break;
    }
    case 4: { // FileFsDeviceInformation {DeviceType, Characteristics}
        reinterpret_cast<uint32_t *>(out)[0] = f->read_only ? 0x02 /*CD_ROM*/ : 0x07 /*DISK*/;
        reinterpret_cast<uint32_t *>(out)[1] = f->read_only ? 0x01 : 0;
        info = 8;
        break;
    }
    case 5: { // FileFsAttributeInformation
        memset(out, 0, std::min<uint32_t>(Length, 12));
        reinterpret_cast<uint32_t *>(out)[1] = 255;
        const char *fs = f->read_only ? "GDFX" : "FATX";
        uint32_t n = std::min<uint32_t>(4, Length > 12 ? Length - 12 : 0);
        reinterpret_cast<uint32_t *>(out)[2] = n;
        memcpy(out + 12, fs, n);
        info = 12 + n;
        break;
    }
    default:
        LOG_WARN("io", "NtQueryVolumeInformationFile class %u not implemented", FsInformationClass);
        s = X_STATUS_INVALID_PARAMETER;
    }
    IoStatusBlock->Status = s;
    IoStatusBlock->Information = info;
    return s;
}

KEXPORT(NTSTATUS) kx_NtQueryDirectoryFile(XHANDLE FileHandle, XHANDLE Event, void *ApcRoutine, void *ApcContext,
                                          XIO_STATUS_BLOCK *IoStatusBlock, void *FileInformation, uint32_t Length,
                                          uint32_t FileInformationClass, XANSI_STRING *FileMask, uint8_t RestartScan)
{
    FileObj *f = ob_lookup_as<FileObj>(FileHandle, ObjKind::File);
    if (!f || !f->is_dir)
        return X_STATUS_INVALID_HANDLE;
    if (FileInformationClass != 1)
        LOG_WARN("io", "NtQueryDirectoryFile class %u treated as FileDirectoryInformation", FileInformationClass);
    std::lock_guard<std::mutex> lock(f->m);
    WIN32_FIND_DATAA fd;
    for (;;) {
        if (f->find == INVALID_HANDLE_VALUE || RestartScan) {
            if (f->find != INVALID_HANDLE_VALUE)
                FindClose(f->find);
            std::string mask = FileMask && FileMask->Length ? ansi_to_string(FileMask) : "*";
            f->find = FindFirstFileA((f->host_path + "\\" + mask).c_str(), &fd);
            RestartScan = 0;
            if (f->find == INVALID_HANDLE_VALUE) {
                IoStatusBlock->Status = X_STATUS_NO_SUCH_FILE;
                IoStatusBlock->Information = 0;
                return X_STATUS_NO_SUCH_FILE;
            }
        } else if (!FindNextFileA(f->find, &fd)) {
            IoStatusBlock->Status = X_STATUS_NO_MORE_FILES;
            IoStatusBlock->Information = 0;
            return X_STATUS_NO_MORE_FILES;
        }
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, ".."))
            break;
    }
    auto *di = static_cast<XFILE_DIRECTORY_INFORMATION *>(FileInformation);
    uint32_t name_len = uint32_t(strlen(fd.cFileName));
    uint32_t need = offsetof(XFILE_DIRECTORY_INFORMATION, FileName) + name_len;
    if (Length < need) {
        IoStatusBlock->Status = X_STATUS_BUFFER_OVERFLOW;
        return X_STATUS_BUFFER_OVERFLOW;
    }
    WIN32_FILE_ATTRIBUTE_DATA a{};
    a.dwFileAttributes = fd.dwFileAttributes;
    a.ftCreationTime = fd.ftCreationTime;
    a.ftLastAccessTime = fd.ftLastAccessTime;
    a.ftLastWriteTime = fd.ftLastWriteTime;
    a.nFileSizeHigh = fd.nFileSizeHigh;
    a.nFileSizeLow = fd.nFileSizeLow;
    XFILE_NETWORK_OPEN_INFORMATION ni;
    fill_network_info(a, &ni);
    di->NextEntryOffset = 0;
    di->FileIndex = 0;
    di->CreationTime = ni.CreationTime;
    di->LastAccessTime = ni.LastAccessTime;
    di->LastWriteTime = ni.LastWriteTime;
    di->ChangeTime = ni.ChangeTime;
    di->EndOfFile = ni.EndOfFile;
    di->AllocationSize = ni.AllocationSize;
    di->FileAttributes = ni.FileAttributes;
    di->FileNameLength = name_len;
    memcpy(di->FileName, fd.cFileName, name_len);
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    IoStatusBlock->Information = need;
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES *ObjectAttributes, void *Attributes)
{
    std::string xp = object_path(ObjectAttributes);
    HostPath hp = to_host(xp);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!hp.valid || !query_attrs(hp.path, a)) {
        LOG_DEBUG("io", "NtQueryFullAttributesFile '%s': not found", xp.c_str());
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    fill_network_info(a, static_cast<XFILE_NETWORK_OPEN_INFORMATION *>(Attributes));
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtDeleteFile(XOBJECT_ATTRIBUTES *ObjectAttributes)
{
    HostPath hp = to_host(object_path(ObjectAttributes));
    if (!hp.valid || hp.read_only)
        return X_STATUS_ACCESS_DENIED;
    DWORD fa = GetFileAttributesA(hp.path.c_str());
    BOOL ok = (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY)) ? RemoveDirectoryA(hp.path.c_str())
                                                                                  : DeleteFileA(hp.path.c_str());
    return ok ? X_STATUS_SUCCESS : win32_to_nt(GetLastError());
}

KEXPORT(NTSTATUS) kx_NtFlushBuffersFile(XHANDLE FileHandle, XIO_STATUS_BLOCK *IoStatusBlock)
{
    FileObj *f = ob_lookup_as<FileObj>(FileHandle, ObjKind::File);
    if (!f)
        return X_STATUS_INVALID_HANDLE;
    if (!f->is_dir && !f->read_only)
        FlushFileBuffers(f->h);
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    IoStatusBlock->Information = 0;
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtFsControlFile(XHANDLE FileHandle, XHANDLE Event, void *ApcRoutine, void *ApcContext,
                                     XIO_STATUS_BLOCK *IoStatusBlock, uint32_t FsControlCode, void *InputBuffer,
                                     uint32_t InputBufferLength, void *OutputBuffer, uint32_t OutputBufferLength)
{
    LOG_DEBUG("io", "NtFsControlFile(%X, code %08X)", FileHandle, FsControlCode);
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    IoStatusBlock->Information = 0;
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtDeviceIoControlFile(XHANDLE FileHandle, XHANDLE Event, void *ApcRoutine, void *ApcContext,
                                           XIO_STATUS_BLOCK *IoStatusBlock, uint32_t IoControlCode, void *InputBuffer,
                                           uint32_t InputBufferLength, void *OutputBuffer, uint32_t OutputBufferLength)
{
    LOG_WARN("io", "NtDeviceIoControlFile(%X, code %08X) not supported", FileHandle, IoControlCode);
    IoStatusBlock->Status = X_STATUS_INVALID_DEVICE_REQUEST;
    IoStatusBlock->Information = 0;
    return X_STATUS_INVALID_DEVICE_REQUEST;
}

// ---- symbolic links --------------------------------------------------------
KEXPORT(NTSTATUS) kx_IoCreateSymbolicLink(XANSI_STRING *SymbolicLinkName, XANSI_STRING *DeviceName)
{
    std::string link = ansi_to_string(SymbolicLinkName), target = ansi_to_string(DeviceName);
    if (ob_find_named(link))
        return X_STATUS_OBJECT_NAME_COLLISION;
    auto *s = new SymlinkObj();
    s->name = link;
    s->target = target;
    while (!s->target.empty() && s->target.back() == '\\')
        s->target.pop_back();
    ob_add_named(s);
    LOG_INFO("io", "symlink %s -> %s", link.c_str(), target.c_str());
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_IoDeleteSymbolicLink(XANSI_STRING *SymbolicLinkName)
{
    std::string link = ansi_to_string(SymbolicLinkName);
    XObject *o = ob_find_named(link);
    if (!o || o->kind != ObjKind::SymbolicLink)
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    ob_remove_named(link);
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtOpenSymbolicLinkObject(XHANDLE *LinkHandle, XOBJECT_ATTRIBUTES *ObjectAttributes)
{
    std::string name = ansi_to_string(ObjectAttributes->ObjectName);
    XObject *o = ob_find_named(name);
    if (!o || o->kind != ObjKind::SymbolicLink)
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    *LinkHandle = ob_insert(o);
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_NtQuerySymbolicLinkObject(XHANDLE LinkHandle, XANSI_STRING *LinkTarget, uint32_t *ReturnedLength)
{
    SymlinkObj *s = ob_lookup_as<SymlinkObj>(LinkHandle, ObjKind::SymbolicLink);
    if (!s)
        return X_STATUS_INVALID_HANDLE;
    uint32_t n = uint32_t(s->target.size());
    if (ReturnedLength)
        *ReturnedLength = n;
    if (LinkTarget->MaximumLength < n)
        return X_STATUS_BUFFER_TOO_SMALL;
    memcpy(LinkTarget->Buffer, s->target.data(), n);
    LinkTarget->Length = uint16_t(n);
    return X_STATUS_SUCCESS;
}

// ---- driver model (only reachable from XDK device drivers we HLE away) -----
KEXPORT(NTSTATUS) kx_IoCreateDevice(void *DriverObject, uint32_t DeviceExtensionSize, XANSI_STRING *DeviceName,
                                    uint32_t DeviceType, uint8_t Exclusive, void **DeviceObject)
{
    LOG_WARN("io", "IoCreateDevice(%s) - device drivers are not emulated", ansi_to_string(DeviceName).c_str());
    // Hand back zeroed storage large enough for DEVICE_OBJECT + extension.
    uint8_t *d = static_cast<uint8_t *>(calloc(1, 0x100 + DeviceExtensionSize));
    *reinterpret_cast<void **>(d + 0x18) = d + 0x100; // DeviceExtension
    *DeviceObject = d;
    return X_STATUS_SUCCESS;
}

KEXPORT(NTSTATUS) kx_IoInvalidDeviceRequest(void *DeviceObject, void *Irp) { return X_STATUS_INVALID_DEVICE_REQUEST; }
KEXPORT(void) kx_IoStartPacket(void *DeviceObject, void *Irp, uint32_t *Key) { LOG_WARN("io", "IoStartPacket ignored"); }
KEXPORT(void) kx_IoStartNextPacket(void *DeviceObject) {}
KEXPORT(void) kx_IoMarkIrpMustComplete(void *Irp) {}
KEXPORT_FASTCALL(NTSTATUS) kx_IofCallDriver(void *DeviceObject, void *Irp)
{
    LOG_WARN("io", "IofCallDriver ignored");
    return X_STATUS_INVALID_DEVICE_REQUEST;
}
KEXPORT_FASTCALL(void) kx_IofCompleteRequest(void *Irp, uint8_t PriorityBoost) {}
KEXPORT(void *) kx_IoBuildSynchronousFsdRequest(uint32_t MajorFunction, void *DeviceObject, void *Buffer, uint32_t Length,
                                                int64_t *StartingOffset, void *Event, XIO_STATUS_BLOCK *IoStatusBlock)
{
    LOG_WARN("io", "IoBuildSynchronousFsdRequest not supported");
    return nullptr;
}
KEXPORT(NTSTATUS) kx_IoSynchronousFsdRequest(uint32_t MajorFunction, void *DeviceObject, void *Buffer, uint32_t Length,
                                             int64_t *StartingOffset)
{
    LOG_WARN("io", "IoSynchronousFsdRequest not supported");
    return X_STATUS_INVALID_DEVICE_REQUEST;
}
KEXPORT(NTSTATUS) kx_IoSynchronousDeviceIoControlRequest(uint32_t IoControlCode, void *DeviceObject, void *InputBuffer,
                                                         uint32_t InputBufferLength, void *OutputBuffer,
                                                         uint32_t OutputBufferLength, uint32_t *ReturnedOutputBufferLength,
                                                         uint8_t InternalDeviceIoControl)
{
    LOG_WARN("io", "IoSynchronousDeviceIoControlRequest(%08X) not supported", IoControlCode);
    return X_STATUS_INVALID_DEVICE_REQUEST;
}
