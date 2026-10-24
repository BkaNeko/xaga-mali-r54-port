# 二进制组件

移植所需的全部二进制件。放进镜像前请核对 md5。

许可提醒：这些是 ARM 和 MediaTek 的专有二进制，不适用本仓库的 GPL/MIT 许可，
详见 [../LICENSES.md](../LICENSES.md)。

## vendor.img（13 个）

| 目标路径 | 大小 | md5 |
|---|---|---|
| `/vendor/lib64/android.hardware.graphics.common-V6-ndk.so` | 31,512 | `c1bca7a90685` |
| `/vendor/lib64/egl/libGLES_meow.so` | 2,165,008 | `3eb1c1def2a7` |
| `/vendor/lib64/egl/libMEOW_data.so` | 202,360 | `bfdb3dac2d74` |
| `/vendor/lib64/egl/libMEOW_gift.so` | 859,392 | `76cfa9f0680f` |
| `/vendor/lib64/egl/libMEOW_mfrc_ext.so` | 731,304 | `e12515194375` |
| `/vendor/lib64/egl/libMEOW_qt.so` | 201,872 | `24e55141344e` |
| `/vendor/lib64/egl/libMEOW_trace.so` | 592,392 | `e39b1f8a4e3f` |
| `/vendor/lib64/egl/mt6895/libGLES_mali.so` | 51,726,104 | `4fa6d6747d39` |
| `/vendor/lib64/libged.so` | 101,944 | `f28930eddfa5` |
| `/vendor/lib64/libgpud.so` | 462,976 | `359112746293` |
| `/vendor/lib64/libmtk_mali_user.so` | 52,128 | `9758abf5e910` |
| `/vendor/lib64/mt6895/arm.mali.platform-V2-ndk.so` | 85,344 | `eed8eea367cc` |
| `/vendor/lib64/mt6895/libarm_mali_config_sysprops.so` | 52,048 | `601e785792d0` |

## vendor_dlkm.img（5 个）

| 目标路径 | 大小 | md5 |
|---|---|---|
| `/vendor_dlkm/lib/modules/ged.ko` | 2,028,912 | `c5c26e9a9d93` |
| `/vendor_dlkm/lib/modules/gpueb.ko` | 183,752 | `f9a1cdf3e603` |
| `/vendor_dlkm/lib/modules/mali_kbase_mt6895.ko` | 2,571,656 | `c9909233f442` |
| `/vendor_dlkm/lib/modules/mali_mgm.ko` | 28,704 | `45833415252c` |
| `/vendor_dlkm/lib/modules/mali_prot_alloc.ko` | 24,240 | `3580df6a568f` |

## 用法

```bash
# 解原版镜像并保留 xattr
sudo mount -o loop,ro vendor.img /mnt/v
sudo cp -a --preserve=all /mnt/v/. work_vendor/
sudo umount /mnt/v

# 覆盖
sudo cp -a blobs/vendor/. work_vendor/
sudo chown -R 0:0 work_vendor
sudo chmod 644 work_vendor/lib64/egl/mt6895/libGLES_mali.so
```

`vendor_dlkm` 同理：把 `blobs/vendor_dlkm/*.ko` 拷到
`work_vendor_dlkm/lib/modules/`，权限 `644`，标签 `u:object_r:vendor_file:s0`。

标签必须按 `docs/PORTING.zh-CN.md` 第 3.7 节用原版镜像逐文件对齐，
尤其是 `libGLES_mali.so` 和 `mali_csffw.bin` 必须是 `same_process_hal_file`。

## 不在这里的

| 文件 | 位置 |
|---|---|
| `mali_csffw.bin`（UK 1.38） | [../firmware/](../firmware/) |
| `gpueb` 固件（原厂 / 950MHz） | [../firmware/](../firmware/) |
| `build.prop` 的 44 条 EGL 属性 | [../files/vendor-build.prop.egl-r54](../files/) |
| `mali_platform.config` | [../files/mali_platform.config](../files/) |
| `init.insmod.mt6895.cfg` | [../files/init.insmod.mt6895.cfg](../files/) |
| Vulkan 声明 XML | [../files/](../files/) |
