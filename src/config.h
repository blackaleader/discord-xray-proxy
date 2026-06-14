#pragma once

// ─────────────────────────────────────────────
//  Discord SOCKS5 Proxy  ─  Shared Configuration
// ─────────────────────────────────────────────

// Upstream SOCKS5 proxy (your Xray / V2Ray core)
#define PROXY_HOST   "127.0.0.1"
#define PROXY_PORT   10808

// Discord process names to intercept (all variants)
static const char* DISCORD_PROCESSES[] = {
    "Discord.exe",
    "DiscordPTB.exe",
    "DiscordCanary.exe",
    "DiscordDevelopment.exe",
    nullptr
};
