/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Pin.h"
#include "Uart.h"

// Single source of truth for the STM32G030F6P6 (TSSOP20) pin assignment.
// Matches the schematics in Hardware/ and the pin plan in
// Node-Bus-Hardware-Design-Spec.md Sec6.2.
//
// Two physical boards share this map:
//   - "Base" board -> ControllerNode
//   - "Main" board -> MainController and TemperatureNode
// Nothing here is board-conditional: a firmware image simply doesn't reference
// the pins it doesn't use. Where one net has different roles per board the
// comment says so.
//
// ST's GPIOx macros are pointer casts and can't be constexpr, so these are
// inline const (one shared definition, trivial load-time init).

namespace Board
{
    // --- RS485 main bus, USART1 (all boards) --------------------------------
    inline const Hal::Pin BusTx{GPIOB, GPIO_PIN_6}; // pin 20  USART1_TX  -> transceiver DI
    inline const Hal::Pin BusRx{GPIOB, GPIO_PIN_7}; // pin 1   USART1_RX  -> transceiver RO
    inline const Hal::Pin BusDe{GPIOA, GPIO_PIN_12}; // pin 17  USART1_DE  -> transceiver DE + /RE

    // Ready to hand to Hal::Uart::Init(). TX/RX are AF0, DE is AF1 -- no single
    // AF covers all three on this package.
    inline const Hal::UartPins BusUart{
        {BusTx, 0},
        {BusRx, 0},
        {BusDe, 1},
    };

    // --- Status indicators & user button (all boards) ----------------------
    inline const Hal::Pin ActivityLed{GPIOA, GPIO_PIN_7}; // pin 14  net "LED"         (NodeLib ledPin)
    inline const Hal::Pin ErrorLed{GPIOB, GPIO_PIN_0}; // pin 15  net "LED_ERROR"   (NodeLib errorLedPin)
                                                       //   pin 15 bonds PB0/PB1/PB2/PA8 -- configure PB0 only
    inline const Hal::Pin UserButton{GPIOA, GPIO_PIN_11}; // pin 16  net "USER_BUTTON" active-low, InputPullUp

    // --- USART2 -----------------------------------------------------------
    //   Main board: debug console on header H1.
    //   Base board: point-to-point link to this room's Thermostat.
    inline const Hal::Pin Usart2Tx{GPIOA, GPIO_PIN_2}; // pin 9
    inline const Hal::Pin Usart2Rx{GPIOA, GPIO_PIN_3}; // pin 10
    inline const Hal::Pin Usart2De{GPIOA, GPIO_PIN_1}; // pin 8   Base board only, if the link is half-duplex RS485
    constexpr uint8_t     Usart2Af = 1;

    // --- Base board -- ControllerNode -------------------------------------
    inline const Hal::Pin ServoPwm{GPIOA, GPIO_PIN_6}; // pin 13  net "PWM", TIM3_CH1 (AF1)

    // --- Main board -- MainController: bus disable ----------------------
    inline const Hal::Pin BusEnableDrive{GPIOA, GPIO_PIN_0}; // pin 7  net "RESET_NODES", push-pull output
                                                             //   HIGH = MOSFET on = ENABLE bus low = slaves disabled

    // --- Main board -- TemperatureNode: DS18B20 1-Wire ------------------
    inline const Hal::Pin OneWire1{GPIOA, GPIO_PIN_5}; // pin 12  net "ONEWIRE"   open-drain, 4.7k pull-up on board
    inline const Hal::Pin OneWire2{GPIOA, GPIO_PIN_4}; // pin 11  net "ONEWIRE2"  open-drain, 4.7k pull-up on board

    // --- Fixed by silicon (no assignment choice) -----------------------
    //   NRST        pin 6
    //   SWDIO  PA13 pin 18
    //   SWCLK  PA14 pin 19
} // namespace Board
