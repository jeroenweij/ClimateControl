/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include "Pin.h"
#include "Uart.h"

// Single source of truth for the STM32G031F8P6 (TSSOP20) pin assignment.
// Matches the schematics in Hardware/ and the pin plan in
// Node-Bus-Hardware-Design-Spec.md Sec6.2.
//
// MCU locked in 2026-09-08 as STM32G031F8P6 -- a drop-in for the earlier
// STM32G030F6P6 (identical TSSOP20 pinout, same 64 MHz M0+ / 8 KB SRAM) with
// 64 KB flash instead of 32 KB (headroom for a bus-resident DFU bootloader +
// the NINA driver) and the extra G031 peripherals: LPUART1, RTC + backup
// registers (a wear-free "enter bootloader" flag), TIM2.
//
// Three physical boards share this map, all built on the same STM32G031F8P6:
//   - "Main" board       -> MainController (bus master + 48V injection + NINA-W152
//                           Wi-Fi) and TemperatureNode (DS18B20 duct sensor) as
//                           two populate variants of one PCB
//   - "Node" board        -> ControllerNode (damper servo, main-bus slave)
//   - "Thermostat" board  -> Thermostat (room UI, I2C OLED + sensor + 2 buttons)
//
// Nothing here is board-conditional: a firmware image simply doesn't reference
// the pins it doesn't use. Where one physical pin has a different role per board
// the comment says so, and both names are defined at the same pin (e.g. pin 13
// is ServoPwm on the Node board and NinaReset on the Main board).
//
// ST's GPIOx macros are pointer casts and can't be constexpr, so these are
// inline const (one shared definition, trivial load-time init).

namespace Board
{
    // --- RS485 main bus, USART1 (Main + Node boards) -----------------------
    //   Not present on the Thermostat board -- there PB6/PB7 are re-used as
    //   I2C1 (see the Thermostat section).
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

    // Wire bit rate for the RS485 main bus and the ControllerNode<->Thermostat
    // link -- an exact integer USART divisor, well within range for the ~100 m /
    // 20-node terminated bus. See RS485-Node-Protocol-Spec-STM32G030.md §9.
    constexpr uint32_t BusBaudRate = 250000;

    // --- Status indicators & user button (all three boards) ---------------
    inline const Hal::Pin ActivityLed{GPIOA, GPIO_PIN_7}; // pin 14  net "LED"         (NodeLib ledPin)
    inline const Hal::Pin ErrorLed{GPIOB, GPIO_PIN_0}; // pin 15  net "LED_ERROR"   (NodeLib errorLedPin)
                                                       //   pin 15 bonds PB0/PB1/PB2/PA8 -- configure PB0 only
    inline const Hal::Pin UserButton{GPIOA, GPIO_PIN_11}; // pin 16  net "USER_BUTTON" active-low, InputPullUp
                                                          //   Thermostat board: this is "button 1" (see Button2) --
                                                          //   driven by BS212C-1 KOUT1 (pin 3) there, not a switch;
                                                          //   NMOS-with-internal-pullup output, same polarity as a
                                                          //   plain switch-to-GND, so no firmware difference.

    // --- USART2 pin group (all three boards; role differs) ---------------
    //   Main board       : point-to-point link to the on-board NINA-W152
    //                      (u-connectXpress AT firmware, 115200 8N1, 4-wire HW
    //                      flow control -- see NinaCts / NinaRts / NinaReset).
    //   Node board        : point-to-point link to this room's Thermostat.
    //   Thermostat board  : point-to-point link to its ControllerNode.
    //   All USART2 signals are AF1 on this package.
    inline const Hal::Pin Usart2Tx{GPIOA, GPIO_PIN_2}; // pin 9
    inline const Hal::Pin Usart2Rx{GPIOA, GPIO_PIN_3}; // pin 10
    inline const Hal::Pin Usart2De{GPIOA, GPIO_PIN_1}; // pin 8   USART2_RTS_DE
                                                       //   Node / Thermostat: link transceiver DE, only if that
                                                       //     link ends up half-duplex RS485
                                                       //     (ControllerNode-Thermostat-Link-Spec.md Sec3, open).
                                                       //   Main board: hardware RTS to the NINA -- same pin and AF,
                                                       //     aliased as NinaRts below.
    constexpr uint8_t     Usart2Af = 1;

    // Thermostat link, ready for Hal::Uart::Init() (mirrors BusUart; the DE
    // entry is inert if the link is wired full-duplex / plain UART).
    inline const Hal::UartPins LinkUart{
        {Usart2Tx, Usart2Af},
        {Usart2Rx, Usart2Af},
        {Usart2De, Usart2Af},
    };

    // --- Main board -- MainController ------------------------------------

    // Bus disable. Moved here from PA0 on 2026-09-08 to free PA0/PA1 for the
    // NINA's USART2 flow-control lines.
    //   HIGH = MOSFET on = ENABLE bus pulled low = slaves disabled.
    //   Push-pull output; reset-safe because the 10k gate pull-down at the
    //   BSS123 holds the bus enabled while PB9 is Hi-Z (POR / unprogrammed).
    //   Pin 2 bonds PB9 with PC14-OSC32_IN: configure PB9, leave PC14 analog.
    //   Committing PB9 to GPIO rules out a 32.768 kHz LSE crystal on pins 2/3.
    inline const Hal::Pin BusEnableDrive{GPIOB, GPIO_PIN_9}; // pin 2  net "RESET_NODES"

    // NINA-W152 on USART2 (Usart2Tx -> NINA UART_RXD, Usart2Rx <- NINA UART_TXD)
    // with 4-wire hardware flow control:
    inline const Hal::Pin NinaCts{GPIOA, GPIO_PIN_0}; // pin 7  USART2_CTS  (AF1) <- NINA UART_RTS (module pin 20)
    inline const Hal::Pin NinaRts{GPIOA, GPIO_PIN_1}; // pin 8  USART2_RTS  (AF1) -> NINA UART_CTS (module pin 21)
                                                      //   same physical pin as Usart2De.

    // NINA RESET_N (module pin 19), net "RESET_NINA". Active low, open-drain:
    // drive low >= 50 us to reset, release (Hi-Z) to run -- the module has a
    // 100k internal pull-up + 10nF. Never drive push-pull high.
    // Same physical pin as the Node board's ServoPwm.
    inline const Hal::Pin NinaReset{GPIOA, GPIO_PIN_6}; // pin 13  net "RESET_NINA"

    // Note: with USART1 = main bus and USART2 = NINA, the Main board has no
    // spare hardware UART for a debug console. Options: bit-bang Tools::Logger
    // on a free pin (PC15 pin 3, PA4 pin 11, PA5 pin 12) or drop the console.

    // --- Main board -- TemperatureNode variant -------------------------
    //   Same PCB with the 48V-injection front-end and NINA DNP, DS18B20 1-Wire
    //   populated instead. PA0/PA1/PA6/PB9 above are unused on this variant.
    inline const Hal::Pin OneWire1{GPIOA, GPIO_PIN_5}; // pin 12  net "ONEWIRE"   open-drain, 4.7k pull-up on board
    inline const Hal::Pin OneWire2{GPIOA, GPIO_PIN_4}; // pin 11  net "ONEWIRE2"  open-drain, 4.7k pull-up on board

    // --- Node board -- ControllerNode ---------------------------------
    inline const Hal::Pin ServoPwm{GPIOA, GPIO_PIN_6}; // pin 13  net "PWM", TIM3_CH1 (AF1)
                                                       //   externally pulled to the safe damper position.
                                                       //   Same physical pin as the Main board's NinaReset.
    inline const Hal::Pin ServoEnable{GPIOA, GPIO_PIN_5}; // pin 12  servo power-enable, HIGH = servo powered.
                                                          //   Off by default (pin Hi-Z at reset / unprogrammed):
                                                          //   the 5V servo rail sits behind a MCU-gated high-side
                                                          //   switch, energised only for a move. See
                                                          //   Node-Bus-Power-Path-Spec.md §3.1.

    // --- Thermostat board -- Thermostat ------------------------------
    //   No main bus: USART1's PB6/PB7 become I2C1 for the OLED + room sensor;
    //   USART2 (Usart2Tx/Rx, + Usart2De if half-duplex) is the link to this
    //   room's ControllerNode.
    inline const Hal::Pin I2cSda{GPIOB, GPIO_PIN_7}; // pin 1   I2C1_SDA (AF6)  SSD1306/SSD1315 OLED + CHT40MEMS sensor
    inline const Hal::Pin I2cScl{GPIOB, GPIO_PIN_6}; // pin 20  I2C1_SCL (AF6)
                                                     //   pin 1 bonds PB7/PB8; pin 20 bonds PB3/PB4/PB5/PB6.
    constexpr uint8_t     I2cAf = 6;

    inline const Hal::Pin Button2{GPIOA, GPIO_PIN_12}; // pin 17  UI set/adjust, active-low, InputPullUp
                                                       //   (button 1 is UserButton / PA11 above -- ErrorHandler ack
                                                       //    + clear link-lost). PA12 is USART1_DE on the other boards.
                                                       //   Thermostat board: driven by BS212C-1 KOUT2 (pin 4), same
                                                       //   NMOS-with-internal-pullup polarity, no firmware difference.

    // Decided 2026-09-12, per the OLED datasheet's I2C-with-internal-charge-pump
    // reference circuit (ControllerNode-Thermostat-Link-Spec.md §4.1):
    inline const Hal::Pin OledReset{GPIOA, GPIO_PIN_0}; // pin 7   net "RES", OLED RES# -- active-low, hold low >=
                                                        //   3us then release to run (datasheet §4.3 reset circuit).
    inline const Hal::Pin OledPowerEnable{GPIOA, GPIO_PIN_4}; // pin 11  net "GPIO", gates the Q3/Q4 load-switch pair
                                                              //   feeding the OLED's VBAT -- HIGH = VBAT on. Off by
                                                              //   default (Hi-Z at reset), same reset-safe pattern as
                                                              //   ServoEnable above. Required by the datasheet's own
                                                              //   warning: without this switch, VBAT leaks current
                                                              //   whenever the charge pump is enabled.

    // --- Fixed by silicon (no assignment choice) -----------------------
    //   NRST        pin 6
    //   SWDIO  PA13 pin 18
    //   SWCLK  PA14 pin 19
} // namespace Board
