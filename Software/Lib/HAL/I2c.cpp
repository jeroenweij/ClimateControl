/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Tick.h"

#include "I2c.h"

using Hal::I2c;
using Hal::I2cPins;

namespace
{
    // Any single wait (address match, byte-ready, STOP) comfortably finishes
    // within a bus period or two at 100 kHz -- a device that never answers
    // (missing/dead) should fail fast rather than hang the caller's main loop.
    constexpr uint32_t timeoutMs = 5;

    // TIMINGR for Standard Mode (100 kHz) at a 64 MHz I2C kernel clock
    // (I2C1 defaults to PCLK1, which is SYSCLK on this project's un-prescaled
    // bus, Software-Architecture-Spec.md's 64 MHz Cortex-M0+ clock), derived
    // by hand from RM0444 Sec32.4.9 rather than copied from an unrelated
    // family/clock's CubeMX output:
    //   PRESC=15  -> tPRESC = 16 / 64MHz     = 250 ns
    //   SCLL=21   -> tLOW   = 22 * tPRESC    = 5500 ns (>= 4700 ns spec min)
    //   SCLH=18   -> tHIGH  = 19 * tPRESC    = 4750 ns (>= 4000 ns spec min)
    //   -> tSCL ~= 10250 ns -> ~97.6 kHz, safely under the 100 kHz Sm cap.
    //   SCLDEL=4, SDADEL=2 -- conservative setup/hold margins, same ballpark
    //   as ST's own published Sm timing examples.
    // TEMP: not yet verified on a scope against real silicon -- no Thermostat
    // board on the bench yet (OTA-Debugging-TODO.md's bench notes are
    // TemperatureNode-only). Re-check tLOW/tHIGH/rise-time once one exists.
    constexpr uint32_t Presc  = 15;
    constexpr uint32_t SclDel = 4;
    constexpr uint32_t SdaDel = 2;
    constexpr uint32_t SclH   = 18;
    constexpr uint32_t SclL   = 21;
    constexpr uint32_t TimingRValue =
        (Presc << 28) | (SclDel << 20) | (SdaDel << 16) | (SclH << 8) | SclL;

    void ConfigureAfPin(const Hal::Pin pin, const uint8_t alternateFunction)
    {
        GPIO_InitTypeDef init = {};
        init.Pin              = pin.pin;
        init.Mode             = GPIO_MODE_AF_OD; // I2C is open-drain; external pull-ups (CHT40MEMS's reference circuit)
        init.Pull             = GPIO_NOPULL;
        init.Speed            = GPIO_SPEED_FREQ_HIGH;
        init.Alternate        = alternateFunction;

        HAL_GPIO_Init(pin.port, &init);
    }

    // Waits (bounded by timeoutMs) for any bit in 'mask' to be set in I2C1->ISR.
    // Returns false on timeout; 'seen' holds whichever bits were set otherwise.
    bool WaitForAny(const uint32_t mask, uint32_t& seen)
    {
        const uint32_t start = Hal::Tick::Millis();
        while (true)
        {
            seen = I2C1->ISR & mask;
            if (seen != 0)
            {
                return true;
            }
            if (Hal::Tick::Millis() - start >= timeoutMs)
            {
                return false;
            }
        }
    }
} // namespace

I2c::I2c(const I2cPins& pins) :
    pins(pins)
{
}

void I2c::Init()
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    ConfigureAfPin(pins.sda, pins.alternateFunction);
    ConfigureAfPin(pins.scl, pins.alternateFunction);

    __HAL_RCC_I2C1_CLK_ENABLE();

    I2C1->CR1     = 0; // PE=0 while (re)configuring, per RM0444 Sec32.4.6
    I2C1->TIMINGR = TimingRValue;
    I2C1->CR1     = I2C_CR1_PE;
}

bool I2c::Write(const uint8_t address7, const uint8_t* const data, const uint8_t len)
{
    I2C1->CR2 = (static_cast<uint32_t>(address7) << 1) | (static_cast<uint32_t>(len) << I2C_CR2_NBYTES_Pos) |
        I2C_CR2_AUTOEND | I2C_CR2_START;

    for (uint8_t i = 0; i < len; i++)
    {
        uint32_t flags;
        if (!WaitForAny(I2C_ISR_TXIS | I2C_ISR_NACKF, flags) || (flags & I2C_ISR_NACKF) != 0)
        {
            I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
            return false;
        }
        I2C1->TXDR = data[i];
    }

    uint32_t flags;
    WaitForAny(I2C_ISR_STOPF, flags);
    I2C1->ICR = I2C_ICR_STOPCF;
    return true;
}

bool I2c::Read(const uint8_t address7, uint8_t* const data, const uint8_t len)
{
    I2C1->CR2 = (static_cast<uint32_t>(address7) << 1) | I2C_CR2_RD_WRN |
        (static_cast<uint32_t>(len) << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;

    for (uint8_t i = 0; i < len; i++)
    {
        uint32_t flags;
        if (!WaitForAny(I2C_ISR_RXNE | I2C_ISR_NACKF, flags) || (flags & I2C_ISR_NACKF) != 0)
        {
            I2C1->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
            return false;
        }
        data[i] = static_cast<uint8_t>(I2C1->RXDR);
    }

    uint32_t flags;
    WaitForAny(I2C_ISR_STOPF, flags);
    I2C1->ICR = I2C_ICR_STOPCF;
    return true;
}
