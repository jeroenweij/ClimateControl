/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Tick.h"

#include "Adc.h"

using Hal::Adc;

namespace
{
    // ADC voltage regulator startup time (t_ADCVREG_SETUP), DS12992 Table 57 --
    // max 20 us. Must elapse before calibration or ADEN.
    constexpr uint32_t RegulatorStartupUs = 20;

    // Sample time applied to every channel (ADC_SMPR's SMP1 field) -- 19.5 ADC
    // clocks. The INA180 current-sense output driving this pin is a low-
    // impedance op-amp output, so even the fastest (1.5-cycle) setting would
    // be fine; this is margin, not a requirement, and costs nothing given
    // this is an occasional poll, not a high-rate sample loop.
    constexpr uint32_t SampleTimeSelect = ADC_SMPR_SMP1_2; // 0b100 -> 19.5 cycles
} // namespace

Adc::Adc(const uint8_t channel) :
    channel(channel)
{
    __HAL_RCC_ADC_CLK_ENABLE();

    // Synchronous clock, PCLK/2 -- avoids configuring RCC's separate
    // asynchronous-ADC-clock mux for a peripheral this project only ever
    // polls occasionally; comfortably inside the ADC's own max-clock range
    // (DS12992 Table 57) for any reasonable PCLK on this project's
    // HSI16-derived clock tree (RM0444 Sec15.3.5).
    ADC1->CFGR2 = ADC_CFGR2_CKMODE_0;

    // RM0444 Sec15.3.2: enable the regulator and wait before calibrating.
    ADC1->CR |= ADC_CR_ADVREGEN;
    Hal::Tick::DelayUs(RegulatorStartupUs);

    // RM0444 Sec15.3.3 "Software calibration procedure" -- ADEN/DMAEN are
    // both still 0 here (this runs once, right after reset), satisfying the
    // procedure's prerequisite.
    ADC1->CR |= ADC_CR_ADCAL;
    while ((ADC1->CR & ADC_CR_ADCAL) != 0U)
    {
    }

    // RM0444 Sec15.3.4 "Follow this procedure to enable the ADC".
    ADC1->ISR = ADC_ISR_ADRDY; // rc_w1: clear any stale flag before enabling
    ADC1->CR |= ADC_CR_ADEN;
    while ((ADC1->ISR & ADC_ISR_ADRDY) == 0U)
    {
    }

    // Bind this instance's channel permanently (never reconfigured again, so
    // CCRDY only needs clearing -- not re-waited-on -- this one time).
    // RM0444 Sec15.12.3 note: ADSTART is silently ignored until CCRDY is set
    // after a CHSELR write.
    ADC1->CHSELR = 1UL << channel;
    ADC1->SMPR   = SampleTimeSelect;
    while ((ADC1->ISR & ADC_ISR_CCRDY) == 0U)
    {
    }
}

uint16_t Adc::Read()
{
    ADC1->CR |= ADC_CR_ADSTART;
    while ((ADC1->ISR & ADC_ISR_EOC) == 0U)
    {
    }
    return static_cast<uint16_t>(ADC1->DR); // reading DR also clears EOC
}
