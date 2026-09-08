/*************************************************************
* Created by J. Weij
*
* Minimal GCC startup for STM32G031xx (Cortex-M0+). Vector table
* (16 core + 30 device IRQs, gaps at 8 and 20), Reset_Handler that
* inits .data/.bss then calls SystemInit, __libc_init_array, main.
* Shared verbatim by every Modules/* image; VTOR is relocated at
* runtime (Hal::System::SetVectorTable) so the app can sit above the
* bootloader -- see Spec/Node-Flash-Layout-and-Bootloader-Spec.md.
*************************************************************/

  .syntax unified
  .cpu cortex-m0plus
  .fpu softvfp
  .thumb

.global g_pfnVectors
.global Default_Handler

.word _sidata
.word _sdata
.word _edata
.word _sbss
.word _ebss

  .section .text.Reset_Handler
  .weak Reset_Handler
  .type Reset_Handler, %function
Reset_Handler:
  ldr   r0, =_estack
  mov   sp, r0

  /* copy .data initialisers from flash to RAM */
  movs  r1, #0
  b     LoopCopyDataInit
CopyDataInit:
  ldr   r3, =_sidata
  ldr   r3, [r3, r1]
  str   r3, [r0, r1]
  adds  r1, r1, #4
LoopCopyDataInit:
  ldr   r0, =_sdata
  ldr   r3, =_edata
  adds  r2, r0, r1
  cmp   r2, r3
  bcc   CopyDataInit

  /* zero .bss */
  ldr   r2, =_sbss
  ldr   r4, =_ebss
  movs  r3, #0
  b     LoopFillZerobss
FillZerobss:
  str   r3, [r2]
  adds  r2, r2, #4
LoopFillZerobss:
  cmp   r2, r4
  bcc   FillZerobss

  bl    SystemInit
  bl    __libc_init_array
  bl    main
LoopForever:
  b     LoopForever
  .size Reset_Handler, .-Reset_Handler

  .section .text.Default_Handler,"ax",%progbits
  .type Default_Handler, %function
Default_Handler:
Infinite_Loop:
  b     Infinite_Loop
  .size Default_Handler, .-Default_Handler

  .section .isr_vector,"a",%progbits
  .type g_pfnVectors, %object
g_pfnVectors:
  .word _estack
  .word Reset_Handler
  .word NMI_Handler
  .word HardFault_Handler
  .word 0
  .word 0
  .word 0
  .word 0
  .word 0
  .word 0
  .word 0
  .word SVC_Handler
  .word 0
  .word 0
  .word PendSV_Handler
  .word SysTick_Handler

  .word WWDG_IRQHandler                    /*  0 */
  .word PVD_IRQHandler                     /*  1 */
  .word RTC_TAMP_IRQHandler                /*  2 */
  .word FLASH_IRQHandler                   /*  3 */
  .word RCC_IRQHandler                     /*  4 */
  .word EXTI0_1_IRQHandler                 /*  5 */
  .word EXTI2_3_IRQHandler                 /*  6 */
  .word EXTI4_15_IRQHandler                /*  7 */
  .word 0                                  /*  8 */
  .word DMA1_Channel1_IRQHandler           /*  9 */
  .word DMA1_Channel2_3_IRQHandler         /* 10 */
  .word DMA1_Ch4_5_DMAMUX1_OVR_IRQHandler  /* 11 */
  .word ADC1_IRQHandler                    /* 12 */
  .word TIM1_BRK_UP_TRG_COM_IRQHandler     /* 13 */
  .word TIM1_CC_IRQHandler                 /* 14 */
  .word TIM2_IRQHandler                    /* 15 */
  .word TIM3_IRQHandler                    /* 16 */
  .word LPTIM1_IRQHandler                  /* 17 */
  .word LPTIM2_IRQHandler                  /* 18 */
  .word TIM14_IRQHandler                   /* 19 */
  .word 0                                  /* 20 */
  .word TIM16_IRQHandler                   /* 21 */
  .word TIM17_IRQHandler                   /* 22 */
  .word I2C1_IRQHandler                    /* 23 */
  .word I2C2_IRQHandler                    /* 24 */
  .word SPI1_IRQHandler                    /* 25 */
  .word SPI2_IRQHandler                    /* 26 */
  .word USART1_IRQHandler                  /* 27 */
  .word USART2_IRQHandler                  /* 28 */
  .word LPUART1_IRQHandler                 /* 29 */
  .size g_pfnVectors, .-g_pfnVectors

  .macro weak_handler name
  .weak \name
  .thumb_set \name, Default_Handler
  .endm

  weak_handler NMI_Handler
  weak_handler HardFault_Handler
  weak_handler SVC_Handler
  weak_handler PendSV_Handler
  weak_handler SysTick_Handler
  weak_handler WWDG_IRQHandler
  weak_handler PVD_IRQHandler
  weak_handler RTC_TAMP_IRQHandler
  weak_handler FLASH_IRQHandler
  weak_handler RCC_IRQHandler
  weak_handler EXTI0_1_IRQHandler
  weak_handler EXTI2_3_IRQHandler
  weak_handler EXTI4_15_IRQHandler
  weak_handler DMA1_Channel1_IRQHandler
  weak_handler DMA1_Channel2_3_IRQHandler
  weak_handler DMA1_Ch4_5_DMAMUX1_OVR_IRQHandler
  weak_handler ADC1_IRQHandler
  weak_handler TIM1_BRK_UP_TRG_COM_IRQHandler
  weak_handler TIM1_CC_IRQHandler
  weak_handler TIM2_IRQHandler
  weak_handler TIM3_IRQHandler
  weak_handler LPTIM1_IRQHandler
  weak_handler LPTIM2_IRQHandler
  weak_handler TIM14_IRQHandler
  weak_handler TIM16_IRQHandler
  weak_handler TIM17_IRQHandler
  weak_handler I2C1_IRQHandler
  weak_handler I2C2_IRQHandler
  weak_handler SPI1_IRQHandler
  weak_handler SPI2_IRQHandler
  weak_handler USART1_IRQHandler
  weak_handler USART2_IRQHandler
  weak_handler LPUART1_IRQHandler
