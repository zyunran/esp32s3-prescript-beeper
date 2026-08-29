#ifndef __LOOM_H
#define __LOOM_H

#include <stdint.h>
#include <stddef.h>

/* LOOM 组件: 「织机」彩蛋(v1.03 起从主菜单隐藏, 改为手势解锁的独立组件)
 * 入口: 主界面 Konami 序列 —— 摇动(上上 下下 左右左右)或按键(下下上上+OK·LNG·OK·LNG, 因摇U/D事件与键反向)
 *       (上上 下下 确认 长按确认 确认 长按确认)均可; 由 main 在主界面态喂 LOOM_Konami.
 * 功能: 纺织时间(made in heaven 时间加速) / 纺织记忆(全系统白框滤镜) / 平衡(六轴姿态, v1.16 自设置移入) */

/* 动作码(LOOM_Key 返回, 导航由调用方 main 执行 —— LCD 单写者约定) */
#define LOOM_KEY_NONE    0
#define LOOM_KEY_EXIT    1    /* 选"退出"/长按OK: 调用方 ui_pop() */
#define LOOM_KEY_TIME    2    /* 纺织时间已切换: 调用方播 MADE IN HEAVEN 确认动画 */
#define LOOM_KEY_MEMORY  3    /* 纺织记忆已切换: 调用方 ui_to_main() 同步白框画面 */
#define LOOM_KEY_BAL     4    /* 选中"平衡": 调用方 ui_push(ST_MPU), 页内返回回织机菜单(不清 busy) */

void LOOM_Init(void);                 /* 复位内部状态 */
uint8_t LOOM_Busy(void);              /* 1=织机子菜单显示中 */
void LOOM_Enter(void);                /* 渲染织机子菜单(纺织时间/纺织记忆/平衡/退出) */
uint8_t LOOM_Key(uint8_t up, uint8_t ok, uint8_t down, uint8_t lng); /* 返回动作码 */
void LOOM_Tick(void);                 /* 预留推进(当前无动画, 由主循环调用) */
/* Konami 喂入返回码 */
#define LOOM_KON_PASS  0    /* 与序列无关: 调用方照常处理本事件 */
#define LOOM_KON_DONE  2    /* 序列完成: 调用方进入织机 */
uint8_t LOOM_Konami(uint8_t evt);     /* 喂入事件码(与 main EVT_* 一致); 2秒无输入自动复位.
                                       * 前4步(上上下下)不吞事件照常滚动, 失败零副作用. */
uint8_t LOOM_KonamiArmed(void);       /* 1=已进后4步拦截区(左右左右): 调用方须拦下 OK/长按OK 不开子菜单 */
uint8_t LOOM_TimeOn(void);            /* 1=纺织时间加速中 */
void LOOM_TimeToggle(void);           /* 切换加速(WEB 彩蛋指令/子菜单共用), 后查 TimeOn 取新态 */
void LOOM_TimeGet(char *d, size_t dn, char *t, size_t tn, char *w, size_t wn); /* 加速后的 日期/时间/星期 */

#endif
