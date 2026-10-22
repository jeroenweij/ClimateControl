/*************************************************************
 * Created by J. Weij
 *************************************************************/

#pragma once

#include <stdint.h>

// Shared per-room "does this supply air help this room" logic, used by both
// ControllerNode's RoomControlLoop and MainController's BudgetAllocator
// (Damper-Budget-Spec.md §2) so the two firmware images can never disagree
// on the answer. Pure, allocation-free -- host-testable without any Hal.
namespace NodeLib
{
    // A room temperature not refreshed for this long is dropped, by the
    // ControllerNode (ThermostatLink) and by MainController (BudgetAllocator)
    // alike: three missed 60 s keepalives (Publisher::keepaliveMs). The
    // Thermostat stops reporting a sensor that has died rather than repeating
    // its last reading, so silence is what a dead sensor looks like.
    constexpr uint32_t RoomTempStaleMs = 3 * 60 * 1000;

    // Endpoint::RoomTempOffset's accepted range, centi-degC: +-10.00 degC is
    // far beyond any self-heating worth correcting, so anything larger is a
    // typo rather than a calibration. Checked by the Thermostat that stores
    // it and by the ControllerNode before relaying a Set.
    constexpr int16_t RoomTempOffsetLimit = 1000;

    // True if supply air at supplyCentiC would move a room at roomTempCentiC
    // toward setpointCentiC -- evaluated per room, not against a house-wide
    // heating/cooling label. A room too warm needs supply colder than itself;
    // a room too cold needs supply warmer than itself. False if supply sits on
    // the wrong side to help, or the room is already within deadbandCentiC of
    // setpoint.
    bool SupplyHelpsRoom(
        const int16_t supplyCentiC,
        const int16_t roomTempCentiC,
        const int16_t setpointCentiC,
        const int16_t deadbandCentiC = 30);

    // What the duct air does for a room (Endpoint::AirMode coding).
    enum class AirMode : uint8_t
    {
        Unknown = 0, // no supply or room temperature
        Heating = 1, // supply warmer than the room
        Cooling = 2, // supply colder than the room
    };

    // Supply more than bandCentiC above/below the room is Heating/Cooling;
    // inside the band 'previous' is kept, so a tie doesn't flicker.
    AirMode UpdateAirMode(
        const AirMode previous,
        const int16_t supplyCentiC,
        const int16_t roomTempCentiC,
        const int16_t bandCentiC = 30);

    // 0..100: magnitude of this room's unmet demand, scaled linearly from 0 at
    // deadbandCentiC to 100 at fullAuthorityCentiC past deadband. 0 whenever
    // !SupplyHelpsRoom().
    uint8_t RoomDemandPercent(
        const int16_t supplyCentiC,
        const int16_t roomTempCentiC,
        const int16_t setpointCentiC,
        const int16_t deadbandCentiC      = 30,
        const int16_t fullAuthorityCentiC = 300);
} // namespace NodeLib
