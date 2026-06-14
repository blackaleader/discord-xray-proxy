/*
 * proxy_hook.cpp  ─  Discord SOCKS5 Proxy Hook DLL  v4
 *
 * TCP  ──  IAT-hook connect / WSAConnect / ConnectEx / WSAIoctl
 *   Every outgoing TCP connection from Discord is redirected through the
 *   upstream SOCKS5 proxy (Xray / V2Ray on 127.0.0.1:10808).
 *
 * UDP  ──  IAT-hook sendto / WSASendTo
 *   Non-loopback UDP is intentionally blocked (WSAENETUNREACH).
 *   Discord's WebRTC detects the UDP failure and automatically falls back to
 *   TCP TURN relay (Discord's own TURN servers on port 443/TCP).
 *   That TCP TURN connection is then transparently proxied by the TCP hook.
 *
 *   Result: voice + video go through Xray via TCP TURN, fully proxied.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "config.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

// ─────────────────────────────────────────────────────────────────────────────
//  Logging
// ─────────────────────────────────────────────────────────────────────────────
static FILE*      g_log     = nullptr;
static std::mutex g_log_mtx;

static void log_init(HMODULE hMod) {
    char path[MAX_PATH];
    GetModuleFileNameA(hMod, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy_s(dot, MAX_PATH - (dot - path), ".log");
    fopen_s(&g_log, path, "a");
}

#define LOG(fmt, ...) do { \
    if (g_log) { \
        std::lock_guard<std::mutex> _lk(g_log_mtx); \
        fprintf(g_log, "[%u] " fmt "\n", GetCurrentThreadId(), ##__VA_ARGS__); \
        fflush(g_log); \
    } \
} while (0)

// ─────────────────────────────────────────────────────────────────────────────
//  Function pointer types
// ─────────────────────────────────────────────────────────────────────────────
typedef int    (WSAAPI* fn_connect)    (SOCKET, const sockaddr*, int);
typedef int    (WSAAPI* fn_WSAConnect) (SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
typedef int    (WSAAPI* fn_WSAIoctl)   (SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int    (WSAAPI* fn_ioctlsocket)(SOCKET, long, u_long*);
typedef BOOL   (PASCAL* fn_ConnectEx)  (SOCKET, const sockaddr*, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef int    (WSAAPI* fn_sendto)     (SOCKET, const char*, int, int, const sockaddr*, int);
typedef int    (WSAAPI* fn_WSASendTo)  (SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
typedef int    (WSAAPI* fn_closesocket)(SOCKET);
typedef HANDLE (WINAPI* fn_CreateIoCompletionPort)(HANDLE, HANDLE, ULONG_PTR, DWORD);

static fn_connect                 g_real_connect              = nullptr;
static fn_WSAConnect              g_real_WSAConnect           = nullptr;
static fn_WSAIoctl                g_real_WSAIoctl             = nullptr;
static fn_ioctlsocket             g_real_ioctlsocket          = nullptr;
static fn_ConnectEx               g_real_ConnectEx            = nullptr;
static fn_sendto                  g_real_sendto               = nullptr;
static fn_WSASendTo               g_real_WSASendTo            = nullptr;
static fn_closesocket             g_real_closesocket          = nullptr;
static fn_CreateIoCompletionPort  g_real_CreateIoCompletionPort = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────
static bool is_loopback(const sockaddr* a) {
    if (!a) return true;
    if (a->sa_family == AF_INET) {
        uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(a)->sin_addr.s_addr);
        return (ip >> 24) == 127;
    }
    if (a->sa_family == AF_INET6) {
        static const uint8_t lo6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        return memcmp(&reinterpret_cast<const sockaddr_in6*>(a)->sin6_addr, lo6, 16) == 0;
    }
    return true;
}

static int sock_type(SOCKET s) {
    int t = 0, l = sizeof(t);
    getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&t), &l);
    return t;
}

static bool recv_all(SOCKET s, void* buf, int len) {
    int got = 0;
    while (got < len) {
        int r = recv(s, reinterpret_cast<char*>(buf) + got, len - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SOCKS5 TCP handshake
// ─────────────────────────────────────────────────────────────────────────────
static bool socks5_tcp_handshake(SOCKET s, const sockaddr* dest) {
    // Greeting – no auth
    const uint8_t greet[3] = {0x05, 0x01, 0x00};
    if (send(s, reinterpret_cast<const char*>(greet), 3, 0) != 3) return false;
    uint8_t choice[2];
    if (!recv_all(s, choice, 2) || choice[1] != 0x00) return false;

    // CONNECT request
    uint8_t req[22]; int rlen = 0;
    req[rlen++] = 0x05; req[rlen++] = 0x01; req[rlen++] = 0x00;

    if (dest->sa_family == AF_INET) {
        const auto* a4 = reinterpret_cast<const sockaddr_in*>(dest);
        req[rlen++] = 0x01;
        memcpy(req + rlen, &a4->sin_addr, 4);  rlen += 4;
        memcpy(req + rlen, &a4->sin_port, 2);  rlen += 2;
    } else if (dest->sa_family == AF_INET6) {
        const auto* a6 = reinterpret_cast<const sockaddr_in6*>(dest);
        req[rlen++] = 0x04;
        memcpy(req + rlen, &a6->sin6_addr, 16); rlen += 16;
        memcpy(req + rlen, &a6->sin6_port, 2);  rlen += 2;
    } else {
        return false;
    }

    if (send(s, reinterpret_cast<const char*>(req), rlen, 0) != rlen) return false;

    uint8_t resp[22];
    if (!recv_all(s, resp, 10) || resp[0] != 0x05 || resp[1] != 0x00) {
        LOG("SOCKS5 CONNECT rejected: rep=%u", resp[1]); return false;
    }
    // Drain extra addr bytes
    if (resp[3] == 0x04) { uint8_t ex[12]; recv_all(s, ex, 12); }
    else if (resp[3] == 0x03) {
        uint8_t dl; recv_all(s, &dl, 1);
        uint8_t dp[258]; recv_all(s, dp, dl + 2);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Core TCP proxy logic
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex g_nb_mtx;
static std::unordered_map<SOCKET, bool> g_was_nonblocking;

static int do_tcp_proxy(SOCKET s, const sockaddr* orig_dest) {
    // Force blocking for the synchronous handshake
    u_long mode = 0;
    g_real_ioctlsocket(s, FIONBIO, &mode);

    DWORD ms = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));

    // Connect to SOCKS5 proxy (using the real, un-hooked connect)
    sockaddr_in proxy = {};
    proxy.sin_family = AF_INET;
    proxy.sin_port   = htons(PROXY_PORT);
    inet_pton(AF_INET, PROXY_HOST, &proxy.sin_addr);

    if (g_real_connect(s, reinterpret_cast<sockaddr*>(&proxy), sizeof(proxy)) != 0) {
        LOG("TCP: connect to SOCKS5 failed %d", WSAGetLastError());
        return SOCKET_ERROR;
    }
    if (!socks5_tcp_handshake(s, orig_dest)) {
        WSASetLastError(WSAECONNREFUSED); return SOCKET_ERROR;
    }

    // Clear timeouts
    DWORD zero = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&zero), sizeof(zero));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&zero), sizeof(zero));

    // Restore non-blocking if caller had set it
    {
        std::lock_guard<std::mutex> lk(g_nb_mtx);
        auto it = g_was_nonblocking.find(s);
        if (it != g_was_nonblocking.end()) {
            if (it->second) { mode = 1; g_real_ioctlsocket(s, FIONBIO, &mode); }
            g_was_nonblocking.erase(it);
        }
    }

    char ip[64] = {};
    uint16_t port = 0;
    if (orig_dest->sa_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(orig_dest)->sin_addr, ip, sizeof(ip));
        port = ntohs(reinterpret_cast<const sockaddr_in*>(orig_dest)->sin_port);
    } else if (orig_dest->sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(orig_dest)->sin6_addr, ip, sizeof(ip));
        port = ntohs(reinterpret_cast<const sockaddr_in6*>(orig_dest)->sin6_port);
    }
    LOG("TCP proxied -> %s:%u", ip, port);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IOCP socket registry + NtQueryInformationFile fallback
//
//  For ConnectEx: Chrome's network stack associates each socket with an IOCP
//  completion port.  We need to post a completion there so Chrome unblocks.
//  We capture it in hook_CreateIoCompletionPort.  As a fallback we query the
//  kernel directly via NtQueryInformationFile(FileCompletionInformation).
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex g_iocp_mtx;
static std::unordered_map<SOCKET, HANDLE>    g_socket_iocp;
static std::unordered_map<SOCKET, ULONG_PTR> g_socket_iocp_key;

// NtQueryInformationFile – used to retrieve IOCP associated with a socket handle
typedef LONG (NTAPI* fn_NtQIF)(HANDLE, PVOID, PVOID, ULONG, ULONG);
static fn_NtQIF g_NtQueryInformationFile = nullptr;

struct FileCompletionInfo { HANDLE Port; ULONG_PTR Key; };

// Get the IOCP handle for a socket: check our table first, then ask the kernel
static bool get_socket_iocp(SOCKET s, HANDLE& iocp_out, ULONG_PTR& key_out) {
    {
        std::lock_guard<std::mutex> lk(g_iocp_mtx);
        auto it = g_socket_iocp.find(s);
        if (it != g_socket_iocp.end()) {
            iocp_out = it->second;
            key_out  = g_socket_iocp_key[s];
            return true;
        }
    }
    // Fallback: query the kernel (FileCompletionInformation = class 30)
    if (g_NtQueryInformationFile) {
        struct { LONG Status; ULONG_PTR Info; } isb = {};
        FileCompletionInfo ci = {};
        LONG st = g_NtQueryInformationFile(
            reinterpret_cast<HANDLE>(s), &isb, &ci, sizeof(ci), 30);
        if (st == 0 && ci.Port) {
            iocp_out = ci.Port;
            key_out  = ci.Key;
            return true;
        }
    }
    return false;
}

static void signal_connectex_complete(SOCKET s, LPOVERLAPPED ov, DWORD bytes) {
    if (!ov) return;
    ov->Internal     = 0; // STATUS_SUCCESS
    ov->InternalHigh = bytes;
    // Signal event if present (used by WSAWaitForMultipleEvents callers)
    if (ov->hEvent) SetEvent(ov->hEvent);
    // Post to IOCP so GetQueuedCompletionStatus / IOCP callers unblock
    HANDLE iocp; ULONG_PTR key;
    if (get_socket_iocp(s, iocp, key))
        PostQueuedCompletionStatus(iocp, bytes, key, ov);
}

// ─────────────────────────────────────────────────────────────────────────────
//  TCP hooks
// ─────────────────────────────────────────────────────────────────────────────
static int WSAAPI hook_connect(SOCKET s, const sockaddr* name, int namelen) {
    if (is_loopback(name) || sock_type(s) != SOCK_STREAM)
        return g_real_connect(s, name, namelen);
    return do_tcp_proxy(s, name);
}

static int WSAAPI hook_WSAConnect(SOCKET s, const sockaddr* name, int namelen,
    LPWSABUF cd, LPWSABUF ce, LPQOS sq, LPQOS gq)
{
    if (is_loopback(name) || sock_type(s) != SOCK_STREAM)
        return g_real_WSAConnect(s, name, namelen, cd, ce, sq, gq);
    return do_tcp_proxy(s, name);
}

static BOOL PASCAL hook_ConnectEx(SOCKET s, const sockaddr* name, int namelen,
    PVOID sendBuf, DWORD sendLen, LPDWORD bytesSent, LPOVERLAPPED ov)
{
    if (is_loopback(name) || sock_type(s) != SOCK_STREAM) {
        if (g_real_ConnectEx)
            return g_real_ConnectEx(s, name, namelen, sendBuf, sendLen, bytesSent, ov);
        WSASetLastError(WSAEOPNOTSUPP); return FALSE;
    }

    // Bind first (ConnectEx requires it)
    sockaddr_in local = {};
    local.sin_family = AF_INET; local.sin_addr.s_addr = INADDR_ANY;
    bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));

    if (do_tcp_proxy(s, name) != 0) {
        if (ov && ov->hEvent) SetEvent(ov->hEvent);
        return FALSE;
    }

    setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);

    if (sendBuf && sendLen) {
        send(s, reinterpret_cast<const char*>(sendBuf), sendLen, 0);
        if (bytesSent) *bytesSent = sendLen;
    }
    signal_connectex_complete(s, ov, sendLen);
    return TRUE;
}

static int WSAAPI hook_WSAIoctl(SOCKET s, DWORD code,
    LPVOID in_buf, DWORD in_len, LPVOID out_buf, DWORD out_len,
    LPDWORD bytes_ret, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE fn)
{
    static GUID cx_guid = WSAID_CONNECTEX;
    if (code == SIO_GET_EXTENSION_FUNCTION_POINTER &&
        in_len == sizeof(GUID) && out_len >= sizeof(fn_ConnectEx) &&
        memcmp(in_buf, &cx_guid, sizeof(GUID)) == 0)
    {
        *reinterpret_cast<fn_ConnectEx*>(out_buf) = hook_ConnectEx;
        if (bytes_ret) *bytes_ret = sizeof(fn_ConnectEx);
        return 0;
    }
    return g_real_WSAIoctl(s, code, in_buf, in_len, out_buf, out_len, bytes_ret, ov, fn);
}

static int WSAAPI hook_ioctlsocket(SOCKET s, long cmd, u_long* argp) {
    if (cmd == FIONBIO && argp) {
        std::lock_guard<std::mutex> lk(g_nb_mtx);
        g_was_nonblocking[s] = (*argp != 0);
    }
    return g_real_ioctlsocket(s, cmd, argp);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UDP hooks  ──  BLOCK non-loopback UDP
//
//  Blocking UDP forces Discord's WebRTC ICE to fail its host/STUN candidates
//  and fall back to TCP TURN relay (turn.discord.media:443).
//  That TCP connection is then caught by hook_connect / hook_ConnectEx above
//  and tunnelled through Xray.
// ─────────────────────────────────────────────────────────────────────────────
static int WSAAPI hook_sendto(SOCKET s, const char* buf, int len, int flags,
                               const sockaddr* to, int tolen)
{
    // Always allow loopback (Discord IPC, local STUN reflections, etc.)
    if (!to || is_loopback(to))
        return g_real_sendto(s, buf, len, flags, to, tolen);

    // Allow TCP sockets that somehow end up here
    if (sock_type(s) != SOCK_DGRAM)
        return g_real_sendto(s, buf, len, flags, to, tolen);

    // Block external UDP → forces WebRTC to fall back to TCP TURN
    LOG("UDP blocked (forcing TCP TURN fallback)");
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}

static int WSAAPI hook_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD buf_count,
    LPDWORD bytes_sent, DWORD flags, const sockaddr* to, int tolen,
    LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE fn)
{
    if (!to || is_loopback(to) || sock_type(s) != SOCK_DGRAM)
        return g_real_WSASendTo(s, bufs, buf_count, bytes_sent, flags, to, tolen, ov, fn);

    // Block external UDP  (fast-fail so WebRTC retries via TCP TURN immediately)
    if (ov) {
        ov->Internal = WSAENETUNREACH;
        ov->InternalHigh = 0;
        signal_connectex_complete(s, ov, 0);
        WSASetLastError(WSA_IO_PENDING);
        return SOCKET_ERROR;
    }
    WSASetLastError(WSAENETUNREACH);
    return SOCKET_ERROR;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CreateIoCompletionPort hook  ──  track IOCP per socket for ConnectEx
// ─────────────────────────────────────────────────────────────────────────────
static HANDLE WINAPI hook_CreateIoCompletionPort(HANDLE file, HANDLE existing_iocp,
                                                  ULONG_PTR key, DWORD threads)
{
    HANDLE result = g_real_CreateIoCompletionPort(file, existing_iocp, key, threads);
    if (result && file != INVALID_HANDLE_VALUE && existing_iocp != nullptr) {
        std::lock_guard<std::mutex> lk(g_iocp_mtx);
        SOCKET s = reinterpret_cast<SOCKET>(file);
        g_socket_iocp[s]     = result;
        g_socket_iocp_key[s] = key;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IAT patcher
// ─────────────────────────────────────────────────────────────────────────────
struct Patch { const char* dll; const char* fn; void* hook; };

static void patch_module(HMODULE mod, const Patch* patches, int n) {
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(mod);
    if (IsBadReadPtr(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    auto& idir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!idir.VirtualAddress) return;

    auto* desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
        reinterpret_cast<uint8_t*>(mod) + idir.VirtualAddress);

    for (; desc->Name; ++desc) {
        const char* dll_name = reinterpret_cast<const char*>(
            reinterpret_cast<uint8_t*>(mod) + desc->Name);

        auto* thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<uint8_t*>(mod) + desc->FirstThunk);
        auto* orig  = reinterpret_cast<PIMAGE_THUNK_DATA>(
            reinterpret_cast<uint8_t*>(mod) + desc->OriginalFirstThunk);

        for (; thunk->u1.Function; ++thunk, ++orig) {
            if (orig->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            const auto* byname = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(
                reinterpret_cast<uint8_t*>(mod) + orig->u1.AddressOfData);

            for (int i = 0; i < n; ++i) {
                if (_stricmp(dll_name, patches[i].dll) != 0) continue;
                if (strcmp(reinterpret_cast<const char*>(byname->Name), patches[i].fn) != 0) continue;
                DWORD old;
                VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
                thunk->u1.Function = reinterpret_cast<ULONG_PTR>(patches[i].hook);
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
            }
        }
    }
}

static void patch_all() {
    Patch patches[] = {
        { "ws2_32.dll",   "connect",                reinterpret_cast<void*>(hook_connect)              },
        { "ws2_32.dll",   "WSAConnect",             reinterpret_cast<void*>(hook_WSAConnect)           },
        { "ws2_32.dll",   "WSAIoctl",               reinterpret_cast<void*>(hook_WSAIoctl)             },
        { "ws2_32.dll",   "ioctlsocket",            reinterpret_cast<void*>(hook_ioctlsocket)          },
        { "ws2_32.dll",   "sendto",                 reinterpret_cast<void*>(hook_sendto)               },
        { "ws2_32.dll",   "WSASendTo",              reinterpret_cast<void*>(hook_WSASendTo)            },
        { "kernel32.dll", "CreateIoCompletionPort", reinterpret_cast<void*>(hook_CreateIoCompletionPort) },
    };

    HMODULE mods[2048]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;

    DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
        patch_module(mods[i], patches, _countof(patches));

    LOG("IAT patched across %u modules", count);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DllMain
// ─────────────────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hMod);

    log_init(hMod);
    LOG("===== proxy_hook v4 loaded  PID=%u =====", GetCurrentProcessId());
    LOG("SOCKS5: %s:%u", PROXY_HOST, PROXY_PORT);
    LOG("Strategy: TCP proxied via SOCKS5 | UDP blocked -> forces TCP TURN fallback");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    HMODULE ws2   = LoadLibraryA("ws2_32.dll");
    HMODULE k32   = GetModuleHandleA("kernel32.dll");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    g_NtQueryInformationFile = reinterpret_cast<fn_NtQIF>(
        GetProcAddress(ntdll, "NtQueryInformationFile"));

    g_real_connect      = reinterpret_cast<fn_connect>    (GetProcAddress(ws2, "connect"));
    g_real_WSAConnect   = reinterpret_cast<fn_WSAConnect> (GetProcAddress(ws2, "WSAConnect"));
    g_real_WSAIoctl     = reinterpret_cast<fn_WSAIoctl>   (GetProcAddress(ws2, "WSAIoctl"));
    g_real_ioctlsocket  = reinterpret_cast<fn_ioctlsocket>(GetProcAddress(ws2, "ioctlsocket"));
    g_real_sendto       = reinterpret_cast<fn_sendto>     (GetProcAddress(ws2, "sendto"));
    g_real_WSASendTo    = reinterpret_cast<fn_WSASendTo>  (GetProcAddress(ws2, "WSASendTo"));
    g_real_closesocket  = reinterpret_cast<fn_closesocket>(GetProcAddress(ws2, "closesocket"));
    g_real_CreateIoCompletionPort = reinterpret_cast<fn_CreateIoCompletionPort>(
                                        GetProcAddress(k32, "CreateIoCompletionPort"));

    // Get real ConnectEx BEFORE hooking WSAIoctl
    SOCKET tmp = socket(AF_INET, SOCK_STREAM, 0);
    if (tmp != INVALID_SOCKET) {
        GUID cx = WSAID_CONNECTEX; DWORD cb = 0;
        g_real_WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER,
                         &cx, sizeof(cx), &g_real_ConnectEx, sizeof(g_real_ConnectEx),
                         &cb, nullptr, nullptr);
        g_real_closesocket(tmp);
    }

    patch_all();

    LOG("All hooks installed — ready.");
    return TRUE;
}
