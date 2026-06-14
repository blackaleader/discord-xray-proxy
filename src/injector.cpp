/*
 * injector.cpp  ─  Discord SOCKS5 Proxy  ─  Process Monitor & DLL Injector
 *
 * Runs as a standalone console application (administrator required).
 *
 *  • Scans running processes every 2 seconds.
 *  • When a Discord process is found that has not yet been injected,
 *    it injects proxy_hook.dll using the classic LoadLibrary technique.
 *  • Cleans up the injected-PID set when processes exit.
 *  • Press Ctrl+C to stop.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static bool is_discord_process(const char* name) {
    for (int i = 0; DISCORD_PROCESSES[i]; ++i)
        if (_stricmp(name, DISCORD_PROCESSES[i]) == 0) return true;
    return false;
}

static bool process_alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DLL injection via CreateRemoteThread + LoadLibraryA
// ─────────────────────────────────────────────────────────────────────────────
static bool inject_dll(DWORD pid, const char* dll_path) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!proc) {
        printf("  [!] OpenProcess(%u) failed: %u\n", pid, GetLastError());
        return false;
    }

    // Allocate memory in the target process for the DLL path string
    SIZE_T path_len = strlen(dll_path) + 1;
    void* remote_path = VirtualAllocEx(proc, nullptr, path_len,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_path) {
        printf("  [!] VirtualAllocEx failed: %u\n", GetLastError());
        CloseHandle(proc);
        return false;
    }

    if (!WriteProcessMemory(proc, remote_path, dll_path, path_len, nullptr)) {
        printf("  [!] WriteProcessMemory failed: %u\n", GetLastError());
        VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // Call LoadLibraryA in the target process
    FARPROC load_library = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE thread = CreateRemoteThread(
        proc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library),
        remote_path, 0, nullptr);

    if (!thread) {
        printf("  [!] CreateRemoteThread failed: %u\n", GetLastError());
        VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    // Wait for LoadLibrary to finish (5 second timeout)
    DWORD wait = WaitForSingleObject(thread, 5000);
    DWORD exit_code = 0;
    GetExitCodeThread(thread, &exit_code);

    CloseHandle(thread);
    VirtualFreeEx(proc, remote_path, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (wait != WAIT_OBJECT_0) {
        printf("  [!] Remote thread timed out\n");
        return false;
    }
    if (exit_code == 0) {
        printf("  [!] LoadLibraryA returned NULL (DLL load failed inside target)\n");
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Verify the injected DLL is visible in the target process module list
// ─────────────────────────────────────────────────────────────────────────────
static bool dll_present(DWORD pid, const char* dll_name) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return false;

    HMODULE mods[1024];
    DWORD needed = 0;
    bool found = false;

    if (EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
        DWORD count = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < count; ++i) {
            char name[MAX_PATH];
            if (GetModuleBaseNameA(proc, mods[i], name, MAX_PATH)) {
                if (_stricmp(name, dll_name) == 0) { found = true; break; }
            }
        }
    }

    CloseHandle(proc);
    return found;
}

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    // Resolve absolute path to proxy_hook.dll (must be in the same folder)
    char dll_path[MAX_PATH];
    GetModuleFileNameA(nullptr, dll_path, MAX_PATH);
    char* slash = strrchr(dll_path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - dll_path), "proxy_hook.dll");

    // Check that the DLL actually exists
    if (GetFileAttributesA(dll_path) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "[ERROR] Cannot find proxy_hook.dll at:\n  %s\n", dll_path);
        fprintf(stderr, "Build the project first with build.bat\n");
        return 1;
    }

    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║      Discord SOCKS5 Proxy Injector               ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  Proxy  : %s:%-5u                       ║\n", PROXY_HOST, PROXY_PORT);
    printf("║  DLL    : proxy_hook.dll                         ║\n");
    printf("║  Press Ctrl+C to stop.                           ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    std::set<DWORD> injected_pids;  // successfully injected
    std::set<DWORD> skipped_pids;   // failed / sandboxed – don't retry

    while (true) {
        // ── Enumerate all running processes ──
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }

        PROCESSENTRY32 pe;
        pe.dwSize = sizeof(pe);

        std::set<DWORD> alive_discord_pids;

        if (Process32First(snap, &pe)) {
            do {
                if (!is_discord_process(pe.szExeFile)) continue;

                alive_discord_pids.insert(pe.th32ProcessID);

                // Skip PIDs we already handled (success or permanent failure)
                if (injected_pids.count(pe.th32ProcessID)) continue;
                if (skipped_pids.count(pe.th32ProcessID))  continue;

                printf("[+] Detected %s  (PID %u)\n", pe.szExeFile, pe.th32ProcessID);

                // Small delay – give Discord time to fully initialise its Winsock
                Sleep(1500);

                printf("    Injecting proxy_hook.dll ...\n");
                if (inject_dll(pe.th32ProcessID, dll_path)) {
                    printf("    Injection OK");
                    if (dll_present(pe.th32ProcessID, "proxy_hook.dll"))
                        printf("  + DLL confirmed in module list\n");
                    else
                        printf("  (module not yet visible - may still be loading)\n");
                    injected_pids.insert(pe.th32ProcessID);
                } else {
                    printf("    Skipping PID %u (sandboxed renderer or access denied)\n",
                           pe.th32ProcessID);
                    skipped_pids.insert(pe.th32ProcessID); // don't retry
                }

            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);

        // ── Prune PIDs that are no longer running ──
        std::vector<DWORD> dead;
        for (DWORD pid : injected_pids)
            if (!alive_discord_pids.count(pid)) dead.push_back(pid);
        for (DWORD pid : dead) {
            printf("[-] PID %u exited\n", pid);
            injected_pids.erase(pid);
            skipped_pids.erase(pid); // allow retry if Discord restarts with same PID
        }

        Sleep(2000);
    }

    return 0;
}
