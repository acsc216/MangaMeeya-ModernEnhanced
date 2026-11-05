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
| `unicode/mmunicode.c` | 文件名兼容层源码（32 位 DLL） |

## 应用补丁

需要 Python 3；重新汇编 `arc.dll` 补丁时才需要 NASM；重新编译 `mmunicode.dll` 需要 32 位（x86）MSVC 或 MinGW 工具链。

```bat
:: 1) arc.dll（ZIP64）
python apply_patch.py ..\arc.dll ..\arc.dll.patched

:: 2) MangaMeeya.exe（LAA）
python patch_mangameeya_laa.py ..\MangaMeeya.exe ..\MangaMeeya.exe.laa

:: 3) MangaMeeya.exe（新增 mmunicode.dll 导入；在 1/2 之后执行）
python patch_mangameeya_unicode.py ..\MangaMeeya.exe.laa ..\MangaMeeya.exe
```

`apply_patch.py` 会校验原版字节，只在原始 `arc.dll`（2006-07-19，229376 字节）上工作；已经打过补丁的文件会被拒绝。
`patch_mangameeya_unicode.py` 通过新增 `.mmuimp` 节来定位，重复执行会被拒绝。

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

arc.dll 的补丁代码写入 `.text` 尾部 `0x10029170`–`0x10029479` 的空白区；该范围在原节表 `SizeOfRawData` 内，不需要修改任何 PE 头。

## 为什么不是改 DllCharacteristics

`IMAGE_FILE_LARGE_ADDRESS_AWARE` 是 **FileHeader.Characteristics** 的 `0x0020` 位。
`DllCharacteristics` 的 `0x0020` 是 `IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA`，设置它不会扩大 32 位进程的地址空间。

## mmunicode.dll 的工作方式

MangaMeeya 只使用 ANSI（A 后缀）API。当路径中的字符在当前 ANSI 代码页（简体中文为 CP936）中无法表示时，A 调用看到的路径里该字符会变成 `?`，于是 `CreateFileA` 等调用失败。

DLL 在启动时挂钩主程序、`arc.dll` 与 Susie 插件的 IAT。原调用失败且路径含 `?` 时，它从根目录开始逐级枚举父目录，把每个条目的宽字符名按相同规则转换成 ANSI 后与请求名比较，从而还原出真实 Unicode 路径并改用 W API 重试。程序内部的 ANSI 字符串没有改变，因此界面上的特殊字符仍会显示为 `?`，但文件可以正常打开。
