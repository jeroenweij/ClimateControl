/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"

#include "HalNinaPort.h"

using Hal::UartPin;

HalNinaPort::HalNinaPort() :
    ninaRts(Board::NinaRts, Hal::Gpio::Mode::Output),
    uart()
{
}

void HalNinaPort::Init(const uint32_t baudRate)
{
    // NinaRts (STM32 PA1) -> NINA UART_CTS (module pin 21): held low so the
    // module always considers itself clear to transmit -- u-connectXpress
    // ships with 4-wire HW flow control on by default and nothing else on
    // this link asserts it (proven on the bench by Software/NinaEnable).
    ninaRts.Write(false);

    uart.Init(baudRate, Hal::Uart::Instance::Usart2, UartPin{Board::Usart2Tx, Board::Usart2Af}, UartPin{Board::Usart2Rx, Board::Usart2Af});
}

bool HalNinaPort::Available() const
{
    return uart.Available();
}

uint8_t HalNinaPort::ReadByte()
{
    return uart.ReadByte();
}

void HalNinaPort::WriteBytes(const uint8_t* const data, const size_t len)
{
    uart.WriteBytes(data, len);
}

void HalNinaPort::Flush()
{
    uart.FlushTx();
}
