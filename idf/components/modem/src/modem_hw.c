#include "modem/modem_hw.h"

#include "core/board.h"
#include "core/log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void modem_hw_init(void)
{
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << BOARD_MODEM_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&en);

    gpio_config_t led = {
        .pin_bit_mask = 1ULL << BOARD_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led);
    led_set(false);
}

void modem_power_off(void)
{
    gpio_set_level(BOARD_MODEM_EN_GPIO, BOARD_MODEM_EN_OFF);
}

void modem_power_on(void)
{
    gpio_set_level(BOARD_MODEM_EN_GPIO, BOARD_MODEM_EN_ON);
}

void modem_power_cycle(void)
{
    log_line("EN 拉低：关闭模组");
    modem_power_off();
    vTaskDelay(pdMS_TO_TICKS(1200));
    log_line("EN 拉高：开启模组，等待启动完成");
    modem_power_on();
    vTaskDelay(pdMS_TO_TICKS(6000));
}

void led_set(bool on)
{
    gpio_set_level(BOARD_LED_GPIO, on ? BOARD_LED_ON : BOARD_LED_OFF);
}

void led_blink(uint32_t gap_ms)
{
    led_set(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    led_set(false);
    vTaskDelay(pdMS_TO_TICKS(gap_ms));
}
