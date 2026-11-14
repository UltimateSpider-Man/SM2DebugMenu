// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only live evidence: no thread suspension, injection or process writes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <vector>
struct ThreadBasic {
    LONG exitStatus; void* teb; HANDLE processId, threadId; ULONG_PTR affinity; LONG priority, basePriority;
};
using QueryThread = LONG (NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
template<class T> bool Read(HANDLE process, uintptr_t address, T& value) {
    SIZE_T bytes = 0;
    return address >= 0x10000 && ReadProcessMemory(process, reinterpret_cast<void*>(address), &value, sizeof(value), &bytes) && bytes == sizeof(value);
}
struct Thread {
    DWORD id = 0; uintptr_t teb = 0, array = 0, block = 0; uint32_t role = 0;
    int32_t arena = -1, slot = -1; bool readable = false, window = false;
};
struct Windows { DWORD pid; std::vector<DWORD> threads; };
BOOL CALLBACK Window(HWND window, LPARAM parameter) {
    auto& out = *reinterpret_cast<Windows*>(parameter); DWORD pid = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &pid);
    wchar_t name[64]{};
    if (pid == out.pid && GetClassNameW(window, name, 64) && std::wcscmp(name, L"GameNxApp") == 0) out.threads.push_back(thread);
    return TRUE;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const DWORD pid = std::wcstoul(argv[1], nullptr, 10);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return 3;
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W module{}; module.dwSize = sizeof(module); uintptr_t base = 0;
    if (modules != INVALID_HANDLE_VALUE && Module32FirstW(modules, &module)) do {
        if (std::wcscmp(module.szModule, L"Spider-Man2.exe") == 0) { base = reinterpret_cast<uintptr_t>(module.modBaseAddr); break; }
    } while (Module32NextW(modules, &module));
    if (modules != INVALID_HANDLE_VALUE) CloseHandle(modules);
    if (!base) { CloseHandle(process); return 4; }
    uintptr_t vtable = 0, method = 0; uint32_t mainRole = 0, index = 0;
    const bool imageReadable = Read(process, base + 0xac3b7c0, vtable) && Read(process, base + 0x52f4e78, method) &&
        Read(process, base + 0xa018c98, mainRole) && Read(process, base + 0xc349cf0, index) && index < 4096;
    FARPROC queryAddress = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    QueryThread query = nullptr;
    static_assert(sizeof(query) == sizeof(queryAddress));
    std::memcpy(&query, &queryAddress, sizeof(query));
    if (!imageReadable || !query) { CloseHandle(process); return 5; }
    Windows windows{pid, {}}; EnumWindows(Window, reinterpret_cast<LPARAM>(&windows));
    std::vector<Thread> observed;
    HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    if (threads != INVALID_HANDLE_VALUE && Thread32First(threads, &entry)) do {
        if (entry.th32OwnerProcessID != pid) continue;
        Thread t; t.id = entry.th32ThreadID;
        for (DWORD id : windows.threads) if (id == t.id) t.window = true;
        HANDLE handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, t.id);
        ThreadBasic basic{};
        if (handle && query(handle, 0, &basic, sizeof(basic), nullptr) >= 0) {
            t.teb = reinterpret_cast<uintptr_t>(basic.teb);
            t.readable = Read(process, t.teb + 0x58, t.array) && t.array &&
                Read(process, t.array + static_cast<uintptr_t>(index) * 8, t.block) && t.block &&
                Read(process, t.block + 0xb0, t.role) && Read(process, t.block + 0x20, t.arena) && Read(process, t.block + 0x24, t.slot);
        }
        if (handle) CloseHandle(handle);
        observed.push_back(t);
    } while (Thread32Next(threads, &entry));
    if (threads != INVALID_HANDLE_VALUE) CloseHandle(threads);
    uint32_t finalRole = 0; const bool consistent = Read(process, base + 0xa018c98, finalRole) && finalRole == mainRole;
    std::printf("{\"schema\":1,\"read_only\":true,\"pid\":%lu,\"image_base\":%llu,\"vtable\":%llu,\"original_method\":%llu,"
        "\"vtable_expected\":%s,\"method_expected\":%s,\"main_role\":%u,\"tls_index\":%u,\"role_stable\":%s,\"threads\":[",
        static_cast<unsigned long>(pid), static_cast<unsigned long long>(base), static_cast<unsigned long long>(vtable), static_cast<unsigned long long>(method),
        vtable == base + 0x52f4e58 ? "true" : "false", method == base + 0x373b20 ? "true" : "false", mainRole, index, consistent ? "true" : "false");
    for (size_t i = 0; i < observed.size(); ++i) {
        const auto& t = observed[i];
        std::printf("%s{\"thread_id\":%lu,\"readable\":%s,\"game_window_thread\":%s,\"teb\":%llu,\"tls_block\":%llu,\"tls_role\":%u,\"arena_index\":%d,\"arena_slot\":%d,\"matches_main_role\":%s}",
            i ? "," : "", static_cast<unsigned long>(t.id), t.readable ? "true" : "false", t.window ? "true" : "false",
            static_cast<unsigned long long>(t.teb), static_cast<unsigned long long>(t.block), t.role, t.arena, t.slot,
            t.readable && mainRole && t.role == mainRole ? "true" : "false");
    }
    std::printf("],\"process_writes\":0,\"thread_suspensions\":0,\"injection\":false}\n");
    CloseHandle(process); return 0;
}
