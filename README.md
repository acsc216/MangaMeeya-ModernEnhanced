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
- 文件名含当前系统代码页无法表示的字符（例如简体中文 Windows 下的日文「・」U+30FB）：字符被转换成 `?`，程序提示“无效路径”，文件无法打开。

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
- 不修改入口点、不改动任何原有代码字节、不修改系统区域设置或代码页。

这样既能打开文件名/目录名含特殊字符的 ZIP、图片和文件夹，也能处理拖放、文件对话框、命令行和历史记录里保存的有损路径。

> 界面标题与列表仍然按 ANSI 显示，因此特殊字符会显示为 `?`（或压缩包内条目的乱码）。这是程序内部字符串仍为 ANSI 导致的显示问题；本修复解决的是“能否打开”。

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

### 已知限制

- **28000×28000（约 2.35GB DIB）及以上仍会失败。** 32 位进程的 4GB 地址空间被系统 DLL 分成约 1.7GB 和 2.0GB 两个空闲块，无法获得 >2GB 连续 DIB。这是 32 位架构限制；突破需要分块/缩放解码，本补丁未涉及。
- 只修改了 ZIP 解析；**RAR/LZH 未改动**，超大 RAR 未测试。
- 未修改 `SusiePlugin`、`AvisynthPlugin` 中的任何插件。
- `mmunicode.dll` 必须与 `MangaMeeya.exe` 放在同一目录。缺失时 Windows 会提示找不到 DLL，程序无法启动。
- 特殊字符修复只处理“文件名/目录名在当前 ANSI 代码页下不可表示”的情况；程序内部的 ANSI 字符串没有改成 Unicode，因此界面上的特殊字符仍显示为 `?`。
- `mmunicode.dll` 在进程内挂钩文件 API，少数安全软件可能将其误报；源码公开，可自行审阅或重新编译。

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

见 [patch-source/README.md](patch-source/README.md)。需要 Python 3；重新汇编 `arc.dll` 补丁需要 NASM；重新编译 `mmunicode.dll` 需要 32 位（x86）MSVC 或 MinGW 工具链。

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
- file names containing characters the active ANSI code page cannot represent (for example U+30FB KATAKANA MIDDLE DOT “・” on a Simplified Chinese / CP936 system) — the character becomes `?` and the app reports an invalid path.

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
- the entry point, existing code bytes, the system locale and the system code page are not modified.

This covers files, folders, drag-and-drop, file dialogs, command lines and history entries that were previously stored as lossy ANSI paths.

> Window titles and lists still render ANSI, so the special characters appear as `?` (or as mojibake for archive entry names). That is a display-only consequence of the program's internal ANSI strings; this fix is about being able to open the file.

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

### Known limitations

- **28000×28000 (about 2.35GB DIB) and larger still fail.** The 4GB address space of a 32-bit process is split by system DLLs into roughly 1.7GB and 2.0GB free blocks, so a contiguous DIB larger than 2GB cannot be allocated. This is a 32-bit architecture limit; overcoming it requires tiled/scaled decoding, which this patch does not attempt.
- Only ZIP parsing is patched. **RAR/LZH are unchanged**; large RAR archives were not tested.
- No plugin in `SusiePlugin/` or `AvisynthPlugin/` was modified.
- `mmunicode.dll` must stay next to `MangaMeeya.exe`. If it is missing, Windows reports that the DLL cannot be found and the program does not start.
- The special-character fix only addresses characters that are unrepresentable in the ANSI code page. The program's internal strings remain ANSI, so the UI still displays `?`; this is cosmetic. Characters that are illegal in NTFS file names are, of course, not handled.
- `mmunicode.dll` hooks file APIs inside the process, so a few security products may flag it; the source is included and can be reviewed or rebuilt.

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

See [patch-source/README.md](patch-source/README.md). Python 3 is required; NASM is required only to reassemble the `.asm` files, and a 32-bit (x86) MSVC or MinGW toolchain is required to rebuild `mmunicode.dll`.

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
