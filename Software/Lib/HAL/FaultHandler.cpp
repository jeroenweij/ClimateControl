/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Backup.h"
#include "System.h"
#include "Tick.h"

#include "Fault.h"

namespace
{
    // RCC_CSR[31:24] as returned by Hal::System::ResetCause().
    constexpr uint8_t IwdgResetFlag = static_cast<uint8_t>(RCC_CSR_IWDGRSTF >> 24U);
} // namespace

// Called from HardFault_Handler with the exception frame the core stacked:
// r0 r1 r2 r3 r12 lr pc xpsr. The frame pointer is range-checked first -- a
// stack overflow can be the fault itself, and a second fault in here would
// lock the core up instead of resetting it.
extern "C" __attribute__((used, noinline)) void HardFaultCapture(const uint32_t* const frame)
{
    const uint32_t address = reinterpret_cast<uint32_t>(frame);
    uint32_t       pc      = FLASH_BASE;
    if (address >= SRAM_BASE && address + 8U * sizeof(uint32_t) <= SRAM_BASE + SRAM_SIZE_MAX)
    {
        pc = frame[6];
    }

    // 64 KB of flash -- the offset from FLASH_BASE fits the 16-bit context.
    Hal::Fault::Save(Hal::Fault::Code::HardFault, Hal::Tick::Millis(), static_cast<uint16_t>(pc - FLASH_BASE));
    Hal::System::Reset();
}

// Overrides the startup file's weak alias to its endless Default_Handler loop.
// EXC_RETURN bit 2 (in lr) says which stack the frame went to; Cortex-M0+
// Thumb has no ite/tst-immediate, hence the branches.
extern "C" __attribute__((naked)) void HardFault_Handler()
{
    __asm volatile(
        "movs r0, #4          \n"
        "mov  r1, lr          \n"
        "tst  r0, r1          \n"
        "bne  1f              \n"
        "mrs  r0, msp         \n"
        "b    2f              \n"
        "1:                   \n"
        "mrs  r0, psp         \n"
        "2:                   \n"
        "bl   HardFaultCapture\n");
}

void Hal::Fault::Init()
{
    if ((Hal::System::ResetCause() & IwdgResetFlag) != 0U)
    {
        Save(Code::Watchdog, Hal::Backup::Read(Hal::Backup::Reg::AliveUptime), 0);
    }
}
