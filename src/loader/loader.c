/*
 * swbf2.exe - process bootstrap.
 *
 * The Xbox executable was linked to run at virtual address 0x00010000 and has
 * no relocations, so its code must live at exactly that address. By the time
 * any normal program reaches main(), Windows has already put heaps and other
 * allocations in low memory. The only reliable way to own the range is to be
 * the EXE image itself: this module is linked at 0x00010000 with a 64 MiB
 * uninitialised section (.xbox) that the runtime later fills with the XBE.
 *
 * The loader therefore has no C runtime and keeps its few instructions in a
 * custom code section (.zldr) right after the PE header at 0x00011000. Those
 * instructions are overwritten when the XBE's .text section is copied in,
 * which is fine because control never returns here.
 *
 * Before anything else is loaded, the loader also reserves the other fixed
 * ranges the Xbox code hard-codes (its RAM views at 0x80000000 and
 * 0xF0000000 and the device registers at 0xFD000000/0xFE000000). Under Wine
 * (Proton) native Linux libraries such as the OpenGL driver share the 32-bit
 * address space and could otherwise land there first. The runtime releases
 * each reservation just before mapping the range for real.
 */
#include <windows.h>

/* No CRT: disable the debug-build stack/RTC checks that would call into it. */
#pragma runtime_checks("", off)
#pragma check_stack(off)

#define XBOX_SPACE_SIZE (0x04000000 - 0x12000)

#pragma bss_seg(".xbox")
__declspec(dllexport) char xbox_space[XBOX_SPACE_SIZE];
#pragma bss_seg()

typedef void(__cdecl *runtime_main_fn)(void *space, unsigned size);

#pragma code_seg(".zldr")
static void fail(const char *msg)
{
    MessageBoxA(NULL, msg, "Star Wars Battlefront II", MB_ICONERROR);
    ExitProcess(1);
}

void __stdcall loader_entry(void)
{
    HMODULE rt;
    runtime_main_fn run;

    VirtualAlloc((void *)0x80000000, 0x04000000, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc((void *)0xF0000000, 0x04000000, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc((void *)0xFD000000, 0x01000000, MEM_RESERVE, PAGE_NOACCESS);
    VirtualAlloc((void *)0xFE000000, 0x01000000, MEM_RESERVE, PAGE_NOACCESS);

    /* Load from the directory that contains this EXE only. */
    SetDllDirectoryA("");
    rt = LoadLibraryExA("swbf2rt.dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rt)
        fail("Could not load swbf2rt.dll (it must sit next to swbf2.exe).");
    run = (runtime_main_fn)GetProcAddress(rt, "swbf2_runtime_main");
    if (!run)
        fail("swbf2rt.dll is missing its entry point.");
    run(xbox_space, XBOX_SPACE_SIZE);
    ExitProcess(0);
}
#pragma code_seg()
