/* SOUND 组件: MAX98357A I2S 功放(16bit 单声道 16000Hz)
 *  - I2S_TX 标准(Philips)模式, 后台任务+信号量, SOUND_Play 非阻塞
 *  - 无数据时 auto_clear 输出静音, 防止喇叭底噪 */
#include "SOUND.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SOUND_BCLK   GPIO_NUM_16
#define SOUND_LRC    GPIO_NUM_17
#define SOUND_DIN    GPIO_NUM_18
#define SOUND_SD     GPIO_NUM_13   /* 功放关断(低=关): 音量0时拉低避免底噪 */
#define SOUND_RATE   16000        /* 采样率, 与 wav2c.py 转换一致 */

static i2s_chan_handle_t snd_tx;
static SemaphoreHandle_t snd_sem;       /* 唤醒播放任务 */
static const int16_t *snd_pcm;          /* 待播缓冲 */
static uint32_t snd_frames;
static uint8_t snd_busy;
static uint8_t snd_loop;                /* 1=循环播放直到 SOUND_Stop */
static uint8_t snd_gen;                 /* 播放代次: Play/Stop 时递增, 任务据此即时切换/停止 */
static uint8_t snd_vol = 100;           /* 音量 0~100(默认满) */
static int16_t snd_scratch[1024];       /* 音量缩放暂存(与写入块 2048B 对应) */

/* 播放任务: 收到信号量后写入 PCM; 代次变化(新 Play/Stop)则立即中止当前缓冲 */
static void snd_task(void *arg)
{
    for (;;)
    {
        xSemaphoreTake(snd_sem, portMAX_DELAY);
        uint8_t gen = snd_gen;
        do
        {
            uint8_t *p = (uint8_t *)snd_pcm;
            uint32_t bytes = snd_frames * 2, written = 0;
            while (written < bytes && gen == snd_gen)
            {
                uint32_t n = (bytes - written > 2048) ? 2048 : (bytes - written);
                size_t w = 0;
                if (snd_vol >= 100)
                {
                    /* 满音量: 直接写原始数据 */
                    if (i2s_channel_write(snd_tx, p + written, n, &w, portMAX_DELAY) != ESP_OK)
                    {
                        written = bytes;
                        break;
                    }
                }
                else
                {
                    /* 缩放音量: 逐采样 *vol/100 写入暂存 */
                    const int16_t *src = (const int16_t *)(p + written);
                    uint16_t ns = (uint16_t)(n / 2), i;
                    int32_t v = snd_vol;
                    for (i = 0; i < ns; i++)
                    {
                        int32_t s = (int32_t)src[i] * v / 100;
                        if (s > 32767) s = 32767;
                        else if (s < -32768) s = -32768;
                        snd_scratch[i] = (int16_t)s;
                    }
                    if (i2s_channel_write(snd_tx, snd_scratch, n, &w, portMAX_DELAY) != ESP_OK)
                    {
                        written = bytes;
                        break;
                    }
                }
                written += w;
            }
        } while (snd_loop && gen == snd_gen);
        snd_busy = 0;
    }
}

void SOUND_Init(void)
{
    /* 先关功放(SD低), 初始化I2S, 最后开功放: 防初始化瞬间喇叭刺声 */
    gpio_set_direction(SOUND_SD, GPIO_MODE_OUTPUT);
    gpio_set_level(SOUND_SD, 0);     /* 功放关断 */

    i2s_chan_config_t cc = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear = true,     /* 空闲输出 0, 喇叭静音 */
    };
    i2s_new_channel(&cc, &snd_tx, NULL);

    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SOUND_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   /* MAX98357A 不用 MCLK */
            .bclk = SOUND_BCLK,
            .ws = SOUND_LRC,
            .dout = SOUND_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .bclk_inv = false, .ws_inv = false },
        },
    };
    i2s_channel_init_std_mode(snd_tx, &sc);
    i2s_channel_enable(snd_tx);

    /* I2S 就绪(auto_clear 静音)后再开功放 */
    gpio_set_level(SOUND_SD, 1);

    snd_sem = xSemaphoreCreateCounting(1, 0);
    xTaskCreate(snd_task, "sound", 4096, NULL, 4, NULL);
}

void SOUND_SetVolume(uint8_t percent)
{
    snd_vol = (percent > 100) ? 100 : percent;
    /* 音量 0 -> 拉低 SD 关功放, 彻底静音避免底噪 */
    gpio_set_level(SOUND_SD, (snd_vol > 0) ? 1 : 0);
}

void SOUND_Play(const int16_t *pcm, uint32_t frames)
{
    if (!snd_sem || !pcm || frames == 0)
    {
        return;
    }
    snd_gen++;
    snd_pcm = pcm;
    snd_frames = frames;
    snd_loop = 0;
    snd_busy = 1;
    xSemaphoreGive(snd_sem);
}

void SOUND_PlayLoop(const int16_t *pcm, uint32_t frames)
{
    if (!snd_sem || !pcm || frames == 0)
    {
        return;
    }
    snd_gen++;
    snd_pcm = pcm;
    snd_frames = frames;
    snd_loop = 1;
    snd_busy = 1;
    xSemaphoreGive(snd_sem);
}

void SOUND_Stop(void)
{
    snd_gen++;      /* 代次+1: 播放任务立即中止当前缓冲 */
    snd_busy = 0;
}

