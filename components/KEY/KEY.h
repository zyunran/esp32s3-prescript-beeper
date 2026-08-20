#ifndef __KEY_H
#define __KEY_H

#include <stdint.h>

/* KEY 组件: 三颗按键 GPIO 初始化.
 * 上=GPIO4 / 确认=GPIO5 / 下=GPIO6(输入 + 内部上拉, 按下=低电平).
 * 按键的轮询/长按/连发逻辑在 main.c input_task, 本组件只负责引脚方向与上下拉配置. */
void KEY_Init(void);

#endif