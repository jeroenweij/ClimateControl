/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Boot
{
    // Minimal USART2 driver for the on-board NINA-W152 link only -- plain
    // point-to-point UART with 4-wire hardware RTS/CTS flow control
    // (Lib/Board/BoardPins.h's NinaCts/NinaRts), NOT RS485 half-duplex DE
    // (unlike Modules/Bootloader/OtaUart.cpp, which this was forked from --
    // the NINA link has no shared bus and no DE net; u-connectXpress's own
    // 4-wire flow control expects the MCU side to honour hardware RTS/CTS
    // instead). Bypasses Lib/HAL/Uart so the ~17 KB STM32Cube UART driver
    // stays out of the 10 KB bootloader image.
    //
    // RX is interrupt-driven, backed by a small ring buffer -- same reasoning
    // as Bootloader/OtaUart.cpp's: a poll-only receiver can miss a byte that
    // lands while the caller is off doing something else for a stretch (a
    // multi-page flash erase/program run being the main one here too), with
    // nothing deeper than the single-byte hardware RDR to hold it.
    class NinaUart
    {
      public:
        void Init(const uint32_t baudRate);

        bool    Available() const;
        uint8_t ReadByte();

        void WriteBytes(const uint8_t* const data, const size_t len);
    };
} // namespace Boot
