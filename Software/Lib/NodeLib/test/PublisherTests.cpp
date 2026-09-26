/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "EEndpoint.h"
#include "EOperation.h"
#include "Publisher.h"

#include "FakeClock.h"
#include "Test.h"

using NodeLib::Endpoint;
using NodeLib::Message;
using NodeLib::Operation;
using NodeLib::Publisher;

namespace
{
    const uint8_t kNodeId = 4;

    // Drains everything due right now; returns how many Reports, the last in 'last'.
    int Drain(Publisher& p, const bool connected, Message* const last = nullptr)
    {
        int     n = 0;
        Message m;
        while (p.Next(connected, kNodeId, m))
        {
            n++;
            if (last)
            {
                *last = m;
            }
        }
        return n;
    }

    int16_t Value16(const Message& m)
    {
        return static_cast<int16_t>(m.data[0] | (m.data[1] << 8));
    }
} // namespace

CC_TEST(Publisher, ReportsAValueOnceItIsKnownThenOnlyOnChange)
{
    FakeClock::Reset();
    Publisher p;
    p.Add(Endpoint::DamperActual, 1, 1);

    CC_CHECK_EQ(Drain(p, true), 0); // not known yet

    p.Update(Endpoint::DamperActual, 40);
    Message m;
    CC_CHECK_EQ(Drain(p, true, &m), 1);
    CC_CHECK_EQ(m.id.node, kNodeId);
    CC_CHECK(m.id.endpoint == Endpoint::DamperActual);
    CC_CHECK(m.id.operation == Operation::Report);
    CC_CHECK_EQ(m.len, 1);
    CC_CHECK_EQ(m.data[0], 40);

    p.Update(Endpoint::DamperActual, 40);
    CC_CHECK_EQ(Drain(p, true), 0); // unchanged

    p.Update(Endpoint::DamperActual, 41);
    CC_CHECK_EQ(Drain(p, true, &m), 1);
    CC_CHECK_EQ(m.data[0], 41);
}

CC_TEST(Publisher, SmallChangesInsideMinChangeWaitForTheKeepalive)
{
    FakeClock::Reset();
    Publisher p;
    p.Add(Endpoint::RoomTemp, 2, NodeLib::TempMinChange);
    p.Update(Endpoint::RoomTemp, 2100);
    Drain(p, true);

    p.Update(Endpoint::RoomTemp, 2109); // 0.09 degC -- below the 0.1 degC step
    CC_CHECK_EQ(Drain(p, true), 0);

    p.Update(Endpoint::RoomTemp, 2110); // measured from what was reported, not the last Update
    Message m;
    CC_CHECK_EQ(Drain(p, true, &m), 1);
    CC_CHECK_EQ(m.len, 2);
    CC_CHECK_EQ(Value16(m), 2110);

    p.Update(Endpoint::RoomTemp, 2095); // and downward
    CC_CHECK_EQ(Drain(p, true, &m), 1);
    CC_CHECK_EQ(Value16(m), 2095);
}

CC_TEST(Publisher, NegativeValuesEncodeAsTwosComplement)
{
    FakeClock::Reset();
    Publisher p;
    p.Add(Endpoint::SupplyTemp, 2, NodeLib::TempMinChange);
    p.Update(Endpoint::SupplyTemp, -550); // -5.5 degC
    Message m;
    CC_CHECK_EQ(Drain(p, true, &m), 1);
    CC_CHECK_EQ(Value16(m), -550);
}

CC_TEST(Publisher, KeepaliveResendsEachValueStaggeredOncePerInterval)
{
    FakeClock::Reset();
    Publisher p;
    p.Add(Endpoint::DamperTarget, 1, 1);
    p.Add(Endpoint::DamperActual, 1, 1);
    p.Update(Endpoint::DamperTarget, 10);
    p.Update(Endpoint::DamperActual, 20);
    CC_CHECK_EQ(Drain(p, true), 2);

    // Two entries -> one keepalive every 30 s, alternating.
    FakeClock::Advance(Publisher::keepaliveMs / 2 + 1);
    Message m;
    CC_CHECK_EQ(Drain(p, true, &m), 1);
    CC_CHECK(m.id.endpoint == Endpoint::DamperTarget);

    FakeClock::Advance(Publisher::keepaliveMs / 2 + 1);
    CC_CHECK_EQ(Drain(p, true, &m), 1);
    CC_CHECK(m.id.endpoint == Endpoint::DamperActual);

    FakeClock::Advance(1000);
    CC_CHECK_EQ(Drain(p, true), 0); // nothing in between
}

CC_TEST(Publisher, NothingGoesOutWithoutTheMasterAndEverythingAgainWhenItReturns)
{
    FakeClock::Reset();
    Publisher p;
    p.Add(Endpoint::DamperTarget, 1, 1);
    p.Add(Endpoint::DamperActual, 1, 1);
    p.Update(Endpoint::DamperTarget, 10);
    p.Update(Endpoint::DamperActual, 20);

    CC_CHECK_EQ(Drain(p, false), 0); // not polled yet
    CC_CHECK_EQ(Drain(p, true), 2); // first poll: both

    CC_CHECK_EQ(Drain(p, false), 0); // master lost
    p.Update(Endpoint::DamperActual, 25);
    FakeClock::Advance(Publisher::keepaliveMs * 2);
    CC_CHECK_EQ(Drain(p, false), 0); // no keepalives into the void either

    CC_CHECK_EQ(Drain(p, true), 2); // back: the unchanged one too
}

CC_TEST(Publisher, AClearedValueIsNotReportedUntilItIsKnownAgain)
{
    FakeClock::Reset();
    Publisher p;
    p.Add(Endpoint::SupplyTemp, 2, NodeLib::TempMinChange);
    p.Update(Endpoint::SupplyTemp, 1500);
    Drain(p, true);

    p.Clear(Endpoint::SupplyTemp); // probe gone
    FakeClock::Advance(Publisher::keepaliveMs * 2);
    CC_CHECK_EQ(Drain(p, true), 0); // no stale keepalive

    p.Update(Endpoint::SupplyTemp, 1500); // back, same reading
    CC_CHECK_EQ(Drain(p, true), 1); // reported afresh
}

CC_TEST(Publisher, AddRejectsBadSizesDuplicatesAndAFullTable)
{
    Publisher p;
    CC_CHECK(!p.Add(Endpoint::DamperTarget, 3, 1));
    CC_CHECK(p.Add(Endpoint::DamperTarget, 1, 1));
    CC_CHECK(!p.Add(Endpoint::DamperTarget, 1, 1));

    Publisher full;
    for (uint8_t i = 0; i < Publisher::maxPublished; i++)
    {
        CC_CHECK(full.Add(static_cast<Endpoint>(0x30 + i), 1, 1));
    }
    CC_CHECK(!full.Add(Endpoint::RoomLink, 1, 1));
}
