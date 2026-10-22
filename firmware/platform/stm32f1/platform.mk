# STM32F103 (Cortex-M3) behind NI's DFU bootloader
CROSS   = arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

PLATFORM_CFLAGS = -mcpu=cortex-m3 -mthumb -DSTM32F103xB
PLATFORM_INCLUDES = -Iplatform/stm32f1/libc -Ilib/cmsis_device_f1/Include -Ilib/CMSIS_6/CMSIS/Core/Include
PLATFORM_SRCS = $(wildcard platform/stm32f1/*.c)
PLATFORM_TUSB_SRCS = $(TUSB)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c $(TUSB)/portable/st/stm32_fsdev/fsdev_common.c
