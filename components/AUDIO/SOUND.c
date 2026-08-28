/* SOUND 组件: MAX98357A I2S 功放(16bit 单声道 16000Hz)
 *  - I2S_TX 标准(Philips)模式, 后台任务+信号量, SOUND_Play 非阻塞
 *  - 无数据时 auto_clear 输出静音, 防止喇叭底噪 */
#include "SOUND.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SOUND_BCLK   GPIO_NUM_16
#define SOUND_LRC    GPIO_NUM_17
#define SOUND_DIN    GPIO_NUM_18
#define SOUND_SD     GPIO_NUM_13   /* 功放关断(低=关): 音量0时拉低避免底噪 */
#define SOUND_RATE   16000        /* 采样率, 与 wav2c.py 转换一致 */
static const char *TAG = "SOUND";

static i2s_chan_handle_t snd_tx;
static SemaphoreHandle_t snd_sem;       /* 唤醒播放任务 */
/* 播放请求经长度 1 的队列(xQueueOverwrite)传递: FreeRTOS 拷贝语义 = 原子快照,
 * 消除旧实现 snd_pcm/snd_frames/snd_loop 三处独立 volatile 存储在双核下
 * "第二次 Play 的指针与旧帧数错配"的数据竞争(以旧长度播新缓冲). */
typedef struct {
    const int16_t *pcm;
    uint32_t frames;
    uint8_t loop;                       /* 1=循环播放直到 Stop/被打断的单次音恢复 */
} snd_req_t;
static QueueHandle_t snd_q = NULL;      /* 长度 1: 覆盖写(后到请求顶掉未消费的, 与旧覆盖语义一致) */
static volatile uint16_t snd_gen;         /* 播放代次: Play/Stop 时递增, 任务据此即时切换/停止 */
static volatile uint8_t  snd_pending;     /* 1=有待播请求(Play 置1; Stop 清0; 任务消费后清0) */
static volatile uint8_t  snd_loop_active = 0;    /* 最近发布的请求是循环音(PlayLoop 置1 / Play·Stop 清0) */
static volatile uint8_t  snd_loop_suspended = 0; /* 1=循环进行音被单次音(按键音等)打断, 播完自动恢复 */
static snd_req_t snd_loop_cur;            /* 当前循环音参数(恢复用); ui_task 写, 任务在恢复时读 */
/* 以下播放状态由调用任务(ui_task: Play/PlayLoop/Stop/SetVolume)与 snd_task 跨任务共享:
 *  - volatile: 防编译器把代次/音量缓存在寄存器导致无限循环或读到陈旧值(M3)
 *  - snd_gen 用 uint16: 播/停满 256 次即回绕的 uint8 会在 "gen==snd_gen" 判定上误判代次 */
static volatile uint8_t  snd_vol = 100;   /* 音量 0~100(默认满; WEB/设置任务可改, 播放任务缩放时读) */
static int16_t snd_scratch[1024];       /* 音量缩放暂存(与写入块 2048B 对应) */

/* 播放任务: 收到信号量后从队列取请求写入 PCM; 代次变化(新 Play/Stop)则立即中止当前缓冲 */
static void snd_task(void *arg)
{
    for (;;)
    {
        snd_req_t r;
        xSemaphoreTake(snd_sem, portMAX_DELAY);
        if (!snd_pending)
        {
            continue;   /* 该次唤醒已被 Stop 作废(Play 后被 Stop, give 与消费之间): 回信号量重等 */
        }
        snd_pending = 0;   /* 消费本次请求; 此后播放中的停止由块间 gen 检查生效 */
        xQueueReceive(snd_q, &r, 0);
        for (;;)
        {
            /* resume=1: 本段(单次音)播完且需切换到被打断的循环音 —— continue 回 for 顶层,
             * mygen/p/bytes/written 按新请求全部重置 */
            uint8_t resume = 0;
            uint16_t mygen = snd_gen;
            do
            {
                uint8_t *p = (uint8_t *)r.pcm;
                uint32_t bytes = r.frames * 2, written = 0;   /* 循环音每轮重播都要重置, 须在 do 体内 */
                while (written < bytes && mygen == snd_gen)
                {
                    uint32_t n = (bytes - written > 2048) ? 2048 : (bytes - written);
                    size_t w = 0;
                    if (snd_vol >= 100)
                    {
                        /* 满音量: 直接写原始数据 */
                        if (i2s_channel_write(snd_tx, p + written, n, &w, portMAX_DELAY) != ESP_OK)
                        {
                            written = bytes;
                            snd_gen++;   /* 写失败: 提前终止, 防 PlayLoop 自旋饿死 UI */
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
                            snd_gen++;   /* 同满音量路径: 写失败即终止, 防自旋 */
                            break;
                        }
                    }
                    written += w;
                }
            } while (r.loop && mygen == snd_gen);   /* 循环音: 整段反复, 直到 Stop/新请求 */
            if (mygen != snd_gen) break;            /* 代次已变: 不恢复, 回信号量等待 */
            if (!r.loop && snd_loop_suspended) resume = 1;   /* 单次音(按键音)播完: 恢复被打断的循环进行音 */
            if (resume)
            {
                r = snd_loop_cur;
                snd_loop_suspended = 0;
                snd_loop_active = 1;
                continue;
            }
            break;   /* 播完或被打断: 回信号量等待, 不再引用旧缓冲 */
        }
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
    if (i2s_new_channel(&cc, &snd_tx, NULL) != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s new channel failed");
        gpio_set_level(SOUND_SD, 0);   /* 保持功放关断(静音) */
        return;
    }

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
    if (i2s_channel_init_std_mode(snd_tx, &sc) != ESP_OK ||
        i2s_channel_enable(snd_tx) != ESP_OK)
    {
        ESP_LOGE(TAG, "i2s init/enable failed");
        gpio_set_level(SOUND_SD, 0);   /* 保持静音; SOUND_Play 因 snd_sem 为 NULL 直接返回 */
        return;
    }

    /* I2S 就绪(auto_clear 静音)后再开功放 */
    gpio_set_level(SOUND_SD, 1);

    snd_sem = xSemaphoreCreateCounting(1, 0);
    snd_q = xQueueCreate(1, sizeof(snd_req_t));
    if (!snd_q)
    {
        ESP_LOGE(TAG, "snd queue create failed");
        return;
    }
    xTaskCreate(snd_task, "sound", 4096, NULL, 4, NULL);
}

void SOUND_SetVolume(uint8_t percent)
{
    snd_vol = (percent > 100) ? 100 : percent;
    /* 音量 0 -> 拉低 SD 关功放, 彻底静音避免底噪 */
    gpio_set_level(SOUND_SD, (snd_vol > 0) ? 1 : 0);
}

/* 发布请求到队列(覆盖写)并唤醒任务; 供 Play/PlayLoop 共用 */
static void snd_publish(const int16_t *pcm, uint32_t frames, uint8_t loop)
{
    snd_req_t r = { pcm, frames, loop };
    snd_gen++;
    if (loop)
    {
        snd_loop_cur = r;          /* 循环音参数存底: 被单次音打断后据此恢复 */
        snd_loop_active = 1;
        snd_loop_suspended = 0;
    }
    else if (snd_loop_active && !snd_loop_suspended)
    {
        snd_loop_suspended = 1;    /* 单次音打断循环进行音: 播完自动恢复 */
    }
    else if (!loop)
    {
        snd_loop_active = 0;
    }
    snd_pending = 1;   /* 置请求标志后再 give: 任务醒来先查 pending, 防 Stop 夹在中间把本次播放作废 */
    xQueueOverwrite(snd_q, &r);
    xSemaphoreGive(snd_sem);
}

void SOUND_Play(const int16_t *pcm, uint32_t frames)   /* 一次性播放(按键音/音效; 会临时打断循环音并自动恢复) */
{
    if (!snd_sem || !snd_q || !pcm || frames == 0)
    {
        return;
    }
    snd_publish(pcm, frames, 0);
}

void SOUND_PlayLoop(const int16_t *pcm, uint32_t frames)
{
    if (!snd_sem || !snd_q || !pcm || frames == 0)
    {
        return;
    }
    snd_publish(pcm, frames, 1);
}

void SOUND_Stop(void)
{
    snd_pending = 0;    /* 作废未消费的播放请求: 任务即使刚被 give 唤醒也直接重等 */
    snd_gen++;          /* 代次+1: 播放任务立即中止当前缓冲 */
    snd_loop_active = 0;
    snd_loop_suspended = 0;   /* 连循环恢复一起作废: Stop 是显式停, 不该再续上 */
}

