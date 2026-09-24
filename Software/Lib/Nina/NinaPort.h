/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stddef.h>
#include <stdint.h>

// The NINA module's UART, as the driver sees it. The application runs it on
// Hal::Uart (interrupt-driven TX ring, HalNinaPort), the bootloader on its own
// register-level NinaUart (which keeps the ~17 KB STM32Cube UART driver out of
// the 10 KB image); host tests plug in fakes. Each adapter also owns whatever
// flow-control wiring its UART needs, so the driver above never touches pins
// other than the module's reset line.
class NinaPort
{
  public:
    // Brings the UART (and its flow-control lines) up at 'baudRate' 8N1.
    virtual void Init(const uint32_t baudRate) = 0;

    virtual bool    Available() const = 0;
    virtual uint8_t ReadByte()        = 0;

    // Queues (or, on a blocking adapter, sends) 'len' bytes.
    virtual void WriteBytes(const uint8_t* const data, const size_t len) = 0;

    // Returns once everything written so far has left the UART -- needed
    // before a reset kills the peripheral. A no-op where WriteBytes() blocks.
    virtual void Flush() = 0;
};
