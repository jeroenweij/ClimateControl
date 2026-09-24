/*************************************************************
 * Created by J. Weij
 *************************************************************/

// The application's NINA bring-up and link supervision, driven end to end
// against a scripted module (test/fake/ScriptedNina.h) over the same Hal::Uart
// fake the bus tests use. The bus itself is not run here: NodeMaster is only
// constructed (UplinkHandler needs a reference), never Init()'d, so FakeBus
// carries nothing but the NINA UART.

#include "BoardPins.h"
#include "Gpio.h"
#include "NodeMaster.h"

#include "FakeBus.h"
#include "FakeClock.h"
#include "FakeConfigStore.h"
#include "ScriptedNina.h"
#include "Test.h"

#include "BudgetAllocator.h"
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
    struct AppHarness
    {
        AppHarness() :
            master(),
            allocator(master),
            uplink(master, allocator)
        {
        }

        void Start()
        {
            FakeBus::Reset();
            FakeClock::Reset();
            FakeConfig::Reset();
            nina.Reset();
            resetLow = false;
            Board::NinaReset.port->ODR |= Board::NinaReset.pin; // released, as after power-up
            uplink.Init(); // pulses the module's reset
        }

        void Step(const uint32_t ms)
        {
            FakeClock::Advance(ms);
            uplink.Loop();

            const bool low = (Board::NinaReset.port->ODR & Board::NinaReset.pin) == 0;
            if (low != resetLow)
            {
                resetLow = low;
                nina.OnResetLine(low, FakeClock::Now());
            }

            if (FakeBus::TxLen() > 0)
            {
                nina.OnMcuBytes(FakeBus::Tx(), FakeBus::TxLen(), FakeClock::Now());
                FakeBus::TruncateTx(0);
            }
            nina.Tick(FakeClock::Now());

            uint8_t      buf[512];
            const size_t n = nina.TakeOutput(buf, sizeof(buf));
            if (n > 0)
            {
                FakeBus::InjectRx(buf, n);
            }
        }

        bool InDataMode() const
        {
            return UplinkHandlerTestAccess::InDataMode(uplink);
        }

        NodeLib::NodeMaster master;
        BudgetAllocator     allocator;
        UplinkHandler       uplink;
        ScriptedNina        nina;
        bool                resetLow = false;
    };
} // namespace

#include "NinaBringUpScenarios.h"

CC_NINA_SCENARIOS(AppHarness)
