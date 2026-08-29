/* OneNET 安全鉴权 token 生成(HMAC-SHA256), 设备级(password 字段)
 * 规则(新版 DMP 平台, 文档 1486《Token算法》2026-01 更新):
 *   token = version=2018-10-31&res={URL编码}&et={过期秒}&method=sha256&sign={URL编码}
 *     res  = products/{pid}/devices/{name}
 *     签名明文按参数名字符序仅取 value, '\n' 分隔:
 *     StringForSignature = et + "\n" + method + "\n" + res + "\n" + version
 *     sign = base64(HMAC-SHA256(base64decode(key), StringForSignature))
 * 与 tools/onenet_token.py 同一套规则(真机联调前可先脚本+MQTTX 验证三元组);
 * 平台相关逻辑与 cloud_onenet.c 同属适配层 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cloud_priv.h"

/* RFC3986 保留字符全部编码(OneNET 要求 res/sign 均为 URL 编码), 0=成功 */
static int url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in; in++)
    {
        unsigned char c = (unsigned char)*in;
        uint8_t safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        size_t need = safe ? 1 : 3;
        if (o + need + 1 > n) return -1;
        if (safe)
        {
            out[o++] = (char)c;
        }
        else
        {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
    return 0;
}

int cloud_token_gen(const char *pid, const char *name, const char *key_b64,
                    char *out, size_t outn)
{
    char res[112], plain[192], enc_res[340], enc_sign[160];
    unsigned char key[64], mac[32], sign_b64[48];
    size_t key_len = 0, mac_len = 0;

    if (!pid || !name || !key_b64 || !out || outn == 0) return -1;
    int n = snprintf(res, sizeof(res), "products/%s/devices/%s", pid, name);
    if (n < 0 || n >= (int)sizeof(res)) return -1;

    /* OneNET 下发的 AccessKey 是 base64, HMAC 前先解码 */
    if (mbedtls_base64_decode(key, sizeof(key), &key_len,
                              (const unsigned char *)key_b64, strlen(key_b64)) != 0)
        return -1;

    /* et: 时钟已同步用 当前+1年; 未同步(RTC 无效且未校时)退回 2030 固定值 ——
     * et 只参与签名并由服务端校验"未过期", 两种取法都与本机时钟精度无关 */
    time_t now = time(NULL);
    unsigned long long et = (now > 1700000000)
                                ? (unsigned long long)now + 365ULL * 86400ULL
                                : 1893456000ULL;

    /* 新版 DMP 平台签名明文: 按参数名字符序仅取 value, '\n' 分隔(文档 1486《Token算法》) */
    n = snprintf(plain, sizeof(plain), "%llu\nsha256\n%s\n2018-10-31", et, res);
    if (n < 0 || n >= (int)sizeof(plain)) return -1;
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, key, key_len,
                               (const unsigned char *)plain, (size_t)n, mac) != 0)
        return -1;
    if (mbedtls_base64_encode(sign_b64, sizeof(sign_b64), &mac_len, mac, sizeof(mac)) != 0)
        return -1;

    if (url_encode(res, enc_res, sizeof(enc_res)) != 0) return -1;
    if (url_encode((const char *)sign_b64, enc_sign, sizeof(enc_sign)) != 0) return -1;
    n = snprintf(out, outn, "version=2018-10-31&res=%s&et=%llu&method=sha256&sign=%s",
                 enc_res, et, enc_sign);
    if (n < 0 || n >= (int)outn) return -1;
    return 0;
}
