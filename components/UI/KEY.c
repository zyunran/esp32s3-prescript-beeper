/* KEY: 三键 GPIO 初始化(上5/OK4/下6,上拉)。轮询/长按在 main input_task。 */
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
