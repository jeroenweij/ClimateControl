/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

namespace Tools
{
    // The app <-> bootloader boot-fail counter (Hal::Backup::Reg::
    // BootCounter, Node-Flash-Layout-and-Bootloader-Spec.md §5/§8): a
    // CRC-valid image that crashes before ever proving itself healthy would
    // otherwise boot-loop forever, always "valid" by the CRC gate alone.
    namespace BootHealth
    {
        // Bootloader side: call once, right before deciding whether to jump
        // to the app. Increments the counter and returns true once it has
        // climbed past the retry budget -- the caller should treat that the
        // same as an invalid image (stay resident) rather than jumping again.
        bool TooManyFailedBoots();

        // Bootloader side: a freshly received image deserves a clean slate,
        // not the previous image's failure count. Call when a new transfer
        // starts (Firmware[Begin]).
        void ResetFailedBootCount();

        // App side: call once per main-loop iteration. After a fixed amount
        // of uptime, clears the counter exactly once -- proof this boot
        // wasn't a crash loop. A no-op on every call after the first clear.
        void ConfirmBootHealthy();
    } // namespace BootHealth
} // namespace Tools
