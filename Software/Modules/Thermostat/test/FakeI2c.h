/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

// Scripted stand-in for Hal::I2c, keyed by 7-bit device address so a test can
// drive the OLED and the room sensor independently on the same shared bus.
namespace FakeI2c
{
    void ResetAll();

    // True: every Write()/Read() to this address fails (device absent/NACK).
    void SetNack(const uint8_t address7, const bool nack);

    void QueueRead(const uint8_t address7, const uint8_t* const data, const size_t len);

    size_t         WrittenLen(const uint8_t address7);
    const uint8_t* Written(const uint8_t address7);
    void           ClearWritten(const uint8_t address7);
} // namespace FakeI2c
