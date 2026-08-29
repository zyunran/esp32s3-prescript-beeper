#ifndef __BUZZER_H
#define __BUZZER_H

#include <stdint.h>

/* BUZZER 组件: 有源蜂鸣器(GPIO15, 高电平有效: 高=响 低=静, 高电平触发模块)
 *  - 非阻塞调度: Beep 只排程, 实际通断由 Tick 推进(主循环每圈调用)
 *  - 状态被 ui_task(Tick)与 httpd 任务(网页试响)并发读写:
 *    volatile 防跨核缓存; 单字节/对齐 32 位写读在本芯片原子 */

/* 蜂鸣可调参数(改这里即可调整) */
#define BZ_BEEP_MS   220        /* 每次蜂鸣时长 ms */
#define BZ_BEEP_MIN  150        /* 相邻蜂鸣最短间隔 ms */
#define BZ_BEEP_MAX  500        /* 相邻蜂鸣最长间隔 ms */
#define BZ_RAPID_ON    90       /* 急促连响: 每声时长 ms */
#define BZ_RAPID_OFF   60       /* 急促连响: 声间间隔 ms(固定且短, 连贯) */
#define BZ_RAPID_TOTAL(n) ((n) * BZ_RAPID_ON + ((n) - 1) * BZ_RAPID_OFF)  /* n 声总时长 ms */

void BUZZER_Init(void);             /* GPIO 初始化(下拉空闲低=静 + 最大驱动提音量) */
void BUZZER_Tick(void);             /* 蜂鸣推进, 主循环每圈调用(息屏也走完防卡响发烫) */
void BUZZER_Beep(uint8_t times);    /* 独立蜂鸣 n 下(重新调度, 正在响先断防卡响) */
void BUZZER_RapidBeep(uint8_t times, uint16_t on_ms, uint16_t off_ms);   /* 急促连响: 固定短间隔立即开始(破译结尾与完成时刻对齐用) */
void BUZZER_SetEnable(uint8_t on);  /* 总开关(1=响 0=静音, 默认开; 关闭时正在响立即停) */
void BUZZER_Stop(void);             /* 急停: 断音并作废待响排程(退出页面等场景) */

#endif
