# 许可

本仓库包含三部分，许可不同：

## 补丁（GPL-2.0）

```
patches/
  ged-6.12-to-5.10.patch
  mali/
```

这些是 MediaTek GED 内核源码（以 GPL-2.0 发布）和 ARM Mali kbase 驱动
（ARM 以 GPL-2.0 发布的 MTK 平台部分）的修改。派生作品同样受 GPL-2.0 约束。

完整许可证文本见 <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>。

## 固件（专有，不属于本仓库许可范围）

```
firmware/
  mali_csffw.bin      ARM Mali CSF 固件（ARM Ltd. 专有）
  gpueb_stock.bin     MediaTek GPUEB 协处理器固件（MediaTek Inc. 专有）
  gpueb_950mhz.bin    同上，由 gpueb_patch.py 修改 5 个字节生成
```

这些是厂商的二进制固件，**不是开源代码**，不适用本仓库的 GPL-2.0 / MIT 许可。
收录目的仅为记录移植工作所需的配对版本、以及使 950 MHz 补丁可复现与可回滚。

重新分发前请自行确认 ARM 与 MediaTek 的授权条款。若你只关心源码层面的改动，
可以删除整个 `firmware/` 目录，仓库其余部分不受影响。

## 工具（MIT）

```
tools/
```

`gpueb_patch.py`、`glbench.c`、`vkprobe.c`、`gedboot.c`、`gedvsyncd.c`、
`gtprobe.c`、`payload_extract.py` 为本次移植工作新写的工具，以 MIT 许可发布。

具体来说，以下文件源自 MediaTek 的 GPL-2.0 代码，其版权归 MediaTek Inc. 所有：

- `patches/ged-6.12-to-5.10.patch` — 衍生自 `oppo-source/android_kernel_modules_oppo_mt6896`
- `patches/mali/**` — 衍生自 ARM/MediaTek Mali 内核驱动

## 工具（MIT）

```
tools/
  glbench.c
  vkprobe.c
  gedboot.c
  payload_extract.py
```

```
MIT License

Copyright (c) 2026 xaga-mali-r54-port contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 文档（CC BY-4.0）

```
docs/
README.md
```

文档内容采用 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) 许可。
