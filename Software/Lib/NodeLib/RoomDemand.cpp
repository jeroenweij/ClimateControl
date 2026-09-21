/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "RoomDemand.h"

bool NodeLib::SupplyHelpsRoom(
    const int16_t supplyCentiC,
    const int16_t roomTempCentiC,
    const int16_t setpointCentiC,
    const int16_t deadbandCentiC)
{
    const int16_t error = roomTempCentiC - setpointCentiC; // >0 too warm (wants cooling), <0 too cold (wants heat)

    if (error > deadbandCentiC)
    {
        return supplyCentiC < roomTempCentiC; // wants cooling -- supply must be colder than the room
    }
    if (error < -deadbandCentiC)
    {
        return supplyCentiC > roomTempCentiC; // wants heating -- supply must be warmer than the room
    }
    return false; // already within deadband of setpoint
}

uint8_t NodeLib::RoomDemandPercent(
    const int16_t supplyCentiC,
    const int16_t roomTempCentiC,
    const int16_t setpointCentiC,
    const int16_t deadbandCentiC,
    const int16_t fullAuthorityCentiC)
{
    if (!SupplyHelpsRoom(supplyCentiC, roomTempCentiC, setpointCentiC, deadbandCentiC))
    {
        return 0;
    }

    const int16_t error     = roomTempCentiC - setpointCentiC;
    const int16_t magnitude = error > 0 ? error : static_cast<int16_t>(-error);
    const int16_t want      = static_cast<int16_t>(magnitude - deadbandCentiC); // > 0, guaranteed by SupplyHelpsRoom
    const int16_t span      = static_cast<int16_t>(fullAuthorityCentiC - deadbandCentiC);

    if (span <= 0)
    {
        return 100; // degenerate config -- any qualifying demand is full authority
    }
    if (want >= span)
    {
        return 100;
    }
    // Round to nearest, not floor -- (want * 100) / span truncates down, which
    // would understate every room's demand by up to ~1 point for no reason.
    const int32_t numerator = static_cast<int32_t>(want) * 100;
    return static_cast<uint8_t>((numerator + span / 2) / span);
}
