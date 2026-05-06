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
