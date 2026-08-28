#ifndef __ANSWER_H
#define __ANSWER_H

#include <stdint.h>

/* ANSWER 组件: 「询问」答案之书
 *  - 进入后显示 4 个分类子菜单: 回答/吃什么/喝什么/玩什么/退出
 *  - 选中分类后 OK 随机抽一条该分类答案, 复用 INSTRUCTION 的 INS_Show 做乱码破译显示
 *  - 破译完成后答案停留; OK/上/下 再抽一条(同分类), 长按OK 回分类子菜单
 *  - 答案库: 内置默认(4 类) + 网页全量覆盖(NVS "ans"/"c0".."c3"):
 *    网页文本框显示并编辑该分类【全部】答案(内置+自定义), 清空保存=恢复内置默认
 *  - 随机: esp_random; 非阻塞状态机, 由 UI 主任务驱动 Enter/OnEvent */

/* 分类(子菜单项顺序与此一致) */
typedef enum {
    ANS_CAT_ANSWER = 0,   /* 回答 */
    ANS_CAT_EAT,          /* 吃什么 */
    ANS_CAT_DRINK,        /* 喝什么 */
    ANS_CAT_PLAY,         /* 玩什么 */
    ANS_CAT_N,            /* 分类数(含"退出"前的实际项数) */
} ans_cat_t;

#define ANS_CAT_NAMES  "回答", "吃什么", "喝什么", "玩什么"   /* 分类显示名(与枚举顺序一致) */

/* 答案长度/数量上限(WEB 校验共用) */
#define ANS_LINE_MAX    48    /* 单条答案最大字节数(含 '\0'; 15 个汉字约 45 字节, 破译自动折行) */
#define ANS_TOTAL_MAX   60    /* 每分类答案总条数上限(内置最多30 + 网页可增) */

/* ================= API(非阻塞, 由 RTOS 主任务驱动) =================
 * 事件码与 main.c EVT_* 一致: 1=上 2=确认 3=下 4=OK长按
 * 流程: Enter 渲染分类子菜单 -> OnEvent 选分类/抽答案 -> Busy 0 时回主界面 */
void    ANS_Init(void);                          /* 加载答案库(NVS "ans", 无则内置默认) */
void    ANS_Enter(void);                         /* 进入「询问」界面(渲染分类子菜单) */
void    ANS_OnEvent(uint8_t evt);                /* 按键事件(1=UP 2=OK 3=DOWN 4=LONG_OK) */
uint8_t ANS_Busy(void);                          /* 1=询问界面运行中 */

/* 答案库访问(网页用) */
const char *ANS_Custom(uint8_t cat);             /* 该分类【全部】答案整串文本(换行分隔; 无 NVS 覆盖=内置默认) */
void ANS_FromText(uint8_t cat, const char *text);/* 网页覆盖整类答案(NVS, 每行一条; 空串=清空恢复内置) */
void ANS_SaveBatchBegin(void);                   /* 网页批量保存开始: 逐类只记脏掩码不落盘 */
void ANS_SaveBatchEnd(void);                     /* 批量结束: 脏分类合并为一次 open+commit(配对调用) */

#endif
