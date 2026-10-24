#!/usr/bin/env bash
# 构建 mali_kbase_mt6895.ko（r54 内核模块）。
#
# 出厂 blob 就是用这条流水线出来的：未 strip 约 46 MB，llvm-strip 后约 2.57 MB。
# 换成原厂构建脚本之前，这里就是唯一事实源。
#
# 用法：
#   scripts/build-mali-ko.sh                        # 用默认路径
#   KDIR=/path/to/kernel OUTDIR=/tmp scripts/build-mali-ko.sh
#
# 关键点（踩过的坑，别删）：
#   1. KBUILD_MODPOST_WARN=1  —— 必须。mali_kbase 引用 ged / mtk_gpufreq_wrapper /
#      mtk_swpm 等其它模块导出的符号（gpufreq_commit、ged_dvfs_gpu_freq_commit_fp…），
#      单独 M= 构建时这些模块不在同一次链接里，modpost 会以
#      "undefined!" 直接失败。它们本来就是运行时由已加载模块解析的，改成警告不影响 ko。
#   2. KCFLAGS 里的 -Wno-error=*  —— 这棵树带着 MTK 平台层的隐式声明和指针类型告警，
#      内核默认 -Werror 会中断构建。只是降级告警，不改变代码。
#   3. llvm-strip --strip-debug  —— 46 MB -> 2.57 MB，几乎全是 DWARF。不 strip 装不进分区。
#   4. CONFIG_MALI_MTK_MEMTRACK 在 gpu/arm/midgard/Makefile 的 mt6895 分支里已经是 y，
#      编出来就带 /proc/mtk_mali/gpu_memory。用 nm 查 mtk_gpu_memory_show 确认。
set -euo pipefail

KDIR="${KDIR:-/home/bkaneko/work/xaga_kernel_5.10}"
MALI_REL="drivers/gpu/mediatek/mali-r54-test"
TOOLCHAIN="${TOOLCHAIN:-/home/bkaneko/work/android-clang12/bin}"
OUTDIR="${OUTDIR:-$(mktemp -d)}"
mkdir -p "$OUTDIR"

export PATH="$TOOLCHAIN:$PATH"

# memtrack 需要两个预处理器宏：CONFIG_MALI_MTK_PROC_FS 管住
# mtk_common_procfs_init() 的调用点，CONFIG_MALI_MTK_MEMTRACK 管住 memtrack.o 与节点创建。
# 这棵树 .config 里没有这两个符号，只在 Makefile 里设了 CONFIG_MALI_MTK_MEMTRACK := y
# （make 变量进不了预处理器），所以这里用 -D 补上，否则 /proc/mtk_mali 不会出现。
KCFLAGS_ALL="-DCONFIG_MALI_MTK_PROC_FS=1 -DCONFIG_MALI_MTK_MEMTRACK=1 \
-Wno-error=implicit-function-declaration \
-Wno-error=macro-redefined \
-Wno-error=incompatible-function-pointer-types \
-Wno-error=incompatible-pointer-types \
-Wno-error=int-conversion \
-Wno-error=declaration-after-statement"

echo "== 构建 =="
echo "   KDIR      = $KDIR"
echo "   TOOLCHAIN = $TOOLCHAIN"
echo "   OUTDIR    = $OUTDIR"

make -C "$KDIR" -j"$(nproc)" \
    ARCH=arm64 LLVM=1 LLVM_IAS=1 CROSS_COMPILE=aarch64-linux-gnu- \
    M="$MALI_REL" modules \
    MTK_PLATFORM=mt6895 MTK_PLATFORM_VERSION=mt6895 \
    KBUILD_MODPOST_WARN=1 \
    KCFLAGS="$KCFLAGS_ALL"

KO="$KDIR/$MALI_REL/gpu/arm/midgard/mali_kbase_mt6895.ko"
[ -f "$KO" ] || { echo "构建产物不存在: $KO" >&2; exit 1; }

echo
echo "== strip =="
cp "$KO" "$OUTDIR/mali_kbase_mt6895.ko"
BEFORE=$(stat -c %s "$OUTDIR/mali_kbase_mt6895.ko")
llvm-strip --strip-debug "$OUTDIR/mali_kbase_mt6895.ko"
AFTER=$(stat -c %s "$OUTDIR/mali_kbase_mt6895.ko")
printf "   %'d -> %'d 字节\n" "$BEFORE" "$AFTER"

echo
echo "== 校验 =="
printf "   文件: %s\n" "$OUTDIR/mali_kbase_mt6895.ko"
printf "   md5 : %s\n" "$(md5sum "$OUTDIR/mali_kbase_mt6895.ko" | cut -d' ' -f1)"
printf "   vermagic: %s\n" "$(strings "$OUTDIR/mali_kbase_mt6895.ko" | grep -m1 '^vermagic=')"
# 注意：不要写成 `llvm-nm ... | grep -q`，pipefail 下 grep 提前退出会让
# llvm-nm 收到 SIGPIPE，整条管道被判失败，结果永远落到 else 分支。
if command -v llvm-nm >/dev/null; then
    SYMS="$(llvm-nm -a "$OUTDIR/mali_kbase_mt6895.ko" 2>/dev/null || true)"
    case "$SYMS" in
        *mtk_gpu_memory_show*)
            printf "   memtrack: 已编入（/proc/mtk_mali/gpu_memory）\n" ;;
        *)
            printf "   memtrack: 未编入 —— 检查 Makefile 的 mt6895 分支\n" ;;
    esac
fi
