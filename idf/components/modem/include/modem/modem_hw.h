/**
 * @file modem_hw.h
 * @brief 模组电源（EN 引脚）与板载 LED 控制。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void modem_hw_init(void);

/** 断电（EN 拉低）。 */
void modem_power_off(void);
/** 上电（EN 拉高）。 */
void modem_power_on(void);
/** 断电 1.2s 再上电，等待 6s 让模组完成启动。 */
void modem_power_cycle(void);

void led_set(bool on);
void led_blink(uint32_t gap_ms);
