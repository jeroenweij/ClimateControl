/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "Pin.h"

// Scripted stand-in for Hal::OneWire, keyed by pin so a test can drive two
// single-drop lines (e.g. the return and supply probes) independently.
// Hal::OneWire::Crc8 stays a real Dallas CRC-8 -- see FakeOneWire.cpp.
namespace FakeOneWire
{
    void ResetAll();

    void SetPresent(const Hal::Pin& pin, bool present);
    void QueueRead(const Hal::Pin& pin, const uint8_t* data, size_t len);

    size_t         WrittenLen(const Hal::Pin& pin);
    const uint8_t* Written(const Hal::Pin& pin);
    int            ResetCount(const Hal::Pin& pin);
} // namespace FakeOneWire
