/*************************************************************
 * Host test double for the STM32Cube HAL umbrella header.
 *
 * Only what Lib/HAL/Pin.h, Lib/Board/BoardPins.h and Lib/HAL/OneWire.cpp
 * actually reference: a GPIO_TypeDef register block, the GPIO_PIN_x masks,
 * the GPIOx port pointers, and the four CMSIS PRIMASK intrinsics.
 *
 * Excluded from clang-format via the Makefile's "-not -name 'stm32*'" rule.
 *************************************************************/

#pragma once

#include <stdint.h>

typedef struct
{
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
    volatile uint32_t BRR;
} GPIO_TypeDef;

/* Defined in Software/test/fake/FakeGpio.cpp. */
extern GPIO_TypeDef* const GPIOA;
extern GPIO_TypeDef* const GPIOB;
extern GPIO_TypeDef* const GPIOC;
extern GPIO_TypeDef* const GPIOD;
extern GPIO_TypeDef* const GPIOF;

#define GPIO_PIN_0  ((uint16_t)0x0001)
#define GPIO_PIN_1  ((uint16_t)0x0002)
#define GPIO_PIN_2  ((uint16_t)0x0004)
#define GPIO_PIN_3  ((uint16_t)0x0008)
#define GPIO_PIN_4  ((uint16_t)0x0010)
#define GPIO_PIN_5  ((uint16_t)0x0020)
#define GPIO_PIN_6  ((uint16_t)0x0040)
#define GPIO_PIN_7  ((uint16_t)0x0080)
#define GPIO_PIN_8  ((uint16_t)0x0100)
#define GPIO_PIN_9  ((uint16_t)0x0200)
#define GPIO_PIN_10 ((uint16_t)0x0400)
#define GPIO_PIN_11 ((uint16_t)0x0800)
#define GPIO_PIN_12 ((uint16_t)0x1000)
#define GPIO_PIN_13 ((uint16_t)0x2000)
#define GPIO_PIN_14 ((uint16_t)0x4000)
#define GPIO_PIN_15 ((uint16_t)0x8000)

#ifdef __cplusplus
extern "C"
{
#endif

    static inline uint32_t __get_PRIMASK(void)
    {
        return 0u;
    }

    static inline void __set_PRIMASK(uint32_t primask)
    {
        (void)primask;
    }

    static inline void __disable_irq(void)
    {
    }

    static inline void __enable_irq(void)
    {
    }

#ifdef __cplusplus
}
#endif
