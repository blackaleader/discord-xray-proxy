# Discord Voice UDP + Xray Proxy

> Route **all** Discord traffic — including voice UDP — through an Xray / V2Ray proxy using a kernel-level TUN adapter and a custom C++ Winsock hook DLL.

---

## What This Project Does

Discord splits itself into multiple sandboxed processes. Standard proxy tools that hook the Windows API (DLL injection) cannot reach inside Discord's sandboxed voice process — the sandbox blocks them by design.

This project solves that with two complementary layers:

| Layer | Tool | Covers |
|---|---|---|
| **TUN adapter** (kernel level) | sing-box + wintun | All Discord processes including sandboxed voice UDP |
| **Winsock hook DLL** (API level) | Custom C++ DLL | Additional TCP coverage for non-sandboxed processes |

The TUN adapter creates a virtual network card and wins the Windows routing table with **metric 0** — the highest possible priority. Every outgoing packet from every Discord process must pass through it before reaching the real internet. sing-box reads the process name and routes `Discord.exe` traffic through Xray's SOCKS5 inbound, which tunnels everything through your VLESS / VMess server.

```
Discord (any process, any sandbox)
        │
        ▼  all packets
  Windows routing table
        │
        ▼  metric 0 wins
     tun0 (wintun virtual adapter)
        │
        ▼
     sing-box
        │  process = Discord.exe?
        ├─ YES ──► Xray SOCKS5 (127.0.0.1:10808)
        │               │
        │               ▼
        │         VLESS / VMess
        │               │
        │               ▼
        │         Your proxy server
        │               │
        │               ▼
        │         Discord servers
        │
        └─ NO ───► Direct (everything else unchanged)
```

---

## Requirements

| Requirement | Notes |
|---|---|
| Windows 10 / 11 (x64) | Tested on Windows 11 |
| Xray / V2Ray running | SOCKS5 inbound on `127.0.0.1:10808` with `"udp": true` |
| Administrator privileges | Required for TUN adapter creation |
| Visual Studio 2019 / 2022 | **Only** needed if you want to rebuild the C++ DLL from source |

> **sing-box and wintun.dll are bundled** in the `tools/` folder — no v2rayN installation required to run.

---

## Project Structure

```
discord-xray-proxy/
│
├── start_tun.bat              ← ONE-CLICK launcher (run as Admin)  ← START HERE
├── stop_tun.bat               ← Stop the TUN when done
├── sing-box-tun.json          ← sing-box routing config
├── launch_discord_proxied.bat ← Alternative: Chromium proxy flags only (no TUN)
│
├── tools/                     ← Bundled — no extra installs needed
│   ├── sing-box.exe           ← sing-box binary (routes TUN traffic)
│   ├── wintun.dll             ← WireGuard TUN driver for Windows
│   ├── LICENSE-sing-box.txt
│   └── LICENSE-wintun.txt
│
├── src/                       ← C++ source (optional, for rebuilding the DLL)
│   ├── config.h
│   ├── proxy_hook.cpp
│   └── injector.cpp
│
├── bin/                       ← Built after running build.bat
│   ├── proxy_hook.dll
│   └── discord_proxy.exe
│
├── logs/                      ← Created at runtime
│   └── sing-box-tun.log
│
└── build.bat                  ← Rebuilds C++ DLL+injector (needs MSVC)
```

---

## Quick Start (Recommended — TUN mode)

This is the only method that fully proxies Discord voice UDP.

> **sing-box and wintun are already bundled** in `tools/`. Just clone and run.

```bash
git clone https://github.com/blackaleader/discord-xray-proxy
cd discord-xray-proxy
# right-click start_tun.bat → Run as Administrator
```

### Step 1 — Configure Xray

Make sure your Xray / v2rayN SOCKS5 inbound has UDP enabled:

```json
{
  "tag": "socks",
  "port": 10808,
  "listen": "127.0.0.1",
  "protocol": "mixed",
  "settings": {
    "auth": "noauth",
    "udp": true
  }
}
```

### Step 2 — Start the proxy

Right-click `start_tun.bat` → **Run as Administrator**

The script will:
1. Verify Xray is running on port 10808
2. Start sing-box with the TUN adapter
3. Kill any existing Discord instance
4. Launch Discord fresh (all traffic now routed through Xray)

### Step 4 — Verify it works

Open `sing-box-tun.log` in the project folder. You should see:

```
outbound/socks[xray-socks5]: outbound connection to <discord-ip>:443
outbound/socks[xray-socks5]: outbound packet connection to <discord-ip>:443
```

`outbound connection` = TCP (chat, gateway)
`outbound packet connection` = UDP (voice)

### Step 5 — Stop

Run `stop_tun.bat` (as Administrator) or close the sing-box window.

---

## Alternative — C++ Winsock Hook DLL

This method hooks Discord's Winsock API directly inside the process. It works for TCP connections (chat, gateway) but **cannot reach sandboxed voice processes**. Use it alongside TUN mode or on its own for TCP-only proxying.

### Build

```bat
build.bat
```

Auto-detects Visual Studio via `vswhere`. Output goes to `bin\`.

### Run

```bat
bin\discord_proxy.exe   (run as Administrator)
```

Monitors for Discord processes and injects `proxy_hook.dll` automatically.

### Change proxy settings

Edit `src\config.h`:

```cpp
#define PROXY_HOST  "127.0.0.1"
#define PROXY_PORT  10808
```

Then rebuild with `build.bat`.

---

## How It Works — Deep Dive

### TUN mode

sing-box creates a virtual network adapter using `wintun.dll` (the same driver used by WireGuard). Windows adds a default route (`0.0.0.0/0`) through this adapter with **metric 0**. Since Windows picks the route with the lowest metric, every outgoing IP packet is pulled into sing-box before it reaches any physical network card.

sing-box inspects each packet, identifies the sending process using `GetExtendedTcpTable` / `GetExtendedUdpTable`, and applies routing rules:

- `Discord.exe` → SOCKS5 outbound → Xray at `127.0.0.1:10808`
- everything else → direct

Xray receives the traffic through its `mixed` inbound (which supports both TCP and SOCKS5 UDP ASSOCIATE) and tunnels it through the configured outbound (VLESS, VMess, etc.) to your server.

### C++ Winsock Hook DLL

The DLL patches the **Import Address Table (IAT)** of every loaded module inside the target Discord process. It replaces the IAT entries for:

- `ws2_32!connect`
- `ws2_32!WSAConnect`
- `ws2_32!WSAIoctl` (to intercept `ConnectEx` retrieval)
- `ws2_32!ioctlsocket`
- `ws2_32!sendto` / `WSASendTo`

When Discord calls `connect()` to an external address, our hook intercepts it, connects instead to the SOCKS5 proxy, performs the handshake with the original destination, and returns success — transparent to Discord.

The real function pointers are saved via `GetProcAddress` **before** any patching, so hooks can call the originals without recursion.

---

## Configuration

Open `start_tun.bat` and edit the two lines at the top:

```bat
set "PROXY_HOST=127.0.0.1"
set "PROXY_PORT=10808"
```

| Setting | Default | Notes |
|---|---|---|
| `PROXY_HOST` | `127.0.0.1` | Almost always loopback |
| `PROXY_PORT` | `10808` | v2rayN default. Change to match your Xray SOCKS5 inbound port |

Common ports: `10808` (v2rayN), `1080` (classic SOCKS5), `7890` (Clash).

The script auto-generates `sing-box-tun.json` on every run using these values, so you never need to edit the JSON directly.

---

## Troubleshooting

**`ConnectionReset` / `update-failure` errors on Discord startup**
- Your proxy port is wrong. Open `start_tun.bat` and set `PROXY_PORT` to the port your Xray/v2rayN SOCKS5 inbound is actually listening on.
- Check with: `netstat -ano | findstr LISTENING` — find the port your proxy is on.

**sing-box fails to start**
- Make sure you are running as Administrator
- Check `sing-box-tun.log` for error details
- Ensure `wintun.dll` is in the same folder as `sing-box.exe`

**Voice still not proxied**
- Confirm `"udp": true` is set in your Xray SOCKS5 inbound config
- Check the log for `outbound packet connection` entries — these confirm UDP is routed
- Make sure no old Discord processes are running before `start_tun.bat` (the script kills them automatically)

**Discord won't start**
- Make sure Discord is installed in `%LOCALAPPDATA%\Discord\`
- Update the `DISCORD_EXE` path in `start_tun.bat` if needed

**build.bat fails**
- Install Visual Studio 2019 or 2022 with the **Desktop development with C++** workload

---

## License

MIT — do whatever you want with it.
