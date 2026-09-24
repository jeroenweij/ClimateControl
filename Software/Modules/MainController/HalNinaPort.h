/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Gpio.h"
#include "Uart.h"

#include "NinaPort.h"

// The application's NinaPort: Hal::Uart on USART2 (interrupt-driven TX ring),
// plus the flow-control line the module needs.
class HalNinaPort : public NinaPort
{
  public:
    HalNinaPort();

    void    Init(const uint32_t baudRate) override;
    bool    Available() const override;
    uint8_t ReadByte() override;
    void    WriteBytes(const uint8_t* const data, const size_t len) override;
    void    Flush() override;

  private:
    Hal::Gpio ninaRts;
    Hal::Uart uart;
};
