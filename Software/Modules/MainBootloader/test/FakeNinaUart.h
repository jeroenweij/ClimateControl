/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

// Boot::NinaUart double: the bootloader's own raw-register USART driver for
// the NINA link, replaced by two plain buffers (MCU -> module in Tx(),
// module -> MCU via InjectRx()).
namespace FakeNinaUart
{
    void Reset();

    const uint8_t* Tx();
    size_t         TxLen();
    void           TruncateTx(const size_t length);

    void InjectRx(const uint8_t* const data, const size_t length);
} // namespace FakeNinaUart
