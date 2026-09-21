/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "RoomDemand.h"

#include "Test.h"

using NodeLib::RoomDemandPercent;
using NodeLib::SupplyHelpsRoom;

CC_TEST(SupplyHelpsRoom, ColdSupplyHelpsAWarmRoomWantingCooling)
{
    // room 22.0C, setpoint 20.0C (wants cooling), supply 18.0C (colder than room)
    CC_CHECK(SupplyHelpsRoom(1800, 2200, 2000));
}

CC_TEST(SupplyHelpsRoom, WarmSupplyHelpsAColdRoomWantingHeat)
{
    // room 18.0C, setpoint 20.0C (wants heat), supply 22.0C (warmer than room)
    CC_CHECK(SupplyHelpsRoom(2200, 1800, 2000));
}

CC_TEST(SupplyHelpsRoom, HouseWideTrendDoesNotOverrideThisRoomsOwnComparison)
{
    // The scenario from the design discussion: return 20.0C / supply 19.0C
    // reads as "the house is trending cooling" on average, but this room at
    // 18.0C wanting 21.0C (heat) is still helped by 19.0C supply air, because
    // 19.0C is warmer than this room's own 18.0C -- house-wide trend is
    // irrelevant to this room's own comparison.
    CC_CHECK(SupplyHelpsRoom(1900, 1800, 2100));
}

CC_TEST(SupplyHelpsRoom, SupplyOnTheWrongSideDoesNotHelp)
{
    // room 18.0C, setpoint 20.0C (wants heat), supply 15.0C -- colder than the
    // room, opening the damper would cool it further away from setpoint.
    CC_CHECK(!SupplyHelpsRoom(1500, 1800, 2000));
}

CC_TEST(SupplyHelpsRoom, WithinDeadbandNeverHelps)
{
    // room 20.1C, setpoint 20.0C, default 0.3C deadband -- already close enough.
    CC_CHECK(!SupplyHelpsRoom(1500, 2010, 2000));
    CC_CHECK(!SupplyHelpsRoom(2500, 2010, 2000));
}

CC_TEST(RoomDemandPercent, ZeroWhenSupplyCannotHelp)
{
    CC_CHECK_EQ(RoomDemandPercent(1500, 1800, 2000), 0); // supply colder, room wants heat
}

CC_TEST(RoomDemandPercent, ZeroAtSetpoint)
{
    CC_CHECK_EQ(RoomDemandPercent(1500, 2000, 2000), 0);
}

CC_TEST(RoomDemandPercent, RampsLinearlyFromDeadbandToFullAuthority)
{
    // defaults: deadband 0.3C, full authority 3.0C past deadband
    // error exactly at deadband -> 0
    CC_CHECK_EQ(RoomDemandPercent(1500, 2030, 2000), 0);
    // error at deadband + half of (fullAuthority - deadband) -> ~50
    const uint8_t half = RoomDemandPercent(1500, 2165, 2000); // error = 1.65C
    CC_CHECK(half >= 45 && half <= 55);
    // error at/after full authority -> saturates at 100
    CC_CHECK_EQ(RoomDemandPercent(1500, 2400, 2000), 100);
    CC_CHECK_EQ(RoomDemandPercent(1500, 3000, 2000), 100);
}

CC_TEST(RoomDemandPercent, WorksSymmetricallyForHeating)
{
    CC_CHECK_EQ(RoomDemandPercent(2400, 1600, 2000), 100); // 4.0C below setpoint, warm supply
    CC_CHECK_EQ(RoomDemandPercent(1600, 1600, 2000), 0); // cold supply can't help a cold room
}

CC_TEST(RoomDemandPercent, CustomDeadbandAndFullAuthorityAreHonoured)
{
    // 1.0C deadband, 2.0C span past it -> error of 2.0C is exactly full authority
    CC_CHECK_EQ(RoomDemandPercent(1500, 2300, 2000, 100, 300), 100);
    CC_CHECK_EQ(RoomDemandPercent(1500, 2100, 2000, 100, 300), 0);
}

CC_TEST(RoomDemandPercent, RoundsToNearestRatherThanFlooring)
{
    // 1.0C deadband, 3.0C span (fullAuthority 300, deadband 100) -> span = 200.
    // error 1.99C -> want = 99, exactly half of span -> true value 49.5%,
    // which should round up to 50, not floor to 49.
    CC_CHECK_EQ(RoomDemandPercent(1500, 2199, 2000, 100, 300), 50);

    // Default deadband(30)/fullAuthority(300) -> span = 270. error 1.64C ->
    // want = 134 -> true value 134*100/270 = 49.6%, rounds up to 50 (floor
    // would give 49).
    CC_CHECK_EQ(RoomDemandPercent(1500, 2164, 2000), 50);

    // error 0.58C -> want = 28 -> true value 28*100/270 = 10.37%, which
    // rounds DOWN to 10, same as floor would give -- confirms this weighs
    // the actual fractional part rather than just always adding one.
    CC_CHECK_EQ(RoomDemandPercent(1500, 2058, 2000), 10);
}
