# MangaMeeya 现代增强版 · Modern Enhanced Edition

**中文** | [English](#english)

基于 **MangaMeeya v7.4 中文版** 的非官方社区修复版，重点解决超大压缩包、超大图片，以及特殊字符文件名在 64 位 Windows 上的报错。
An unofficial community patch for **MangaMeeya v7.4 (Chinese edition)**, focused on large archives, large images, and file names containing characters the ANSI code page cannot represent on 64-bit Windows.

> 本仓库不是官方版本；原始程序版权归原作者所有。详见 [NOTICE.md](NOTICE.md)。
> This is not an official release. Original program copyright belongs to its authors. See [NOTICE.md](NOTICE.md).

---

## 中文

### 简介

MangaMeeya 7.4 是 2006 年的 32 位程序。它在以下几类场景会失败：

- 2GB 以上的 ZIP：只列出前 2GB 内的文件，之后全部丢失；
- 4GB 以上的 ZIP：直接提示“没有可以读入的文件”；
- 超大图片：标题显示 `Scale=0%`，图片不显示；
- 文件名含当前系统代码页无法表示的字符（例如简体中文 Windows 下的日文「・」U+30FB）：字符被转换成 `?`，程序提示“无效路径”，文件无法打开；
- 以 `.txt` 文本作为页面的压缩包（例如带 BepInEx 本地化文本的模组包）在“预读 2 页以上”（`PrepageForwardNum=2`）时直接崩溃退出（0xC0000005）。

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

#### 3. mmunicode.dll — 特殊字符文件名兼容

MangaMeeya 的老代码只调用 ANSI 版本（A 后缀）的 Win32 API。文件名中凡是在当前 ANSI 代码页（简体中文系统为 CP936）中无法表示的字符，都会在转换时变成 `?`。例如日文片假名中点「・」（U+30FB）在 CP936 下不可表示，`CreateFileA` 因此返回 `ERROR_INVALID_NAME`，程序显示“无效路径”。

修复方式是一个随程序加载的小型兼容层 `mmunicode.dll`：

- 在主程序导入表中新增一项 `mmunicode.dll!MMUInit`，由 Windows 加载器在程序启动前加载；
- 兼容层在进程内挂钩主程序、`arc.dll` 及 Susie 图片插件的导入表（IAT），覆盖 `CreateFileA`、`GetFileAttributesA`、`GetFileAttributesExA`、`SetCurrentDirectoryA`、`DeleteFileA`、`MoveFileA`、`CopyFileA`、`CreateDirectoryA`、`RemoveDirectoryA`、`GetShortPathNameA`、`FindFirstFileA/FindNextFileA`；
- 原 ANSI 调用失败且路径中含 `?` 时，兼容层从根目录开始逐级扫描父目录，把每个条目的宽字符名按同样规则转换成 ANSI（有损）后与请求中的名字比对，还原真实 Unicode 路径，再用 W 版本 API 重试；
- 扫描 ANSI 路径时按当前代码页的双字节规则逐字符前进，不会把 GBK/Shift-JIS 双字节字符的尾字节 `0x5C`（`\`）误当成目录分隔符（例如 `診` 的 GBK 编码是 `D4 5C`，旧版本解析这类文件名会错位）；
- 不修改入口点、不改动任何原有代码字节、不修改系统区域设置或代码页。

这样既能打开文件名/目录名含特殊字符的 ZIP、图片和文件夹，也能处理拖放、文件对话框、命令行和历史记录里保存的有损路径。

> 界面标题与列表仍然按 ANSI 显示，因此特殊字符会显示为 `?`（或压缩包内条目的乱码）。这是程序内部字符串仍为 ANSI 导致的显示问题；本修复解决的是“能否打开”。

#### 4. MangaMeeya.exe — 文本页扫描越界

以 `.txt` 文件作为页面的压缩包（例如包含 BepInEx 本地化文本的模组包）在 `PrepageForwardNum=2`（预读 2 页）时会直接崩溃退出。原因是文本渲染函数 `sub_453040` 的行扫描循环：

- 在检查下标是否到达缓冲区长度**之前**就先读取 `buffer[index]`；
- 结束判断使用 `index == length`（等于），而不是“大于等于”。

当文本末尾是多字节字符的首字节（如 UTF-8 韩文/日文的某个高位字节）时，下标会一次加 2，跳过“等于长度”的判断；之后 `index` 永远不再等于长度，循环便越过缓冲区继续扫描，直到读到未映射的页（0xC0000005）。是否崩溃取决于文本内容、堆布局和预读页数，因此同一个包在默认 `PrepageForwardNum=1` 时可能只是翻到某一页才崩。

修复方式：在 `.text` 末尾的空白区写入一段检查，在读取前判断 `index >= length`；越界时把下标夹到长度并跳到正常的文本结束分支。原位置用 5 字节 `jmp` 跳入补丁代码。未修改入口点，也不影响正常文本的排版结果。

> 验证：`[HS2] Next-Gen Skin v1.85` 这类含 3 个本地化 `.txt` 的模组包在 `PrepageForwardNum=2` 下不再崩溃，并可以正常翻到第 3 页（韩文）；以 UTF-8 韩文/日文、以多字节首字节结尾的文本为页面的测试包也均正常。

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
| 文件名含 `・`（U+30FB）的 ZIP | 正常打开，13 页全部列出 |
| 目录名含 `・` 的图片文件夹 / 直接打开其中的图片 | 正常打开 |
| 多级目录（父、子目录均含 `・`）；特殊目录内放普通 ZIP | 正常打开 |
| ZIP 条目名含 `・`（UTF-8 标记） | 正常解码；标题中的条目名可能乱码（arc.dll 按 ANSI / Shift-JIS 解读，属另一问题） |
| 文件名同时含 `・` 和尾字节为 `0x5C` 的 GBK 字符（`診` → `D4 5C`） | 正常打开（44 页） |
| `[HS2] Next-Gen Skin` 模组 ZIP（3 个本地化 txt，预读 2 页） | 正常列出 3 页，不再崩溃；可翻到第 3 页（韩文） |
| txt 页面为 UTF-8 韩文/日文、末尾为多字节首字节、超长单行等 | 正常渲染，不再越界 |

### 已知限制

- **28000×28000（约 2.35GB DIB）及以上仍会失败。** 32 位进程的 4GB 地址空间被系统 DLL 分成约 1.7GB 和 2.0GB 两个空闲块，无法获得 >2GB 连续 DIB。这是 32 位架构限制；突破需要分块/缩放解码，本补丁未涉及。
- 只修改了 ZIP 解析；**RAR/LZH 未改动**，超大 RAR 未测试。
- 未修改 `SusiePlugin`、`AvisynthPlugin` 中的任何插件。
- `mmunicode.dll` 必须与 `MangaMeeya.exe` 放在同一目录。缺失时 Windows 会提示找不到 DLL，程序无法启动。
- 特殊字符修复只处理“文件名/目录名在当前 ANSI 代码页下不可表示”的情况；程序内部的 ANSI 字符串没有改成 Unicode，因此界面上的特殊字符仍显示为 `?`。
- `mmunicode.dll` 在进程内挂钩文件 API，少数安全软件可能将其误报；源码公开，可自行审阅或重新编译。
- 文本页越界修复针对已复现的 `sub_453040` 行扫描路径；其他文本排版分支未单独做模糊测试。

### 使用（即开即用）

**仓库根目录就是完整可运行版本**：克隆或解压后直接运行 `MangaMeeya.exe`，无需替换文件或安装依赖。

1. 解压到任意目录（避免需要管理员权限的路径），保持 `mmunicode.dll` 与 `MangaMeeya.exe` 在同一目录。
2. 运行 `MangaMeeya.exe`。
3. 本仓库**不包含** `MangaMeeya.ini`、`历史.lst` 等用户文件；首次运行会自动生成默认配置。

恢复原版：用 `original/` 中的两个未修改文件覆盖根目录同名文件（见 [original/README.md](original/README.md)）。原版 `MangaMeeya.exe` 不依赖 `mmunicode.dll`，恢复后可删除该 DLL。

### 目录

```
MangaMeeya.exe        主程序（已开启 LAA，并导入 mmunicode.dll）
mmunicode.dll         特殊字符文件名兼容层（新增）
arc.dll               压缩包插件（已打 ZIP64 补丁）
gif/jpg/png/jp2/pdf.dll
SusiePlugin/          原版 Susie 插件
AvisynthPlugin/       原版 Avisynth 插件
docs/                 原版说明
MangaMeeya 中文版说明.txt   原中文版作者说明
original/             未修改的原版 MangaMeeya.exe / arc.dll（对照与回退用）
patch-source/         补丁源码、应用脚本与预编译补丁块（含 unicode/ 子目录）
SHA256SUMS.txt        补丁版与原版二进制的 SHA-256
```

### 重新打补丁

见 [patch-source/README.md](patch-source/README.md)。需要 Python 3；重新汇编 `arc.dll` 补丁需要 NASM；重新编译 `mmunicode.dll` 需要 32 位（x86）MSVC 或 MinGW 工具链。文本页崩溃补丁仅修改 `MangaMeeya.exe` 的机器码，Python 即可完成。

### 隐私

本仓库不包含任何用户配置、阅读历史、书架、收藏夹、播放列表或本机路径。`MangaMeeya.ini`、`*.lst` 已在 `.gitignore` 中排除。

### 版权与致谢

- 原版 MangaMeeya 作者及其维护者。
- 中文版作者 **Fenghou**（2006-07-22）。
- 本补丁由社区制作，与原作者无关联。
- `patch-source/` 下的汇编、C 源码与 Python 脚本采用 MIT 许可；原始程序二进制不适用该许可。

---

<a id="english"></a>

## English

### Overview

MangaMeeya 7.4 is a 32-bit application from 2006. It fails on:

- ZIP archives **≥ 2GB** — only entries before the 2GB boundary are listed;
- ZIP64 archives **≥ 4GB** — the app reports “no readable files”;
- images whose decoded DIB exceeds roughly **1.5GB** — the title shows `Scale=0%` and nothing is displayed;
- file names containing characters the active ANSI code page cannot represent (for example U+30FB KATAKANA MIDDLE DOT “・” on a Simplified Chinese / CP936 system) — the character becomes `?` and the app reports an invalid path;
- archives whose pages are `.txt` files (for example mod packs containing BepInEx localization text) — the app crashes with 0xC0000005 when two or more pages are prefetched (`PrepageForwardNum=2`).

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

#### 3. mmunicode.dll — special-character file names

MangaMeeya's original code only calls the ANSI (A-suffix) Win32 APIs. Any character in a file name that the active ANSI code page cannot represent (CP936 on Simplified Chinese Windows) is converted to `?`. For example U+30FB KATAKANA MIDDLE DOT “・” is not representable in CP936, so `CreateFileA` fails with `ERROR_INVALID_NAME` and the app reports an invalid path.

The fix is a small compatibility layer, `mmunicode.dll`, loaded with the program:

- a new import, `mmunicode.dll!MMUInit`, is added to the executable's import table, so the Windows loader loads the DLL before the program starts;
- the DLL patches the IAT of the main executable, `arc.dll` and the Susie image plugins, covering `CreateFileA`, `GetFileAttributesA`, `GetFileAttributesExA`, `SetCurrentDirectoryA`, `DeleteFileA`, `MoveFileA`, `CopyFileA`, `CreateDirectoryA`, `RemoveDirectoryA`, `GetShortPathNameA`, and `FindFirstFileA/FindNextFileA`;
- when an ANSI call fails on a path containing `?`, the DLL walks the path component by component, enumerates each parent directory and matches entries by their lossy ANSI conversion, reconstructs the real Unicode path, and retries through the W API;
- ANSI paths are scanned character by character using the active code page's DBCS rules, so a double-byte character's trail byte `0x5C` (`\`) is not mistaken for a directory separator (for example `診` is `D4 5C` in GBK; the earlier build mis-parsed such names);
- the entry point, existing code bytes, the system locale and the system code page are not modified.

This covers files, folders, drag-and-drop, file dialogs, command lines and history entries that were previously stored as lossy ANSI paths.

> Window titles and lists still render ANSI, so the special characters appear as `?` (or as mojibake for archive entry names). That is a display-only consequence of the program's internal ANSI strings; this fix is about being able to open the file.

#### 4. MangaMeeya.exe — text-page scanner overrun

Archives whose pages are `.txt` files (for example mod packs containing BepInEx localization text) crash with 0xC0000005 when `PrepageForwardNum=2` (two pages prefetched). The line scanner in the text renderer `sub_453040`:

- reads `buffer[index]` **before** checking whether the index has reached the buffer length;
- uses equality (`index == length`) rather than “greater or equal” as its end test.

When the text ends with the first byte of a multi-byte character (common in UTF-8 Korean/Japanese), the index advances by two and skips the equality test. After that it never equals the length again, so the loop keeps scanning past the allocation until it touches an unmapped page (0xC0000005). Whether it crashes depends on the text bytes, heap layout and prefetch count, which is why the same archive may only crash when a particular page is reached with the default `PrepageForwardNum=1`.

The patch puts a bounds check in the zero padding at the end of `.text`, in front of the read: if `index >= length`, clamp the index to the length and jump to the normal end-of-text branch. The original instruction becomes a 5-byte `jmp` into the patch. The entry point is untouched, and normal text layout is unchanged.

> Verified: `[HS2] Next-Gen Skin v1.85` (three localization `.txt` pages) no longer crashes with two-page prefetch and page 3 (Korean) renders; synthetic UTF-8 Korean/Japanese pages, pages ending in a multi-byte lead byte, and very long single lines also render without crashing.

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
| ZIP whose file name contains `・` (U+30FB) | Opens normally; all 13 pages listed |
| Image folder whose directory name contains `・`; opening an image directly from it | Opens normally |
| Nested directories where both parent and child contain `・`; ordinary ZIP inside such a folder | Opens normally |
| ZIP entry names containing `・` (UTF-8 flag) | Decodes normally; entry names in the title may be mojibake (arc.dll interprets them as ANSI / Shift-JIS — a separate issue) |
| File name containing both `・` and a GBK character whose trail byte is `0x5C` (`診` → `D4 5C`) | Opens normally (44 pages) |
| `[HS2] Next-Gen Skin` mod ZIP (three localization txt pages, two-page prefetch) | All 3 pages listed, no crash; page 3 (Korean) renders |
| UTF-8 Korean/Japanese txt pages, pages ending in a multi-byte lead byte, very long single lines | Render normally, no overrun |

### Known limitations

- **28000×28000 (about 2.35GB DIB) and larger still fail.** The 4GB address space of a 32-bit process is split by system DLLs into roughly 1.7GB and 2.0GB free blocks, so a contiguous DIB larger than 2GB cannot be allocated. This is a 32-bit architecture limit; overcoming it requires tiled/scaled decoding, which this patch does not attempt.
- Only ZIP parsing is patched. **RAR/LZH are unchanged**; large RAR archives were not tested.
- No plugin in `SusiePlugin/` or `AvisynthPlugin/` was modified.
- `mmunicode.dll` must stay next to `MangaMeeya.exe`. If it is missing, Windows reports that the DLL cannot be found and the program does not start.
- The special-character fix only addresses characters that are unrepresentable in the ANSI code page. The program's internal strings remain ANSI, so the UI still displays `?`; this is cosmetic. Characters that are illegal in NTFS file names are, of course, not handled.
- `mmunicode.dll` hooks file APIs inside the process, so a few security products may flag it; the source is included and can be reviewed or rebuilt.
- The text-page overrun fix targets the reproduced line-scanner path (`sub_453040`); other text layout branches were not separately fuzzed.

### Usage (ready to run)

**The repository root is the complete runnable build**: clone or extract, then launch `MangaMeeya.exe`. No file replacement or dependency installation is needed.

1. Extract to any folder (avoid paths that require administrator rights), keeping `mmunicode.dll` next to `MangaMeeya.exe`.
2. Run `MangaMeeya.exe`.
3. No `MangaMeeya.ini` or `历史.lst` is included; the app creates default configuration on first run.

To revert to the original behavior, overwrite the two files in the root with the unmodified copies from `original/` (see [original/README.md](original/README.md)). The original `MangaMeeya.exe` does not import `mmunicode.dll`, so the DLL can be deleted afterwards.

### Layout

```
MangaMeeya.exe        main executable (LAA enabled, imports mmunicode.dll)
mmunicode.dll         special-character file name compatibility layer (new)
arc.dll               archive plugin (ZIP64 patched)
gif/jpg/png/jp2/pdf.dll
SusiePlugin/          original Susie plugins
AvisynthPlugin/       original Avisynth plugins
docs/                 original documentation
MangaMeeya 中文版说明.txt   original Chinese edition readme
original/             unmodified MangaMeeya.exe / arc.dll (reference and rollback)
patch-source/         patch sources, apply scripts and prebuilt blobs (incl. unicode/)
SHA256SUMS.txt        SHA-256 of patched and original binaries
```

### Rebuilding the patches

See [patch-source/README.md](patch-source/README.md). Python 3 is required; NASM is required only to reassemble the `.asm` files, and a 32-bit (x86) MSVC or MinGW toolchain is required to rebuild `mmunicode.dll`. The text-page crash patch only rewrites machine code in `MangaMeeya.exe` and needs Python only.

### Privacy

This repository contains no user configuration, reading history, bookshelf, favorites, playlist, or local paths. `MangaMeeya.ini` and `*.lst` are excluded by `.gitignore`.

### Credits & disclaimer

- The original MangaMeeya authors and maintainers.
- Chinese edition by **Fenghou** (2006-07-22).
- This patch is community-made and is not affiliated with the original authors.
- The assembly, C source and Python scripts under `patch-source/` are released under the MIT license; the original program binaries are not covered by that license.

---

*MangaMeeya 为免费软件但未开源；本仓库按社区惯例分发补丁版与原版二进制，若权利人提出要求将移除二进制、仅保留补丁源码。*
*MangaMeeya is freeware but not open source. This repository redistributes the patched and original binaries under the customary community assumption; binaries will be removed upon request from the rights holder, leaving only the patch source.*
