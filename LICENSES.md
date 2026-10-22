# 许可

本仓库包含两部分，许可不同：

## 补丁（GPL-2.0）

```
patches/
  ged-6.12-to-5.10.patch
  mali/
```

这些是 MediaTek GED 内核源码（以 GPL-2.0 发布）和 ARM Mali kbase 驱动
（ARM 以 GPL-2.0 发布的 MTK 平台部分）的修改。派生作品同样受 GPL-2.0 约束。

完整许可证文本见 <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>。

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
