/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

namespace Boot
{
    namespace AppImage
    {
        // True when the image in the application slot is safe to jump to:
        //   - descriptor magic matches, and
        //   - if the descriptor says a CRC-32 is present, it verifies.
        // An image flashed raw over SWD (no finalize step) has no CRC flag and
        // is accepted on the magic alone -- see
        // Spec/Node-Flash-Layout-and-Bootloader-Spec.md §4/§8.
        bool IsValid();
    } // namespace AppImage
} // namespace Boot
