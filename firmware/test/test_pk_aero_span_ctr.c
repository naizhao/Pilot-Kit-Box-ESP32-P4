/* test_pk_aero_span_ctr.c — host proof：AES-CTR 随机解密的**块号算术**。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_ctr firmware/test/test_pk_aero_span_ctr.c && /tmp/test_ctr
 *
 * 为什么单独测这一条：窗口机制要从 payload 的任意偏移开始解密（W1.3），
 * 而整读路径（pk_aero_db.c）是从 payload 起点一路流下来的。两者必须在
 * 同一个偏移上给出同一个计数器，否则解出来是一段看不出错的乱字节——
 * 不会崩、不会报错，只会让机场坐标变成太平洋中间。
 *
 * 口径（pk_aero_reader.h 文件头 + pk_aero_db.c 的 iv 构造）：
 *   iv[0..7]  = header 偏移 24 的 nonce
 *   iv[8..15] = payload 内偏移 / 16，**大端**
 * pk_aero_db.c 起手是 memset(iv+8, 0, 8)，即 payload 偏移 0 → 块号 0，
 * 所以偏移 off 处的块号就是 off/16。下面把这条恒等式钉死。
 *
 * ⚠ 本测试只证算术。"解出来的字节确实等于整读路径的同一段"这件事在
 *   host 上做不了（真库 17 MB 且加密，且 PSA 依赖 IDF），证据在真机自检
 *   pk_win_selftest.inc 的 verify_against_full_db()——它拿窗口读出的机场
 *   记录和 pk_aero_db 全量路径逐字段对拍。
 */
#include <stdio.h>
#include <string.h>

#include "../main/pk_aero_span.h"

static int g_fail = 0;

static void chk(const char *what, bool cond)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fail++;
}

/* 参考实现：把 pk_aero_db.c 的"从 0 开始一路数块"这件事**真的数一遍**，
 * 而不是照抄被测函数的公式——照抄就只能证明"我抄对了我自己"。 */
static void ref_iv(const uint8_t nonce[8], uint32_t off, uint8_t out[16])
{
    uint8_t iv[16];
    memcpy(iv, nonce, 8);
    memset(iv + 8, 0, 8);              /* pk_aero_db.c:201 —— 块计数从 0 起 */
    for (uint32_t at = 0; at < off; at += 16) {
        /* 大端 +1（CTR 模式每处理一个 16 B 块，计数器加一） */
        for (int i = 15; i >= 8; i--) {
            if (++iv[i] != 0) break;
        }
    }
    memcpy(out, iv, 16);
}

int main(void)
{
    printf("== pk_aero_span AES-CTR 块号算术 host test ==\n");

    const uint8_t nonce[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x23, 0x45, 0x67 };

    /* 1. nonce 原样落在高 64 位 */
    uint8_t iv[16];
    pk_aero_span_ctr_iv(nonce, 0, iv);
    chk("偏移 0 时 nonce 原样在 iv[0..7]", memcmp(iv, nonce, 8) == 0);
    bool zero = true;
    for (int i = 8; i < 16; i++) if (iv[i] != 0) zero = false;
    chk("偏移 0 时块号全 0（与整读路径起手一致）", zero);

    /* 2. 与"从 0 数上来"的参考实现逐个对拍（覆盖进位、跨字节） */
    const uint32_t offs[] = {
        0, 16, 32, 48, 256, 4096, 4096 * 255, 4096 * 256,
        16 * 255, 16 * 256, 16 * 65535, 16 * 65536,
        1048576, 8388608, 16777216, 0x00FFFFF0u,
    };
    bool all_ok = true;
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        uint8_t got[16], want[16];
        pk_aero_span_ctr_iv(nonce, offs[i], got);
        ref_iv(nonce, offs[i], want);
        if (memcmp(got, want, 16) != 0) {
            printf("  [FAIL] off=%lu\n", (unsigned long)offs[i]);
            all_ok = false;
        }
    }
    chk("16 个偏移与「从 0 数块」的参考实现全等（含进位）", all_ok);

    /* 3. 大端序：块号 1 落在 iv[15]，不是 iv[8] */
    pk_aero_span_ctr_iv(nonce, 16, iv);
    chk("块号 1 → iv[15]==1（大端，不是小端）",
        iv[15] == 1 && iv[8] == 0 && iv[14] == 0);
    pk_aero_span_ctr_iv(nonce, 16 * 256, iv);
    chk("块号 256 → iv[14]==1 且 iv[15]==0", iv[14] == 1 && iv[15] == 0);

    /* 4. 同一块内的不同偏移给出同一个 IV（调用方负责向下对齐到块边界，
     *    多读的那 ≤15 B 前缀由调用方丢掉——这里把这条前提也钉住） */
    uint8_t a[16], b[16];
    pk_aero_span_ctr_iv(nonce, 4096, a);
    pk_aero_span_ctr_iv(nonce, 4096 + 15, b);
    chk("同一 16 B 块内的偏移给出同一个 IV（对齐前提）",
        memcmp(a, b, 16) == 0);
    pk_aero_span_ctr_iv(nonce, 4096 + 16, b);
    chk("下一个块的 IV 不同", memcmp(a, b, 16) != 0);

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL PASS" : "FAILURES", g_fail);
    return g_fail == 0 ? 0 : 1;
}
