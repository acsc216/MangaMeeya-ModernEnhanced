# NOTICE / 版权与分发说明

## 原始程序

- **MangaMeeya** 是日本作者发布的免费漫画阅读软件，**免费但未开源**。
- 本仓库基于 **MangaMeeya v7.4 中文版**，中文版作者为 **Fenghou**（`fenghou1st@gmail.com`，2006-07-22，见 `MangaMeeya 中文版说明.txt`）。
- 原始程序及其插件的版权归各自作者所有。

## 本仓库的性质

- 本仓库是**非官方**的兼容性补丁版本，与原作者、中文版作者均无关联。
- 修改仅涉及：
  - `arc.dll` 的 ZIP64 解析逻辑（三处代码补丁）；
  - `MangaMeeya.exe` 的 PE `FileHeader.Characteristics` LAA 标志位；
  - `MangaMeeya.exe` 的导入表（新增 `mmunicode.dll!MMUInit`，用于加载文件名兼容层）；
  - `MangaMeeya.exe` 文本页行扫描的越界检查（`.text` 末尾补丁代码 + 一处 5 字节跳转）。
- 新增 `mmunicode.dll`（C 源码见 `patch-source/unicode/`），用于解决 ANSI 代码页无法表示的特殊字符文件名；它不修改任何原有代码，只在进程内挂钩文件相关的 ANSI API。
- 未修改任何第三方插件的代码。

## 分发

MangaMeeya 为免费软件，但“免费”不等于“可自由再分发”。本仓库按社区惯例分发**补丁版与未修改的原版二进制**，前提是原软件允许免费分发；如果原作者或权利人提出要求，将移除二进制文件，仅保留 `patch-source/` 中的补丁源码。

根目录为补丁版；`original/` 为未修改的原版 `MangaMeeya.exe` 与 `arc.dll`，供对照与回退。

## 补丁源码许可

`patch-source/` 下的汇编、C 源码与 Python 脚本（不含原始程序的二进制）采用 MIT 许可发布，可自由用于学习、修改和再分发。`mmunicode.dll` 由本仓库编写并随源码以同一许可提供。

## 隐私

本仓库**不包含**任何用户配置、阅读历史、书架、收藏夹、播放列表或本机路径。首次运行生成的 `MangaMeeya.ini`、`*.lst` 等文件已由 `.gitignore` 排除。

## 无担保

本程序按“现状”提供，不附带任何明示或暗示的担保。使用本补丁造成的任何数据损失或兼容性问题由使用者自行承担。
