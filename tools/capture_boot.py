# -*- coding: utf-8 -*-
"""一次性诊断: 打开 COM9, 硬复位 ESP32-S3, 抓取 8 秒开机串口日志(重点看 [MPU] 行)."""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200

p = serial.Serial(PORT, BAUD, timeout=0.5)
time.sleep(0.2)
# 硬复位(与 esptool hard_reset 同极性): DTR=IO0 高, RTS=EN 拉低再释放
p.setDTR(False)
p.setRTS(True)
time.sleep(0.1)
p.setRTS(False)
time.sleep(0.3)
p.reset_input_buffer()

end = time.time() + 8
while time.time() < end:
    d = p.read(4096)
    if d:
        sys.stdout.buffer.write(d)
        sys.stdout.buffer.flush()
p.close()
print('\n[capture done]')
