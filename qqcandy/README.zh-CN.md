# Mali r54p1 on qqcandy (ossi / MT6895)

把 13T（mt6895）的 Mali r54p1 GPU 栈搬到 **qqcandy**。和 xaga 移植（仓库根目录）是同一套 DDK，
但**落盘方式完全不同**：这台机器的内核在 `early-init` 就加载 GPU 模块，而且
OPPO 的内核安全驱动禁止任何非 recovery 进程写受保护分区，所以内核模块必须写真分区，
用户态则用 KernelSU 元模块做 systemless overlay。

## 结果

```
GLES: ARM, Mali-G610 MC6, OpenGL ES 3.2 v1.r54p1-11eac0
mali: CSF firmware was successfully initialized by process 'surfaceflinger'
android.hardware.vulkan.version = 4206592  (1.3.0)
ro.vendor.arm.egl.configs.*     44 条生效
ro.vendor.arm.gralloc.*         12 条生效
```

## 目标机

| 项 | 值 |
|---|---|
| 机型 | `ro.vendor.build.fingerprint = oplus/ossi/ossi:12/...` |
| 平台 | MT6895 |
| 内核 | `5.10.236-android12-Furruka`（自定义内核，vermagic 校验被关掉，所以 5.10.209 的模块也能加载） |
| 原厂 DDK | r32p1-01eac0（UK 1.18）→ 目标 r54p1-11eac0（UK 1.38） |
| 分区 | vendor / vendor_dlkm 都在 super 里（逻辑分区），gpueb 是物理分区 |

## 两部分

### 1. 用户态 —— KernelSU 元模块（systemless）

`magic_mount_rs` 元模块在 `post-fs-data` 把 r54 的用户态文件 bind 到 `/vendor` 上，
不碰分区。模块内容见 `scripts/build_ksu_module.sh`（它从仓库根目录的 `blobs/` 取 10 个 r54 库，
再叠加本目录 `blobs/vendor_extra/` 里 qqcandy 特有的 5 个文件）。

属性**不能**走 overlay 的 `vendor/build.prop`：properties 在 `early-init` 就读完了，
比 `post-fs-data` 早。属性必须放模块的 `system.prop`（KernelSU 在 `post-fs-data` 用 resetprop 加载）。

需要的文件：
- 10 个 r54 用户态库（仓库根目录 `blobs/vendor/`）
- `blobs/vendor_extra/libc++.so` —— 底包 VNDK libc++ 缺 `__libcpp_verbose_abort`
- `blobs/vendor_extra/android.hardware.graphics.{common-V6,allocator-V2}-ndk.so`
  + `android.hardware.common-V2-ndk.so` —— libgpud 的新依赖，sphal 命名空间看不到 /system
- `blobs/vendor_extra/libged.so` —— r54 libged 引用了老 libcutils 没有的
  `atrace_async_for_track_*_body`，已等长改指 `atrace_begin_body`/`atrace_end_body`
- `files/system.prop` —— 44 条 EGL + 12 条 gralloc

### 2. 内核态 —— 必须写真分区

`scripts/build_dlkm_image.py` 用底包 `vendor_dlkm.img` + `blobs/vendor_dlkm/` 里的 3 个 r54 模块
生成新镜像，再用 `scripts/make_recovery_zip.py` 包成 recovery 刷机包。

**`ged.ko` / `gpueb.ko` 保持原厂**，原因见 `docs/PITFALLS.zh-CN.md` 第 3 条。

## 刷写

```bash
# 1) 用户态：把 KSU 模块放到 /data/adb/modules/r54_mali（或 KernelSU Manager 安装 zip）
bash scripts/build_ksu_module.sh           # 生成 r54_mali.zip
# KernelSU Manager → 模块 → 从本地安装

# 2) 内核态：recovery 里刷（fastboot 不行，见 PITFALLS 第 1 条）
adb push r54_dlkm.zip /sdcard/
adb shell                                  # 在 OrangeFox 里
mount /data                                # 默认没挂载！
twrp install /sdcard/r54_dlkm.zip
```

重启后验证：

```bash
dumpsys SurfaceFlinger | grep -m1 '^GLES:'    # 应为 v1.r54p1-11eac0
getprop | grep -c arm.egl.configs             # 应为 44
```

## 950 MHz —— 这台机器做不到

xaga 上 950 MHz 是改 gpueb 固件里 OPP 表 entry[1]（880→950）实现的。
**qqcandy 的 gpueb 固件带 RSA 签名**（尾部有 `cert1` 段，内含 DER 编码的 RSA-2048 公钥），
payload 一个字节都不能动，改了 GPUEB 协处理器就起不来、整机卡死。

详见 `docs/PITFALLS.zh-CN.md` 第 6 条。回滚镜像：`blobs/gpueb_stock_restore.img`。

## 目录

```
qqcandy/
├── README.zh-CN.md
├── docs/
│   ├── PORTING.zh-CN.md      操作步骤
│   └── PITFALLS.zh-CN.md     六个坑，含现象和判断方法
├── files/
│   ├── system.prop           44 EGL + 12 gralloc 属性
│   ├── module.prop           KSU 模块描述
│   └── magic_mount.config.toml
├── blobs/
│   ├── vendor_dlkm/          r54 的 3 个内核模块
│   ├── vendor_extra/         qqcandy 特有的 5 个用户态文件
│   └── gpueb_stock_restore.img
└── scripts/
    ├── build_ksu_module.sh
    ├── build_dlkm_image.py
    └── make_recovery_zip.py
```
