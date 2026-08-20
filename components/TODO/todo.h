#ifndef __TODO_H
#define __TODO_H

#include <stdint.h>

/* 单条待办文本最大字节数(与 WEB 输入校验共用) */
#define TODO_TEXT_MAX 60

/* TODO 组件: 待办/指令日志(NVS "todo" 持久化)
 *  - 产生: 指令文本带 {TODO} 前缀时, 破译显示的同时自动存入待办(去重)
 *  - 设备: 主菜单"待办" -> 列表(OK 重新破译显示, 长按OK 标记 PASS/恢复, 末项"退出")
 *  - 网页: /api/todo 增/删/标记 PASS/清空(见 WEB 组件)
 * 流程: UI 主任务驱动 TODO_Enter/Key; 重显示由 main 调 INS_Show(本组件不依赖 INSTRUCTION) */

void    TODO_Init(void);                       /* 加载 NVS 待办 */
void    TODO_Enter(void);                      /* 进入待办列表 */
uint8_t TODO_Key(uint8_t up, uint8_t ok, uint8_t down, uint8_t lng); /* 按键; 返回动作码 */
#define TODO_KEY_NONE   0                      /* 已处理 */
#define TODO_KEY_SHOW   1                      /* OK 选待办: 需重显示(取 TODO_CurText) */
#define TODO_KEY_EXIT   2                      /* 选"退出": 回主界面 */
uint8_t TODO_Count(void);                      /* 待办条数 */
uint8_t TODO_Add(const char *text);            /* 追加(去重), 返回1=新增 */
const char *TODO_Text(uint8_t i);              /* 第i条文本(须 i<TODO_Count) */
uint8_t TODO_Done(uint8_t i);                  /* 1=已PASS */
const char *TODO_CurText(void);                /* 当前选中待办文本(在"退出"上=NULL) */
/* 网页用 */
void TODO_Toggle(uint8_t i);                   /* 切换 PASS/恢复 */
void TODO_Del(uint8_t i);                      /* 删除第i条(后续前移) */
void TODO_Clear(void);                         /* 清空全部 */

#endif
