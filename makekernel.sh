git clone --depth=1 --single-branch https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9 -b pie-release
git clone --depth=1 --single-branch https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9 -b pie-release
git clone https://github.com/kdrag0n/proton-clang.git proton-clang/
export ARCH=arm64 && export SUBARCH=arm64 && export DTC_EXT=dtc
make O=out ARCH=arm64 cherry-pine_defconfig
export PATH="$(pwd)/pronto-clang/bin:$(pwd)/aarch64-linux-android-4.9/bin:$(pwd)/arm-linux-androideabi-4.9/bin:${PATH}"
make -j$(nproc --all) O=out \
                      ARCH=arm64 \
                      CC=clang \
                      CLANG_TRIPLE=aarch64-linux-gnu- \
                      CROSS_COMPILE=aarch64-linux-android- \
                      CROSS_COMPILE_ARM32=arm-linux-androideabi-