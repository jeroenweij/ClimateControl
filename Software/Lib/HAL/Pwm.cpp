/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include "stm32g0xx_hal.h"

#include "Pwm.h"

using Hal::Pwm;

namespace
{
    constexpr uint32_t TickHz = 1000000; // 1 us resolution

    // RM0444 Sec5.4.18: a timer runs at PCLK when the APB prescaler is 1, and
    // at 2x PCLK otherwise.
    uint32_t TimerClockHz()
    {
        const uint32_t pclk = HAL_RCC_GetPCLK1Freq();
        return ((RCC->CFGR & RCC_CFGR_PPRE) == 0U) ? pclk : 2U * pclk;
    }

    void ConfigureAfPin(const Hal::Pin pin, const uint8_t alternateFunction)
    {
        GPIO_InitTypeDef init = {};
        init.Pin              = pin.pin;
        init.Mode             = GPIO_MODE_AF_PP;
        init.Pull             = GPIO_NOPULL;
        init.Speed            = GPIO_SPEED_FREQ_LOW; // 50 Hz servo signal -- slow edges are fine
        init.Alternate        = alternateFunction;

        HAL_GPIO_Init(pin.port, &init);
    }
} // namespace

Pwm::Pwm(const PwmPins& pins, const uint16_t periodUs) :
    pins(pins),
    periodUs(periodUs)
{
}

void Pwm::Init()
{
    __HAL_RCC_TIM3_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    TIM3->CR1  = 0; // counter stopped while configuring
    TIM3->PSC  = TimerClockHz() / TickHz - 1U;
    TIM3->ARR  = periodUs - 1U;
    TIM3->CCR1 = 0;

    // Channel 1: output, PWM mode 1 (high while CNT < CCR1), CCR1 preloaded so
    // a new pulse width only lands at an update event.
    TIM3->CCMR1 = TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1PE;
    TIM3->CCER  = TIM_CCER_CC1E; // active high

    TIM3->CR1 = TIM_CR1_ARPE;
    TIM3->EGR = TIM_EGR_UG; // load PSC/ARR/CCR1 from their preload registers now
    TIM3->CR1 |= TIM_CR1_CEN;

    // Only now hand the pin over, so it never sees an unconfigured timer.
    ConfigureAfPin(pins.out, pins.alternateFunction);
}

void Pwm::SetPulseUs(const uint16_t pulseUs)
{
    TIM3->CCR1 = pulseUs > periodUs ? periodUs : pulseUs;
}
