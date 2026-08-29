#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OneNET Studio 设备级安全鉴权 token 生成器 (M0 手动验证用, 无第三方依赖).

用法(在 tools 目录下):
  python onenet_token.py <ProductID> <DeviceName> <DeviceAccessKey> [有效天数=365]

输出 password 字段使用的 token, 配合 MQTTX 手动验证设备接入:
  host     = mqtts.heclouds.com
  port     = 1883 (TCP) 或 8883 (TLS, 需勾选 CA 证书)
  clientId = DeviceName
  username = ProductID
  password = <本脚本输出的 token>

规则依据 OneNET 新版 DMP 平台安全鉴权文档(1486《Token算法》, 2026-01 更新):
  签名明文按参数名字符序仅取 value, '\\n' 分隔:
    StringForSignature = et + "\\n" + method + "\\n" + res + "\\n" + version
  key = base64 解码后的 AccessKey;
  sign = base64(HMAC-SHA256(key, StringForSignature)), 拼接时 res 与 sign 需 URL 编码.
"""
import base64
import hashlib
import hmac
import sys
import time
from urllib.parse import quote


def gen_token(pid, name, key_b64, days=365):
    version = "2018-10-31"
    res = "products/{}/devices/{}".format(pid, name)
    et = str(int(time.time()) + days * 86400)
    method = "sha256"
    # 新版 DMP 平台: 签名明文 = et\nmethod\nres\nversion(按参数名字符序, 仅取 value)
    plaintext = "{}\n{}\n{}\n{}".format(et, method, res, version).encode("utf-8")
    key = base64.b64decode(key_b64)
    sign_b64 = base64.b64encode(hmac.new(key, plaintext, hashlib.sha256).digest()).decode()
    return ("version={}&res={}&et={}&method={}&sign={}"
            .format(version, quote(res, safe=""), et, method, quote(sign_b64, safe="")))


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    days = int(sys.argv[4]) if len(sys.argv) > 4 else 365
    print(gen_token(sys.argv[1], sys.argv[2], sys.argv[3], days))
