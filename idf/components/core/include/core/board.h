/**
 * @file board.h
 * @brief ESP32-C3 SuperMini + ML307R 4G 模组的板级引脚定义。
 *
 * 所有与硬件相关的引脚编号集中在此，修改硬件只需改这里。
 */
#pragma once

/* ---- 4G 模组串口 ---- */
#define BOARD_MODEM_UART_NUM 1   /* 使用 UART1，UART0 保留给日志 */
#define BOARD_MODEM_TX_GPIO 3    /* ESP32 TX -> 模组 RX */
#define BOARD_MODEM_RX_GPIO 4    /* ESP32 RX <- 模组 TX */
#define BOARD_MODEM_BAUD 115200

/* ---- 模组电源控制（EN 引脚，低电平断电）---- */
#define BOARD_MODEM_EN_GPIO 5
#define BOARD_MODEM_EN_ON 1
#define BOARD_MODEM_EN_OFF 0

/* ---- 板载 LED（低电平点亮）---- */
#define BOARD_LED_GPIO 8
#define BOARD_LED_ON 0
#define BOARD_LED_OFF 1
