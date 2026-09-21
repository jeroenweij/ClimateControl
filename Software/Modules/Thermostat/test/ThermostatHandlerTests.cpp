/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "BoardPins.h"
#include "Node.h"

#include "Cht40.h"
#include "ThermostatHandler.h"

#include "BusHelpers.h"
#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "FakeI2c.h"
#include "Test.h"

using NodeLib::ConfigStore;
using NodeLib::Endpoint;
using NodeLib::Id;
using NodeLib::Message;
using NodeLib::Node;
using NodeLib::Operation;

namespace
{
    const uint8_t nodeId = 6;

    // Button GPIOs are active-low with an internal pull-up; the fake register
    // block resets to 0 (every pin reading as "pressed"), so every test must
    // explicitly mark both released before exercising anything else.
    void ReleaseBothButtons()
    {
        GPIOA->IDR |= Board::UserButton.pin | Board::UserButton2.pin;
    }

    void PressButton(const Hal::Pin& button, const bool pressed)
    {
        if (pressed)
        {
            GPIOA->IDR &= ~static_cast<uint32_t>(button.pin);
        }
        else
        {
            GPIOA->IDR |= button.pin;
        }
    }

    void ResetWorld()
    {
        FakeBus::Reset();
        FakeClock::Reset();
        FakeConfig::Reset();
        FakeConfig::SetValid(true);
        FakeConfig::SetNodeId(nodeId);
        FakeConfig::SetModule(ConfigStore::Module::Thermostat);
        FakeI2c::ResetAll();
        ReleaseBothButtons();
    }

    // Queues a well-formed CHT40 measurement so SampleRoom() succeeds.
    void QueueSensorReading(const int16_t centiDegC, const uint16_t centiRH)
    {
        const int32_t  rawTValue     = ((static_cast<int32_t>(centiDegC) + 4500) * 65535) / 17500;
        const int32_t  rawRHValue    = ((static_cast<int32_t>(centiRH) + 600) * 65535) / 12500;
        const uint16_t rawT          = static_cast<uint16_t>(rawTValue);
        const uint16_t rawRH         = static_cast<uint16_t>(rawRHValue);
        const uint8_t  rawTBytes[2]  = {static_cast<uint8_t>(rawT >> 8), static_cast<uint8_t>(rawT)};
        const uint8_t  rawRHBytes[2] = {static_cast<uint8_t>(rawRH >> 8), static_cast<uint8_t>(rawRH)};
        const uint8_t  frame[6]      = {rawTBytes[0],
                                        rawTBytes[1],
                                        Cht40::Crc8(rawTBytes, 2),
                                        rawRHBytes[0],
                                        rawRHBytes[1],
                                        Cht40::Crc8(rawRHBytes, 2)};
        FakeI2c::QueueRead(Cht40::DefaultAddress, frame, sizeof(frame));
    }

    // Bundles Node + ThermostatHandler wired the same way Modules/Thermostat/
    // main.cpp does.
    struct World
    {
        Node              node;
        ThermostatHandler handler;

        World() :
            node(Hal::Uart::Instance::Usart2, Board::LinkUart, 0),
            handler(node)
        {
            handler.Init();
            node.RegisterHandler(&handler);
            node.Init();
        }

        // A ThermostatHandler only queues -- it never writes the bus directly.
        // Real traffic goes out when the paired ControllerNode's LinkMaster
        // polls it (Node::HandleInternalMessage flushes on Operation::Poll);
        // simulate that one round trip.
        void FlushToPeer()
        {
            bus::InjectFrame(Message(nodeId, Operation::Poll));
            node.Loop();
        }
    };

    bool FindMessage(Message* const tx, const int n, const Endpoint endpoint, const Operation op, int* const outIndex)
    {
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == endpoint && tx[i].id.operation == op)
            {
                if (outIndex)
                {
                    *outIndex = i;
                }
                return true;
            }
        }
        return false;
    }

    int16_t ReadI16(const uint8_t* const p)
    {
        return static_cast<int16_t>(p[0] | (p[1] << 8));
    }

    Message MakeMessage(const Id& id, const uint8_t* const data, const uint8_t len)
    {
        Message m(id);
        for (uint8_t i = 0; i < len; i++)
        {
            m.data[i] = data[i];
        }
        m.len = len;
        return m;
    }

    CC_TEST(ThermostatHandler, InitPowersAndResetsTheOled)
    {
        ResetWorld();
        World w;

        CC_CHECK((GPIOA->ODR & Board::OledPowerEnable.pin) != 0); // VBAT left on
        CC_CHECK((GPIOA->ODR & Board::OledReset.pin) != 0); // RES# released (idle high) after the pulse
    }

    CC_TEST(ThermostatHandler, ButtonDownStepsSetpointDownHalfADegree)
    {
        ResetWorld();
        World w;

        PressButton(Board::UserButton, true);
        w.handler.Loop();
        w.FlushToPeer();

        Message   tx[8];
        const int n   = bus::DecodeTx(tx, 8);
        int       idx = -1;
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomSetpoint, Operation::Report, &idx));
        if (idx >= 0)
        {
            CC_CHECK_EQ(ReadI16(tx[idx].data), 2050); // 21.00 -> 20.50
        }
    }

    CC_TEST(ThermostatHandler, ButtonUpStepsSetpointUpHalfADegree)
    {
        ResetWorld();
        World w;

        PressButton(Board::UserButton2, true);
        w.handler.Loop();
        w.FlushToPeer();

        Message   tx[8];
        const int n   = bus::DecodeTx(tx, 8);
        int       idx = -1;
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomSetpoint, Operation::Report, &idx));
        if (idx >= 0)
        {
            CC_CHECK_EQ(ReadI16(tx[idx].data), 2150); // 21.00 -> 21.50
        }
    }

    CC_TEST(ThermostatHandler, SetpointClampsToTheComfortRange)
    {
        ResetWorld();
        World w;

        // Way more presses than needed to hit the floor from 21.00.
        for (int i = 0; i < 10; i++)
        {
            PressButton(Board::UserButton, true);
            w.handler.Loop();
            PressButton(Board::UserButton, false);
            w.handler.Loop();
        }
        w.FlushToPeer();

        // Every intermediate step gets its own Report (report-on-change), so
        // find the LAST RoomSetpoint Report, not just any one of them.
        Message   tx[16];
        const int n   = bus::DecodeTx(tx, 16);
        int       idx = -1;
        for (int i = 0; i < n; i++)
        {
            if (tx[i].id.endpoint == Endpoint::RoomSetpoint && tx[i].id.operation == Operation::Report)
            {
                idx = i;
            }
        }
        CC_CHECK(idx >= 0);
        if (idx >= 0)
        {
            CC_CHECK_EQ(ReadI16(tx[idx].data), 1900); // clamped at 19.00, not below
        }
    }

    CC_TEST(ThermostatHandler, HoldingAButtonStepsOnlyOnce)
    {
        ResetWorld();
        World w;

        PressButton(Board::UserButton2, true);
        w.handler.Loop();
        w.handler.Loop(); // still held -- must not step again
        w.FlushToPeer();

        Message   tx[8];
        const int n   = bus::DecodeTx(tx, 8);
        int       idx = -1;
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomSetpoint, Operation::Report, &idx));
        if (idx >= 0)
        {
            CC_CHECK_EQ(ReadI16(tx[idx].data), 2150); // one step, not two (21.00 -> 21.50 only)
        }
    }

    CC_TEST(ThermostatHandler, SampleRoomUpdatesFromTheSensorAndPublishesOnTheDeadband)
    {
        ResetWorld();
        World w;

        QueueSensorReading(2350, 5000); // outside the temp deadband from the 21.00 default
        FakeClock::Advance(3000); // past sampleIntervalMs (2000)
        w.handler.Loop();
        w.FlushToPeer();

        Message   tx[8];
        const int n   = bus::DecodeTx(tx, 8);
        int       idx = -1;
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomTemp, Operation::Report, &idx));
        if (idx >= 0)
        {
            // 2349, not 2350 -- QueueSensorReading()'s encode and Cht40::Measure()'s
            // decode each truncate independently, same as the real 16-bit ADC ticks
            // would; not an exact round trip.
            CC_CHECK_EQ(ReadI16(tx[idx].data), 2349);
        }
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomHumidity, Operation::Report, nullptr));
    }

    CC_TEST(ThermostatHandler, GetRoomTempForcesAReport)
    {
        ResetWorld();
        World w;
        FakeBus::Reset();

        bus::InjectFrame(Message(Id(nodeId, Endpoint::RoomTemp, Operation::Get)));
        w.FlushToPeer(); // processes the Get, then the injected Poll flushes the forced Report

        Message   tx[8];
        const int n = bus::DecodeTx(tx, 8);
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomTemp, Operation::Report, nullptr));
    }

    CC_TEST(ThermostatHandler, SetOnARoomTempEndpointIsRejected)
    {
        ResetWorld();
        World w;
        FakeBus::Reset();

        uint8_t value = 0;
        bus::InjectFrame(MakeMessage(Id(nodeId, Endpoint::RoomTemp, Operation::Set), &value, 1));
        w.FlushToPeer();

        Message   tx[8];
        const int n = bus::DecodeTx(tx, 8);
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomTemp, Operation::Nack, nullptr));
    }

    CC_TEST(ThermostatHandler, MasterSetpointOverrideIsAccepted)
    {
        ResetWorld();
        World w;

        uint8_t payload[2] = {0xD0, 0x07}; // 2000 = 20.00C, little-endian
        bus::InjectFrame(MakeMessage(Id(nodeId, Endpoint::RoomSetpoint, Operation::Set), payload, 2));
        w.node.Loop(); // applies the override -- no reply expected

        FakeBus::Reset();
        bus::InjectFrame(Message(Id(nodeId, Endpoint::RoomSetpoint, Operation::Get)));
        w.FlushToPeer();

        Message   tx[8];
        const int n   = bus::DecodeTx(tx, 8);
        int       idx = -1;
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomSetpoint, Operation::Report, &idx));
        if (idx >= 0)
        {
            CC_CHECK_EQ(ReadI16(tx[idx].data), 2000);
        }
    }

    CC_TEST(ThermostatHandler, CachesDamperActualAndModeFromTheControllerNode)
    {
        ResetWorld();
        World w;

        uint8_t actual = 42;
        w.handler.ReceivedMessage(MakeMessage(Id(nodeId, Endpoint::DamperActual, Operation::Set), &actual, 1));
        uint8_t mode = 3;
        w.handler.ReceivedMessage(MakeMessage(Id(nodeId, Endpoint::DamperMode, Operation::Set), &mode, 1));

        // No public getter -- a button press wakes the display, which then
        // renders (and Flush()es) using the cached values; absence of a crash
        // plus a non-trivial write to the OLED is the observable contract here.
        PressButton(Board::UserButton, true);
        w.handler.Loop();
        CC_CHECK(FakeI2c::WrittenLen(0x3C) > 0);
    }

    CC_TEST(ThermostatHandler, DisplayWakesOnPressAndStaysAsleepOtherwise)
    {
        ResetWorld();
        World w;

        FakeI2c::ClearWritten(0x3C);
        w.handler.Loop(); // no button activity -- panel stays off, nothing drawn
        CC_CHECK_EQ(FakeI2c::WrittenLen(0x3C), 0);

        PressButton(Board::UserButton, true);
        w.handler.Loop();
        CC_CHECK(FakeI2c::WrittenLen(0x3C) > 0); // On() + a rendered frame went out
    }

    CC_TEST(ThermostatHandler, ConnectionLostDoesNotCrashAndPreservesTheSetpoint)
    {
        ResetWorld();
        World w;

        w.handler.ConnectionLost();
        w.handler.Snoop(Message());

        PressButton(Board::UserButton2, true);
        w.handler.Loop();
        w.FlushToPeer();

        Message   tx[8];
        const int n   = bus::DecodeTx(tx, 8);
        int       idx = -1;
        CC_CHECK(FindMessage(tx, n, Endpoint::RoomSetpoint, Operation::Report, &idx));
        if (idx >= 0)
        {
            CC_CHECK_EQ(ReadI16(tx[idx].data), 2150); // still steps normally from 21.00
        }
    }
} // namespace
