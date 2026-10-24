# 把移植打成可刷入的 `super.img`

`docs/PORTING.zh-CN.md` 讲的是**内核侧怎么做移植**（以 LineageOS 24 + 5.10.269 为基准）。
本篇讲另一件事：**已经能在别的 ROM 上跑起来的这套 r54 栈，怎么落到一台真实设备的原厂包里**，
产出一个直接 `fastboot flash super` 就能起的镜像。

实测底包：xaga（Redmi Note 11T Pro）× HyperOS 4（`Hyper4.00.7test`，Android 17 system + Android 12 vendor）。
底包特征：VNDK v31（`libc++` 缺 257 个新符号）、HIDL gralloc（无 AIDL 分配器）、EGL 实现是 MTK 的
`meow` 包装层、内核 `5.10.237-Fuutao`、已 `--disable-verity` 刷入。

---

## 1. 产出与原则

| 产出 | 说明 |
|---|---|
| `super.img` | 只替换 `vendor` 与 `vendor_dlkm` 两个分区，其余分区按底包原样重打；几何（分区表、大小）与底包完全一致 |
| `gpueb.img` | OPP 表 entry[1] 880 → 950 MHz（5 字节，尺寸不变） |

原则：**底包不动、只做增量**。所有改动都发生在 mount 出来的分区树上，再按原 owner/权限/SELinux 标签重建镜像。
这样出问题时回滚就是刷回原包 `images/super.img`。

## 2. 依赖

```bash
# 镜像工具（lpunpack/lpmake 建议用系统的：qcom-port 自带那份对系统 simg2img 的输出会报 invalid checksum）
apt install android-sdk-libsparse-utils erofs-utils   # simg2img / dump.erofs / mkfs.erofs
# mkfs.erofs 必须能接受 --fs-config-file 与 --file-contexts，否则 SELinux 标签只能靠源文件 xattr
#   → 用 AOSP 分支的 mkfs.erofs.aosp
```

挂载用 `erofsfuse`（非特权即可挂 erofs，且能读到 `security.selinux` xattr）：

```bash
erofsfuse vendor_a.img mnt/vendor
```

## 3. 七步流程

```bash
# 1) sparse super → raw，读 liblp 元数据（分区表 + 每分区大小）
simg2img images/super.img super_raw.img

# 2) 解出各分区
lpunpack --slot=0 super_raw.img parts/    # system/system_ext/product/vendor/odm/odm_dlkm/vendor_dlkm

# 3) 挂载 vendor 与 vendor_dlkm，**同时记录**每个文件的 owner / mode / security.selinux
erofsfuse parts/vendor_a.img mnt/vendor

# 4) 注入 payload（见第 4 节）

# 5) 按原几何重建分区：fs_config（owner/mode）+ file_contexts（SELinux）
mkfs.erofs.aosp \
  --fs-config-file fs_config/vendor.fsconfig \
  --file-contexts fs_config/vendor.file_contexts \
  -z lz4hc,9 -b 4096 --mount-point /vendor \
  repacked/vendor.img mnt/vendor

# 6) 按底包 liblp 元数据重打 super（顺序、大小、组、slot 后缀都要一致）
lpmake --metadata-size 65536 --super-name super \
  --device super:9126805504 \
  --group main:9126805504 \
  --partition system:readonly:...:main \
  ... \
  --sparse --output out/super.img

# 7) gpueb：0x15d78 880000→950000，0x15d80 75000→78750（见 tools/gpueb_patch.py）
```

> `super` 的原始大小必须与底包一致（本例 6,242,172,928 B），sparse 打包后约 5.9 GB。

## 4. 九个必踩的坑

每一条都是"现象 → 原因 → 做法"，全部可离线完成（改二进制时**不改变文件大小**）。

| # | 现象 | 原因 | 做法 |
|---|---|---|---|
| 1 | `mali_kbase_mt6895: Unknown symbol cancel_work (err -2)` | r54 kbase 引用的 `cancel_work` 在 5.10 内核里没导出 | 符号表里把该 undefined 符号的 `st_name` 改指到已导出的 `cancel_work_sync`（`.symtab`/`.dynsym` 都要看） |
| 2 | 模块加载被拒 | 移植仓库模块 vermagic `5.10.269-…`，底包是 `5.10.226-android12-9-ga738ee7cbe68` | 重写 `.modinfo`：去掉 `srcversion`，必要时去 `parmtype`/`description`，把 vermagic 换成长度相同的目标串 |
| 3 | `dlopen failed: cannot locate symbol _ZNSt3__122__libcpp_verbose_abortEPKcz` | VNDK v31 的 `libc++` 是 Android 12 的，缺 r54 需要的 257 个符号 | 从更新的 ROM（如 13T A16）取 `libc++.so`，装成 `vendor/lib64/libc++.so` |
| 4 | zygote64 反复 `terminate: std::bad_cast`，卡开机 | 若把新 libc++ **改名旁挂**（如 `libcxx.so`），一个进程里就有两份 libc++；`std::locale::use_facet` 的 facet id 是各库内静态对象，跨副本必然不匹配 | **不要旁挂**。让 sphal 命名空间里只解析到一份：直接把新 libc++ 装成 `vendor/lib64/libc++.so`，r54 库的 `DT_NEEDED` 保持 `libc++.so` 不动 |
| 5 | `library "android.hardware.graphics.allocator-V2-ndk.so" not found: needed by libgpud.so` | 底包是 HIDL gralloc，没有 AIDL 分配器客户端 | 从新 ROM 补 `allocator-V2-ndk.so` 与 `common-V2-ndk.so`（`common-V2` 也可由 r54 自带的 `common-V6` 满足，二者选一） |
| 6 | `cannot locate symbol atrace_async_for_track_begin_body`（libged） | VNDK v31 的 libcutils 只有 `atrace_begin_body` 一类老 API | 把 `libged.so` 的两个 undefined 符号改指 `atrace_begin_body` / `atrace_end_body`；**只改 `lib64`**，32 位那份改了会踩到别的库 |
| 7 | `eglInitialize` 抛 `bad_optional_access` / `no suitable EGLConfig` | r54 不再内置 EGL config 表，靠 44 条 `ro.vendor.arm.egl.configs.*` 组装 | 注入 `files/vendor-build.prop.egl-r54` 那 44 条 |
| 8 | 起不来后看 SF 日志：`Access denied finding property "ro.vendor.arm.egl.configs.*"`，紧接着又是 `bad_optional_access` | surfaceflinger 属于 plat 域，读不了任何 `vendor_*_prop` 类型 | 在 `vendor/etc/selinux/vendor_property_contexts` 里把 `ro.vendor.arm.` 改标成 `system_prop`（`ro.vendor.arm.gralloc.` 同理） |
| 9 | `fastboot flash super` 报 `Error reading sparse file` | 该 fastboot 版本默认 256 MB 分块读稀疏文件会噎住 | 加 `-S 64M`，约 4 分钟写完 |

## 5. 保尺寸改二进制的几种手法（可复用）

| 目标 | 手法 |
|---|---|
| 改 vermagic / 去 `srcversion` | 解析 `.modinfo` 的 key/value 序列，重排后整段重写（长度变化没关系，因为 `.modinfo` 是独立 section；只要不改文件总大小） |
| 让 A 符号指向 B | 在 `.strtab` 里追加（或复用）目标符号名，改 undefined 符号的 `st_name`——长度无关 |
| 改 `DT_NEEDED` / `DT_SONAME` | `.dynstr` 原地替换，**必须等长**（如 `libc++.so` ⇄ 9 字节以内） |
| 隐藏某个 GL 扩展 | 驱动把扩展名以独立 NUL 结尾字符串放在 `.rodata`，**等长**改名（如 `GL_KHR_blend_equation_advanced` → `...advanceX`），客户端就探测不到；r54 的扩展串是运行时拼装的，没有可改的过滤表 |

## 6. r54 独有的驱动开关（r32 完全没有）

r54 引入了 **DFC（Driver Feature Control）** 与一批 `GLES_*` 特性开关，配置来源：

```
/vendor/etc/mali_platform.config       ← 最稳，随镜像走
vendor.mali.platform.config            属性（可写，但驱动默认不打印是否生效）
/mali_app.config、/data/data/com.mali.testjava/mali_debug.config
```

语法就是 `KEY=VALUE`，一行一条。常用键（语义取自驱动内描述串）：

| 键 | 作用 |
|---|---|
| `MALI_DFC_ALLOW_ARM_COMPRESSION` | 是否允许 ARM 压缩（AFBC） |
| `MALI_DFC_ALLOW_ARM_VENDOR_EXTENSIONS` | 是否对外枚举 `GL_ARM_*` 扩展 |
| `MALI_DFC_ALLOW_NAN_PROPAGATION` / `_INFINITY_PROPAGATION` | NaN/Inf 传播 vs 归零（影响带 NaN 判定的 SDF 类着色器） |
| `MALI_DFC_ALLOW_RELAXED_PRECISION` / `ALLOW_UNSAFE_MATH_OPTIMIZATIONS` | 放松精度/不安全数学优化 |
| `MALI_DFC_ALLOW_SUBPASS_FUSION` / `ALLOW_EMPTY_TILE_ELIMINATION` / `ALLOW_FORWARD_PIXEL_KILL` / `ALLOW_TRANSACTION_ELIMINATION` | 各类省界/省栅格化优化，出"脏画面"时可逐个关 |
| `GLES_DISABLE_SURFACE_AFBC` / `GLES_DISABLE_TEXTURE_AFBC` | 关 surface / texture 的 AFBC |
| `GLES_DELAY_AFBC_DECOMPRESSION` / `GLES_FORCE_LINEAR_LAYOUT_UNCOMPRESSED_TEXTURE` | AFBC 延迟解压 / 强制线性未压缩布局 |
| `GLES_FB_STRICT_FLUSH` / `GLES_ALWAYS_BLIT_TRANSFER_PATH` | 渲染目标改动后严格 flush；强制走 blit 传输 |

排障顺序建议：**先用压缩族**（对"刚画完的 mask/纹理被读成旧值或读错"最有效），再动数学族。

## 7. 验收清单（设备实测）

```bash
getprop sys.boot_completed                         # 1
dumpsys SurfaceFlinger | grep "GLES: ARM"           # … OpenGL ES 3.2 v1.r54p1-11eac0…
cat /sys/class/misc/mali0/device/gpuinfo            # Mali-G610 6 cores r0p0
cat /proc/modules | grep -E '^mali_kbase|^ged|^gpueb'   # 三个都在
sed -n 2p /proc/gpufreqv2/gpu_signed_opp_table      # freq: 950000
cmd gpu vkjson | head -3                            # apiVersion 4210688 = 1.4.0
pm list features | grep -E 'vulkan.(version|deqp)'  # 4206592 / 132711169
# 日志侧应为 0：GPU fault / MMU fault / CSF timeout / Unknown symbol / avc denied(ro.vendor.arm.*)
```

## 8. 已知问题

### 8.1 柔光玻璃材质出现"直角深色内层"（r54 有、r32 无）

只在 `material_style=1`（柔光玻璃）时出现：圆角面板内部多出一块直角、深色的矩形。静态分析结论：

- 实现链：SystemUI `FocusNotificationGlassEffect` / `GlassCustomBgEffect` → 隐藏 API
  `setMiGlass(view,float[42])`、`setMiGlassClip(rect)`、`setForceSDFT(true)` → hwui 原生
  `FillRectOpMiGlassImpl` / `SkCanvas::drawRectMiGlass`。
- **圆角完全由 SDF 掩码给出**：`setClipToOutline(false)`，只靠 `setGlassOutlineEnable`；
  `setMiGlassClip` 只给矩形四边。所以掩码一失效，矩形 clip 就是唯一裁剪 → 直角泄漏。
- SDF 判定式：`color = (sdf <= 0.) ? half4(0.0) : renderGlassShape(...)`，且 `radius=0` 时
  `shiftedPos / scaledRadius` 会除零出 NaN —— NaN 的走向决定"透明"还是"整块压暗"。
- 因此方向是 r54 的 math/精度或 AFBC 行为让 SDF 掩码失效，可用第 6 节的 DFC 开关做 A/B。

（该材质的 `SOFT_LIGHT` 混合其实是 SystemUI 侧 `BlendModeToken` 下发的，走 hwui 的 MiBlend AGSL，
与 `GL_KHR_blend_equation_advanced` 无关；隐藏该扩展对这种走 MiGlass 的面板无效。）

### 8.2 memtrack 拿不到显存

r54 kbase 不再导出 `/proc/mtk_mali/gpu_memory` 与 `/proc/mali/memory_usage`，`dumpsys meminfo` 的
GPU 内存为 0。属统计缺失（LMK 少一份参考），不影响渲染。

### 8.3 Tiler heap 反复回收

模块未开 `CONFIG_LARGE_PAGE_SUPPORT`，dmesg 会出现
`Tiler heap reclaim scan freed pages: N (unused: N)`，有一定 GPU 性能损失，需重编模块才能根治。

## 9. 刷写与回滚

```bash
FB=<底包>/Tools/linux/fastboot
$FB -S 64M flash super  out/super.img
$FB flash gpueb_a        out/gpueb.img
# 回滚
$FB -S 64M flash super  <底包>/images/super.img
$FB flash gpueb_a        <底包>/firmware/gpueb_stock.bin
```

AVB 不处理：底包本身以 `--disable-verity` 刷入、vbmeta 校验值早已不匹配，沿用同样刷法即可。
