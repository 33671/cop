#include <stdint.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
void sanitize_utf8(uint8_t *data, size_t len) {
    if (!data) return;
    size_t i = 0;
    while (i < len) {
        uint8_t b = data[i];

        // ASCII 直接保留
        if (b <= 0x7F) {
            i++;
            continue;
        }

        // 确定该首字节期望的后续字节数，以及要求的最小码点（防超长编码）
        int extra;
        uint32_t min_codepoint;

        if (b >= 0xC2 && b <= 0xDF) {
            extra = 1; min_codepoint = 0x80;
        } else if (b == 0xE0) {
            extra = 2; min_codepoint = 0x800;
        } else if (b >= 0xE1 && b <= 0xEC) {
            extra = 2; min_codepoint = 0x1000;
        } else if (b == 0xED) {
            extra = 2; min_codepoint = 0xD000;  // 后面会单独拒绝代理区
        } else if (b >= 0xEE && b <= 0xEF) {
            extra = 2; min_codepoint = 0xF000;
        } else if (b == 0xF0) {
            extra = 3; min_codepoint = 0x10000;
        } else if (b >= 0xF1 && b <= 0xF3) {
            extra = 3; min_codepoint = 0x40000;
        } else if (b == 0xF4) {
            extra = 3; min_codepoint = 0x100000;
        } else {
            // 非法首字节（0xC0, 0xC1, 0xF5-0xFF）
            data[i] = '?';
            i++;
            continue;
        }

        // 剩余长度不足则视为非法序列，只替换当前首字节
        if (i + extra >= len) {
            data[i] = '?';
            i++;
            continue;
        }

        // 提取首字节中携带的码点高位（掩码取决于 extra）
        uint32_t cp = b & ((1 << (6 - extra)) - 1);
        bool ok = true;

        // 检查所有后续字节并组装码点
        for (int j = 0; j < extra; j++) {
            uint8_t nb = data[i + 1 + j];
            if ((nb & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (nb & 0x3F);
        }

        if (!ok) {
            // 后续字节非法：只把当前首字节替换，后续字节留给下一次循环处理
            data[i] = '?';
            i++;
            continue;
        }

        // 码点范围校验
        if (cp < min_codepoint) {                     // 超长编码
            data[i] = '?';
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {    // 代理对
            data[i] = '?';
        } else if (cp > 0x10FFFF) {                    // 超过最大合法码点
            data[i] = '?';
        } else {
            // 完全合法，跳过整个多字节序列
            i += 1 + extra;
            continue;
        }

        i++; // 非法序列已替换首字节，前进一个字节
    }
}

int utf8_char_width(const char *s, int *bytes) {
    unsigned char c = (unsigned char)s[0];
    uint32_t codepoint;
    int len;

    // 解析 UTF-8 首字节
    if ((c & 0x80) == 0) { len = 1; codepoint = c; }
    else if ((c & 0xE0) == 0xC0) { len = 2; codepoint = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; codepoint = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; codepoint = c & 0x07; }
    else { *bytes = 1; return 0; }  // 非法字节

    for (int i = 1; i < len; ++i) {
        codepoint = (codepoint << 6) | ((unsigned char)s[i] & 0x3F);
    }
    *bytes = len;

    // 零宽字符
    if (codepoint == 0x200B || codepoint == 0x200C || codepoint == 0x200D ||
        codepoint == 0xFEFF) return 0;
    // 组合符号（粗略范围）
    if (codepoint >= 0x0300 && codepoint <= 0x036F) return 0;
    if (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) return 0;
    if (codepoint >= 0x20D0 && codepoint <= 0x20FF) return 0;
    if (codepoint >= 0xFE00 && codepoint <= 0xFE0F) return 0; // 异体选择符

    // 控制字符
    if (codepoint < 0x20) return 0;

    // 全角字符范围（East Asian Wide / Fullwidth）
    if ((codepoint >= 0x1100 && codepoint <= 0x115F) ||  // 韩文
        (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||  // CJK 及其它
        (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||  // 韩文音节
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||  // CJK 兼容
        (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
        (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
        (codepoint >= 0xFF01 && codepoint <= 0xFF60) ||
        (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||
        (codepoint >= 0x1F300 && codepoint <= 0x1F64F) || // 杂项符号（很多 emoji 可视为宽 2）
        (codepoint >= 0x1F680 && codepoint <= 0x1F6FF) ||
        (codepoint >= 0x2600  && codepoint <= 0x26FF)  ||
        (codepoint >= 0x2700  && codepoint <= 0x27BF)  ||
        (codepoint >= 0x1F900 && codepoint <= 0x1F9FF) ||
        (codepoint >= 0x1FA00 && codepoint <= 0x1FA6F) ||
        (codepoint >= 0x1FA70 && codepoint <= 0x1FAFF))
        return 2;

    return 1;  // 其余半角
}

int utf8_string_width(const char *s) {
    int total = 0;
    while (*s) {
        int bytes;
        int w = utf8_char_width(s, &bytes);
        if (w > 0) total += w;
        s += bytes;
    }
    return total;
}