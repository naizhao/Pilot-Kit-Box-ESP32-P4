# firmware/scripts —— i18n 词条与字库生成

英文版：[`README.md`](README.md)


本目录放固件的代码/资源生成脚本。本文说明**多语言词条**这条链路：
词条源 → 批量翻译 → 重生成字库 → 编进固件。

```
i18n_catalog.py            词条唯一真源（KEY + 各语言译文）
      │
      ├── translate_catalog.py    ① 批量补齐缺失语言的译文（调 LLM，回填本文件）
      │
      ├── gen_i18n_assets.py      ② 生成 C 查表（**只有**这一样）
      │                              → firmware/main/i18n_catalog.{h,c}
      │
      ├── gen_pfd_aa_font.py      ③ 生成屏上真正在用的字形（含 CJK 子集）
      │                              → firmware/main/pfd_aa_font.c
      │
      └── gen_lv_font.py          ④ 生成 LVGL 控件用的字体（toast 等）
                                     → firmware/main/lv_font_zh.c
```

> **字体管线有三套，不是一套。**②③④ 各管一摊，漏跑哪一个的症状都不一样：
> 漏 ③ 是**直绘页面**（PFD/设置/关于…）上的汉字整片空白（查表落空只推进
> 宽度、不画），漏 ④ 是 LVGL 控件（toast）变豆腐块。②不产任何字形——
> 2026-08-03 之前它还生成 `text_font_cjk*.{h,c}`，那两档随最后一个调用者
> `text.c` 一起删了（见 `gen_i18n_assets.py` 文件头）。

> **注意**：`translate_catalog.py` 按要求列在 `.gitignore` 里，只在本地保留，
> 不随仓库分发（脚本本身不含任何密钥，见 `.gitignore` 里那段说明）。
> 克隆仓库后如果找不到这个文件，找维护者要一份，或按本文的接口说明重写：
> 它只读 `i18n_catalog.py`、调一个 OpenAI 兼容端点、把译文插回原文件。

---

## 1. 配 .env

翻译脚本**不含任何密钥**，key 只从环境变量或仓库根 `.env` 读。
**变量名和 5 级 provider fallback 沿用我们其它项目里同类翻译脚本的既有约定**
（不是这里临时发明的一套），同一份 `.env` 跨仓库通用，维护的人不用学两套。

```bash
cp .env.example .env      # 在仓库根目录
# 编辑 .env，填 ZHIPU_API_KEY 和/或 GEMINI_API_KEY
```

5 级 provider chain（前一级额度耗尽/限流后**自动切下一级**，缺 key 的级别跳过）：

| 级 | provider | 模型（可覆盖） | key |
|---|---|---|---|
| 1 | 智谱 GLM Coding Plan（付费，主力） | `ZHIPU_CODING_MODEL` = `glm-5.2` | `ZHIPU_API_KEY` |
| 2 | Google Gemma（免费，RPD 宽） | `GEMMA_MODEL` = `gemma-4-31b-it` | `GEMINI_API_KEY` |
| 3 | Google Gemini flash（免费，质量顶） | `GEMINI_TIER2_MODEL` = `gemini-3.5-flash` | `GEMINI_API_KEY` |
| 4 | Google Gemini flash（免费，更快） | `GEMINI_TIER3_MODEL` = `gemini-3.6-flash` | `GEMINI_API_KEY` |
| 5 | 智谱免费兜底 | `ZHIPU_FREE_MODEL` = `glm-4.5-flash` | `ZHIPU_API_KEY` |

只填一个 key 也能跑：只填 `ZHIPU_API_KEY` 就用第 1、5 级，只填 `GEMINI_API_KEY`
就用第 2、3、4 级。**两个都不填也能跑 `--dry-run`**（provider chain 是惰性构造的）。

其它可选项：`LLM_TIMEOUT`（默认 300）、`LLM_MAX_TOKENS`（默认 8192）、
各级 `*_BASE_URL`（把 `GEMINI_BASE_URL` 指到 `http://127.0.0.1:11580/v1`
就统一走本机 ServBay AI Gateway）。完整清单见 `.env.example`。

想临时钉死一个模型、绕开 fallback：`--model` / `--base-url`（逃生口，一旦指定
就只用这一个 provider）。

`.env` / `.env.local` 已在 `.gitignore` 里。**任何时候都不要把真实 key 写进
`.env.example`、脚本、文档或提交信息。**

---

## 2. 加一种新语言

以日语 `ja` 为例，四步：

### ① 先 dry-run 看要翻什么

```bash
python3 firmware/scripts/translate_catalog.py --lang ja --dry-run
```

不发任何请求、不写任何文件，打印三份清单：

- **已有译文**：一个字都不会碰；
- **需翻译**：本次要送 LLM 的条目，以及每条命中的受保护术语；
- **术语原样保留**：直接复制英文，不送翻译（见第 3 节）。

### ② 真跑

```bash
python3 firmware/scripts/translate_catalog.py --lang ja
```

- **分批**：默认每批 ≤25 条、≤4000 字符（`--batch-size` / `--batch-max-chars`）。
- **限速**：批与批之间默认停 1 秒（`--sleep`，0 = 不等）。免费级 Gemini 的 RPM
  很紧，连发会直接吃 429 把整级熔断掉。
- **重试 / fallback**：单批失败重试 2 次、指数退避（`--retries`）；
  遇到额度耗尽或限流**不算失败**，当前 provider 就地熔断、自动换下一级接着翻。
  五级全熔断才落盘退出（退出码 2）。
- **断点续跑**：每批成功后立刻把结果写进
  `firmware/scripts/.i18n_translate_cache/<lang>.json`。额度耗尽、网络断、
  Ctrl-C 之后**重跑同一条命令**即可接着翻，已翻的不会重复花钱。全部回填成功后
  该检查点自动删除。
- **逐条校验才收下**（见第 4 节）：占位符对不上、术语被翻掉、单行翻出换行的
  条目直接驳回，不写进 catalog，留给下次重跑。
- **回填**：按行插进 `i18n_catalog.py` 对应条目，注释、缩进、排版一律不动；
  同时把新语言加进 `LANGS`（不想自动加就用 `--no-update-langs`）。
- **写盘前自检**：重新解析文件并逐条断言——条目数没变、其它语言的原值没被
  动过、目标语言的值等于本次译文。任一条不过就放弃写入并报错。

想先小范围试水：`--limit 10` 只送前 10 条去翻译（术语条目不受此限，
它们不花额度，照样全部回填）。
只想把术语条目（英文原样）补上、完全不调 API：`--verbatim-only`。

多语言一次跑：`--lang ja --lang ko` 或 `--lang ja,ko`。

### ③ 重生成词条表与字库（**三条都不能跳过**）

```bash
python3 firmware/scripts/gen_i18n_assets.py    # 词条表
python3 firmware/scripts/gen_pfd_aa_font.py    # 直绘页面的字形（含 CJK）
python3 firmware/scripts/gen_lv_font.py        # LVGL 控件的字体
```

字模是**按 catalog 里实际出现的字符**做子集的
（`gen_pfd_aa_font.py` 的 `collect_cjk_codes()`，另外并进八个方向箭头）。
新语言的字形不在子集里，屏上就是空白。后两条需要 ImageMagick 的 `magick`
命令与 `fontTools`。

### ④ 目测检查

用 `sim/` 或直接上机看一遍：

- 有没有渲染成空白的字。`pfd_aa_text.c` 查表落空时**只推进宽度、不画**
  （宁可留白也不画错位字形），所以症状是"少了几个字"而不是豆腐块；
- 列宽有没有被撑破。非 ASCII 码位一律按该档的 `PK_AA_*_CJK_W` 宽格排版——
  西里尔字母、带音标的拉丁字母（`é` / `ü` / `ã`）也走这条宽格路径，不是只有
  汉字。列表页的列宽有一组 `_Static_assert` 钉着（见 `adsb_list.c` 顶部），
  撑破了编译就会报。

---

## 3. 术语不译表在哪

两层，都在 `translate_catalog.py` 里：

1. **按条目判定（主）**——`is_verbatim_entry()`：某条的 `zh` 与 `en` **完全相同**，
   说明人工已经判定「这条是术语，不译」，于是对**所有**语言原样保留英文。
   当前 catalog 里有 24 条这样的词条（QNH / IMU / PFD / ALT / TRK / GS / V/S /
   ICAO / ESP-IDF / LVGL / microSD …）。

   为什么按条目而不是按词面：同一个词面在不同语境里处置不同。
   `DIAG_CARD_IMU` 是 `en=IMU / zh=IMU`（诊断卡片标题，不译），
   而 `ABOUT_IMU` 是 `en=IMU / zh=姿态`（关于页的说明行，要译）。
   按词面一刀切会把后者也变成 `IMU`，覆盖掉人工决策。

2. **词面表 `KEEP_VERBATIM_TERMS`（辅）**——只做两件事：

   - 给**还没有 `zh` 译文**的新条目兜底（整条正好是一个术语 → 不译）；
   - **整句里的术语保护**：把句子里命中的术语按条塞进 prompt 的
     `keep_verbatim` 字段，要求模型原样保留。例如 `"QNH REF"` 会提示保留
     `QNH`，`"int WDT"` 会提示保留 `WDT`。

   匹配带字母数字边界，所以 `SETTINGS` 不会误命中 `GS`、`left` 不会误命中 `ft`。
   整条正好等于某个术语时**不**发提示——能走到翻译流程说明人工已判定它要译
   （如 `LIST_D_SQUAWK` 的 `zh` 是「应答机」），再提示 keep verbatim 就自相矛盾。

翻译原则的原始出处在 `i18n_catalog.py:500-509` 的注释里（航电术语不译 /
状态描述要译 / 带数字的行不做整句格式串）。改动术语策略前先读那段。

---

## 4. 译文校验（模型返回后、写进 catalog 之前）

每一条译文都要过 `validate_value()` 才会被收下。硬规矩三条，任一条不过就**驳回
这一条**（不是整批），驳回的留在检查点外面，下次重跑再翻：

| 检查 | 为什么 |
|---|---|
| printf 占位符集合必须与源一致（`%d` / `%s` / `%.1f` / `%02d`） | C 侧 `snprintf` 的参数写死了，占位符多一个少一个就是**越界读栈** |
| `keep_verbatim` 里的术语必须原样出现在译文里 | 模型偶尔会把 `QNH REF` 整句意译掉，航电缩写不能翻 |
| 源是单行时译文不许有换行 | 屏上是定高单行控件，`\n` 会画成方框 |

外加一条软规矩：译文比源长过 `--max-len-ratio`（默认 1.6）倍时**只告警不驳回**
——德语/俄语天然就长，一刀切会把好译文也扔掉，但值得上机看一眼列宽。

收下之前还会先做一次清洗（`sanitize_value()`）：剥掉代码围栏 ` ```…``` ` 和
首尾成对引号，这是模型最爱多加的两样东西。

> 这一套「响应必须通过校验，否则视为失败」的思路来自我们另一个项目里翻译
> release note 的脚本（那份校验译文必须以 `##` 开头、必须含版本号、
> 只能有一个版本块）——模型偶尔会返回结构正确但内容跑偏的东西，
> 不校验就会静默污染产物。

---

## 5. 加词条（不涉及新语言）时

改 `i18n_catalog.py` 加 `("KEY", {"en": ..., "zh": ...})` 之后，**必须**重跑
第 2 节 ③ 那三条命令：漏 `gen_i18n_assets.py` 是新词条根本没有枚举值（编译期
报错，看得见）；漏 `gen_pfd_aa_font.py` 是新汉字不在字模子集里、屏上那几个字
直接消失（**静默**，只有肉眼看得出）。

新汉字如果全部已经在既有子集里（比如复用「校」「准」这种别处已经出现过的
字），字库文件重跑后不会变，`git diff` 是空的——这不是没跑成功。
