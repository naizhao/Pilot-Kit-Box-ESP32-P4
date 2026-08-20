#!/usr/bin/env python3
"""把航空数据库的 AES-128 密钥从环境变量生成成头文件，让它不再躺在源码里。

## 背景

密钥原先直接写死在 `firmware/main/pk_aero_db.c` 和 `pk_aero_span.c` 里
（两份逐字节相同的副本），随公开仓库一起泄露过。2026-08-20 轮换并改成本方案。

原来的代码注释很诚实：拆成两半异或存放「只是防轻易提取的混淆——真正的保护是
量产前打开 flash encryption + secure boot」。这个判断没错，但它保护不了**源码
仓库本身是公开的**这件事：`strings` 提不到，`git clone` 一样能读到。

## 做法

    .env 的 PK_AERO_KEY  ──▶  本脚本  ──▶  main/pk_aero_key.h（已 gitignore）
                                              │
                                    两个 .c 都 include 它

- 源码树里**一个密钥字节都没有**
- 只有一份定义，消除了原来两处副本不同步的风险
- 仍然拆成两半异或存放，保留对 `strings` 的混淆（这一层没有变弱）

## 没有密钥时也要能构建

外部开发者 clone 下来是没有 `.env` 的。这时用一个**公开的占位密钥**，并在
编译期打警告。用占位密钥编出来的固件能跑，但解不开官方分发的数据文件——
这正是期望的行为，而不是让构建直接失败。

## 用法

    gen_aero_key.py [输出路径]        默认 ../main/pk_aero_key.h

密钥来源按顺序：环境变量 PK_AERO_KEY → 仓库根的 .env → 占位密钥
"""
import os
import pathlib
import re
import secrets
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent / "main" / "pk_aero_key.h"

# 公开占位密钥。**故意**是个显眼的固定值，让人一眼看出"这不是真密钥"。
# 不用随机值：随机的话每次构建产物都不同，还会让人以为自己配对了密钥。
PLACEHOLDER = "00112233445566778899AABBCCDDEEFF"


def read_key():
    k = os.environ.get("PK_AERO_KEY", "").strip()
    if k:
        return k, "环境变量 PK_AERO_KEY"
    env = REPO / ".env"
    if env.exists():
        m = re.search(r"^\s*PK_AERO_KEY\s*=\s*(\S+)", env.read_text(), re.M)
        if m:
            return m.group(1).strip().strip('"\''), ".env"
    return PLACEHOLDER, "占位密钥"


key, src = read_key()
if not re.fullmatch(r"[0-9A-Fa-f]{32}", key):
    sys.exit(f"❌ PK_AERO_KEY 必须是 32 个十六进制字符（16 字节），当前来自{src}、"
             f"长度 {len(key)}")

kb = bytes.fromhex(key)
is_placeholder = key.upper() == PLACEHOLDER

# 拆成两半异或。mask 每次生成都重随机——同一把密钥在不同构建产物里的字节形态
# 不同，让批量特征匹配更费劲。解出来的 kb 当然还是同一个。
mask = secrets.token_bytes(16)
a = bytes(x ^ y for x, y in zip(kb, mask))


def carr(bs):
    rows = [", ".join(f"0x{b:02X}" for b in bs[i:i + 8]) for i in range(0, 16, 8)]
    return "\n".join(f"        {r}," for r in rows)


OUT.parent.mkdir(parents=True, exist_ok=True)

# ⚠️ 这两行必须先算成变量，不能直接塞进下面的 f-string：
# f-string 表达式里出现反斜杠是 Python 3.12 才放开的，而且引号嵌套一层就绕晕。
# （刚在 tools/gen_bom_smt.py 上栽过同一个坑，这里不重复。）
_warn = ('#warning "PK_AERO_KEY not configured - using public placeholder key, '
         'cannot decrypt official data files"') if is_placeholder else \
        "/* 使用已配置的真实密钥 */"
_ph = 1 if is_placeholder else 0

OUT.write_text(f'''/* 自动生成，请勿手改，也请勿提交——本文件已在 .gitignore 里。
 *
 * 由 firmware/scripts/gen_aero_key.py 从 {src} 生成。
 * 要换密钥：改 .env 的 PK_AERO_KEY，重新构建即可。
 *
 * 拆成两半异或存放，避免整串密钥出现在 .rodata 里被 `strings` 直接捞走。
 * 这只是混淆，不是保护——真正的保护是量产时打开 flash encryption +
 * secure boot。但至少密钥不再随源码进公开仓库。
 */
#pragma once
#include <stdint.h>

{_warn}
#define PK_AERO_KEY_IS_PLACEHOLDER {_ph}

static inline void pk_aero_key_assemble(uint8_t out[16])
{{
    static const uint8_t a[16] = {{
{carr(a)}
    }};
    static const uint8_t b[16] = {{
{carr(mask)}
    }};
    for (int i = 0; i < 16; i++) out[i] = a[i] ^ b[i];
}}
''')

tag = "⚠️ 占位密钥（外部构建会走这条路）" if is_placeholder else f"指纹 {key[:4]}…{key[-4:]}"
print(f"✓ {OUT.relative_to(REPO)}  ←  {src}   {tag}")
