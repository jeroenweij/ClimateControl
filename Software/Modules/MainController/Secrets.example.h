/*************************************************************
 * Created by J. Weij
 *
 * Template for Secrets.h -- copy this file to Secrets.h (gitignored, never
 * committed) and fill in the real values. Mirrors Webserver's own
 * config.example.json / config.json split.
 *
 * UplinkToken must match the server's config.json "uplinkToken" field
 * exactly (Webserver/README.md) -- read it from the deployed server, it is
 * generated there on first start. Convert the 32 hex characters to 16 bytes,
 * e.g. "0123456789abcdef..." -> {0x01, 0x23, 0x45, 0x67, 0x89, ...}.
 *************************************************************/

#pragma once

#include <stdint.h>

namespace Secrets
{
    inline constexpr const char* WifiSsid     = "your-ssid";
    inline constexpr const char* WifiPassword = "your-wifi-password";

    inline constexpr const char* ServerHost = "your-server-host-or-ip";
    inline constexpr uint16_t    ServerPort = 9000;

    // Must match the server's config.json "uplinkToken" (32 hex chars -> 16 bytes).
    inline constexpr uint8_t UplinkToken[16] = {
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    };
} // namespace Secrets
