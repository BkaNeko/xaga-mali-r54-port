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
