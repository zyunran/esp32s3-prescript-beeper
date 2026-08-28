/* MPU6050 六轴组件: 软件I2C + 六轴读写 + 互补滤波姿态 + 摇动方向检测.
 * 参考 STM32 work_watch 工程 Hardware/MPU6050.c(寄存器) + memu.c(互补滤波) 按 ESP32 移植.
 *  - I2C: 推挽 SCL + SDA 方向切换 bit-bang(SCL=39 / SDA=38, 约 100kHz)
 *  - 姿态: 陀螺积分 + 加速度 atan2 互补滤波(STM32 同款 a=0.9; 陀螺按 ±2000dps 正确标定)
 *  - 摇动(四向, 送入按键队列): 上摇/下摇 = 上下键(EVT_UP/DOWN); 左摇 = 确认(EVT_OK), 右摇 = 退出(EVT_LONG_OK)
 *    判定: 转腕型取三轴角速度最大者(横滚gx=上下, 俯仰gy/航向gz=左右), 平移型取动态加速度竖直/水平分量
 *    (安装/握持姿态不同方向可能反, 见 mpu_shake 互换说明) */
#include "MPU6050.h"
#include "UI.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <math.h>
#include <stdio.h>

/* ================= 引脚与寄存器 ================= */
#define MPU_SCL         GPIO_NUM_39      /* 软件I2C 时钟(推挽恒驱动) */
#define MPU_SDA         GPIO_NUM_38      /* 软件I2C 数据(输出/输入方向切换) */
#define MPU_ADDR_W      0xD0    /* 0x68<<1 写地址(AD0=低) */
#define MPU_ADDR_R      0xD1    /* 0x68<<1 读地址 */

static uint8_t mpu_addr_w = MPU_ADDR_W;   /* 探测到的从机地址(0x68 或 AD0拉高的 0x69) */
static uint8_t mpu_addr_r = MPU_ADDR_R;

#define REG_SMPLRT_DIV  0x19
#define REG_CONFIG      0x1A
#define REG_GYRO_CONFIG 0x1B
#define REG_ACCEL_CONFIG 0x1C
#define REG_ACCEL_XOUT_H 0x3B
#define REG_PWR_MGMT_1  0x6B
#define REG_PWR_MGMT_2  0x6C
#define REG_WHO_AM_I    0x75

/* ================= 软件I2C =================
 * SCL 推挽输出(主控恒驱动); SDA 输出/输入方向切换(经典 bit-bang, 不依赖开漏上拉).
 * SDA 释放时转输入并开内部上拉(模块自带上拉时并行, 更稳). */
static void i2c_scl(uint8_t v) { gpio_set_level(MPU_SCL, v); }

static void i2c_sda_out(uint8_t v)
{
    gpio_set_direction(MPU_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level(MPU_SDA, v);
}

static void i2c_sda_release(void)   /* 释放 SDA: 转输入, 内部上拉拉高 */
{
    gpio_set_direction(MPU_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MPU_SDA, GPIO_PULLUP_ONLY);
}

static int i2c_sda_read(void) { return gpio_get_level(MPU_SDA); }

static void i2c_delay(void) { esp_rom_delay_us(15); }   /* 半周期 15us -> ~33kHz(弱上拉也可靠) */

static void i2c_init(void)
{
    gpio_set_direction(MPU_SCL, GPIO_MODE_OUTPUT);
    gpio_set_direction(MPU_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MPU_SDA, GPIO_PULLUP_ONLY);
    i2c_scl(1);
    i2c_sda_release();          /* 总线空闲: 高 */
}

static void i2c_start(void)
{
    i2c_sda_release(); i2c_scl(1); i2c_delay();
    i2c_sda_out(0); i2c_delay();     /* SDA 在 SCL 高时拉低 */
    i2c_scl(0); i2c_delay();
}

static void i2c_stop(void)
{
    i2c_sda_out(0); i2c_delay();
    i2c_scl(1); i2c_delay();
    i2c_sda_release(); i2c_delay();  /* SDA 在 SCL 高时拉高 */
}

/* I2C 总线恢复: 从机可能卡在传输中途(如任务被中途挂起/异常掉电), 把 SDA 拉低不放.
 * 标准恢复 = 释放 SDA + 对 SCL 打 9 个时钟脉冲(从机收完当前字节后释放 SDA) + STOP.
 * 若不做恢复, 之后所有读写都会失败(表现为"未连接传感器"). */
static void i2c_bus_recover(void)
{
    uint8_t i;
    printf("[MPU] bus stuck (SDA low) -> recover: 9x SCL + STOP\n");
    i2c_sda_release();
    for (i = 0; i < 9; i++)
    {
        i2c_scl(0); i2c_delay();
        i2c_scl(1); i2c_delay();
    }
    i2c_stop();
}

static void i2c_tx_bit(uint8_t b)
{
    i2c_sda_out(b); i2c_delay();
    i2c_scl(1); i2c_delay();
    i2c_scl(0); i2c_delay();
}

static void i2c_send_byte(uint8_t d)
{
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        i2c_tx_bit((d >> (7 - i)) & 1);   /* MSB 先发 */
    }
}

/* 读 ACK: 释放 SDA 由从机拉低. 返回 0=ACK, 1=NACK */
static int i2c_wait_ack(void)
{
    int ack;
    i2c_sda_release(); i2c_delay();
    i2c_scl(1); i2c_delay();
    ack = i2c_sda_read();
    i2c_scl(0); i2c_delay();
    return ack;
}

static uint8_t i2c_recv_byte(void)
{
    uint8_t d = 0, i;
    i2c_sda_release();          /* 释放 SDA, 由从机驱动 */
    for (i = 0; i < 8; i++)
    {
        i2c_scl(1); i2c_delay();
        d = (uint8_t)((d << 1) | (uint8_t)i2c_sda_read());
        i2c_scl(0); i2c_delay();
    }
    return d;
}

static void i2c_send_ack(uint8_t ack)   /* 0=ACK(拉低), 1=NACK(释放, 最后字节) */
{
    if (ack) i2c_sda_release(); else i2c_sda_out(0);
    i2c_delay();
    i2c_scl(1); i2c_delay();
    i2c_scl(0); i2c_delay();
}

/* ================= 寄存器读写(地址用探测到的 mpu_addr) ================= */
static uint8_t mpu_write_reg(uint8_t reg, uint8_t val)
{
    i2c_start();
    i2c_send_byte(mpu_addr_w);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    i2c_send_byte(reg);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    i2c_send_byte(val);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    i2c_stop();
    return 0;
}

static uint8_t mpu_read_reg(uint8_t reg, uint8_t *val)
{
    i2c_start();
    i2c_send_byte(mpu_addr_w);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    i2c_send_byte(reg);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    i2c_start();                 /* 重启动切读 */
    i2c_send_byte(mpu_addr_r);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    *val = i2c_recv_byte();
    i2c_send_ack(1);             /* 单字节: 主机发 NACK */
    i2c_stop();
    return 0;
}

/* 突发读 n 字节(从 reg 起, 寄存器自动递增). 失败(总线毛刺/从机掉线)返回 1 */
static uint8_t mpu_burst(uint8_t reg, uint8_t *buf, uint8_t n)
{
    uint8_t i;
    i2c_start();
    i2c_send_byte(mpu_addr_w);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    i2c_send_byte(reg);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    i2c_start();
    i2c_send_byte(mpu_addr_r);
    if (i2c_wait_ack()) { i2c_stop(); return 1; }
    for (i = 0; i < n; i++)
    {
        buf[i] = i2c_recv_byte();
        i2c_send_ack((i == n - 1) ? 1 : 0);
    }
    i2c_stop();
    return 0;
}

/* ================= 初始化 / 探活 ================= */
static volatile uint8_t mpu_ok = 0;   /* 采样任务写, UI/平衡页跨任务读(volatile 防缓存旧值) */

/* 探测从机地址(0x68=AD0低, 0x69=AD0高)并用 WHO_AM_I 校验 */
static uint8_t mpu_probe_addr(void)
{
    uint8_t a, id;
    for (a = 0; a < 2; a++)
    {
        mpu_addr_w = (uint8_t)(MPU_ADDR_W | (a ? 0x02 : 0x00));   /* 0xD0 / 0xD2 */
        mpu_addr_r = (uint8_t)(mpu_addr_w | 0x01);
        if (mpu_read_reg(REG_WHO_AM_I, &id) == 0 && (id & 0xFE) == 0x68)   /* WHO_AM_I bit0 随 AD0(0x68/0x69) */
        {
            return 1;
        }
    }
    return 0;
}

static void mpu_probe(void)
{
    uint8_t id = 0, pm1 = 0;
    /* 总线恢复: 若 SDA 被从机拉住(前次传输异常中断), 先打 9 脉冲释放总线再探测 */
    if (!i2c_sda_read())
    {
        i2c_bus_recover();
    }
    mpu_ok = mpu_probe_addr();
    if (!mpu_ok) return;

    /* 寄存器配置参考 STM32 MPU6050_Init; 差异:
     *   - CONFIG 用 0x01(DLPF 188Hz) 替代 0x06(5Hz): 5Hz 会把摇动瞬时滤掉, 本机要摇动检测
     *   - ACCEL_CONFIG 用 0x00(±2g) 替代 0x18(±16g): 提升分辨率(1g=16384 LSB), 姿态/摇动更准 */
    mpu_write_reg(REG_PWR_MGMT_1, 0x01);      /* 解除休眠, 时钟=PLL X 轴 */
    mpu_write_reg(REG_PWR_MGMT_2, 0x00);      /* 所有轴开启 */
    mpu_write_reg(REG_SMPLRT_DIV, 0x07);      /* 输出 1kHz(DLPF 开启须 ≤1kHz) */
    mpu_write_reg(REG_CONFIG, 0x01);          /* DLPF 188Hz */
    mpu_write_reg(REG_GYRO_CONFIG, 0x18);     /* 陀螺 ±2000 dps */
    mpu_write_reg(REG_ACCEL_CONFIG, 0x00);    /* 加速度 ±2g */

    /* 配置生效校验: 读回 PWR_MGMT_1 应为 0x01(非休眠 0x40) */
    if (mpu_read_reg(REG_WHO_AM_I, &id) != 0 || (id & 0xFE) != 0x68) mpu_ok = 0;
    mpu_read_reg(REG_PWR_MGMT_1, &pm1);
    if (pm1 & 0x40) mpu_ok = 0;

    printf("[MPU] addr=0x%02X WHO_AM_I=0x%02X PWR1=0x%02X ok=%d\n",
           (int)(mpu_addr_w >> 1), (int)id, (int)pm1, (int)mpu_ok);
}

/* 诊断: 全地址 ACK 扫描 + 空闲电平, 定位接线/供电/总线问题 */
static void mpu_scan_bus(void)
{
    uint8_t a;
    printf("[MPU] bus idle SDA=%d SCL=%d (expect 1 1; SDA=0 -> 模块未供电/未接线/短路)\n",
           i2c_sda_read(), gpio_get_level(MPU_SCL));
    for (a = 0x08; a <= 0x77; a++)
    {
        i2c_start();
        i2c_send_byte((uint8_t)(a << 1));
        if (!i2c_wait_ack())
        {
            printf("[MPU]   device ACK at 0x%02X\n", a);
        }
        i2c_stop();
    }
    printf("[MPU] bus scan done\n");
}

void MPU_Init(void)
{
    i2c_init();
    mpu_probe();
    if (!mpu_ok)
    {
        printf("[MPU] NO device: no ACK at 0x68/0x69\n");
        mpu_scan_bus();
    }
}

/* ================= 姿态: 互补滤波(参考 STM32 memu.c MPU_Calculation) =================
 *  - 陀螺积分: 陀螺原始值按 ±2000dps 标定成 °/s(STM32 参考未标定, 此处修正)
 *  - 加速度角: atan2, 与参考公式一致(pitch 用 -ax)
 *  - 互补: a=0.9 陀螺 + 0.1 加速度; yaw 仅陀螺积分, 会漂移 */
#define FILTER_A    0.9f
#define YAW_LEAK_A  0.997f                     /* yaw 泄漏系数(30ms/帧 → 时间常数≈10s): 漂移有界且转动时可读 */
#define GYRO_SCALE  (2000.0f / 32768.0f)   /* ±2000dps -> °/s per LSB */
#define ACCEL_G     (16384.0f)             /* ±2g -> LSB/g */
#define PI_F        3.1415926536f
#define DEG(x)      ((x) * 180.0f / PI_F)

static volatile float f_roll, f_pitch, f_yaw;   /* 采样任务写, UI 只读(32位对齐读原子) */
static uint8_t f_inited = 0;

float MPU_Roll(void)   { return f_roll; }
float MPU_Pitch(void)  { return f_pitch; }
float MPU_Yaw(void)    { return f_yaw; }

/* ================= 摇动检测(PCB 版安装校准) =================
 * PCB 静态基准实测: 平躺 横0/俯+10/航0; 竖立 俯-60.
 * 动作实测摆幅:   左右摇 横滚 0→±30; 上下摇 俯仰 +10→+40(下摇) / +10→-30(上摇).
 * 方向: 转腕型取三轴主导角速度判动作, 符号判方向(与旧板相反: 横滚=左右[已对调], 俯仰=上下);
 *       分轴阈值: 上下摇(俯仰)更灵敏, 左右摇(横滚/航向)收紧防误触;
 *       平移型由重力投影判向: vert = dyn·ĝ(动态分量沿真实竖直, 与安装无关).
 *   物理上摇(-30侧)发 EVT_DOWN / 下摇(+40侧)发 EVT_UP [事件码与物理方向反向]; 彩蛋序列已在LOOM按3,3,1,1起始适配「上上 下下」
 * 触发: ① 竖直加速度超 SHAKE_G(平移摇动);
 *       ② 角速度超分轴阈值 SHAKE_GYRO_UD/LR 且竖直分量够(转腕/甩动型) */
#define SHAKE_G        0.8f                  /* 平移竖直加速度触发阈值 g(重力投影法与安装无关, 不随 PCB 改) */
#define SHAKE_THRESH   ((int32_t)(SHAKE_G * ACCEL_G))
#define SHAKE_OK_G     1.3f                  /* 平移水平分量的 确认/返回 触发阈值 g: 须显著高于竖直阈值 ——
                                              * 放下/拿起设备的冲击即达 0.8g 级, 若与翻页同阈会把冲击误判成 OK/长按OK(菜单误确认/误退出) */
#define SHAKE_OK_THRESH ((int32_t)(SHAKE_OK_G * ACCEL_G))
#define SHAKE_COOLDOWN 300u                  /* 两次摇动最小间隔 ms */
#define SHAKE_GYRO_LR  70.0f                 /* 左右摇(横滚/航向)阈值 °/s: 实测易误触, 调高收紧 */
#define SHAKE_GYRO_UD  45.0f                 /* 上下摇(俯仰)阈值 °/s: 实测有时失效, 调低提灵敏度 */
#define RETURN_GUARD_MS 400u                 /* 回摆防护窗 ms: 同轴反号且更弱才判回弹(见下) */
#define REBOUND_RATIO   0.75f                /* 反号触发强度达上次 75% 以上 = 有意反向甩, 放行(Konami 依赖此) */
#define DOMINANCE       1.25f                /* 主导轴优势系数: 次轴达主导80%即混合动作, 不触发 */
/* 归位锁: 触发后需设备静止片刻才允许再次触发, 防止后摇归位(反向回摆)误触发 */
#define STILL_VERT     (0.25f * ACCEL_G)     /* 静止判定: 竖直加速度阈值 g */
#define STILL_GYRO     45.0f                 /* 静止判定: 角速度阈值 °/s(与上下摇阈值一致) */
#define STILL_MS       60u                   /* 需连续静止时长 ms */
#define EVT_UP         1                     /* 与 main.c 按键事件一致 */
#define EVT_OK         2
#define EVT_DOWN       3
#define EVT_LONG_OK    4

static QueueHandle_t mpu_q = NULL;
static TaskHandle_t  mpu_task_h = NULL;       /* 采样任务句柄 */
static volatile uint8_t mpu_paused = 0;       /* 待机暂停标志(任务自查, 不打断 I2C 事务) */
static uint32_t mpu_last_retry = 0;           /* 探测重试时刻(全局: 唤醒后强制立即重探测) */
static volatile uint8_t mpu_shake_en = 1;     /* 摇动检测开关(设置可关, 只禁摇动): 采样任务与设置侧跨任务, volatile */
static volatile uint8_t mpu_shake_dir = 0;    /* 最近摇动方向: 1=上 2=下 3=左 4=右(平衡页反馈) */
static volatile uint32_t mpu_shake_at = 0;    /* 摇动时刻 ms */
static volatile uint32_t mpu_ok_evt_at = 0;   /* 最近一次摇动产生的 确认/退出 事件入队时刻 ms(平衡页过滤用) */

void MPU_SetShake(uint8_t on)
{
    mpu_shake_en = on ? 1 : 0;
}

/* 待机暂停采样(省电): 用标志位而非 vTaskSuspend —— vTaskSuspend 可能正好挂在 I2C 传输中途,
 * 从机拉低 SDA 未释放会把总线锁死, 之后所有读写失败(平衡页显示"未连接传感器").
 * 任务自查标志, 只跳过采样, 永不中断进行中的总线事务. */
void MPU_Suspend(void)
{
    mpu_paused = 1;
}

/* 恢复采样并强制立即重新探测(清掉可能残留的总线状态, 最快 ~30ms 内恢复) */
void MPU_Resume(void)
{
    mpu_paused = 0;
    mpu_ok = 0;
    mpu_last_retry = 0;
}

/* 重力低通状态(文件级): 摇动方向判别用(扣屏/抬腕功能已删, 省电交给浅睡眠待机) */
static float gx_f, gy_f, gz_f;
static uint8_t g_init = 0;

static void mpu_shake(int16_t ax, int16_t ay, int16_t az, float gxd, float gyd, float gzd)
{
    static uint32_t cd_until = 0;
    static uint8_t  need_still = 0;        /* 1=刚触发过, 需先静止才允许再次触发 */
    static uint32_t still_since = 0;       /* 开始静止的时刻(0=仍在动) */
    const float alpha = 0.04f;    /* 重力低通: 30ms 采样 -> 时间常数约 0.75s(追踪倾斜, 忽略摇动) */
    uint32_t now;
    float mag, vert, dx, dy, lat;
    uint8_t evt = 0;

    if (!mpu_shake_en) return;   /* 摇动开关: 关闭则不做检测(采样/平衡照常) */

    if (!g_init)
    {
        gx_f = ax; gy_f = ay; gz_f = az;
        g_init = 1;
        return;
    }
    gx_f += alpha * ((float)ax - gx_f);
    gy_f += alpha * ((float)ay - gy_f);
    gz_f += alpha * ((float)az - gz_f);

    now = (uint32_t)(esp_timer_get_time() / 1000);

    if (now < cd_until) return;              /* 最小间隔 */

    mag = sqrtf(gx_f * gx_f + gy_f * gy_f + gz_f * gz_f);
    if (mag < 1.0f) return;                  /* 重力向量异常, 不判 */
    dx = (float)ax - gx_f;                   /* 动态加速度(去重力) */
    dy = (float)ay - gy_f;
    vert = (dx * gx_f + dy * gy_f + (float)(az - gz_f) * gz_f) / mag;  /* 沿竖直分量 */
    lat  = (fabsf(dx) >= fabsf(dy)) ? dx : dy;  /* 设备面内水平分量(取幅值大者, 安装无关) */

    /* 归位锁: 触发后需设备静止片刻(STILL_MS)才允许再次触发, 防止后摇归位反向回摆误触发 */
    if (need_still)
    {
        if (fabsf(vert) > STILL_VERT || fabsf(gxd) > STILL_GYRO ||
            fabsf(lat)  > STILL_VERT || fabsf(gyd) > STILL_GYRO ||
            fabsf(gzd)  > STILL_GYRO) still_since = 0;  /* 还在动 */
        else if (!still_since) still_since = now;
        if (still_since && now - still_since >= STILL_MS) need_still = 0;
        return;
    }

    /* 触发判定: 取主导轴(而非固定优先级, 防单轴动作混入他轴分量被误判):
     *  ① 转腕型(PCB): 主导角速度轴判动作, 且分轴设阈(上下摇灵敏/左右摇收紧)
     *     - 横滚大 = 左右摇(横 0→±30): gx>0 -> EVT_LONG_OK(退出), 反向 -> EVT_OK(确认) [实测对调]
     *     - 俯仰大 = 上下摇: 物理下摇(gy>0,+40侧)->EVT_UP, 上摇(gy<0,-30侧)->EVT_DOWN [事件反向]
     *     - 航向(gz) 大仍归左右摇(平放桌面水平转腕的兜底, 对调与横滚一致)
     *     - 优势校验: 次大轴达主导轴 1/DOMINANCE 即视为混合动作不触发(防上下摇带横滚误报左右)
     *     - 回摆防护: 同轴反号在 RETURN_GUARD_MS 内再触发 = 摇动回弹, 直接丢弃
     *  ② 平移型: |vert|(竖直) 与 水平分量(hmag) 谁大判方向
     *     - 竖直大 = 上下摇; 水平大 = 左右摇(方向取设备横向轴 lat)
     * 若方向仍不合手: 上下摇互换 EVT_UP/EVT_DOWN; 左右摇互换 EVT_OK/EVT_LONG_OK. */
    {
        float hmag = sqrtf(dx * dx + dy * dy);
        static uint8_t  last_ax = 0;     /* 上次转腕触发轴: 1=横滚 2=俯仰 3=航向(回摆防护) */
        static int8_t   last_sn = 0;     /* 上次触发符号(+1/-1) */
        static uint32_t last_at = 0;     /* 上次触发时刻 ms */
        static float    last_gv = 0;     /* 上次触发的角速度幅值(判回弹用) */
        {
            float ga = fabsf(gxd), gb = fabsf(gyd), gc = fabsf(gzd);
            uint8_t ax = 0;
            int8_t  sn = 0;
            /* 三轴取最大定主导轴, 各轴过各自阈值才候选 */
            if (ga >= gb && ga >= gc)      { if (ga >= SHAKE_GYRO_LR) { ax = 1; sn = (gxd > 0) ? 1 : -1; } }
            else if (gb >= gc)             { if (gb >= SHAKE_GYRO_UD) { ax = 2; sn = (gyd > 0) ? 1 : -1; } }
            else                           { if (gc >= SHAKE_GYRO_LR) { ax = 3; sn = (gzd > 0) ? 1 : -1; } }
            /* 主导优势校验: 不够突出 = 混合动作, 本帧不触发(等下一帧更纯的时刻) */
            if (ax != 0)
            {
                float g1 = (ax == 1) ? ga : ((ax == 2) ? gb : gc);
                float g2 = (ax == 1) ? ((gb > gc) ? gb : gc)
                         : (ax == 2) ? ((ga > gc) ? ga : gc)
                                     : ((ga > gb) ? ga : gb);
                if (g1 < g2 * DOMINANCE) ax = 0;
            }
            if (ax != 0)
            {
                float gsel = (ax == 1) ? ga : ((ax == 2) ? gb : gc);
                /* 回摆防护: 同轴反号 + 防护窗内 + 强度明显弱于上次 = 被动回弹, 丢弃;
                 * 等强度反号是有意的反向甩(如 上上下下 / 左右左右 序列), 必须放行 */
                if (last_ax == ax && last_sn == -sn &&
                    now - last_at < RETURN_GUARD_MS &&
                    gsel < last_gv * REBOUND_RATIO)
                {
                    return;
                }
                switch (ax)
                {
                    case 1:  evt = (sn > 0) ? EVT_LONG_OK : EVT_OK;      break;  /* 横滚=左右摇(PCB已对调) */
                    case 2:  evt = (sn > 0) ? EVT_UP      : EVT_DOWN;    break;  /* 俯仰=上下摇: +40=物理下摇发UP, -30=上摇发DOWN */
                    default: evt = (sn > 0) ? EVT_OK      : EVT_LONG_OK; break;  /* 航向=左右摇兜底(同步对调) */
                }
                last_ax = ax; last_sn = sn; last_at = now; last_gv = gsel;
            }
            else if (fabsf(vert) > SHAKE_THRESH || hmag > SHAKE_OK_THRESH)
            {
                /* 平移兜底: 竖直→上下翻页(普通阈); 水平→确认/返回须过专用高阈值 ——
                 * 竖直分量过了普通阈但水平只在中段(放下/拿起的冲击)时不映射确认, 直接丢弃 */
                if (fabsf(vert) >= hmag)         evt = (vert > 0) ? EVT_UP : EVT_DOWN;
                else if (hmag > SHAKE_OK_THRESH) evt = (lat > 0)  ? EVT_OK : EVT_LONG_OK;
                else                             return;
            }
            else
            {
                return;
            }
        }
    }
    cd_until = now + SHAKE_COOLDOWN;
    need_still = 1; still_since = 0;
    mpu_shake_dir = (evt == EVT_UP) ? 1 : (evt == EVT_DOWN) ? 2 : (evt == EVT_OK) ? 3 : 4;
    mpu_shake_at = now;
    if (evt == EVT_OK || evt == EVT_LONG_OK) mpu_ok_evt_at = now;
    if (mpu_q) xQueueSend(mpu_q, &evt, 0);
}

/* 最近(<200ms)是否有摇动产生的 确认/退出 事件: 供平衡页过滤 —— 否则左右摇会立即
 * 触发退出/确认离开本页, 看不到平衡页的左右摇方向反馈; 过滤后仅物理按键可退出 */
uint8_t MPU_EvtWasShake(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return (mpu_ok_evt_at != 0) && (now - mpu_ok_evt_at < 200);
}

/* ================= 采样任务: 读六轴 -> 互补滤波 -> 摇动检测 =================
 * 返回 1=本次读失败(总线毛刺/传感器掉线) */
static uint8_t mpu_sample(void)
{
    uint8_t buf[14];
    static uint32_t last_ms = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    float dt;
    int16_t ax, ay, az, gx, gy, gz;
    float gxd, gyd, gzd, roll_a, pitch_a;

    dt = 0.03f;
    if (last_ms)
    {
        dt = (float)(now - last_ms) / 1000.0f;
        if (dt > 0.10f) dt = 0.10f;          /* 上界, 防长阻塞后积分跳变 */
    }
    last_ms = now;

    if (mpu_burst(REG_ACCEL_XOUT_H, buf, 14)) return 1;   /* 读失败: 跳过本轮 */

    ax = (int16_t)((buf[0] << 8) | buf[1]);
    ay = (int16_t)((buf[2] << 8) | buf[3]);
    az = (int16_t)((buf[4] << 8) | buf[5]);
    gx = (int16_t)((buf[8] << 8) | buf[9]);
    gy = (int16_t)((buf[10] << 8) | buf[11]);
    gz = (int16_t)((buf[12] << 8) | buf[13]);

    gxd = (float)gx * GYRO_SCALE;            /* 陀螺 °/s */
    gyd = (float)gy * GYRO_SCALE;
    gzd = (float)gz * GYRO_SCALE;

    if (!f_inited)
    {
        f_pitch = DEG(atan2f((float)-ax, (float)az));   /* 首帧直接用加速度角 */
        f_roll  = DEG(atan2f((float)ay, (float)az));
        f_yaw   = 0.0f;
        f_inited = 1;
    }
    else
    {
        f_roll  += gxd * dt;
        f_pitch += gyd * dt;
        f_yaw   += gzd * dt;

        roll_a  = DEG(atan2f((float)ay, (float)az));
        pitch_a = DEG(atan2f((float)-ax, (float)az));

        f_roll  = FILTER_A * f_roll  + (1.0f - FILTER_A) * roll_a;
        f_pitch = FILTER_A * f_pitch + (1.0f - FILTER_A) * pitch_a;
        f_yaw   = YAW_LEAK_A * f_yaw;        /* yaw: 泄漏积分(时间常数≈10s, 旧值 0.9 τ≈0.3s 使航向恒显示≈0, v1.12 调整) */
        if (f_yaw > 180.0f) f_yaw -= 360.0f; /* 显示范围 [-180,180) */
        else if (f_yaw < -180.0f) f_yaw += 360.0f;
    }

    mpu_shake(ax, ay, az, gxd, gyd, gzd);
    return 0;
}

static void mpu_task(void *arg)
{
    uint32_t fail = 0;
    for (;;)
    {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (mpu_paused)   /* 待机: 无 I2C 活动, 200ms 轻轮询(与浅睡眠 tick 同频) */
        {
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }
        if (!mpu_ok)
        {
            if (now - mpu_last_retry >= 2000)    /* 未检测到: 每 2 秒重试 */
            {
                mpu_last_retry = now;
                mpu_probe();
            }
            fail = 0;
        }
        else
        {
            if (mpu_sample())
            {
                if (++fail >= 20) mpu_ok = 0;   /* ~600ms 连续读失败 -> 判已断开(平衡页显示未连接) */
            }
            else
            {
                fail = 0;
            }
        }
        vTaskDelay(30 / portTICK_PERIOD_MS);
    }
}

void MPU_Start(void *key_q)
{
    mpu_q = (QueueHandle_t)key_q;
    xTaskCreate(mpu_task, "mpu", 4096, NULL, 4, &mpu_task_h);   /* 4096: 任务内含 printf/协议解析, 3072 偏紧 */
}

/* ================= 「平衡」实时页(设置 -> 平衡, 由 UI 主任务每循环驱动) =================
 * 顶部标题 1 秒内显示最近一次摇动方向(上↑/下↓/左←/右→), 供摇动测试; 主体为 横滚/俯仰/航向. */
void MPU_BalanceTick(void)
{
    static uint32_t last_draw = 0;
    char buf[24];
    const char *st = "平衡";
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t shake = (mpu_shake_dir && (now - mpu_shake_at < 1000));

    /* 帧率限制: ui_task 每圈(~20ms)调用一次, 整屏清屏+重绘+blit 约 6ms/帧太重,
     * 10fps 对姿态显示足够流畅, 主循环让出时间给按键/音频 */
    if (now - last_draw < 100) return;
    last_draw = now;

    if (shake)
    {
        st = (mpu_shake_dir == 1) ? "摇动 ↑" : (mpu_shake_dir == 2) ? "摇动 ↓"
           : (mpu_shake_dir == 3) ? "摇动 ←" : "摇动 →";
    }
    UI_ScrClear(UI_COLOR_BG);
    UI_ScrTextCenter(4, st, UI_COLOR_TIME, UI_COLOR_BG);
    if (!mpu_ok)
    {
        UI_ScrTextCenter(26, "未连接传感器", UI_COLOR_MENU, UI_COLOR_BG);
        UI_ScrTextCenter(44, "摇动功能失效", UI_COLOR_DATE, UI_COLOR_BG);
        UI_ScrBlit();
        return;
    }
    snprintf(buf, sizeof(buf), "横滚 %+.1f", MPU_Roll());
    UI_ScrTextCenter(24, buf, UI_COLOR_MENU, UI_COLOR_BG);
    snprintf(buf, sizeof(buf), "俯仰 %+.1f", MPU_Pitch());
    UI_ScrTextCenter(42, buf, UI_COLOR_MENU, UI_COLOR_BG);
    snprintf(buf, sizeof(buf), "航向 %+.1f", MPU_Yaw());
    UI_ScrTextCenter(60, buf, UI_COLOR_MENU, UI_COLOR_BG);
    UI_ScrBlit();
}
