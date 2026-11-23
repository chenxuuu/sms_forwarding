/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 *                    ESP32-C3 Linker Script Memory Layout
 * This file describes the memory layout (memory blocks) by virtual memory addresses.
 * This linker script is passed through the C preprocessor to include configuration options.
 * Please use preprocessor features sparingly!
 * Restrict to simple macros with numeric values, and/or #if/#endif blocks.
 */
/*

 * Automatically generated file. DO NOT EDIT.

 * Espressif IoT Development Framework (ESP-IDF) 6.1.1 Configuration Header

 */
       
/* List of deprecated options */
/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* CPU instruction prefetch padding size for flash mmap scenario */
/*
 * PMP region granularity size
 * Software may determine the PMP granularity by writing zero to pmp0cfg, then writing all ones
 * to pmpaddr0, then reading back pmpaddr0. If G is the index of the least-significant bit set,
 * the PMP granularity is 2^G+2 bytes.
 */
/* CPU instruction prefetch padding size for memory protection scenario */
/* Memory alignment size for PMS */
    /* rtc timer data (s_rtc_timer_retain_mem, see esp_clk.c files). For rtc_timer_data_in_rtc_mem section. */
/*
 * APP_BUILD_MINIMIZE_BINARY_CHANGES_PADDING
 *
 * Aligns the current location (.) to CONFIG_APP_BUILD_MINIMIZE_BINARY_CHANGES_PADDING.
 * If more than half of the alignment block is already consumed,
 * extra padding is applied to skip to the next boundary.
 * For example, if the current location (.) is 0x1900 and the padding is 4 KB,
 * the location will be moved to 0x3000 instead of 0x2000 because less than
 * half of the current 4 KB block remains (0x2000 - 0x1900 < 2 KB).
 */
/**
 * physical memory is mapped twice to the virtual address (IRAM and DRAM).
 * `I_D_SRAM_OFFSET` is the offset between the two locations of the same physical memory
 */
_iram_dram_shared = 1;
MEMORY
{
  /**
   *  All these values assume the flash cache is on, and have the blocks this uses subtracted from the length
   *  of the various regions. The 'data access port' dram/drom regions map to the same iram/irom regions but
   *  are connected to the data port of the CPU and eg allow byte-wise access.
   */
  /* IRAM for PRO CPU. */
  iram0_0_seg (RX) : org = (0x4037C000 + 0x4000), len = 0x403CE710 - (0x4037C000 - 0x3FC7C000) - (0x3FC7C000 + 0x4000)
  /* Flash mapped instruction data */
  iram0_2_seg (RX) : org = 0x42000020, len = 0x800000-0x20
  /**
   * (0x20 offset above is a convenience for the app binary image generation.
   * Flash cache has 64KB pages. The .bin file which is flashed to the chip
   * has a 0x18 byte file header, and each segment has a 0x08 byte segment
   * header. Setting this offset makes it simple to meet the flash cache MMU's
   * constraint that (paddr % 64KB == vaddr % 64KB).)
   */
  /**
   * Shared data RAM, excluding memory reserved for ROM bss/data/stack.
   * Enabling Bluetooth & Trace Memory features in menuconfig will decrease the amount of RAM available.
   */
  dram0_0_seg (RW) : org = (0x3FC7C000 + 0x4000), len = 0x403CE710 - (0x4037C000 - 0x3FC7C000) - (0x3FC7C000 + 0x4000)
  /* Flash mapped constant data */
  drom0_0_seg (R) : org = 0x3C000020, len = 0x800000-0x20
  /* (See iram0_2_seg for meaning of 0x20 offset in the above.) */
/**
 *  RTC Memory (LP RAM) Layout for ESP32-C3
 *  Total Size: 8KB (0x2000)
 *  Address Range: 0x50000000 - 0x50002000
 *
 *  ESP32-C3 has only unified RTC FAST memory (no separate RTC SLOW).
 *  The LP RAM persists over deep sleep and is used for:
 *  - RTC wake stub code and data
 *  - Data that needs to survive deep sleep
 *  - Bootloader retain memory (for reboot counter, OTA info, etc.)
 *
 *  +-----------------------------+ 0x50002000 (RTC memory end)
 *  | bootloader_data_rtc_mem     |
 *  | (ESP_BOOTLOADER_RESERVE_RTC)|
 *  |- - - - - - - - - - - - - - -| rtc_reserved_seg (RESERVE_RTC_MEM)
 *  | rtc_timer_data_in_rtc_mem   |
 *  | (RTC_TIMER_RESERVE_RTC=24B) |
 *  +-----------------------------+ 0x50002000 - RESERVE_RTC_MEM
 *  | .rtc.force_slow             |
 *  | .rtc_noinit (NOLOAD)        |
 *  | .rtc.bss (NOLOAD)           |
 *  | .rtc.data                   | rtc_iram_seg (= rtc_data_seg = rtc_slow_seg)
 *  | .rtc.force_fast             |
 *  |- - - - - - - - - - - - - - -|
 *  | .rtc.text                   | RTC wake stub code
 *  +-----------------------------+ 0x50000000 (RTC memory start)
 *
 *  RESERVE_RTC_MEM = RTC_TIMER_RESERVE_RTC (24B) + ESP_BOOTLOADER_RESERVE_RTC
 */
  /**
   * RTC fast memory (executable). Persists over deep sleep.
   */
  rtc_iram_seg(RWX) : org = 0x50000000, len = 0x2000 - (0 + (24) + 0)
  /* We reduced the size of rtc_iram_seg by RESERVE_RTC_MEM value.
     It reserves the amount of RTC fast memory that we use for this memory segment.
     This segment is intended for keeping:
       - (lower addr) rtc timer data (s_rtc_timer_retain_mem, see esp_clk.c files).
       - (higher addr) bootloader rtc data (s_bootloader_retain_mem, when a Kconfig option is on).
     The aim of this is to keep data that will not be moved around and have a fixed address.
  */
  rtc_reserved_seg(RW) : org = 0x50000000 + 0x2000 - (0 + (24) + 0), len = (0 + (24) + 0)
}
/* Heap ends at top of dram0_0_seg */
_heap_end = 0x40000000;
/**
 *  The lines below define location alias for .rtc.data section
 *  As C3 only has RTC fast memory, this is not configurable like on other targets
 */
REGION_ALIAS("rtc_text_seg", rtc_iram_seg);
REGION_ALIAS("rtc_data_seg", rtc_iram_seg );
REGION_ALIAS("rtc_slow_seg", rtc_iram_seg );
REGION_ALIAS("rtc_force_fast_seg", rtc_iram_seg);
REGION_ALIAS("rtc_force_slow_seg", rtc_iram_seg);
REGION_ALIAS("iram_text_seg", iram0_0_seg);
REGION_ALIAS("dram_seg", dram0_0_seg);
  REGION_ALIAS("flash_text_seg", iram0_2_seg);
  REGION_ALIAS("flash_rodata_seg", drom0_0_seg);
ALIGN_VECTOR_TABLE = 0x100;
