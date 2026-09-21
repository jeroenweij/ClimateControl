/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

#include "Pin.h"

namespace Hal
{
    struct I2cPins
    {
        Pin     sda;
        Pin     scl;
        uint8_t alternateFunction;
    };

    // Blocking I2C1 master, Standard Mode (100 kHz). Single shared bus for the
    // Thermostat's OLED + room sensor (ControllerNode-Thermostat-Link-Spec.md
    // §4.1/§4.3) -- one instance owned by the caller, handed to each device
    // driver by reference.
    //
    // No STM32Cube I2C HAL is vendored (Lib/HAL/Vendor only carries what
    // earlier peripherals needed), so this talks to the I2C1 registers
    // directly against RM0444 Sec32, the same approach Hal::Adc already takes
    // for ADC1.
    class I2c
    {
      public:
        explicit I2c(const I2cPins& pins);

        void Init();

        // Blocking master write: START, 7-bit address+W, 'len' data bytes,
        // STOP. False on address/data NACK or a bus timeout.
        bool Write(const uint8_t address7, const uint8_t* const data, const uint8_t len);

        // Blocking master read: START, 7-bit address+R, 'len' data bytes
        // (auto-ACKed by hardware except the last), STOP. False on address
        // NACK or a bus timeout.
        bool Read(const uint8_t address7, uint8_t* const data, const uint8_t len);

      private:
        I2cPins pins;
    };
} // namespace Hal
