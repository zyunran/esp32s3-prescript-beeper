#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""WAV -> C 数组(供 SOUND 组件经 MAX98357A 播放)

用法:
  python tools/wav2c.py <输入.wav> [输出.h] [数组名]
  默认输出 <输入名>.h, 数组名取输入文件名(非字母数字转 _)

要求:
  - 16bit 或 8bit PCM; 立体声自动取左声道
  - 采样率须与固件 SOUND_RATE(16000) 一致, 否则变速(建议先 ffmpeg -ar 16000)
  - 转换流程: ffmpeg -i in.mp3 -ac 1 -ar 16000 -sample_fmt s16 out.wav

产出:
  const int16_t <名>[] = { ... };   /* 16000Hz 单声道 16bit */
  const uint32_t <名>_frames = N;   /* 帧数 = 秒数×采样率 */
"""
import sys, os, struct

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(path)[0] + ".h"
    name = sys.argv[3] if len(sys.argv) > 3 else \
        "".join(c if c.isalnum() else "_"
                for c in os.path.splitext(os.path.basename(path))[0])
    if not name[0].isalpha():
        name = "snd_" + name

    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        print("错误: 不是 WAV 文件"); sys.exit(1)

    pos = 12
    fmt = None
    pcm = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (1 if size & 1 else 0)

    if fmt is None or pcm is None:
        print("错误: 缺少 fmt/data 块"); sys.exit(1)

    fmt_code, ch, rate, _, _, bits = struct.unpack("<HHIIHH", fmt[:16])
    if fmt_code != 1:
        print("错误: 仅支持 PCM(format=%d)" % fmt_code); sys.exit(1)
    if bits not in (8, 16):
        print("错误: 仅支持 8/16bit(实际 %d)" % bits); sys.exit(1)
    if rate != 16000:
        print("警告: 采样率 %dHz != 固件 16000Hz, 播放会变速. 建议: ffmpeg -i in.mp3 -ac 1 -ar 16000 -sample_fmt s16 out.wav" % rate)

    bytes_per = bits // 8
    frame_sz = bytes_per * ch
    n_frames = len(pcm) // frame_sz
    samples = []
    for i in range(n_frames):
        off = i * frame_sz
        if bytes_per == 2:
            v = struct.unpack_from("<h", pcm, off)[0]
        else:
            v = (pcm[off] - 128) * 256
        samples.append(v)

    lines = [
        "/* 由 tools/wav2c.py 生成: %s  %dHz %dbit %d声道  %.2f秒 */" % (
            os.path.basename(path), rate, bits, ch, n_frames / rate),
        "const int16_t %s[] = {" % name,
    ]
    per = 16
    for i in range(0, len(samples), per):
        lines.append("    " + ", ".join("0x%04X" % (s & 0xFFFF) for s in samples[i:i + per]) + ",")
    lines.append("};")
    lines.append("const uint32_t %s_frames = %d;   /* 帧数 = 秒数×采样率 */" % (name, n_frames))
    open(out, "w").write("\n".join(lines) + "\n")
    print("已生成 %s: %d 帧 (%.2f 秒), 数组 %d 字节" % (out, n_frames, n_frames / rate, len(samples) * 2))

main()
