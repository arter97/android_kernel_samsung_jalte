#!/bin/bash
if [ ! "${1}" = "skip" ] ; then
	./build_clean.sh
	./build_kernel_e300s.sh CC='$(CROSS_COMPILE)gcc'
	./build_clean.sh noimg
	./build_recovery.sh CC='$(CROSS_COMPILE)gcc'
fi

cp recovery.img recoveryzip/

rm arter97-kernel-"$(cat version)".zip 2>/dev/null
rm *.ko 2>/dev/null
cp boot.img kernelzip/
cd kernelzip/
7z a -mx9 arter97-kernel-"$(cat ../version)"-tmp.zip *
zipalign -v 4 arter97-kernel-"$(cat ../version)"-tmp.zip ../arter97-kernel-"$(cat ../version)".zip
rm arter97-kernel-"$(cat ../version)"-tmp.zip
cd ..
ls -al arter97-kernel-"$(cat version)".zip

rm arter97-recovery-"$(cat version)"-philz_touch_"$(cat version_recovery | awk '{print $1}')".zip
cd recoveryzip/
sed -i -e s/PHILZ_VERSION/$(cat ../version_recovery | awk '{print $1}')/g -e s/CWM_VERSION/$(cat ../version_recovery | awk '{print $2 }')/g META-INF/com/google/android/updater-script
7z a -mx9 arter97-recovery-"$(cat ../version)"-philz_touch_"$(cat ../version_recovery | awk '{print $1}')"-tmp.zip *
zipalign -v 4 arter97-recovery-"$(cat ../version)"-philz_touch_"$(cat ../version_recovery | awk '{print $1}')"-tmp.zip ../arter97-recovery-"$(cat ../version)"-philz_touch_"$(cat ../version_recovery | awk '{print $1}')".zip
rm arter97-recovery-"$(cat ../version)"-philz_touch_"$(cat ../version_recovery | awk '{print $1}')"-tmp.zip
sed -i -e s/$(cat ../version_recovery | awk '{print $1}')/PHILZ_VERSION/g -e s/$(cat ../version_recovery | awk '{print $2 }')/CWM_VERSION/g META-INF/com/google/android/updater-script
mv recovery.img ../
cd ..
ls -al arter97-recovery-"$(cat version)"-philz_touch_"$(cat version_recovery | awk '{print $1}')".zip
