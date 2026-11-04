# MangaMeeya 现代增强版 · Modern Enhanced Edition

**中文** | [English](#english)

基于 **MangaMeeya v7.4 中文版** 的非官方社区修复版，重点解决超大压缩包与超大图片在 64 位 Windows 上的报错。
An unofficial community patch for **MangaMeeya v7.4 (Chinese edition)**, focused on large archives and large images on 64-bit Windows.

> 本仓库不是官方版本；原始程序版权归原作者所有。详见 [NOTICE.md](NOTICE.md)。
> This is not an official release. Original program copyright belongs to its authors. See [NOTICE.md](NOTICE.md).

---

## 中文

### 简介

MangaMeeya 7.4 是 2006 年的 32 位程序。它在处理 **≥2GB 的 ZIP、≥4GB 的 ZIP64、以及解压后超过约 1.5GB 的图片**时会失败：

- 2GB 以上的 ZIP：只列出前 2GB 内的文件，之后全部丢失；
- 4GB 以上的 ZIP：直接提示“没有可以读入的文件”；
- 超大图片：标题显示 `Scale=0%`，图片不显示。

本仓库修复了这些问题，并提供**即开即用**的完整程序。

### 修复内容

#### 1. arc.dll — ZIP64 解析

| 问题 | 原版行为 | 修复 |
|---|---|---|
| 条目偏移 ≥ 2GB | ZIP64 extra field 被当作固定 24 字节读取；只写 8 字节偏移的包从第 2GB 后的条目开始错乱 | 按 ZIP64 规范逐字段解析（未压缩大小 / 压缩大小 / 本地头偏移） |
| 压缩包 ≥ 4GB | 只读普通 EOCD 的 32 位 CD 偏移，遇到 `0xFFFFFFFF` 不跟随 ZIP64 locator/EOCD | 解析 ZIP64 locator + ZIP64 EOCD，支持 64 位 CD 偏移与条目数 |
| >4GB 且带归档注释 | 扫描 EOCD 的分支同样不识别 ZIP64 | 在扫描分支增加 ZIP64 处理，并恢复注释读取位置 |
| 条目数 > 65535 | EOCD 计数字段 `0xFFFF` 被当成 65535 | 计数字段饱和时转入 ZIP64 EOCD |

补丁使用 3 个 5 字节 `jmp` 跳入 `.text` 尾部原有空白区，未修改 PE 头、节表、导入表或导出表。

#### 2. MangaMeeya.exe — 开启 LAA

设置 PE `FileHeader.Characteristics` 的 `IMAGE_FILE_LARGE_ADDRESS_AWARE`（`0x20`）位，让 32 位进程在 64 位 Windows 上使用完整 4GB 用户地址空间。

> 注意：LAA 位在 **FileHeader.Characteristics**，不在 DllCharacteristics。后者 `0x20` 是 `HIGH_ENTROPY_VA`，设置它不会开启 LAA。

效果：24bpp DIB 可分配上限从约 **1.45GB** 提升到约 **1.95GB**，正方形图片从约 22000×22000 提升到约 **26000×26000**。

### 已验证场景

| 场景 | 结果 |
|---|---|
| 2.2GB ZIP，图片位于 >2GB 偏移 | 286 个条目全部列出；>2GB 处图片正常解码 |
| 4.3GB ZIP（ZIP64 EOCD），图片位于 >4GB 偏移 | 555 个条目全部列出；>4GB 处图片正常解码 |
| 4.3GB 带归档注释 ZIP | 552 个条目全部列出并正常打开 |
| 含 4.3GB 单个条目（ZIP64 大小字段）+ 后续图片 | 条目大小 4,328,521,728 正确；后续图片正常 |
| 70000 条目 ZIP | ZIP64 计数路径生效（arc.dll 逐条查询为 O(n²)，整包列出极慢） |
| 普通小 ZIP / 混合非图片条目 ZIP | 回归正常 |
| 24000²、25000²、26000² PNG | 正常解码 |
| 1200×120000、60000×3000 PNG | 正常解码 |
| 20000² JPEG | 正常解码 |

### 已知限制

- **28000×28000（约 2.35GB DIB）及以上仍会失败。** 32 位进程的 4GB 地址空间被系统 DLL 分成约 1.7GB 和 2.0GB 两个空闲块，无法获得 >2GB 连续 DIB。这是 32 位架构限制；突破需要分块/缩放解码，本补丁未涉及。
- 只修改了 ZIP 解析；**RAR/LZH 未改动**，超大 RAR 未测试。
- 未修改 `SusiePlugin`、`AvisynthPlugin` 中的任何插件。

### 使用（即开即用）

**仓库根目录就是完整可运行版本**：克隆或解压后直接运行 `MangaMeeya.exe`，无需替换文件或安装依赖。

1. 解压到任意目录（避免需要管理员权限的路径）。
2. 运行 `MangaMeeya.exe`。
3. 本仓库**不包含** `MangaMeeya.ini`、`历史.lst` 等用户文件；首次运行会自动生成默认配置。

恢复原版：用 `original/` 中的两个未修改文件覆盖根目录同名文件（见 [original/README.md](original/README.md)）。

### 目录

```
MangaMeeya.exe        主程序（已开启 LAA）
arc.dll               压缩包插件（已打 ZIP64 补丁）
gif/jpg/png/jp2/pdf.dll
SusiePlugin/          原版 Susie 插件
AvisynthPlugin/       原版 Avisynth 插件
docs/                 原版说明
MangaMeeya 中文版说明.txt   原中文版作者说明
original/             未修改的原版 MangaMeeya.exe / arc.dll（对照与回退用）
patch-source/         补丁汇编源码、应用脚本与预编译补丁块
SHA256SUMS.txt        补丁版与原版二进制的 SHA-256
```

### 重新打补丁

见 [patch-source/README.md](patch-source/README.md)。需要 Python 3；重新汇编需要 NASM。

### 隐私

本仓库不包含任何用户配置、阅读历史、书架、收藏夹、播放列表或本机路径。`MangaMeeya.ini`、`*.lst` 已在 `.gitignore` 中排除。

### 版权与致谢

- 原版 MangaMeeya 作者及其维护者。
- 中文版作者 **Fenghou**（2006-07-22）。
- 本补丁由社区制作，与原作者无关联。
- `patch-source/` 下的汇编与 Python 脚本采用 MIT 许可；原始程序二进制不适用该许可。

---

<a id="english"></a>

## English

### Overview

MangaMeeya 7.4 is a 32-bit application from 2006. It fails on:

- ZIP archives **≥ 2GB** — only entries before the 2GB boundary are listed;
- ZIP64 archives **≥ 4GB** — the app reports “no readable files”;
- images whose decoded DIB exceeds roughly **1.5GB** — the title shows `Scale=0%` and nothing is displayed.

This repository fixes those issues and ships a **ready-to-run** build.

### Fixes

#### 1. arc.dll — ZIP64 parsing

| Problem | Original behavior | Fix |
|---|---|---|
| Entry offset ≥ 2GB | ZIP64 extra field was read as a fixed 24-byte block; archives that store only the 8-byte offset break from the first post-2GB entry | Parse the extra field field-by-field per the ZIP64 spec (uncompressed size / compressed size / local-header offset) |
| Archive ≥ 4GB | Only the 32-bit CD offset in the normal EOCD was used; `0xFFFFFFFF` was not resolved through the ZIP64 locator/EOCD | Parse the ZIP64 locator + ZIP64 EOCD; support 64-bit offsets and entry counts |
| >4GB with an archive comment | The EOCD-scanning branch also ignored ZIP64 | Added ZIP64 handling to the scan branch and restored the stream to the comment start |
| Entry count > 65535 | The saturated `0xFFFF` count was treated as 65535 | Fall through to the ZIP64 EOCD when the count is saturated |

The patch uses three 5-byte `jmp`s into unused space at the end of `.text`. No PE headers, section table, import table, or export table were modified.

#### 2. MangaMeeya.exe — Large Address Aware

Sets `IMAGE_FILE_LARGE_ADDRESS_AWARE` (`0x20`) in the PE **FileHeader.Characteristics**, giving the 32-bit process the full 4GB user address space on 64-bit Windows.

> Note: the LAA bit lives in **FileHeader.Characteristics**, not in DllCharacteristics. The `0x20` bit in DllCharacteristics is `HIGH_ENTROPY_VA` and does not enable LAA.

Result: the maximum 24bpp DIB grows from about **1.45GB** to about **1.95GB**, i.e. square images from about 22000×22000 to about **26000×26000**.

### Verified

| Scenario | Result |
|---|---|
| 2.2GB ZIP, images beyond the 2GB offset | All 286 entries listed; images at >2GB decode normally |
| 4.3GB ZIP (ZIP64 EOCD), images beyond the 4GB offset | All 555 entries listed; images at >4GB decode normally |
| 4.3GB ZIP with an archive comment | All 552 entries listed and opened normally |
| ZIP containing a 4.3GB single entry (ZIP64 size fields) plus trailing images | Entry size 4,328,521,728 correct; trailing images decode normally |
| 70000-entry ZIP | ZIP64 count path works (arc.dll item lookup is O(n²), so full listing is very slow) |
| Ordinary small ZIPs / mixed non-image entries | Regression pass |
| 24000², 25000², 26000² PNG | Decode normally |
| 1200×120000, 60000×3000 PNG | Decode normally |
| 20000² JPEG | Decodes normally |

### Known limitations

- **28000×28000 (about 2.35GB DIB) and larger still fail.** The 4GB address space of a 32-bit process is split by system DLLs into roughly 1.7GB and 2.0GB free blocks, so a contiguous DIB larger than 2GB cannot be allocated. This is a 32-bit architecture limit; overcoming it requires tiled/scaled decoding, which this patch does not attempt.
- Only ZIP parsing is patched. **RAR/LZH are unchanged**; large RAR archives were not tested.
- No plugin in `SusiePlugin/` or `AvisynthPlugin/` was modified.

### Usage (ready to run)

**The repository root is the complete runnable build**: clone or extract, then launch `MangaMeeya.exe`. No file replacement or dependency installation is needed.

1. Extract to any folder (avoid paths that require administrator rights).
2. Run `MangaMeeya.exe`.
3. No `MangaMeeya.ini` or `历史.lst` is included; the app creates default configuration on first run.

To revert to the original behavior, overwrite the two files in the root with the unmodified copies from `original/` (see [original/README.md](original/README.md)).

### Layout

```
MangaMeeya.exe        main executable (LAA enabled)
arc.dll               archive plugin (ZIP64 patched)
gif/jpg/png/jp2/pdf.dll
SusiePlugin/          original Susie plugins
AvisynthPlugin/       original Avisynth plugins
docs/                 original documentation
MangaMeeya 中文版说明.txt   original Chinese edition readme
original/             unmodified MangaMeeya.exe / arc.dll (reference and rollback)
patch-source/         patch assembly, apply scripts and prebuilt blobs
SHA256SUMS.txt        SHA-256 of patched and original binaries
```

### Rebuilding the patches

See [patch-source/README.md](patch-source/README.md). Python 3 is required; NASM is required only to reassemble the `.asm` files.

### Privacy

This repository contains no user configuration, reading history, bookshelf, favorites, playlist, or local paths. `MangaMeeya.ini` and `*.lst` are excluded by `.gitignore`.

### Credits & disclaimer

- The original MangaMeeya authors and maintainers.
- Chinese edition by **Fenghou** (2006-07-22).
- This patch is community-made and is not affiliated with the original authors.
- The assembly and Python scripts under `patch-source/` are released under the MIT license; the original program binaries are not covered by that license.

---

*MangaMeeya 为免费软件但未开源；本仓库按社区惯例分发补丁版与原版二进制，若权利人提出要求将移除二进制、仅保留补丁源码。*
*MangaMeeya is freeware but not open source. This repository redistributes the patched and original binaries under the customary community assumption; binaries will be removed upon request from the rights holder, leaving only the patch source.*
