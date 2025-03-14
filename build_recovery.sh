#!/bin/bash
export KERNELDIR=`readlink -f .`
export RAMFS_SOURCE=`readlink -f $KERNELDIR/recovery`
export USE_SEC_FIPS_MODE=true

echo "kerneldir = $KERNELDIR"
echo "ramfs_source = $RAMFS_SOURCE"

RAMFS_TMP="/tmp/arter97-jalte-recovery"

echo "ramfs_tmp = $RAMFS_TMP"
cd $KERNELDIR

if [ "${1}" = "skip" ] ; then
	echo "Skipping Compilation"
else
	echo "Compiling kernel"
	cp defconfig .config
	scripts/configcleaner "
CONFIG_RD_GZIP
CONFIG_RD_LZO
CONFIG_CC_OPTIMIZE_FOR_SIZE
"
	echo "
CONFIG_RD_GZIP=y
# CONFIG_RD_LZO is not set
CONFIG_CC_OPTIMIZE_FOR_SIZE=y
" >> .config
	sed -i -e 's/-Ofast/-Os/g' Makefile
	make oldconfig
	make "$@" || exit 1
	git checkout Makefile
fi

echo "Building new ramdisk"
#remove previous ramfs files
rm -rf '$RAMFS_TMP'*
rm -rf $RAMFS_TMP
rm -rf $RAMFS_TMP.cpio
#copy ramfs files to tmp directory
cp -ax $RAMFS_SOURCE $RAMFS_TMP
cd $RAMFS_TMP

find . -name '*.sh' -exec chmod 777 {} \;

$KERNELDIR/ramdisk_fix_permissions.sh 2>/dev/null

#clear git repositories in ramfs
find . -name .git -exec rm -rf {} \;
find . -name EMPTY_DIRECTORY -exec rm -rf {} \;
cd $KERNELDIR
rm -rf $RAMFS_TMP/tmp/*

cd $RAMFS_TMP
find . | fakeroot cpio -H newc -o | gzip > $RAMFS_TMP.cpio.gz
ls -lh $RAMFS_TMP.cpio.gz
cd $KERNELDIR

echo "Making new boot image"
./mkbootimg --kernel $KERNELDIR/arch/arm/boot/zImage --ramdisk $RAMFS_TMP.cpio.gz --base 0x10000000 --pagesize 2048 --ramdisk_offset 0x01000000 --tags_offset 0x00000100 --second_offset 00f00000 -o $KERNELDIR/recovery.img
if [ "${1}" = "CC=\$(CROSS_COMPILE)gcc" ] ; then
	dd if=/dev/zero bs=$((8388608-$(stat -c %s recovery.img))) count=1 >> recovery.img
fi

echo "done"
ls -al recovery.img
echo ""
