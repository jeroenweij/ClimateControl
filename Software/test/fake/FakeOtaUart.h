/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

// Boot::OtaUart double -- same shape as FakeBus, but separate (OtaUart
// deliberately bypasses Lib/HAL/Uart on real hardware, so it needs its own
// fake rather than sharing FakeBus's buffers).
namespace FakeOtaUart
{
    void Reset();

    const uint8_t* Tx();
    size_t         TxLen();

    void InjectRx(const uint8_t* const data, const size_t length);
} // namespace FakeOtaUart
