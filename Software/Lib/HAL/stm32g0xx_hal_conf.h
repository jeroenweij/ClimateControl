/**
 ******************************************************************************
 * @file    stm32g0xx_hal_conf.h
 * @brief   HAL configuration file for ClimateControl.
 *
 *          Trimmed from STMicroelectronics' stm32g0xx_hal_conf_template.h
 *          (see Vendor/STM32G0xx_HAL_Driver/Inc/stm32g0xx_hal_conf_template.h)
 *          down to only the modules Software/Lib/HAL actually wraps. See
 *          Vendor/NOTICE.md for why PWR is enabled even though nothing in
 *          Hal calls it directly (stm32g0xx_hal_rcc_ex.c's LSCO clock-output
 *          function needs it unconditionally).
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2018 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#ifndef STM32G0xx_HAL_CONF_H
#define STM32G0xx_HAL_CONF_H

#ifdef __cplusplus
extern "C"
{
#endif

/* ########################## Module Selection ############################## */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_CRC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* ########################## Register Callbacks selection ############################## */
#define USE_HAL_UART_REGISTER_CALLBACKS 0u

/* ########################## Oscillator Values adaptation ####################*/
#if !defined(HSE_VALUE)
#define HSE_VALUE (8000000UL) /*!< Value of the External oscillator in Hz */
#endif /* HSE_VALUE */

#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT (100UL) /*!< Time out for HSE start up, in ms */
#endif /* HSE_STARTUP_TIMEOUT */

#if !defined(HSI_VALUE)
#define HSI_VALUE (16000000UL) /*!< Value of the Internal oscillator in Hz. Not configured to run \
                                     the clock tree at a higher frequency yet -- see              \
                                     Node-Bus-Hardware-Design-Spec.md, 64MHz max is a future option. */
#endif /* HSI_VALUE */

#if !defined(LSI_VALUE)
#define LSI_VALUE (32000UL) /*!< LSI Typical Value in Hz */
#endif /* LSI_VALUE */

#if !defined(LSE_VALUE)
#define LSE_VALUE (32768UL) /*!< Value of the External oscillator in Hz -- unused (no LSE on this board) */
#endif /* LSE_VALUE */

#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT (5000UL) /*!< Time out for LSE start up, in ms */
#endif /* LSE_STARTUP_TIMEOUT */

#if !defined(EXTERNAL_I2S1_CLOCK_VALUE)
/* stm32g0xx_hal_rcc_ex.c's HAL_RCCEx_GetPeriphCLKFreq() references this
   unconditionally even though I2S isn't an enabled module here -- unused at
   runtime (nothing configures an I2S clock source) but must be defined to
   compile, same pattern as the FLASH/DMA notes above. */
#define EXTERNAL_I2S1_CLOCK_VALUE (48000UL)
#endif /* EXTERNAL_I2S1_CLOCK_VALUE */

/* ########################### System Configuration ######################### */
#define VDD_VALUE (3300UL) /*!< Value of VDD in mv */
#define TICK_INT_PRIORITY ((1UL << __NVIC_PRIO_BITS) - 1UL)
#define USE_RTOS 0U
#define PREFETCH_ENABLE 1U
#define INSTRUCTION_CACHE_ENABLE 1U

/* ########################## Assert Selection ############################## */
/* #define USE_FULL_ASSERT    1U */

/* Includes ------------------------------------------------------------------*/
/* stm32g0xx_hal_uart.h's handle struct references DMA_HandleTypeDef* fields
   unconditionally (not itself guarded by HAL_DMA_MODULE_ENABLED) -- upstream
   assumes DMA is always available. We don't use DMA transfers here, so the
   module isn't "enabled" above, but the type still needs to exist. */
#include "stm32g0xx_hal_dma.h"

/* stm32g0xx_hal_rcc.c's clock-config path unconditionally calls the
   __HAL_FLASH_*_LATENCY macros (flash wait-states must track clock speed even
   though this project stays on default HSI) -- same "needed regardless of
   module-enable" situation as DMA above. */
#include "stm32g0xx_hal_flash.h"

#ifdef HAL_RCC_MODULE_ENABLED
#include "stm32g0xx_hal_rcc.h"
#endif /* HAL_RCC_MODULE_ENABLED */

#ifdef HAL_GPIO_MODULE_ENABLED
#include "stm32g0xx_hal_gpio.h"
#endif /* HAL_GPIO_MODULE_ENABLED */

#ifdef HAL_CORTEX_MODULE_ENABLED
#include "stm32g0xx_hal_cortex.h"
#endif /* HAL_CORTEX_MODULE_ENABLED */

#ifdef HAL_CRC_MODULE_ENABLED
#include "stm32g0xx_hal_crc.h"
#endif /* HAL_CRC_MODULE_ENABLED */

#ifdef HAL_PWR_MODULE_ENABLED
#include "stm32g0xx_hal_pwr.h"
#endif /* HAL_PWR_MODULE_ENABLED */

#ifdef HAL_UART_MODULE_ENABLED
#include "stm32g0xx_hal_uart.h"
#endif /* HAL_UART_MODULE_ENABLED */

/* Exported macro ------------------------------------------------------------*/
#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t*)__FILE__, __LINE__))
    void assert_failed(uint8_t* file, uint32_t line);
#else
#define assert_param(expr) ((void)0U)
#endif /* USE_FULL_ASSERT */

#ifdef __cplusplus
}
#endif

#endif /* STM32G0xx_HAL_CONF_H */
