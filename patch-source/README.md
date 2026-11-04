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

## 应用补丁

需要 Python 3；只有重新汇编时才需要 NASM。

```bat
:: 1) arc.dll（ZIP64）
python apply_patch.py ..\arc.dll ..\arc.dll.patched

:: 2) MangaMeeya.exe（LAA）
python patch_mangameeya_laa.py ..\MangaMeeya.exe ..\MangaMeeya.exe.laa
```

`apply_patch.py` 会校验原版字节，只在原始 `arc.dll`（2006-07-19，229376 字节）上工作；已经打过补丁的文件会被拒绝。

## 重新生成 .bin

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

补丁代码写入 `.text` 尾部 `0x10029170`–`0x10029479` 的空白区；该范围在原节表 `SizeOfRawData` 内，不需要修改任何 PE 头。

## 为什么不是改 DllCharacteristics

`IMAGE_FILE_LARGE_ADDRESS_AWARE` 是 **FileHeader.Characteristics** 的 `0x0020` 位。
`DllCharacteristics` 的 `0x0020` 是 `IMAGE_DLLCHARACTERISTICS_HIGH_ENTROPY_VA`，设置它不会扩大 32 位进程的地址空间。
