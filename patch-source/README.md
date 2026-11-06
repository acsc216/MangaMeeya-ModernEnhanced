# Patch source / 补丁源码

本目录包含 `MangaMeeya 现代增强版` 的全部修改源码。原始二进制不在此目录中。

## 内容

| 文件 | 作用 |
|---|---|
| `zip64_extra.asm` | 修复 ZIP64 extra field 解析（2GB 以上偏移的条目） |
| `zip64_eocd.asm` | 修复普通 EOCD → ZIP64 EOCD（>4GB 包，无注释） |
| `zip64_eocd_comment.asm` | 修复带归档注释的 >4GB 包 |
| `*.bin` | 上述汇编经 `nasm -f bin` 生成的机器码（已测试） |
| `apply_patch.py` | 将三处补丁写入 `arc.dll` |
| `patch_mangameeya_laa.py` | 设置 `MangaMeeya.exe` 的 LAA 标志 |
| `patch_mangameeya_unicode.py` | 给 `MangaMeeya.exe` 增加 `mmunicode.dll` 导入（特殊字符文件名支持） |
| `patch_mangameeya_txtcrash.py` | 修复文本页行扫描越界（`sub_453040`，预读 ≥2 页时崩溃） |
| `patch_mangameeya_shellmenu.py` | Explorer 右键菜单改为按用户写入 `HKCU\Software\Classes` |
| `shellmenu/shellmenu.asm`、`shellmenu.bin` | 右键菜单补丁汇编源码与机器码 |
| `unicode/mmunicode.c` | 文件名兼容层源码（32 位 DLL） |
| `webp/webp.c`、`webp/webp.def` | WebP Susie 图像插件源码（32 位，WIC 解码） |

## 应用补丁

需要 Python 3；重新汇编 `arc.dll` 补丁或 `shellmenu` 时才需要 NASM；重新编译 `mmunicode.dll` 或 `webp.spi` 需要 32 位（x86）MSVC 或 MinGW 工具链。

```bat
:: 1) arc.dll（ZIP64）
python apply_patch.py ..\arc.dll ..\arc.dll.patched

:: 2) MangaMeeya.exe（LAA）
python patch_mangameeya_laa.py ..\MangaMeeya.exe ..\MangaMeeya.exe.laa

:: 3) MangaMeeya.exe（文本页越界修复）
python patch_mangameeya_txtcrash.py ..\MangaMeeya.exe.laa ..\MangaMeeya.exe.txtfix

:: 4) MangaMeeya.exe（Explorer 右键菜单按用户注册）
python patch_mangameeya_shellmenu.py ..\MangaMeeya.exe.txtfix ..\MangaMeeya.exe.shellmenu

:: 5) MangaMeeya.exe（新增 mmunicode.dll 导入）
python patch_mangameeya_unicode.py ..\MangaMeeya.exe.shellmenu ..\MangaMeeya.exe
```

`apply_patch.py` 会校验原版字节，只在原始 `arc.dll`（2006-07-19，229376 字节）上工作；已经打过补丁的文件会被拒绝。
`patch_mangameeya_unicode.py` 通过新增 `.mmuimp` 节来定位，重复执行会被拒绝。
`patch_mangameeya_txtcrash.py` 校验 `0x531E0` 处的原始指令与 `.text` 尾部空区，重复执行会被拒绝。
`patch_mangameeya_shellmenu.py` 校验三个 shell 函数的原始字节与 `0xE4700` 空区，重复执行会被拒绝。四个 exe 补丁互不依赖，执行顺序可以互换。

## 重新编译 mmunicode.dll

`mmunicode.c` 是一个不依赖 CRT DLL 的 32 位 DLL（只导入 `kernel32.dll`）。用 x86 工具链编译：

```bat
:: MSVC（x86 Native Tools / vcvars32）
cl /nologo /LD /MT /O2 /W0 mmunicode.c /link /OUT:mmunicode.dll

:: MinGW-w64（i686）
i686-w64-mingw32-gcc -shared -O2 -static -o mmunicode.dll mmunicode.c -lkernel32
```

把生成的 `mmunicode.dll` 放到 `MangaMeeya.exe` 同目录即可。设置环境变量 `MMU_LOG=1` 可在 `%TEMP%\mmu.log` 查看挂钩/路径还原日志。

## 重新生成 arc.dll 的 .bin

如果修改了 `.asm`，用 NASM 重新生成：

```bat
nasm -f bin zip64_extra.asm          -o zip64_extra.bin
nasm -f bin zip64_eocd.asm           -o zip64_eocd.bin
nasm -f bin zip64_eocd_comment.asm   -o zip64_eocd_comment.bin
```

## 补丁位置

| 文件 | 文件偏移 | 虚拟地址 | 说明 |
|---|---|---|---|
| arc.dll | `0x093C5` | `0x100093C5` | ZIP64 extra field 解析 |
| arc.dll | `0x09BB5` | `0x10009BB5` | EOCD → ZIP64 EOCD（无注释） |
| arc.dll | `0x09DAE` | `0x10009DAE` | EOCD 扫描分支的 ZIP64 处理 |
| MangaMeeya.exe | `e_lfanew + 22` | — | PE FileHeader.Characteristics 置 `0x20` |
| MangaMeeya.exe | 文件末尾 | 新节 `.mmuimp`（原 `SizeOfImage` 处） | 复制原导入描述符数组 + 新增 `mmunicode.dll!MMUInit`，并把 Import Directory 指向新数组 |
| MangaMeeya.exe | `0x0531E0` | `0x4531E0` | 文本行扫描入口改为 5 字节 `jmp`，跳入 `0xE4600` 的越界检查代码 |
| MangaMeeya.exe | `0x058D50` | `0x458D50` | 右键菜单“检查命令”改为 HKCU，跳入 `0xE476D` |
| MangaMeeya.exe | `0x058E60` | `0x458E60` | 右键菜单“写入命令”改为 HKCU，跳入 `0xE47F8` |
| MangaMeeya.exe | `0x058FD0` | `0x458FD0` | 右键菜单“删除命令”改为 HKCU，跳入 `0xE488C` |

arc.dll 的补丁代码写入 `.text` 尾部 `0x10029170`–`0x10029479` 的空白区；该范围在原节表 `SizeOfRawData` 内，不需要修改任何 PE 头。

## 为什么不是改 DllCharacteristics

`IMAGE_FILE_LARGE_ADDRESS_AWARE` 是 **FileHeader.Characteristics** 的 `0x0020` 位。
`DllCharacteristics` 的 `0x0020` 是 `IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA`，设置它不会扩大 32 位进程的地址空间。

## mmunicode.dll 的工作方式

MangaMeeya 只使用 ANSI（A 后缀）API。当路径中的字符在当前 ANSI 代码页（简体中文为 CP936）中无法表示时，A 调用看到的路径里该字符会变成 `?`，于是 `CreateFileA` 等调用失败。

DLL 在启动时挂钩主程序、`arc.dll` 与 Susie 插件的 IAT。原调用失败且路径含 `?` 时，它从根目录开始逐级枚举父目录，把每个条目的宽字符名按相同规则转换成 ANSI 后与请求名比较，从而还原出真实 Unicode 路径并改用 W API 重试。扫描 ANSI 路径时按 `IsDBCSLeadByteEx(CP_ACP, …)` 逐字符前进，不会把双字节字符的尾字节 `0x5C`（`\`）误当成目录分隔符（例如 `診` 在 GBK 中是 `D4 5C`）。程序内部的 ANSI 字符串没有改变，因此界面上的特殊字符仍会显示为 `?`，但文件可以正常打开。

## 重新编译 webp.spi

`webp/` 是一个 32 位 Susie 图像插件，通过 Windows Imaging Component（WIC）解码 WebP。它不包含任何第三方解码库，依赖系统的 WIC WebP 编解码器（Windows 11 自带；Windows 10 可安装“WebP Image Extensions”）。

```bat
cl /nologo /LD /MT /O2 /W0 webp.c /link /DEF:webp.def windowscodecs.lib ole32.lib user32.lib /OUT:webp.spi
```

把 `webp.spi` 复制到 `SusiePlugin\`（即 `[Susie] PluginPath`）即可。插件注册 `*.WEBP`，`GetPicture` 返回 24bpp 自下而上的 DIB；透明通道在插件内合成到白色背景（MangaMeeya 的 Susie 图像路径不做逐像素 alpha 合成），动画 WebP 只显示第一帧。

## 文本页越界修复（patch_mangameeya_txtcrash.py）

文本渲染函数 `sub_453040`（VA `0x453040`）的行扫描循环在读取 `buffer[index]` 之后才检查 `index == length`。当文本以多字节字符的首字节结尾时，下标一次加 2 会跳过这个“等于”判断，之后循环越过缓冲区继续读取，直到访问未映射页而崩溃（`0xC0000005`）。

补丁在 RVA `0xE4600`（`.text` 的原始数据范围内、`VirtualSize` 之后的零填充区，页面已映射且可执行）写入：

```asm
mov esi, [esp+0x10]     ; this
mov eax, [esi+0xD34]    ; length
cmp ebx, eax
jb  short below
mov ebx, eax            ; 越界时夹到 length
jmp 0x4534F7            ; 正常的文本结束分支
below:
mov ecx, [esp+0x14]     ; buffer
mov al, [ebx+ecx]
cmp al, 0Dh
je  0x4534F3
cmp al, 0Ah
je  0x4534F3
jmp 0x4531F7
```

原 `0x4531E0` 处的 `mov ecx,[esp+0x14]` 被替换为 `jmp 0x4E4600`。两个循环回跳目标（`0x4531E0`）及所有外部引用都会经过这段检查；`0x4531E4` 之后没有被其他代码直接跳入，因此替换是安全的。

## Explorer 右键菜单修复（shellmenu/）

Windows 7 以后，普通权限进程不能写 `HKEY_CLASSES_ROOT`；MangaMeeya 的空 `<assembly>` 清单又禁用了 UAC 虚拟化。因此“文件关联设置”里的右键菜单注册在 Win11 上会静默失败（对话框正常关闭，但注册表没有任何变化）。

补丁把三个辅助函数替换为写入当前用户的类注册：

- `sub_458D50`（检查命令） → `check_entry`
- `sub_458E60`（写入命令） → `set_entry`
- `sub_458FD0`（删除命令） → `del_entry`

路径规则：

| 列表项 | 注册表位置 |
|---|---|
| `Folder` | `HKCU\Software\Classes\Folder\shell\用MangaMeeya打开\command` |
| `.zip` / `.rar` / `.lzh` / `.cbz` / `.cbr` / `.cbl` / `.jpg` / `.png` / `.gif` / `.bmp` | `HKCU\Software\Classes\SystemFileAssociations\<ext>\shell\用MangaMeeya打开\command` |
| 第 5 项（显示为 `....`） | 不注册 |

补丁代码位于 `0xE4700`，三个入口各用 5 字节 `jmp` 跳入；参数从原始 cdecl 栈读取，不依赖原函数序言。

重新生成机器码：

```bat
nasm -f bin shellmenu.asm -o shellmenu.bin
```

> Win11 的默认右键菜单只显示新式菜单项；旧式 shell 项（包括本补丁注册的）位于 **“显示更多选项”（Shift+F10）**。进入一级菜单需要带 `IExplorerCommand` 的 MSIX/sparse package，本补丁不涉及。
