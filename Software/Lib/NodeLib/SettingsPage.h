/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NodeLib
{
    // The node's runtime-writable settings (Board::Flash::SettingsBase, Config
    // B -- Node-Flash-Layout-and-Bootloader-Spec.md §3): one small record per
    // node, owned by the application, surviving resets, OTA and re-provisioning.
    //
    // Record: magic(4) payload(10) crc16(2) -- 16 bytes, two flash
    // double-words. Each module picks its own magic, so a board re-flashed as a
    // different module reads a blank record rather than another module's bytes.
    // Unused payload bytes are 0xFF.
    namespace SettingsPage
    {
        constexpr size_t PayloadSize = 10;

        // Copies the stored payload into 'payload' (len <= PayloadSize). False
        // when the page is blank, corrupt or holds another module's magic --
        // 'payload' is then untouched.
        bool Load(const uint32_t magic, uint8_t* const payload, const size_t len);

        // Erases the page and writes a fresh record. Stalls the core for tens
        // of milliseconds (single-bank flash) -- only for settings that change a
        // handful of times in a node's life. False on a flash error.
        bool Save(const uint32_t magic, const uint8_t* const payload, const size_t len);
    } // namespace SettingsPage
} // namespace NodeLib
