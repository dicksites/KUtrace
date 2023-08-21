# KUtrace on Android
Android is the most used mobile operating system in the world (around 70%+ of mobile devices run Android as of the end of 2022). Android is a multi-layered architecture, with each layer providing a key service to users and developers. See the [Android Architecture Overview](https://source.android.com/docs/core/architecture) for more details.

Performance analysis studies and tools for server environments are generally well known and low-overhead. However, a mobile environment is very different from a server environment. Mobile devices have fewer cores, less memory, stricter battery/power constraints, and are highly multi-tenanted in comparison to servers. This causes problems in trying to understand the cause of performance regressions or bugs or even understanding the impact of an optimization. Existing tooling on Android for understanding performance includes using the Linux kernel `perf` and `ftrace` tools. However, these tools have a non-trivial impact to the system dynamics as they induce a noticeable overhead (~10%) while tracing. As such, while these tools are useful for understanding the interactions in a system, the performance overhead of the tools make them unviable to use in instances where minor slowdowns can completely change the system interactions (such as tracing an application processing video frames).

## Patching and Building KUtrace
### Setting Up
We use a Pixel 6 Pro (raven) phone running Android 13 release QPR2 for the rest of this document. You should use whatever phone and Android version you are interested in. *Note that if you are patching a different version of the Linux kernel, some of the locations of the trace events may have moved around. Please make sure that the correct locations have been patched.*

Before we start patching the kernel, we create a directory in which we will clone the Android kernel for the device we are working on. Our instructions are based on [Building the Android Kernel](https://source.android.com/docs/setup/build/building-kernels). Please refer to the page to check what branch to use below. Since we are using the Pixel 6 Pro here, we use the `android-gs-raviole-5.10-android13-qpr2` branch:

```console
$ export SETUP_DIR=~/android-kernel
$ mkdir -p $SETUP_DIR && cd $SETUP_DIR
$ repo init -u https://android.googlesource.com/kernel/manifest -b android-gs-raviole-5.10-android13-qpr2
$ repo sync
```

You will now have a directory which contains two kernels: the AOSP kernel under `aosp/` and the device-specific kernel under `private/gs-google/`. If you are not using a Pixel device, the directory name for the device-specific kernel would be different. We use the `private/gs-google/` directory to refer to the device-specific kernel for the rest of this document. Most of our changes will be done to the AOSP kernel, but we will have to make minor changes to the device-specific kernel.

It is important to make sure that you are using the latest version of both `adb` and `fastboot`. You should download the latest version of the [Android SDK platform tools](https://developer.android.com/tools/releases/platform-tools).

#### Android GKI Project
The Android kernel ecosystem was quite fragmented previously with each vendor potentially having their own kernel changes and patches that made interoperability and updates much harder. The [Generic Kernel Image (GKI) project](https://source.android.com/docs/core/architecture/kernel/generic-kernel-image) addresses this issue by essentially splitting the kernel up into two parts: (i) the core or GKI kernel and (ii) the device-specific kernel. The GKI kernel provides a stable interface (kernel ABI) that the device-specific kernel is based on. This ensures that a device can use any kernel with the same stable kernel ABI, allowing for transparent updates to the core/GKI kernel image without requiring massive coordination across vendors.

For most modern phones, when we build a kernel, we are actually building what is termed a “mixed” build wherein we have a GKI kernel (either from a pre-built or from our own AOSP kernel build) that is used as an input into the device kernel and built together.

The KUtrace patches export some kernel symbols so that the loadable kernel module can hook into them and provide tracing facilities. However, with the GKI project, the Android kernel only exports certain symbols in order to allow for transparent updates to the core kernel. Hence, we need to update the kernel ABI to add the kernel symbols exported by KUtrace.

### Patching KUtrace
Obtain the set of KUtrace patches from this repository. There should be two patches for the two kernels. Ignore the patches to the kernel ABI files (i.e. all files under `aosp/android/` and `private/gs-google/android/`) as those changes are almost certainly going to be different for the kernel version you are using. In order to be hygienic, we make all our changes on a separate branch. For the rest of this document we assume that the branch name is `kutrace-experimental`.

Apply the patches to the two kernel directories like so:

```console
$ cd $SETUP_DIR/aosp
$ git apply /path/to/aosp.patch
$ cd $SETUP_DIR/private/gs-google
$ git apply /path/to/gs-google.patch
```

**Note:** You may have to make edits to some of the changes. We list some of the changes to keep an eye for:

  - The `KUTRACE_RESCHED_IPI` and `KUTRACE_BOTTOM_HALF` values defined in the `kutrace.h` file were picked from a range of interrupt values that had 0 interrupts in the `/proc/interrupts` file on the phone
  - The `KUTRACE_TIMER_IRQ{1,2}` and `KUTRACE_TIMER_COUNT{1,2`} pairs in the kutrace.h file specify two ranges of timer interrupts. The `IRQ1`, `COUNT1` pair is always defined to be the `arch_timer`. The `IRQ2`, `COUNT2` pair defines the second range of timer interrupts. For the Pixel 6 Pro, there are eight different timer interrupts starting from interrupt number 292 (all of them are named `exynos-mct`). You can consult `/proc/interrupts` on the phone to see if you actually have a second range of timer. In case your device doesn’t have a second range of timers, then you should set `IRQ2 = IRQ1` and `COUNT2 = COUNT1`. The compiler should be smart enough to optimize the actual bit manipulation in the code
  - The `get_granular()` function in the KUtrace module (`kutrace_mod.c`) assumes that the CPU base clock is ~2400 MHz for IPC calculation. This value was picked as the roughly middle clock speed for the big core on the Pixel 6 Pro (the Cortex X1). The list of available CPU frequencies can be found by checking the `/sys/devices/system/cpu/cpufreq/policy*/scaling_available_frequencies` files. Don’t pick the highest or lowest frequencies for your device. Picking a middle-ish value gives us a better insight into if code is fast or not
  - If you want to use some other performance counter instead of retired instructions, you will have to edit the KUtrace module (`kutrace_mod.c`) to use the correct performance event number from the CPU reference manual for the event you care about. Note that modern phones often use a big.LITTLE core configuration (in fact the Pixel 6 Pro has three different core types), the event number you are interested in may not exist on all of the different cores. Make sure that the event number you pick is either supported by all cores or if it is not, then it is suppressed for unsupported cores in post-processing (by writing a script that converts the values to 0 for the unsupported cores)


#### Updating the Kernel ABI
We now add the symbols exported by KUtrace to the list of exported kernel symbols in `aosp/android/abi_gki_aarch64_generic`. Note that the file name may be different for your device. Newer kernels and Pixel devices use the `abi_gki_aarch64_pixel` file. For example, for the Pixel 6 Pro, the changes may look something like:

```diff
diff --git a/android/abi_gki_aarch64_generic b/android/abi_gki_aarch64_generic
index 71807939e99e..79b1ff24a9c3 100644
--- a/android/abi_gki_aarch64_generic
+++ b/android/abi_gki_aarch64_generic
@@ -1261,6 +1261,11 @@
   ktime_get_snapshot
   ktime_get_ts64
   ktime_get_with_offset
+  kutrace_global_ops
+  kutrace_net_filter
+  kutrace_pid_filter
+  kutrace_traceblock_per_cpu
+  kutrace_tracing
   kvfree
   kvfree_call_rcu
   kvmalloc_node
```

Copy the edited file to the `private/gs-google/android/` directory as well, overwriting the existing file.

Now we have to update the actual ABI file. If you are using a phone that came with Android 14 and above pre-installed, then the file would be called `abi_gki_aarch64.stg`, otherwise the file will be called `abi_gki_aarch64.xml`. In our case (Pixel 6 Pro), the file is `abi_gki_aarch64.xml`. The file is located in `aosp/android/`.

Follow the steps as outlined in the [How to run ABI Monitoring](https://source.android.com/docs/core/architecture/kernel/howto-abi-monitor) page for the kernel/Android version you are using. For example, the steps to update the ABI for the Pixel 6 Pro are as follows:

```console
$ cd $SETUP_DIR
$ BUILD_CONFIG=aosp/build.config.gki.aarch64 build/build_abi.sh --update
```

You should find that the ABI file has been updated to include the KUtrace symbols.

We recommend committing all changes made to the AOSP and device-specific kernel. This would ensure that the commit date and hash are shown in the output of `uname -a` on the device which is useful for reproducibility.

#### Adding the Kernel Module
We now copy the KUtrace kernel module to the directory with the external device drivers and modules. For the Pixel 6 Pro, this is the `private/google-modules/` directory. It may be different for your phone. Note that as part of patching the device-specific kernel, we added the `kutrace/` directory to the list of kernel modules to build. Make sure that the location of this directory is correct in the `Makefile`.

```console
$ cp /path/to/kutrace/module $SETUP_DIR/private/google-modules/kutrace
```

### Building the Kernel Module and Patched Kernel
Now it’s time to build the actual kernel module and patched kernel. If everything is correctly patched, then you will be able to build both the kernel and the kernel modules using the following command:

```console
$ BUILD_AOSP_KERNEL=1 ./build_slider.sh
```

Note that the name of the build script is going to be different for your device. `build_slider.sh` is specific to the Pixel 6 Pro. For newer kernels and Android versions, Kleaf is used instead. Please see the [Building Android Kernels](https://source.android.com/docs/setup/build/building-kernels) page for up-to-date information on how to build the Android kernel.

The `BUILD_AOSP_KERNEL=1` flag is required as we have changed the AOSP kernel and the ABI. If the flag is not set, then the build system will use a pre-built version of the AOSP kernel which would lead to build errors.

If you want to do an incremental build, you can set the `SKIP_MRPROPER=1` flag while building the kernel, like so:

```console
$ BUILD_AOSP_KERNEL=1 SKIP_MRPROPER=1 ./build_slider.sh
```

After the build is done, confirm the existence of the `kutrace_mod.ko` file in `out/mixed/dist` and also `grep` for the exported KUtrace symbols in the `vmlinux.symvers` file. For example, the output may look like this:

```console
$ grep kutrace $SETUP_DIR/out/mixed/dist/vmlinux.symvers
0xb456a31f kutrace_tracing vmlinux EXPORT_SYMBOL_GPL
0xd8a17f84 kutrace_traceblock_per_cpu      vmlinux EXPORT_SYMBOL_GPL
0xb9876eeb kutrace_net_filter      vmlinux EXPORT_SYMBOL_GPL
0x18161b5e kutrace_global_ops      vmlinux EXPORT_SYMBOL_GPL
0x8344ac91 kutrace_pid_filter      vmlinux EXPORT_SYMBOL_GPL
```

### Building the Control Program
KUtrace tracing is usually controlled by a small control program. You would need to cross-compile the control program in order for it to run on Android. Download and unpack the [latest NDK version](https://developer.android.com/ndk/downloads) to a directory. Then you want to cd into the directory with the kutrace_control program and run the following commands:

```console
$ export NDK="/path/to/NDK/install"
$ $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ --target=aarch64-linux-android33 -static -ffunction-sections -fdata-sections -Wl,--gc-sections -O2 kutrace_control.cc kutrace_lib.cc -o kutrace_control
```

Note that you may have to change the target from `aarch64-linux-android33` to the appropriate [API level](https://apilevels.com/) for the version of Android you are targeting. Since we are targeting Android 13, we use the `android33` target (i.e API level 33). The extra flags are required to satisfy Android requirements for alignment. See [here](https://github.com/termux/termux-packages/issues/8273#issuecomment-1133861593) for more information.

## Flashing and Using KUtrace
### Setting Up
If the kernel and kernel module have been built without errors above, then we are ready to flash our device. But before that we must disable the verification that Android performs at each boot. Android verifies that the core system images and partitions have not changed between boots as a security measure to prevent tampering of core system components. Since we want to update the kernel image, we have to disable the verification using the following commands:

```console
$ adb root
$ adb disable-verity
$ adb reboot
<wait for phone to reboot>
```

### Flashing Kernel Modules and the Patched Kernel

We can now install the patched kernel and the kernel modules using the following commands:

```console
$ cd $SETUP_DIR/out/mixed/dist
$ adb root
$ adb remount
$ adb push *.ko vendor/lib/modules
$ adb reboot bootloader
<wait for phone to reboot into bootloader>
$ fastboot --cmdline androidboot.selinux=permissive flash:raw boot boot.img
$ fastboot flash dtbo dtbo.img
$ fastboot -w
$ fastboot reboot
```

Android mounts core system partitions as read-only. Hence, we first remount the system partitions, then we can push the kernel modules to the phone. Note that any time you build and flash the kernel, you should also build and push the associated kernel modules as device drivers on the phone may not work without the correct version of the kernel modules.

We then reboot the phone into the bootloader and then flash the kernel’s `boot.img` and `dtbo.img` using `fastboot`. Note that we change the kernel boot command line to set SELinux to permissive. This is required as KUtrace uses an unused syscall number to interact with the kernel module. If SELinux (and by extension SECCOMP) is enabled on the device, then the device will only allow calls to whitelisted syscalls. It is easier to turn SELinux off than to add the KUtrace syscall to the list of whitelisted syscalls. However, certain applications may no longer run on your device anymore (such as banking applications) which check the SELinux mode. Do not store any sensitive information on this device!

**Note:** If you are flashing the patched kernel for the first time, then you ***must*** wipe user data using `fastboot -w`. If you don’t wipe user data, you will get stuck in a boot loop since the core kernel ABI has changed. You must do a `fastboot -w` if the kernel ABI is ever changed in the future as well. If you are changing the kernel in the future but haven’t changed the kernel ABI, you can skip the wipe user data step.

After the reboot you should check that the kernel you have built is the one that is flashed using `uname -a`. For example, the output may look something like this:

```console
$ adb shell
$ uname -a
Linux localhost 5.10.149-android13-4-00024-g827ef284462a #1 SMP PREEMPT Wed Jul 19 02:17:54 UTC 2023 aarch64 Toybox
```

The date will have changed if you committed the patches to both the AOSP and device-specific kernel. Otherwise, you may see something like `-dirty` instead of a git hash as a suffix.

### Using the Kernel Module

We can insert the kernel module into the kernel now. Since the KUtrace module is not in the list of standard kernel modules, it is deleted when you do a user data wipe. Re-push the kernel module to the phone like so:

```console
$ cd $SETUP_DIR/out/mixed/dist
$ adb root
$ adb remount
$ adb push kutrace_mod.ko vendor/lib/modules
```

After pushing the kernel module again, we can insert the kernel module like so:

```console
$ adb root
$ adb shell
# insmod vendor/lib/modules/kutrace_mod.ko tracemb=10
```

In the above command, we set the trace buffer size to 10 MB while inserting the module. Check that the kernel module has been installed using `lsmod`. The output may look something like this:

```console
# lsmod | grep kutrace
kutrace_mod            32768  0
```

Note that since the KUtrace module is not in the list of modules the kernel loads at boot time (by design), you will have to insert the module after every device reboot.

Check if you can remove the module using `rmmod` like so:

```console
$ adb root
$ adb shell
# rmmod kutrace_mod
# lsmod | grep kutrace
```

### Installing the Control Program

Navigate to the directory where you compiled the `kutrace_control` program. Install the control program using the following steps:

```console
$ adb root
$ adb remount
$ adb push kutrace_control /bin
$ adb shell
# cd bin
# chown root:shell kutrace_control
# chmod 755 kutrace_control
```

Note that the `chown` and `chmod` steps are important as the control program will not be runnable otherwise.

### Capturing a Trace

Before we capture a trace, we expose the addresses of kernel symbols by doing the following:

```console
$ adb root
$ adb shell
# echo 1 > /proc/sys/kernel/kptr_restrict
```

Android does not expose the addresses of kernel symbols by default. Check that you can view actual addresses for kernel symbols in the `/proc/kallsyms` file. We expose the addresses of the kernel symbols as KUtrace samples the program counter at every timer interrupt. Exposing the kernel symbols allows us to get kernel routine names for samples in kernel code during post-processing.
Note that the base location of the kernel symbols is different for every reboot. You should create a copy of the `/proc/kallsyms` file for every reboot like so:

```console
$ adb shell cat /proc/kallsyms > kallsyms.txt
```

Be careful not to overwrite existing files!

We then use the following commands to capture a trace on the phone:

```console
$ adb root
$ adb shell
# cd /storage/emulated/0/Documents
# kutrace_control
control> goipc
control> stop
```

Here, we’ve navigated to a known location where we then save the trace files. The `kutrace_control` program will save trace files to the current directory. The `goipc` command starts the trace and sets a bit such that you get IPC (instructions per cycle) data for every event. The `stop` command stops the trace and saves the trace file to the current directory.

We then use the following steps to postprocess the trace file:

```console
$ adb pull </path/to/kutrace/trace> .
$ postproc3.sh </path/to/kutrace/trace>
```

The `postproc3.sh` script will generate a JSON file and an HTML file. The JSON file is the decoded set of events that occurred in the trace and the HTML file is the interactable execution timeline. If there are no errors in the trace file, a browser window will open with the execution timeline. We recommend reading the Understanding Software Dynamics book to understand how to interact with the timeline.
