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
static uint8_t mpu_ok = 0;

/* 探测从机地址(0x68=AD0低, 0x69=AD0高)并用 WHO_AM_I 校验 */
static uint8_t mpu_probe_addr(void)
{
    uint8_t a, id;
    for (a = 0; a < 2; a++)
    {
        mpu_addr_w = (uint8_t)(MPU_ADDR_W | (a ? 0x02 : 0x00));   /* 0xD0 / 0xD2 */
        mpu_addr_r = (uint8_t)(mpu_addr_w | 0x01);
        if (mpu_read_reg(REG_WHO_AM_I, &id) == 0 && id == 0x68)
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
    if (mpu_read_reg(REG_WHO_AM_I, &id) != 0 || id != 0x68) mpu_ok = 0;
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
#define GYRO_SCALE  (2000.0f / 32768.0f)   /* ±2000dps -> °/s per LSB */
#define ACCEL_G     (16384.0f)             /* ±2g -> LSB/g */
#define PI_F        3.1415926536f
#define DEG(x)      ((x) * 180.0f / PI_F)

static volatile float f_roll, f_pitch, f_yaw;   /* 采样任务写, UI 只读(32位对齐读原子) */
static uint8_t f_inited = 0;

float MPU_Roll(void)   { return f_roll; }
float MPU_Pitch(void)  { return f_pitch; }
float MPU_Yaw(void)    { return f_yaw; }

/* ================= 摇动检测 =================
 * 方向: 转腕型由横滚角速度 gxd 判向(安装/握持姿态不同方向可能反, 见下方交换说明);
 *       平移型由重力投影判向: vert = dyn·ĝ(动态分量沿真实竖直).
 *   上摇 -> EVT_UP(上滑); 下摇 -> EVT_DOWN(下滑)
 * 触发: ① 竖直加速度超 SHAKE_G(平移摇动);
 *       ② 角速度超 SHAKE_GYRO 且竖直分量够(转腕/甩动型) */
#define SHAKE_G        0.8f                  /* 平移竖直加速度触发阈值 g */
#define SHAKE_THRESH   ((int32_t)(SHAKE_G * ACCEL_G))
#define SHAKE_GYRO     80.0f                 /* 转腕角速度阈值 °/s(实测本机左右摇俯仰 gy 约100, 80 可稳定触发) */
#define SHAKE_MIN_VERT (0.10f * ACCEL_G)     /* 转腕判向所需最小竖直分量 g */
#define SHAKE_COOLDOWN 350u                  /* 两次摇动最小间隔 ms */
/* 归位锁: 触发后需设备静止片刻才允许再次触发, 防止后摇归位(反向回摆)误触发 */
#define STILL_VERT     (0.25f * ACCEL_G)     /* 静止判定: 竖直加速度阈值 g */
#define STILL_GYRO     60.0f                 /* 静止判定: 横滚角速度阈值 °/s */
#define STILL_MS       60u                   /* 需连续静止时长 ms */
#define EVT_UP         1                     /* 与 main.c 按键事件一致 */
#define EVT_OK         2
#define EVT_DOWN       3
#define EVT_LONG_OK    4

static QueueHandle_t mpu_q = NULL;
static TaskHandle_t  mpu_task_h = NULL;       /* 采样任务句柄 */
static volatile uint8_t mpu_paused = 0;       /* 待机暂停标志(任务自查, 不打断 I2C 事务) */
static uint32_t mpu_last_retry = 0;           /* 探测重试时刻(全局: 唤醒后强制立即重探测) */
static uint8_t mpu_shake_en = 1;              /* 摇动检测开关(设置可关, 只禁摇动) */
static volatile uint8_t mpu_shake_dir = 0;    /* 最近摇动方向: 1=上 2=下 3=左 4=右(平衡页反馈) */
static volatile uint32_t mpu_shake_at = 0;    /* 摇动时刻 ms */

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

static void mpu_shake(int16_t ax, int16_t ay, int16_t az, float gxd, float gyd, float gzd)
{
    static float gx_f, gy_f, gz_f;
    static uint8_t g_init = 0;
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

    /* 触发判定: 取主导轴(而非固定优先级, 防左右摇带横滚分量被误判为上下):
     *  ① 转腕型: |gxd|(横滚) 与 |gzd|(航向) 谁大判方向
     *     - 横滚大 = 上下摇: 上摇 gx>0 -> EVT_UP, 下摇 -> EVT_DOWN
     *     - 航向大 = 左右摇: 左转 -> EVT_OK(确认), 右转 -> EVT_LONG_OK(退出)
     *  ② 平移型: |vert|(竖直) 与 水平分量(hmag) 谁大判方向
     *     - 竖直大 = 上下摇; 水平大 = 左右摇(方向取设备横向轴 lat)
     * 若实测左右方向反, 把 EVT_OK/EVT_LONG_OK 互换即可. */
    {
        float hmag = sqrtf(dx * dx + dy * dy);
        {
            /* 转腕主导(三轴取最大): 横滚 gx 大 = 上下摇; 俯仰 gy / 航向 gz 大 = 左右摇
             * 实测: 本机左右摇以俯仰 gy 为主(峰值约100°/s), 上下摇以横滚 gx 为主.
             * 方向: 左右由主导轴符号定, 若左右反了把下方 EVT_OK/EVT_LONG_OK 互换. */
            float ga = fabsf(gxd), gb = fabsf(gyd), gc = fabsf(gzd);
            float gmax = (ga > gb) ? ga : gb;
            if (gc > gmax) gmax = gc;
            if (gmax > SHAKE_GYRO)
            {
                if (gb >= ga && gb >= gc)   evt = (gyd > 0) ? EVT_OK : EVT_LONG_OK;  /* 俯仰=左右 */
                else if (gc > ga)           evt = (gzd > 0) ? EVT_LONG_OK : EVT_OK;  /* 航向=左右 */
                else                        evt = (gxd > 0) ? EVT_UP : EVT_DOWN;     /* 横滚=上下 */
            }
            else if (fabsf(vert) > SHAKE_THRESH || hmag > SHAKE_THRESH)
            {
                if (fabsf(vert) >= hmag)    evt = (vert > 0) ? EVT_UP : EVT_DOWN;
                else                        evt = (lat > 0)  ? EVT_OK : EVT_LONG_OK;
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
    if (mpu_q) xQueueSend(mpu_q, &evt, 0);
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
        f_yaw   = FILTER_A * f_yaw;          /* yaw 仅陀螺积分 */
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
    xTaskCreate(mpu_task, "mpu", 3072, NULL, 4, &mpu_task_h);
}

/* ================= 「平衡」实时页(设置 -> 平衡, 由 UI 主任务每循环驱动) =================
 * 顶部标题 1 秒内显示最近一次摇动方向(上↑/下↓/左←/右→), 供摇动测试; 主体为 横滚/俯仰/航向. */
void MPU_BalanceTick(void)
{
    char buf[24];
    const char *st = "平衡";
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t shake = (mpu_shake_dir && (now - mpu_shake_at < 1000));

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
