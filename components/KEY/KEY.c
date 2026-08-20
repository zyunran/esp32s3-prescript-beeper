/* KEY 组件: 按键 GPIO 初始化.
 * 上(4)/确认(5)/下(6) 三键, 输入+内部上拉(按下=低电平);
 * 按键的轮询/长按/连发逻辑在 main.c input_task, 本组件只负责引脚初始化. */
#include "KEY.h"
#include "driver/gpio.h"

void KEY_Init(void)
{
    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4)|(1ULL << GPIO_NUM_5)|(1ULL << GPIO_NUM_6),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&gpio_conf);
}
