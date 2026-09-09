/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Flash.h"

namespace
{
    constexpr uint32_t Key1 = 0x45670123;
    constexpr uint32_t Key2 = 0xCDEF89AB;

    constexpr uint32_t BusyMask  = FLASH_SR_BSY1 | FLASH_SR_CFGBSY;
    constexpr uint32_t ErrorMask = FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR |
        FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR;

    void WaitIdle()
    {
        while (FLASH->SR & BusyMask)
        {
        }
    }

    void ClearStatus()
    {
        FLASH->SR = ErrorMask | FLASH_SR_EOP;
    }

    bool Finish()
    {
        WaitIdle();
        const uint32_t status = FLASH->SR;
        ClearStatus();
        return (status & ErrorMask) == 0;
    }
} // namespace

void Hal::Flash::Unlock()
{
    if (FLASH->CR & FLASH_CR_LOCK)
    {
        FLASH->KEYR = Key1;
        FLASH->KEYR = Key2;
    }
}

void Hal::Flash::Lock()
{
    FLASH->CR |= FLASH_CR_LOCK;
}

bool Hal::Flash::ErasePage(const uint32_t address)
{
    const uint32_t page = (address - FLASH_BASE) / PageSize;

    WaitIdle();
    ClearStatus();

    FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB) | FLASH_CR_PER | ((page << FLASH_CR_PNB_Pos) & FLASH_CR_PNB);
    FLASH->CR |= FLASH_CR_STRT;

    const bool ok = Finish();
    FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
    return ok;
}

bool Hal::Flash::Program(const uint32_t address, const uint8_t* const data, const size_t len)
{
    WaitIdle();
    ClearStatus();

    bool ok = true;
    FLASH->CR |= FLASH_CR_PG;

    for (size_t offset = 0; offset < len && ok; offset += 8)
    {
        uint32_t words[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
        auto*    bytes    = reinterpret_cast<uint8_t*>(words);
        for (size_t i = 0; i < 8 && (offset + i) < len; i++)
        {
            bytes[i] = data[offset + i];
        }

        volatile uint32_t* const dst = reinterpret_cast<volatile uint32_t*>(address + offset);
        dst[0]                       = words[0];
        __ISB();
        dst[1] = words[1];

        ok = Finish();
    }

    FLASH->CR &= ~FLASH_CR_PG;
    return ok;
}
