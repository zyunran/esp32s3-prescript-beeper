#ifndef __SND_EFFECTS_H
#define __SND_EFFECTS_H

#include <stdint.h>

/* Short UI/game sound effects, 16-bit mono 16000 Hz. */
extern const int16_t snd_key_up[];
extern const uint32_t snd_key_up_frames;
extern const int16_t snd_key_ok[];
extern const uint32_t snd_key_ok_frames;
extern const int16_t snd_key_back[];
extern const uint32_t snd_key_back_frames;

extern const int16_t snd_coin[];
extern const uint32_t snd_coin_frames;
extern const int16_t snd_clash[];
extern const uint32_t snd_clash_frames;
extern const int16_t snd_ten_gray[];
extern const uint32_t snd_ten_gray_frames;
extern const int16_t snd_ten_red[];
extern const uint32_t snd_ten_red_frames;
extern const int16_t snd_ten_gold[];
extern const uint32_t snd_ten_gold_frames;

#endif /* __SND_EFFECTS_H */
