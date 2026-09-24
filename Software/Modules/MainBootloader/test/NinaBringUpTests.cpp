/*************************************************************
 * Created by J. Weij
 *************************************************************/

// MainBootloader's NINA bring-up and link supervision -- the same scenarios
// (test/scenarios/NinaBringUpScenarios.h) the application's driver runs,
// against a scripted module over the bootloader's own NinaUart (faked).

#include "BoardPins.h"
#include "Gpio.h"

#include "FakeClock.h"
#include "FakeFlash.h"
#include "FakeNinaUart.h"
#include "ScriptedNina.h"
#include "Test.h"

#include "Firmware.h"
#include "UplinkHandler.h"

struct UplinkHandlerTestAccess
{
    static bool InDataMode(const UplinkHandler& u)
    {
        return u.link.InDataMode();
    }
};

namespace
{
    struct BootHarness
    {
        BootHarness() :
            firmware(),
            uplink(firmware)
        {
        }

        void Start()
        {
            FakeClock::Reset();
            FakeFlash::Reset();
            nina.Reset();
            resetLow = false;
            Board::NinaReset.port->ODR |= Board::NinaReset.pin; // released, as after power-up
            firmware.Init();
            uplink.Init(); // pulses the module's reset (and resets the fake UART)
        }

        void Step(const uint32_t ms)
        {
            FakeClock::Advance(ms);
            firmware.Loop();
            uplink.Loop();

            const bool low = (Board::NinaReset.port->ODR & Board::NinaReset.pin) == 0;
            if (low != resetLow)
            {
                resetLow = low;
                nina.OnResetLine(low, FakeClock::Now());
            }

            if (FakeNinaUart::TxLen() > 0)
            {
                nina.OnMcuBytes(FakeNinaUart::Tx(), FakeNinaUart::TxLen(), FakeClock::Now());
                FakeNinaUart::TruncateTx(0);
            }
            nina.Tick(FakeClock::Now());

            uint8_t      buf[512];
            const size_t n = nina.TakeOutput(buf, sizeof(buf));
            if (n > 0)
            {
                FakeNinaUart::InjectRx(buf, n);
            }
        }

        bool InDataMode() const
        {
            return UplinkHandlerTestAccess::InDataMode(uplink);
        }

        Boot::Firmware firmware;
        UplinkHandler  uplink;
        ScriptedNina   nina;
        bool           resetLow = false;
    };
} // namespace

#include "NinaBringUpScenarios.h"

CC_NINA_SCENARIOS(BootHarness)

// The server sends a small window of OtaData chunks back to back. Every one
// must be answered -- the reply queue is only a few slots deep, so the driver
// has to send replies out as it goes rather than after the whole burst.
CC_TEST(BootUplink, EveryChunkOfABurstIsAnswered)
{
    using scenario::Endpoint;
    using scenario::Id;
    using scenario::Message;
    using scenario::Operation;

    BootHarness h;
    h.Start();
    CC_CHECK(scenario::RunUntilDataMode(h, 10000));

    Message seen[32];
    for (int i = 0; i < 30; i++) // let the hello through
    {
        h.Step(scenario::StepMs);
        scenario::ServerPump(h, seen, 32, true);
    }

    // Begin: op(1) size(4) crc32(4) fwVersion(2).
    Message begin(Id(0, Endpoint::OtaControl, Operation::Set));
    begin.len     = 11;
    begin.data[0] = 1;
    begin.data[1] = 0; // 256 bytes: past the descriptor floor, 8 chunks
    begin.data[2] = 1;
    uint8_t      wire[64];
    const size_t beginLen = uplinkframes::Encode(begin, wire);
    h.nina.ServerSend(wire, beginLen);
    for (int i = 0; i < 100; i++)
    {
        h.Step(scenario::StepMs);
        scenario::ServerPump(h, seen, 32, true);
    }

    // Eight chunks in one go.
    uint8_t burst[8 * 64];
    size_t  burstLen = 0;
    for (uint16_t c = 0; c < 8; c++)
    {
        Message m(Id(0, Endpoint::OtaData, Operation::Set));
        m.len     = 34;
        m.data[0] = static_cast<uint8_t>((c * 32) & 0xFF);
        m.data[1] = static_cast<uint8_t>((c * 32) >> 8);
        for (int i = 0; i < 32; i++)
        {
            m.data[2 + i] = static_cast<uint8_t>(c * 5 + i);
        }
        burstLen += uplinkframes::Encode(m, &burst[burstLen]);
    }
    h.nina.ServerSend(burst, burstLen);

    int acks = 0;
    for (int i = 0; i < 100; i++)
    {
        h.Step(scenario::StepMs);
        const int n = scenario::ServerPump(h, seen, 32, true);
        for (int k = 0; k < n; k++)
        {
            if (seen[k].id.endpoint == Endpoint::OtaData && seen[k].id.operation == Operation::Ack)
            {
                acks++;
            }
        }
    }
    CC_CHECK_EQ(acks, 8);
}
