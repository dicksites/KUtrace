#
# Build the KUtrace control library and program
#
NDK="/path/to/ndk"

if [ $NDK == "/path/to/ndk" ]; then
  echo "Please set the NDK path in the script first!"
  exit 1
fi

${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ --target=aarch64-linux-android33 -static -ffunction-sections -fdata-sections -Wl,--gc-sections -O2 kutrace_control.cc kutrace_lib.cc -o kutrace_control
