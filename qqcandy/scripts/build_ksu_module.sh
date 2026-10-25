#!/bin/bash
# 组装 qqcandy 的 KernelSU 用户态模块，输出 tar.gz + 设备端安装脚本。
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QROOT="$(cd "$HERE/.." && pwd)"          # qqcandy/
REPO="$(cd "$QROOT/.." && pwd)"          # 仓库根
OUT="${1:-$QROOT/r54_mali.tar.gz}"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

M="$BUILD/r54_mali"
mkdir -p "$M/system/vendor/lib64/egl/mt6895" \
         "$M/system/vendor/lib64/mt6895" \
         "$M/system/vendor/firmware" \
         "$M/system/vendor/etc/permissions"

# --- r54 包里的 10 个用户态库（仓库根 blobs/vendor）---
for f in lib64/egl/mt6895/libGLES_mali.so \
         lib64/egl/libGLES_meow.so \
         lib64/egl/libMEOW_gift.so lib64/egl/libMEOW_trace.so lib64/egl/libMEOW_qt.so \
         lib64/egl/libMEOW_data.so lib64/egl/libMEOW_mfrc_ext.so \
         lib64/libgpud.so lib64/libmtk_mali_user.so \
         lib64/mt6895/arm.mali.platform-V2-ndk.so \
         lib64/mt6895/libarm_mali_config_sysprops.so \
         lib64/mt6895/libarm_gralloc_properties_sysprop.so \
         lib64/mt6895/libarm_egl_properties_sysprop.so \
         lib64/mt6895/arm.graphics-V5-ndk.so; do
    src="$REPO/blobs/vendor/$f"
    [ -f "$src" ] || { echo "缺 $src" >&2; exit 1; }
    cp "$src" "$M/system/vendor/$f"
done

# --- qqcandy 特有的 5 个（blobs/vendor_extra）---
for f in libc++.so libged.so \
         android.hardware.graphics.common-V6-ndk.so \
         android.hardware.graphics.allocator-V2-ndk.so \
         android.hardware.common-V2-ndk.so; do
    cp "$QROOT/blobs/vendor_extra/$f" "$M/system/vendor/lib64/$f"
done
# libged.so 已在 blobs/vendor_extra 里改好 atrace 符号（大小不变）

# --- 固件 UK 1.38（两份必须都是）---
cp "$REPO/firmware/mali_csffw.bin" "$M/system/vendor/firmware/mali_csffw.bin"
cp "$REPO/firmware/mali_csffw.bin" "$M/system/vendor/firmware/mali_csffw_reload.bin"

# --- 平台配置 / Vulkan 声明 ---
cp "$REPO/files/mali_platform.config" "$M/system/vendor/etc/mali_platform.config"
cp "$REPO/files/android.hardware.vulkan.version.xml" \
   "$M/system/vendor/etc/permissions/android.hardware.vulkan.version.xml"
cp "$REPO/files/android.software.vulkan.deqp.level.xml" \
   "$M/system/vendor/etc/permissions/android.software.vulkan.deqp.level.xml"

# --- 模块元信息 + 属性 ---
cp "$QROOT/files/module.prop"  "$M/module.prop"
cp "$QROOT/files/system.prop"  "$M/system.prop"

tar czf "$OUT" -C "$BUILD" r54_mali
echo "生成: $OUT ($(stat -c %s "$OUT") 字节)"

# --- 设备端安装脚本（负责 chmod + chcon）---
INSTALL="$QROOT/install_module.sh"
cat > "$INSTALL" <<'INST'
#!/system/bin/sh
# 在设备上用 root 执行：sh install_module.sh /sdcard/r54_mali.tar.gz
set -e
TAR="${1:-/sdcard/r54_mali.tar.gz}"
D=/data/adb/modules
[ -f "$TAR" ] || { echo "找不到 $TAR"; exit 1; }
rm -rf "$D/r54_mali"
mkdir -p "$D"
tar xzf "$TAR" -C "$D"
find "$D/r54_mali" -type d -exec chmod 755 {} \;
find "$D/r54_mali" -type f -exec chmod 644 {} \;

M="$D/r54_mali/system/vendor"
HAL=u:object_r:same_process_hal_file:s0
VF=u:object_r:vendor_file:s0
VC=u:object_r:vendor_configs_file:s0

# app 进程要通过 sphal 命名空间加载这些，必须是 same_process_hal_file
chcon $HAL $M/lib64/egl/*.so $M/lib64/egl/mt6895/*.so
chcon $HAL $M/lib64/libged.so $M/lib64/libgpud.so $M/lib64/libmtk_mali_user.so
chcon $HAL $M/lib64/libc++.so
chcon $HAL $M/lib64/android.hardware.graphics.common-V6-ndk.so
chcon $HAL $M/lib64/android.hardware.graphics.allocator-V2-ndk.so
chcon $HAL $M/lib64/android.hardware.common-V2-ndk.so
chcon $HAL $M/lib64/mt6895/*.so
chcon $HAL $M/firmware/mali_csffw.bin
# 其余
chcon $VF  $M/firmware/mali_csffw_reload.bin
chcon $VC  $M/etc/mali_platform.config
chcon $VC  $M/etc/permissions/*.xml

echo "安装完成: $D/r54_mali"
echo "重启后验证: dumpsys SurfaceFlinger | grep -m1 '^GLES:'"
INST
chmod +x "$INSTALL"
echo "安装脚本: $INSTALL"
