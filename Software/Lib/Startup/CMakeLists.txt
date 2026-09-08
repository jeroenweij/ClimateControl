# Shared Cortex-M0+ / STM32G031 startup object. Linked into every Modules/*
# image. Object library so the vector table is always pulled in (the linker
# script KEEPs .isr_vector) and it is assembled exactly once.
add_library(Startup OBJECT
    startup_stm32g031xx.s
    syscalls.c
)
