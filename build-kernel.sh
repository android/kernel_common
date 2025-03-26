#!/bin/sh
#
# A small script used to rebuild the BlissOS kernel image
# Shamelessly stol.. "inspired by" prebuilts/qemu-kernel/build-kernel.sh
#

CMD=$1

if [ -z "$ANDROID_BUILD_TOP" ]; then
    echo "ERROR: You must have run lunch in the top level android source directory to run this script."
    exit 1
fi

# TODO(dubielm): We have to decide if we want per family or per product builds
# and then we should be able to use TARGET_PRODUCT directly.
BUILD_FAMILY_NAME=$(basename $ANDROID_PRODUCT_OUT)

OUTPUT=$ANDROID_BUILD_TOP/device/google/desktop/${BUILD_FAMILY_NAME}-kernels/6.6

# Prevent devs from accidentally using this legacy build script for devices
# that should use Kleaf-based kernels.
if [ $BUILD_FAMILY_NAME = "brya" ]; then
  echo -e "\n**********************************************************************"
  echo "ATTENTION: Brya devices migrated to Kleaf-based kernel prebuilts."
  echo "Building Brya kernels with the legacy system is deprecated."
  echo "Follow go/desktop-gki 'GKI Instructions' section instead."
  echo -e "**********************************************************************\n"
  echo "Exiting build..."
  exit 1
fi

# Add legacy_kernel to corsola output
if [ $BUILD_FAMILY_NAME = "corsola" ]; then
  OUTPUT=$OUTPUT/legacy_kernel
fi

# Determine the host architecture, and which default prebuilt tag we need.
# For the toolchain auto-detection.
#
HOST_OS=`uname -s`
case "$HOST_OS" in
    Linux)
        # note that building  32-bit binaries on x86_64 is handled later
        HOST_OS=linux
        HOST_TAG=linux-x86
        BUILD_NUM_CPUS=$(grep -c processor /proc/cpuinfo)
        ;;
    *)
        echo "ERROR: Unsupported OS: $HOST_OS"
        exit 1
esac

if [ ! -f include/linux/vermagic.h ]; then
    echo "ERROR: You must be in the top-level kernel source directory to run this script."
    exit 1
fi

# Extract kernel version, we'll need to put this in the final binaries names
# to ensure the emulator can trivially know it without probing the binary with
# 'file' or other unreliable heuristics.
KERNEL_MAJOR=$(awk '$1 == "VERSION" { print $3; }' Makefile)
KERNEL_MINOR=$(awk '$1 == "PATCHLEVEL" { print $3; }' Makefile)
KERNEL_PATCH=$(awk '$1 == "SUBLEVEL" { print $3; }' Makefile)
KERNEL_VERSION="$KERNEL_MAJOR.$KERNEL_MINOR.$KERNEL_PATCH"
echo "Found kernel version: $KERNEL_VERSION"

# Ensure output dir exists, create if necessary
mkdir -p $OUTPUT

ZIMAGE=zImage

ARCH=$($ANDROID_BUILD_TOP/build/soong/soong_ui.bash --dumpvar-mode TARGET_ARCH)
case $ARCH in
    x86|x86_64)
        ZIMAGE=bzImage
        CONFIG=android-x86_64
        ;;
    arm64)
        ZIMAGE=Image
        CONFIG=android-arm64
        ;;
    mips)
        ZIMAGE=
        ;;
    mips64)
        ZIMAGE=
        ;;
esac

export LLVM=1
PATH=$ANDROID_BUILD_TOP/prebuilts/clang/host/$HOST_TAG/clang-r530567/bin/:$PATH
export ARCH

case $CONFIG in
    defconfig)
        MAKE_DEFCONFIG=$CONFIG
        ;;
    *)
        MAKE_DEFCONFIG=${CONFIG}_defconfig
        ;;
esac

# Temp dir to hold the installed modules, auto-removed on exit
TMP_MODULES="$(mktemp -d)"
trap 'rm -rf -- "$TMP_MODULES"' EXIT

if [[ "$CMD" == "menuconfig" ]]; then
    make -j $BUILD_NUM_CPUS $MAKE_DEFCONFIG && make -j $BUILD_NUM_CPUS $CMD
    exit
fi

if [[ "$CMD" == "savedefconfig" ]]; then
    make -j $BUILD_NUM_CPUS $CMD && cp defconfig arch/$ARCH/configs/$MAKE_DEFCONFIG
    exit
fi

if [ "$ARCH" == "arm64" ] ; then
    # Remove all previous DTBs. This resolves the situation where the DTS
    # is removed in the new kernel. Without removing all artifacts, the FIT
    # image crated later would contain the old DTB taken from previous
    # build.

    # Staryu boards use dts files with corsola prefix.
    if [ $BUILD_FAMILY_NAME = "staryu" ] ; then
        DEVICETREES=$(find ./arch/arm64/boot/dts -name \*corsola\*.dtb)
        DEVICETREE_ARTIFACTS=$(find ./arch/arm64/boot/dts -name \*corsola\*.dtb.\*)
    else
        DEVICETREES=$(find ./arch/arm64/boot/dts -name \*$BUILD_FAMILY_NAME\*.dtb)
        DEVICETREE_ARTIFACTS=$(find ./arch/arm64/boot/dts -name \*$BUILD_FAMILY_NAME\*.dtb.\*)
    fi
    rm -rf $DEVICETREES $DEVICETREE_ARTIFACTS
    # XXX: Under arm64 arch, in a later step we replace kernel image with a FIT
    # image, which extends the kernel with devices trees. Running the script
    # twice results in nested FIT FIT image, which does not boot. Hence, we
    # remove it before building.
    rm -rf arch/$ARCH/boot/$ZIMAGE
fi
# Do the build
#
rm -f include/asm &&
make -j $BUILD_NUM_CPUS $MAKE_DEFCONFIG &&                              # configure the kernel
make -j $BUILD_NUM_CPUS &&                                              # build the kernel
make -j $BUILD_NUM_CPUS modules_install INSTALL_MOD_PATH=$TMP_MODULES   # install kernel modules

if [ $? != 0 ] ; then
    echo "Could not build the kernel. Aborting !"
    exit 1
fi

if [ "$ARCH" == "arm64" ] ; then
    # replace kernel image with a FIT image which contains the kernel image, plus relevant
    # devicetrees for this build family.
    # TODO(svenva@) generate-its-script.sh is really old and generates a legacy FIT.
    #               upgrade to modern FIT format which is much more compact.

    # Staryu boards use dts files with corsola prefix.
    if [ $BUILD_FAMILY_NAME = "staryu" ] ; then
        DEVICETREES=$(find ./arch/arm64/boot/dts -name \*corsola\*.dtb)
    else
        DEVICETREES=$(find ./arch/arm64/boot/dts -name \*$BUILD_FAMILY_NAME\*.dtb)
    fi
    ./chromeos/scripts/generate-its-script.sh -a arm64 -c lz4 -d $(pwd) arch/$ARCH/boot/$ZIMAGE \
        $DEVICETREES | dtc -I dts -O dtb -p 1024 > arch/$ARCH/boot/$ZIMAGE.fit
    mv arch/$ARCH/boot/$ZIMAGE.fit arch/$ARCH/boot/$ZIMAGE
fi

# Copy over the built artifacts
# Clean module destination dir before copying, to prevent stale
# modules from getting left behind.
#
cp -f arch/$ARCH/boot/$ZIMAGE $OUTPUT/ &&
rm -f "${OUTPUT}/vendor_dlkm/"* &&
find $TMP_MODULES/lib/modules/$(cat include/config/kernel.release)/kernel/ \
    -type f -print0 | xargs -0 -I {} cp {} "$OUTPUT/vendor_dlkm/"

if [ $? != 0 ] ; then
    echo "Could not copy the kernel artifacts. Aborting !"
    exit 1
fi

echo "Kernel $CONFIG prebuilt images ($ZIMAGE and modules) copied to $OUTPUT successfully !"


exit 0
