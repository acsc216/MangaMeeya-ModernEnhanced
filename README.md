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
- 以 `.txt` 文本作为页面的压缩包（例如带 BepInEx 本地化文本的模组包）在“预读 2 页以上”（`PrepageForwardNum=2`）时直接崩溃退出（0xC0000005）；
- 无法读取和显示 **WebP 图片**，含 WebP 条目的压缩包也会因没有解码器而无法正常显示；
- **无法打开 `.7z` 档案**（arc.dll 只支持 ZIP/RAR/LZH 等格式），固实档案更是没有任何可用的预解压路径；
- 在 Windows 11 上，“文件关联设置”里的 **Explorer 右键菜单注册会静默失败**（普通权限进程无法写 `HKEY_CLASSES_ROOT`，程序清单又禁用了 UAC 虚拟化）。

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

#### 5. SusiePlugin/webp.spi — WebP 支持

MangaMeeya 7.4 本身没有 WebP 解码器。新增一个 32 位 Susie 图像插件 `SusiePlugin/webp.spi`：

- 按 Susie `00IN` 导入插件接口实现 `GetPluginInfo` / `IsSupported` / `GetPictureInfo` / `GetPicture`；
- 通过 **Windows Imaging Component（WIC）** 解码 WebP，因此支持独立 WebP 文件，也支持 ZIP/RAR 等压缩包内的 `.webp` 条目（由 arc 插件列出后交给本插件解码）；
- 输出 24bpp 自下而上的 DIB，并按需保留第一帧（动画 WebP 只显示第一帧）；
- WebP 的透明通道由插件**合成到白色背景**后再输出。MangaMeeya 的 Susie 图像路径只使用 RGB 通道，不会像内置 PNG/GIF 路径那样做逐像素 alpha 合成，因此透明区域会呈现为白底而不是穿透到软件背景。

> 依赖：Windows 11 自带 WIC WebP 解码器；Windows 10 需要安装 Microsoft Store 的 “WebP Image Extensions”。插件放在 `SusiePlugin` 目录（即 `[Susie] PluginPath`），程序启动时自动加载。

#### 6. MangaMeeya.exe — Explorer 右键菜单按用户注册

Windows 7 以后，普通权限进程不能写 `HKEY_CLASSES_ROOT`；MangaMeeya 的程序清单（空 `<assembly>`）又禁用了 UAC 虚拟化。因此“文件关联设置”里的右键菜单注册在 Win11 上会**静默失败**：对话框看似正常关闭，但注册表里没有生成任何条目。

修复方式：把三个注册表辅助函数（检查 / 写入 / 删除）替换为写入 **HKCU\Software\Classes** 的实现：

- `Folder` → `HKCU\Software\Classes\Folder\shell\用MangaMeeya打开\command`
- `.zip/.rar/.lzh/.cbz/.cbr/.cbl/.jpg/.png/.gif/.bmp` → `HKCU\Software\Classes\SystemFileAssociations\<ext>\shell\用MangaMeeya打开\command`

这样不需要管理员权限，Explorer 会读取当前用户的类注册。列表第 5 项（显示为 `....`）不注册。

> **Win11 菜单位置**：Windows 11 的默认右键菜单只显示新式菜单项；所有旧式 shell 项（包括本补丁注册的）都在 **“显示更多选项”**（Shift+F10）里。要把条目放进一级菜单，需要带 `IExplorerCommand` 的 MSIX/sparse package，本补丁不涉及。

#### 7. SusiePlugin/7z.spi + 7za.exe — 7z 支持与固实块并行解压

MangaMeeya 的 `arc.dll` 不支持 7z。新增 32 位 Susie 归档插件 `SusiePlugin/7z.spi`，它调用同目录的 `7za.exe`（7-Zip 26.00 x86，随包附带）完成解码：

- **非固实档案**：直接 `7za x -so` 把单页流式取到内存，不建缓存；
- **多块固实档案**：`7za l -slt` 的头部给出 `Blocks = N`，每个条目带有 `Block = k`。LZMA2 在单个固实块内部只能顺序解码，但**块与块相互独立**，因此插件按块分组，为每块启动一个 `7za.exe` 进程并行解压（默认 `min(块数, CPU 数, 8)`，可用 `MM7Z_THREADS` 调整）。实测 156MB / 3 块：单进程约 2.7s，3 进程并行 CPU 总时间约 3.8s、墙钟约 1.2s；
- **单块固实档案按需解码**：`Blocks = 1`（漫画包最常见）时，插件常驻一个 `7za x -so` 进程，按**归档顺序**输出解压流，再按列表中的文件大小切分写入缓存。`GetFile` 只把解码前沿推进到“当前页 + 预读窗口”（默认 64 MB，`MM7Z_PREFETCH` 可调）；没有新请求时解码停在原处，**不会在后台把整包解完**。目标文件写完（大小匹配 + CRC32）即返回。实测 1.82 GB / 单块 / 解压后 2.4 GB 的 BallonsTranslator 导出包：在 MangaMeeya 中打开并显示第 64 页约 **1.7 s**，解码在约 **356 MB** 处暂停；只有跳到更靠后的页时才继续解码（跳到最后一页约 19 s，单块顺序解码无法避免）；全部解码后写入 `.done`，之后打开直接读缓存（约 0.3 s）。多块包的并行预解压路径保持不变；
- **缓存**：位于 `%TEMP%\MangaMeeya_7z\<hash>\`，`<hash>` 由档案完整路径、大小、修改时间决定；已存在的完整文件在重新解码时直接复用（跳过写入）；
- **会话级内存缓存（可选）**：`MM7Z_MEMCACHE=1` 时，单块按需解码写出的页面使用 `FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE` 并保持一个“钉住”句柄。Windows 缓存管理器会把数据留在内存中（仅在内存压力下回写磁盘），**不占用 32 位进程的地址空间**；MangaMeeya 退出时文件自动删除，不在 `%TEMP%` 留下残留。代价是不跨会话复用（下次打开重新解码）；内存不足时仍会落盘。默认关闭，使用持久磁盘缓存；
- **压缩端建议**：官方推荐的随机访问做法是使用多个较小的固实块，例如 `7z a -ms=64m`（漫画可用 `-ms=16m`）；实测 `-ms=64m` 把 200MB 数据分成 4 块，插件用 4 个进程并行解码。若追求每页独立读取，应使用 `-ms=off` 或 ZIP（MangaMeeya 原生支持）；
- **缓存上限与配置**：配置文件为 `SusiePlugin\7z.ini`（`[7z]` 段），`CacheMaxMB` 默认 **5120（5 GB）**，设为 0 可关闭自动清理。每次打开档案时统计 `%TEMP%\MangaMeeya_7z` 的总大小，超过上限就按“最久未使用”删除旧缓存目录；当前打开的档案和最近 10 分钟用过的缓存不会被删。同文件还支持 `PrefetchMB`、`Threads`、`MemCache`、`Log`、`RememberPassword`；环境变量 `MM7Z_*` 优先级更高。若没有 `7z.ini`，会读取 `MangaMeeya.ini` 的 `[7z]` 段；
- **加密 7z**：支持仅数据加密的包和加密文件名（`-mhe=on`）的包。打开时在 MangaMeeya 内弹出密码输入框，密码错误最多重试 3 次。密码框内有 **“记住密码 / Remember”复选框，默认勾选**；取消勾选则不保存该密码（并删除该档案已保存的密码），勾选则保存。你的勾选状态会被记住，作为下次弹窗的默认值（`%APPDATA%\MangaMeeya_7z\remember`）。密码通过 Windows **DPAPI**（当前用户）加密后保存在 `%APPDATA%\MangaMeeya_7z\passwords`，下次打开同一路径的档案自动使用；`7z.ini` 的 `RememberPassword` 只是复选框的初始默认值；
- **UTF-8 文件名**：条目清单通过 `-scsUTF-8` 列表文件传给 7za，含日文/中文目录和文件名也能正确匹配；同时修复了用 `lstrcmpiA` 比较 UTF-8 名称时在 CP936 下把 `第1巻` 与 `第2巻` 误判为同一文件的问题；
- **取消**：Susie 00AM 的 `GetFile` 进度回调返回**非 0** 时立即终止所有解码子进程并返回错误码 1（注意与图像插件 `GetPicture` 的“返回 0 取消”相反）；下次读取会自动重新开始解压；
- 诊断：设置 `MM7Z_LOG=1` 可把启动、列表、解码暂停/完成和错误码写入 `%TEMP%\7zspi.log`；`MM7Z_PREFETCH=n` 设置单块固实包的预读窗口（MiB，默认 64，0 表示只解到当前页）。

> 依赖：`7z.spi`、`7za.exe`、`7z-License.txt` 必须一起放在 `SusiePlugin`。`7za.exe` 来自 7-Zip 官方发行包，遵循 7-Zip 许可证（见 `7z-License.txt`）。
> 为什么不能“整包多线程”：7-Zip 作者 Igor Pavlov 明确说明单个 LZMA/LZMA2 流无法多线程解码；LZMA2 虽然可以把流切成独立块，但 7-Zip 的解码端并未实现该并行路径。能并行的是**不同的固实块**（`-ms=64m` 之类的小块），因此单块固实包仍然只有一个解码线程（但具备第一页先到、按需暂停和可选内存缓存），多块固实包才能获得多核加速。

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
| 独立 `.webp` 文件（1920×1080 等） | 正常列出并显示 |
| 仅含 WebP 的 ZIP / JPEG+WebP 混合 ZIP | 页数正确，WebP 页正常显示 |
| 文件名/目录名含 `・` 的 WebP 文件与含 WebP 的 ZIP | 正常打开并显示 |
| 含透明通道的 WebP | 正常显示（透明区域合成到白色背景） |
| 7z 单块固实包（120 条目 / 131MB） | 全部列出并解码，逐条 CRC 校验通过 |
| 7z 多块固实包（`Blocks = 3`，120 条目） | 3 个解码进程并行；全部条目 CRC 校验通过 |
| 7z 多块固实包，目录/文件名含日文（`第1巻/ページ・*.jpg`） | 列表、按块解压与 CRC 均正常 |
| 7z 非固实包（每文件独立块） | 逐条 `x -so` 解压正常 |
| 7z 内存映像输入（`GetArchiveInfo` / `GetFile` 的 `flag=1`） | 正常 |
| 7z 解压过程中取消 | 立即终止解码进程并返回 1；再次读取时重新解压 |
| **176MB / 800 页 / 3 块的真实 JPEG 漫画包（MangaMeeya GUI）** | 打开后 1/800 正常显示；翻页到 800/800 全程正常，无取消/错误；三个固实块并行解压 |
| **1.82GB 单块固实包（解压后 2.4GB，BallonsTranslator 导出）** | 打开并显示第 64 页约 1.7s，解码在约 356MB 处暂停（不再后台解完整包）；跳到最后一页约 19s 后完成并写入 `.done`，CRC 校验通过；之后打开直接读缓存 |
| 单块固实包只读前 8 条（冷缓存） | 1.75s，缓存 285MB，日志显示 `paused`；未解码部分保持未解 |
| 单块固实包跳到末页（从部分缓存） | 19.2s 完成，2.4GB，`.done` 写入，末页 CRC 正确 |
| 7z 数据加密（密码 `SECRET`） | 列出/解码正常；密码写入 DPAPI 存储，新进程重开自动使用 |
| 7z 加密文件名（`-mhe=on`，MangaMeeya GUI） | 弹出密码框；输入后显示 1/2 页；重开不再提示 |
| 密码含空格（`my pass`） | 正常 |
| 错误密码 | 最多重试 3 次；取消或超限后放弃打开，不会保存错误密码 |
| 全空文件包 / 非固实包 / 多块固实包 | 回归通过 |
| 文件关联设置勾选 `Folder` / `.zip` 并确定 | `HKCU\Software\Classes\...` 生成 command，Shell 能枚举到“用MangaMeeya打开” |
| 勾选 `.jpg` / `.bmp` 等图片类型 | `SystemFileAssociations\<ext>` 下生成，对应文件右键菜单可见 |
| 取消注册 | 对应 HKCU 键被删除 |
| Win11 默认右键菜单 | 条目位于“显示更多选项”（Shift+F10）中 |

### 已知限制

- **28000×28000（约 2.35GB DIB）及以上仍会失败。** 32 位进程的 4GB 地址空间被系统 DLL 分成约 1.7GB 和 2.0GB 两个空闲块，无法获得 >2GB 连续 DIB。这是 32 位架构限制；突破需要分块/缩放解码，本补丁未涉及。
- 只修改了 ZIP 解析；**RAR/LZH 未改动**，超大 RAR 未测试。
- 未修改 `SusiePlugin`、`AvisynthPlugin` 中的任何插件。
- `mmunicode.dll` 必须与 `MangaMeeya.exe` 放在同一目录。缺失时 Windows 会提示找不到 DLL，程序无法启动。
- 特殊字符修复只处理“文件名/目录名在当前 ANSI 代码页下不可表示”的情况；程序内部的 ANSI 字符串没有改成 Unicode，因此界面上的特殊字符仍显示为 `?`。
- `mmunicode.dll` 在进程内挂钩文件 API，少数安全软件可能将其误报；源码公开，可自行审阅或重新编译。
- 文本页越界修复针对已复现的 `sub_453040` 行扫描路径；其他文本排版分支未单独做模糊测试。
- WebP 解码依赖系统的 WIC WebP 编解码器。Windows 11 通常自带；Windows 10 如未安装 “WebP Image Extensions”，`webp.spi` 将无法解码并可能显示为空白。
- 透明 WebP 会合成到白底（不会穿透到 MangaMeeya 的背景色）；动画 WebP 只显示第一帧。
- 如果用户修改了 `[Susie] PluginPath`，需要把 `webp.spi` 复制到对应目录。
- 7z 插件依赖同目录的 `7za.exe`（随包附带）；删除或移动它会导致 7z 无法打开。加密的 7z（含加密文件名）不支持。
- 单块固实包按需解码：只解到“当前页 + 预读窗口”就暂停，缓存大小随阅读进度增长；完整解码过一次后才会写 `.done`，之后打开可直接读缓存。缓存不会自动清理，可手动删除 `%TEMP%\MangaMeeya_7z`。
- 单块固实包跳到靠后的页仍需从块首顺序解码到该页（LZMA2 无法跳读）；如果 MangaMeeya 的页序与归档顺序不一致（它按文件名排序），跨目录翻页可能触发较长距离的解码。
- 单个固实块内部的 LZMA2 解码无法多线程化，单块固实包只能单线程解码；多块固实包才会用多个进程并行。
- 按需解码暂停时会保留一个 `7za.exe` 子进程（持有 LZMA2 字典和缓冲；2.4GB 的包约 800MB 工作集）；MangaMeeya 退出时由 Job Object 结束。
- `MM7Z_MEMCACHE=1` 的内存缓存是**会话级**的：退出即删除，下次打开重新解码；它不占本进程地址空间，但在系统内存紧张时仍会被缓存管理器写回磁盘。为每个已解码条目保持一个句柄（条目数 > 20000 时自动关闭该功能）。
- 多块固实包目前仍是“并行预解压所有块”：跳到很靠后的块可能需要等该块所在批次；改为按请求优先级调度可以进一步优化。
- Susie 的 `fileInfo` 大小字段是 32 位，单个条目 ≥ 4GB 时数值会截断。
- 右键菜单的注册写入 `HKCU\Software\Classes`，只对当前用户生效；在 Win11 默认菜单中位于“显示更多选项”。进入一级菜单需要 MSIX/IExplorerCommand 包，本补丁未实现。
- 文件关联设置列表第 5 项（显示为 `....`）不注册（原版会生成无意义的 `HKCR\....`）。
- IE 右键菜单（MenuExt）仍按原有逻辑写入 `HKCU\Software\Microsoft\Internet Explorer\MenuExt`；Edge 不使用该位置，因此只在旧版 IE 中有效。

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
SusiePlugin/          原版 Susie 插件 + 新增 webp.spi（WebP 解码）、7z.spi + 7za.exe（7z 解压）
AvisynthPlugin/       原版 Avisynth 插件
docs/                 原版说明
MangaMeeya 中文版说明.txt   原中文版作者说明
original/             未修改的原版 MangaMeeya.exe / arc.dll（对照与回退用）
patch-source/         补丁源码、应用脚本与预编译补丁块（含 unicode/、webp/、shellmenu/、7z/ 子目录）
SHA256SUMS.txt        补丁版与原版二进制的 SHA-256
```

### 重新打补丁

见 [patch-source/README.md](patch-source/README.md)。需要 Python 3；重新汇编 `arc.dll` 补丁需要 NASM；重新编译 `mmunicode.dll`、`webp.spi` 或 `7z.spi` 需要 32 位（x86）MSVC 或 MinGW 工具链。文本页崩溃补丁和右键菜单补丁仅修改 `MangaMeeya.exe` 的机器码，Python 即可完成（右键菜单补丁的 `.bin` 已附带；修改汇编时才需要 NASM）。

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
- archives whose pages are `.txt` files (for example mod packs containing BepInEx localization text) — the app crashes with 0xC0000005 when two or more pages are prefetched (`PrepageForwardNum=2`);
- **WebP images** cannot be read or displayed, and archives containing WebP entries do not render because there is no decoder;
- **`.7z` archives cannot be opened** (arc.dll only supports ZIP/RAR/LZH and similar), and solid archives have no usable pre-extraction path at all;
- on Windows 11 the **Explorer context-menu registration** in “File association settings” fails silently (a standard process cannot write `HKEY_CLASSES_ROOT`, and the app's manifest disables UAC virtualisation).

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

#### 5. SusiePlugin/webp.spi — WebP support

MangaMeeya 7.4 has no WebP decoder. A 32-bit Susie image plug-in, `SusiePlugin/webp.spi`, is added:

- implements the Susie `00IN` import-filter API (`GetPluginInfo` / `IsSupported` / `GetPictureInfo` / `GetPicture`);
- decodes WebP through **Windows Imaging Component (WIC)**, so standalone `.webp` files and `.webp` entries inside ZIP/RAR archives (listed by the arc plug-in and handed to this decoder) are supported;
- returns a 24bpp bottom-up DIB; animated WebP shows the first frame only;
- WebP alpha is **flattened onto white** by the plug-in. MangaMeeya's Susie image path only uses the RGB channels and, unlike the built-in PNG/GIF paths, does not composite per-pixel alpha, so transparent areas appear as a white background rather than the viewer's background colour.

> Requirement: Windows 11 includes the WIC WebP codec; on Windows 10 install “WebP Image Extensions” from the Microsoft Store. The plug-in lives in the `SusiePlugin` folder (`[Susie] PluginPath`) and is loaded at startup.

#### 6. MangaMeeya.exe — per-user Explorer context menu

Since Windows 7, a non-elevated process cannot write `HKEY_CLASSES_ROOT`, and MangaMeeya's empty `<assembly>` manifest disables UAC virtualisation. As a result, the Explorer context-menu registration in “File association settings” **fails silently** on Windows 11: the dialog closes normally but no registry entries are created.

The three registry helpers (check / set / delete) are replaced with implementations that write to **HKCU\Software\Classes**:

- `Folder` → `HKCU\Software\Classes\Folder\shell\用MangaMeeya打开\command`
- `.zip/.rar/.lzh/.cbz/.cbr/.cbl/.jpg/.png/.gif/.bmp` → `HKCU\Software\Classes\SystemFileAssociations\<ext>\shell\用MangaMeeya打开\command`

No administrator rights are required, and Explorer honours the per-user class registration. The fifth list entry (shown as `....`) is not registered.

> **Placement on Windows 11**: the default Windows 11 context menu only shows the new-style commands; all legacy shell verbs (including these) are under **“Show more options”** (Shift+F10). Putting an entry in the top-level menu requires an MSIX/sparse package with an `IExplorerCommand`; this patch does not implement that.

#### 7. SusiePlugin/7z.spi + 7za.exe — 7z support and parallel solid-block extraction

MangaMeeya's `arc.dll` does not support 7z. A 32-bit Susie archive plug-in, `SusiePlugin/7z.spi`, is added; it calls the bundled `7za.exe` (7-Zip 26.00 x86) located next to it:

- **Non-solid archives** are extracted page by page through `7za x -so`; no cache is built.
- **Multi-block solid archives**: `7za l -slt` reports `Blocks = N` in the header and `Block = k` for every file. LZMA2 decoding is sequential *within* a solid block, but blocks are independent, so the plug-in groups files by block and starts one `7za.exe` process per block (default `min(blocks, CPUs, 8)`, override with `MM7Z_THREADS`). Measured on 131MB / 3 blocks: about 2.7s with one process, versus about 3.8s of total CPU time and about 1.2s wall-clock with three processes.
- **Single-block solid archives are decoded on demand**: when `Blocks = 1` (the common case for manga), the plug-in keeps one `7za x -so` process running, splits its output into files using the sizes from the listing (output is in archive order), and writes them to the cache. `GetFile` only moves the decode frontier to “current page + read-ahead window” (64 MB by default, tunable with `MM7Z_PREFETCH`); with no new requests the decoder **pauses instead of extracting the whole archive in the background**. A requested file is returned as soon as it is complete (size match plus CRC32). Measured on a 1.82GB single-block BallonsTranslator export (2.4GB unpacked): MangaMeeya opens and displays page 64 in about **1.7s** and decoding pauses at about **356MB**; a jump to the last page still requires sequential decoding to that point (about 19s, inherent to one solid block); after a full decode `.done` is written and later opens read the cache directly (about 0.3s). The parallel pre-extraction path for multi-block archives is unchanged.
- **Cache**: `%TEMP%\MangaMeeya_7z\<hash>\`, where the hash covers the archive path, size and modification time. Complete files are reused on re-decode (their writes are skipped).
- **Optional session memory cache**: with `MM7Z_MEMCACHE=1`, pages decoded by the single-block stream are created with `FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE` and kept open by a pin handle. The Windows cache manager keeps them in RAM (writing back only under memory pressure), **outside the 32-bit process's address space**, and they are deleted when MangaMeeya exits, leaving nothing in `%TEMP%`. The trade-off is no reuse across sessions (the next open decodes again); under memory pressure data still spills to disk. Off by default (persistent disk cache).
- **Archive-side advice**: the official way to get fast random access is smaller solid blocks, e.g. `7z a -ms=64m` (use `-ms=16m` for comics). `-ms=64m` split a 200MB test set into 4 blocks, which the plug-in decoded with 4 parallel processes. For fully independent pages use `-ms=off` or a ZIP (natively supported by MangaMeeya).
- **Cache cap and configuration**: `SusiePlugin\7z.ini` (`[7z]` section); `CacheMaxMB` defaults to **5120 (5 GB)** and 0 disables cleanup. When an archive is opened the plug-in adds up the size of `%TEMP%\MangaMeeya_7z` and, if it is over the cap, deletes the least recently used cache directories; the archive currently open and anything used in the last 10 minutes are kept. The same file also supports `PrefetchMB`, `Threads`, `MemCache`, `Log` and `RememberPassword`; `MM7Z_*` environment variables take precedence. If `7z.ini` is absent, the `[7z]` section of `MangaMeeya.ini` is used.
- **Encrypted 7z**: data-only encryption and encrypted headers (`-mhe=on`) are supported. A password dialog appears inside MangaMeeya; up to three attempts are allowed. It contains a **“记住密码 / Remember” checkbox, checked by default**; unchecking does not save the password (and removes any saved one for that archive), checking saves it. The checkbox choice is remembered as the default for the next dialog (`%APPDATA%\MangaMeeya_7z\remember`). Passwords are encrypted with Windows **DPAPI** (current user) under `%APPDATA%\MangaMeeya_7z\passwords` and reused automatically for the same archive path; `RememberPassword` in `7z.ini` only sets the checkbox's initial default.
- **UTF-8 names**: per-block file lists are passed through `-scsUTF-8` list files, so Japanese/Chinese directory and file names match correctly. This also fixes UTF-8 entry names being compared with `lstrcmpiA`, which under CP936 made `第1巻` and `第2巻` look identical.
- **Cancellation**: a **non-zero** return from the 00AM `GetFile` progress callback terminates all decoder processes and returns error code 1 (note this is the opposite of the image-filter `GetPicture` convention, where zero cancels); the next read starts a fresh extraction.
- Diagnostics: `MM7Z_LOG=1` writes startup, listing, decode pause/completion and error codes to `%TEMP%\7zspi.log`; `MM7Z_PREFETCH=n` sets the single-block read-ahead window in MiB (default 64; 0 means decode only up to the current page).

> Requirement: `7z.spi`, `7za.exe` and `7z-License.txt` must be kept together in `SusiePlugin`. `7za.exe` comes from the official 7-Zip distribution and is covered by the 7-Zip license (`7z-License.txt`).
> Why not “multi-thread the whole archive”: a single LZMA2 stream must be decoded sequentially from its start, so no decoder can split it across cores. What *can* run in parallel are separate solid blocks. A single-block solid archive therefore still has one decoder thread (but benefits from first-page streaming); multi-block archives gain real multi-core speedup.

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
| Standalone `.webp` files (1920×1080 etc.) | Listed and displayed normally |
| WebP-only ZIP / mixed JPEG+WebP ZIP | Correct page count; WebP pages render normally |
| WebP files and WebP ZIPs whose names contain `・` | Open and render normally |
| WebP with an alpha channel | Displays normally (alpha flattened onto white) |
| Single-block solid 7z (120 entries / 131MB) | All entries listed and decoded; per-entry CRC verified |
| Multi-block solid 7z (`Blocks = 3`, 120 entries) | Three decoder processes run in parallel; every CRC passes |
| Multi-block solid 7z with Japanese directories/names (`第1巻/ページ・*.jpg`) | Listing, per-block extraction and CRC all pass |
| Non-solid 7z (one independent block per file) | Per-entry `x -so` extraction works |
| 7z memory-image input (`GetArchiveInfo` / `GetFile` with `flag=1`) | Works |
| Cancelling during 7z extraction | Decoder processes are terminated and the call returns 1; the next read restarts extraction |
| **Real 176MB / 800-page / 3-block JPEG manga archive (MangaMeeya GUI)** | Opens at 1/800 and displays; paging through to 800/800 works with no cancellation or errors; all three solid blocks decode in parallel |
| **1.82GB single-block solid archive (2.4GB unpacked, BallonsTranslator export)** | Opens and displays page 64 in about 1.7s; decoding pauses at about 356MB instead of extracting the whole archive; a jump to the last page completes in about 19s and writes `.done` with all CRCs verified; later opens read the cache directly |
| First 8 entries of the single-block archive (cold cache) | 1.75s, 285MB cached, log shows `paused`; the rest stays undecoded |
| Jump to the last page from a partial cache | Completes in 19.2s, 2.4GB, `.done` written, last-page CRC correct |
| Data-encrypted 7z (password `SECRET`) | Lists/decodes normally; password saved via DPAPI and reused by a new process |
| Header-encrypted 7z (`-mhe=on`, MangaMeeya GUI) | Password dialog appears; after entry page 1/2 renders; reopening does not prompt |
| Password containing a space (`my pass`) | Works |
| Wrong password | Up to three attempts; cancelling or exhausting them aborts opening and does not save the wrong password |
| All-empty archive / non-solid / multi-block archives | Regression pass |
| Register `Folder` / `.zip` in File association settings | `HKCU\Software\Classes\...` command created; the shell enumerates “用MangaMeeya打开” |
| Register `.jpg` / `.bmp` and other image types | Created under `SystemFileAssociations\<ext>`; visible in the file context menu |
| Unregister | The corresponding HKCU keys are removed |
| Windows 11 default context menu | The entry is under “Show more options” (Shift+F10) |

### Known limitations

- **28000×28000 (about 2.35GB DIB) and larger still fail.** The 4GB address space of a 32-bit process is split by system DLLs into roughly 1.7GB and 2.0GB free blocks, so a contiguous DIB larger than 2GB cannot be allocated. This is a 32-bit architecture limit; overcoming it requires tiled/scaled decoding, which this patch does not attempt.
- Only ZIP parsing is patched. **RAR/LZH are unchanged**; large RAR archives were not tested.
- No plugin in `SusiePlugin/` or `AvisynthPlugin/` was modified.
- `mmunicode.dll` must stay next to `MangaMeeya.exe`. If it is missing, Windows reports that the DLL cannot be found and the program does not start.
- The special-character fix only addresses characters that are unrepresentable in the ANSI code page. The program's internal strings remain ANSI, so the UI still displays `?`; this is cosmetic. Characters that are illegal in NTFS file names are, of course, not handled.
- `mmunicode.dll` hooks file APIs inside the process, so a few security products may flag it; the source is included and can be reviewed or rebuilt.
- The text-page overrun fix targets the reproduced line-scanner path (`sub_453040`); other text layout branches were not separately fuzzed.
- WebP decoding depends on the system WIC WebP codec. Windows 11 normally includes it; on Windows 10 without “WebP Image Extensions” `webp.spi` cannot decode and may show a blank page.
- Transparent WebP is flattened onto white (it does not show the viewer background); animated WebP shows only the first frame.
- If `[Susie] PluginPath` is changed, `webp.spi` must be copied to that folder.
- The 7z plug-in requires `7za.exe` in the same folder (shipped with the repository); removing or moving it makes 7z archives unopenable. Password-protected 7z archives (including encrypted headers) are not supported.
- Single-block solid archives are decoded on demand: decoding pauses at “current page + read-ahead window” and the cache grows with reading progress. `.done` is written only after a full decode; after that, opens read the cache directly. The cache is not cleaned automatically; delete `%TEMP%\MangaMeeya_7z` to reclaim space.
- Jumping to a later page in a single-block solid archive still requires sequential decoding from the block start (LZMA2 cannot seek). If MangaMeeya's page order differs from the archive order (it sorts by file name), moving across directory boundaries can trigger a longer decode.
- LZMA2 decoding within one solid block cannot be parallelised, so single-block solid archives use one decoder thread; only multi-block solid archives use multiple parallel processes.
- While paused, the on-demand decoder keeps one `7za.exe` child process alive (LZMA2 dictionary plus buffers; about 800MB working set for the 2.4GB example). The Job Object terminates it when MangaMeeya exits.
- `MM7Z_MEMCACHE=1` memory caching is **session-only**: data is deleted on exit and the next open decodes again. It does not consume this process's address space, but the cache manager will still write it back under memory pressure. One handle is kept per decoded entry (the feature disables itself above 20000 entries).
- Multi-block solid archives are still pre-extracted eagerly in parallel: a jump to a block late in the archive may wait for its batch. Priority scheduling by request would improve this further.
- Susie's `fileInfo` size fields are 32-bit; entries ≥ 4GB are truncated.
- The context menu is registered under `HKCU\Software\Classes`, so it is per-user and appears under “Show more options” on Windows 11. Putting it in the top-level menu requires an MSIX/IExplorerCommand package, which this patch does not provide.
- The fifth entry in the file-association list (shown as `....`) is not registered (the original created a meaningless `HKCR\....` key).
- The IE context menu (MenuExt) still uses the original `HKCU\Software\Microsoft\Internet Explorer\MenuExt` path; Edge does not read it, so it only affects legacy IE.

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
SusiePlugin/          original Susie plugins + new webp.spi (WebP decoder), 7z.spi + 7za.exe (7z)
AvisynthPlugin/       original Avisynth plugins
docs/                 original documentation
MangaMeeya 中文版说明.txt   original Chinese edition readme
original/             unmodified MangaMeeya.exe / arc.dll (reference and rollback)
patch-source/         patch sources, apply scripts and prebuilt blobs (incl. unicode/, webp/, shellmenu/, 7z/)
SHA256SUMS.txt        SHA-256 of patched and original binaries
```

### Rebuilding the patches

See [patch-source/README.md](patch-source/README.md). Python 3 is required; NASM is required only to reassemble the `.asm` files, and a 32-bit (x86) MSVC or MinGW toolchain is required to rebuild `mmunicode.dll` or `webp.spi`. The text-page crash patch and the context-menu patch only rewrite machine code in `MangaMeeya.exe` and need Python only (the context-menu `.bin` is included; NASM is needed only if you modify the assembly). A 32-bit toolchain is also required to rebuild `7z.spi`.

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
