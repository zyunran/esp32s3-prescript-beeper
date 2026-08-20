# -*- coding: utf-8 -*-
"""诊断: 打开 COM9(不复位), 连续抓取 30 秒串口日志, 捕捉运行中自行重启的崩溃信息.
不做硬复位, 只监听, 看设备是否自发重启 + 崩溃原因(Guru/abort/watchdog)."""
import serial, time, sys

PORT = 'COM9'
BAUD = 115200
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 30

p = serial.Serial(PORT, BAUD, timeout=0.5)
time.sleep(0.2)
p.reset_input_buffer()

end = time.time() + DURATION
while time.time() < end:
    d = p.read(4096)
    if d:
        sys.stdout.buffer.write(d)
        sys.stdout.buffer.flush()
p.close()
print('\n[capture %ds done]' % int(DURATION))
