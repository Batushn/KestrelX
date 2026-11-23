#define KESTREL_GUEST_DLL_EXPORTS
#include "guest_api.hpp"

namespace {
volatile uint32_t g_attached = 0;
}

extern "C" BOOL WINAPI GuestDllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_attached = 1;
    }
    return TRUE;
}

extern "C" uint32_t __cdecl kestrel_guest_dll_compute(uint32_t x) {
    return x * 2u;
}

extern "C" uint32_t __cdecl kestrel_guest_dll_attached() {
    return g_attached;
}
