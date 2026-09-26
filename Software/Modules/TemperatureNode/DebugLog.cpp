/*************************************************************
 * Created by J. Weij
 *
 * TemperatureNode's Tools::Logger backend -- unlike MainController (both
 * USARTs already committed to the bus and NINA, forcing a bit-banged
 * console, see Modules/MainController/DebugLog.cpp), this variant has NINA
 * DNP and no Thermostat link, so USART2 (Board::Usart2Tx/Usart2Rx) is a free
 * hardware UART -- a plain console, no bit-banging needed. Overrides the
 * weak default in Lib/Tools/Logger.cpp -- see that file's header comment.
 *************************************************************/

#include <string.h>

#include "BoardPins.h"
#include "Uart.h"

#include "Logger.h"

using Hal::Uart;
using Hal::UartPin;

namespace
{
    const uint32_t LogBaud = 115200;

    void WriteChar(Uart& uart, const char character)
    {
        uart.WriteBytes(reinterpret_cast<const uint8_t*>(&character), 1);
    }

    void WriteText(Uart& uart, const char* const text)
    {
        uart.WriteBytes(reinterpret_cast<const uint8_t*>(text), strlen(text));
    }
} // namespace

void Tools::Logger::Write(const char level, const char* const msg)
{
    static Uart uart;
    static bool initialized = false;

    if (!initialized)
    {
        uart.Init(LogBaud, Uart::Instance::Usart2, UartPin{Board::Usart2Tx, Board::Usart2Af}, UartPin{Board::Usart2Rx, Board::Usart2Af});
        initialized = true;
    }

    WriteChar(uart, level);
    WriteText(uart, ": ");
    WriteText(uart, msg);
    WriteText(uart, "\r\n");
}
