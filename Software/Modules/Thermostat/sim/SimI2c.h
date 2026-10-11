/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Host stand-in for Hal::I2c that behaves like the Thermostat's two real I2C
// devices, so ThermostatHandler runs unmodified on a PC:
//
//  - an SSD1306 controller: decodes the command/data stream the driver sends
//    (page/column addressing, display on/off, segment remap, COM scan
//    direction, inverse) into its own 128x64 GDDRAM -- so what the simulator
//    shows is what reached the wire, not a peek at Ssd1306's framebuffer;
//  - a CHT40 room sensor: answers each measurement with the current
//    simulated temperature/humidity, CRC'd like the real part.
namespace SimI2c
{
    // What the simulated sensor reports. 'present' false: the sensor NACKs
    // (dead/unplugged), exercising the handler's sensor-failure path.
    void SetRoom(const int16_t centiDegC, const uint16_t centiRH);
    void SetSensorPresent(const bool present);

    // The panel as the eye would see it: pixel (x, y), x right, y down,
    // after the controller's remap/scan settings. False everywhere while the
    // panel is off (0xAE).
    bool    PanelOn();
    bool    Pixel(const int x, const int y);
    uint8_t Contrast();

    // Bumped on every GDDRAM write or on/off change -- the renderer only
    // redraws when this moves.
    uint32_t Revision();
} // namespace SimI2c
