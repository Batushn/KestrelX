#include <Windows.h>

// Genuine MSVC/ntdll-managed static TLS slot provider (64 KB per-thread block).
// Linked with standard CRT so MSVC's official tlssup.obj emits a textbook
// IMAGE_TLS_DIRECTORY64 that ntdll!LdrpHandleTlsData always assigns a unique
// non-zero _tls_index slot without colliding with slot 0 or driver DLLs.

extern "C" __declspec(align(64)) __declspec(thread) unsigned char g_kestrel_tls_storage[65536] = {0};
extern "C" ULONG _tls_index;

extern "C" __declspec(dllexport) ULONG __cdecl kestrel_tls_get_index() {
    return _tls_index;
}

extern "C" __declspec(dllexport) ULONG __cdecl kestrel_tls_get_capacity() {
    return static_cast<ULONG>(sizeof(g_kestrel_tls_storage));
}

extern "C" __declspec(dllexport) void* __cdecl kestrel_tls_get_thread_ptr() {
    return &g_kestrel_tls_storage[0];
}
