# MangaMeeya 现代增强版 / Modern Enhanced Edition

基于 **MangaMeeya v7.4 中文版** 的社区修复版，重点解决原版在 64 位 Windows 上的两个老问题：

- **超大压缩包**：ZIP/ZIP64 目录解析错误，导致 2GB 以上偏移的条目读不出来，4GB 以上的包甚至列不出文件。
- **超大图片**：32 位进程默认只有 2GB 地址空间，解码大图时 `CreateDIBSection` 失败，页面显示 `Scale=0%`。

本版本不是官方版本，仅为兼容性补丁；原始程序版权归原作者所有（见 [NOTICE.md](NOTICE.md)）。

## 修复内容

### 1. arc.dll — ZIP64 解析

| 问题 | 原版行为 | 修复 |
|---|---|---|
| 条目偏移 ≥ 2GB | ZIP64 extra field 被当作固定 24 字节读取，只写 8 字节偏移的包从第 2GB 后的条目开始全部错乱 | 按 ZIP64 规范逐字段解析（未压缩大小 / 压缩大小 / 本地头偏移，按缺失字段顺序读取） |
| 压缩包 ≥ 4GB | 只读普通 EOCD 的 32 位 CD 偏移；遇到 `0xFFFFFFFF` 不跟随 ZIP64 locator/EOCD | 解析 ZIP64 locator + ZIP64 EOCD，支持 64 位 CD 偏移与条目数 |
| >4GB 且带归档注释 | 走 EOCD 扫描分支，同样不识别 ZIP64 | 在扫描分支中增加 ZIP64 处理，并恢复注释读取位置 |
| 条目数 > 65535 | EOCD 计数字段为 `0xFFFF` 时按 65535 截断 | 计数字段饱和时同样转入 ZIP64 EOCD |

补丁以 3 个 5 字节 `jmp` 跳入 `.text` 尾部原有空白区，未修改 PE 头、节表、导入表或导出表。

### 2. MangaMeeya.exe — 开启 LAA

设置 PE `FileHeader.Characteristics` 的 `IMAGE_FILE_LARGE_ADDRESS_AWARE`（`0x20`）位，让 32 位进程在 64 位 Windows 上使用完整的 4GB 用户地址空间。

> 注意：LAA 位在 **FileHeader.Characteristics**，不在 DllCharacteristics。后者 `0x20` 是 `HIGH_ENTROPY_VA`，设置它不会开启 LAA。

效果：24bpp DIB 的可分配上限从约 **1.45GB** 提升到约 **1.95GB**，即正方形图片从约 22000×22000 提升到约 **26000×26000**。

## 已验证场景

| 场景 | 结果 |
|---|---|
| 2.2GB ZIP，图片位于 >2GB 偏移 | 286 个条目全部列出；>2GB 处图片正常解码 |
| 4.3GB ZIP（ZIP64 EOCD），图片位于 >4GB 偏移 | 555 个条目全部列出；>4GB 处图片正常解码 |
| 4.3GB 带归档注释 ZIP | 552 个条目全部列出并正常打开 |
| 含 4.3GB 单个条目（ZIP64 大小字段）+ 后续图片 | 条目大小 4,328,521,728 正确；后续图片正常 |
| 70000 条目 ZIP | ZIP64 计数路径生效，可正确开始枚举（arc.dll 逐条查询为 O(n²)，整包列出极慢） |
| 普通小 ZIP / 混合非图片条目 ZIP | 回归正常 |
| 24000²、25000²、26000² PNG | 正常解码 |
| 1200×120000、60000×3000 PNG | 正常解码 |
| 20000² JPEG | 正常解码 |

## 已知限制

- **28000×28000（约 2.35GB DIB）及以上仍会失败。** 32 位进程的 4GB 地址空间被系统 DLL 分成约 1.7GB 和 2.0GB 两个空闲块，无法获得 >2GB 的连续 DIB。这是 32 位架构限制；突破需要分块/缩放解码，本补丁未涉及。
- 只修改了 ZIP 解析；**RAR/LZH 未改动**，超大 RAR 未测试。
- 未修改 `SusiePlugin`、`AvisynthPlugin` 中的任何插件。

## 使用

**仓库根目录就是完整的即开即用版本**：解压/克隆后直接运行 `MangaMeeya.exe` 即可，不需要额外安装或替换文件。

1. 解压到任意目录（不要放在需要管理员权限的路径）。
2. 直接运行 `MangaMeeya.exe`。
3. 本仓库**不包含** `MangaMeeya.ini`、`历史.lst` 等用户文件；首次运行会自动生成默认配置。

如果想恢复原版行为，用 `original/` 目录中的两个未修改二进制覆盖根目录同名文件即可（见 [original/README.md](original/README.md)）。

## 目录

```
MangaMeeya.exe        主程序（已开启 LAA）
arc.dll               压缩包插件（已打 ZIP64 补丁）
gif/jpg/png/jp2/pdf.dll
SusiePlugin/          原版 Susie 插件
AvisynthPlugin/       原版 Avisynth 插件
docs/                 原版说明
MangaMeeya 中文版说明.txt   原中文版作者说明
original/             未修改的原版 MangaMeeya.exe / arc.dll（对照与回退用）
patch-source/         本补丁的汇编源码、应用脚本与预编译补丁块
SHA256SUMS.txt        补丁版与原版二进制的 SHA-256
```

## 重新打补丁

见 [patch-source/README.md](patch-source/README.md)。

## 致谢

- 原版 MangaMeeya 作者及其维护者。
- 中文版作者 **Fenghou**（2006-07-22）。
- 本补丁由社区制作，与原作者无关联。

## 免责声明

本程序按“现状”提供，不提供任何担保。使用本补丁造成的任何问题由使用者自行承担。
