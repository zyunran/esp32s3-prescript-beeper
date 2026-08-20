#ifndef __SOUND_H
#define __SOUND_H

#include <stdint.h>

/* SOUND 组件: MAX98357A I2S 功放播采样音频
 *  - 接线: GPIO16=BCLK  GPIO17=LRC  GPIO18=DIN  (SD=GPIO13, 固件控制低=关断)
 *  - 格式: 16bit 单声道 PCM, 采样率 16000Hz(与 tools/wav2c.py 转换一致)
 *  - 播放走独立后台任务, SOUND_Play 非阻塞 */

void SOUND_Init(void);                          /* 初始化 I2S 输出 */
void SOUND_SetVolume(uint8_t percent);          /* 音量 0~100(默认100), 播放时缩放 */
void SOUND_Play(const int16_t *pcm, uint32_t frames); /* 播放 PCM(非阻塞, 新播放覆盖旧的) */
void SOUND_PlayLoop(const int16_t *pcm, uint32_t frames); /* 循环播放直到 SOUND_Stop */
void SOUND_Stop(void);                           /* 停止播放(循环/一次性均停) */

#endif
