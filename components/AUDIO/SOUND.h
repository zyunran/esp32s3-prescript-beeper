#ifndef __SOUND_H
#define __SOUND_H

#include <stdint.h>

/* SOUND: I2S 功放播放 API。 */

void SOUND_Init(void);                          /* 初始化 I2S 输出 */
void SOUND_SetVolume(uint8_t percent);          /* 音量 0~100(默认100), 播放时缩放 */
void SOUND_Play(const int16_t *pcm, uint32_t frames); /* 播放 PCM(非阻塞, 新播放覆盖旧的) */
void SOUND_PlayLoop(const int16_t *pcm, uint32_t frames); /* 循环播放直到 SOUND_Stop */
void SOUND_Stop(void);                           /* 停止播放(循环/一次性均停) */

#endif
