#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OneNET MQTT 连接自测(PC 端): 用与固件 cloud_token.c 完全相同的 token 算法
从 PC 直连平台, 定位设备端 'Connection refused, bad username or password' 的责任方.

  - 脚本也被拒   -> 三元组/密钥与平台不匹配(最常见: 抄错设备密钥/误用产品密钥)
                    -> 平台设备详情页 重置设备密钥 后重填
  - 脚本成功     -> 固件 token 计算有问题 -> 把完整输出发给开发者

自动附加检测: 产品ID 与 设备名 填反的情况(交换后自动再试一次).

用法:
  python tools/onenet_mqtt_test.py <ProductID> <DeviceName> <DeviceKey>
依赖: pip install paho-mqtt==1.6.1
"""
import os
import sys
import time

import paho.mqtt.client as mqtt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from onenet_token import gen_token

BROKER, PORT = "mqtts.heclouds.com", 1883
_rc_seen = {}


def _on_connect(client, userdata, flags, rc):
    _rc_seen["rc"] = rc


def attempt(label, pid, name, key):
    token = gen_token(pid, name, key)
    print("\n[%s]" % label)
    print("  clientId=%s username=%s" % (name, pid))
    print("  res=products/%s/devices/%s" % (pid, name))
    print("  token[:72]=%s..." % token[:72])
    _rc_seen.clear()
    c = mqtt.Client(client_id=name, protocol=mqtt.MQTTv311)
    c.username_pw_set(pid, token)
    c.on_connect = _on_connect
    try:
        c.connect(BROKER, PORT, 60)
        c.loop_start()
        for _ in range(80):   # 最多等 8s CONNACK
            if "rc" in _rc_seen:
                break
            time.sleep(0.1)
        c.loop_stop()
        c.disconnect()
    except Exception as e:
        print("  连接异常: %r" % e)
        return False
    rc = _rc_seen.get("rc")
    print("  CONNACK rc=%s %s" % (rc, "<- 平台接受" if rc == 0 else "<- 被拒绝"))
    return rc == 0


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    pid, name, key = sys.argv[1:4]
    print("目标 %s:%d (token 算法与固件一致)\n=======" % (BROKER, PORT))
    if attempt("标准填法", pid, name, key):
        print("\n结论: 三元组有效且填法正确; 设备仍被拒则是固件问题, 请回报开发者")
        sys.exit(0)
    if attempt("对调填法(检测填反)", name, pid, key):
        print("\n结论: 你把 产品ID 和 设备名 填反了! 按对调后的填法重填网页卡片")
        sys.exit(0)
    print("\n结论: 两种填法均被拒 -> 三元组/密钥与平台不匹配:")
    print("  1. 密钥应为 设备详情页 的 设备密钥(DeviceKey), 不是产品页的产品密钥;")
    print("  2. 设备名/产品ID 与平台不一致(区分大小写, 不能多空格)")
    print("  处理: 平台设备详情 -> 重置设备密钥 -> 重新复制 -> 网页卡片重新保存")
