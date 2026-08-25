/* 全量16x16字库 访问逻辑(二分查找/UTF-8解码/缺字方框)
 * 数据本体在 lcd_font_data.c(生成自 Simplified-Chinese-Characters-main/国标.TXT) */
#include <stdint.h>
#include "lcdchinese.h"


/* 取一个 UTF-8 字符的码点(ASCII 1字节 / 中文 3字节) */
static uint32_t lcd_utf8_cp(const char *ch)
{
    uint8_t b0 = (uint8_t)ch[0];
    if (b0 < 0x80)
    {
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0)
    {
        return ((b0 & 0x1F) << 6) | ((uint8_t)ch[1] & 0x3F);
    }
    if ((b0 & 0xF0) == 0xE0)
    {
        return ((b0 & 0x0F) << 12) | (((uint8_t)ch[1] & 0x3F) << 6) | ((uint8_t)ch[2] & 0x3F);
    }
    return 0;
}

/* 查无此字时的默认图形(方框内问号) */
static const uint8_t lcd_font_box[32] = {
    0xFF, 0x01, 0x01, 0x01, 0x31, 0x09, 0x09, 0x09, 0x09, 0x89, 0x71, 0x01, 0x01, 0x01, 0x01, 0xFF,
    0xFF, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x96, 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFF,
};

/* 查字模: 全字库二分查找, 未命中返回默认方框 */
const uint8_t *lcd_font_get(const char *ch)
{
    uint32_t cp = lcd_utf8_cp(ch);
    int16_t lo = 0, hi = (int16_t)(lcd_font_count - 1);
    while (lo <= hi)
    {
        int16_t mid = (lo + hi) >> 1;
        if (lcd_font_codes[mid] < cp)
        {
            lo = mid + 1;
        }
        else if (lcd_font_codes[mid] > cp)
        {
            hi = mid - 1;
        }
        else
        {
            return lcd_font_data[mid];
        }
    }
    return lcd_font_box;
}
