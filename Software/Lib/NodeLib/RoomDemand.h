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
