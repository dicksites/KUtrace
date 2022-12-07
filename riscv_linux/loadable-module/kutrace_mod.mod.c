#include <linux/module.h>
#define INCLUDE_VERMAGIC
#include <linux/build-salt.h>
#include <linux/elfnote-lto.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

BUILD_SALT;
BUILD_LTO_INFO;

MODULE_INFO(vermagic, VERMAGIC_STRING);
MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};

#ifdef CONFIG_RETPOLINE
MODULE_INFO(retpoline, "Y");
#endif

static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0xddda1c20, "module_layout" },
	{ 0x9001abed, "param_ops_long" },
	{ 0xf9a482f9, "msleep" },
	{ 0x18161b5e, "kutrace_global_ops" },
	{ 0xb9876eeb, "kutrace_net_filter" },
	{ 0x999e8297, "vfree" },
	{ 0xd6ee688f, "vmalloc" },
	{ 0x8da6585d, "__stack_chk_fail" },
	{ 0x1f8dba4f, "__asm_copy_from_user" },
	{ 0x92997ed8, "_printk" },
	{ 0x69acdf38, "memcpy" },
	{ 0xe85a8071, "_raw_spin_unlock_irqrestore" },
	{ 0xfae15d50, "_raw_spin_lock_irqsave" },
	{ 0xe677e6d6, "cpumask_next" },
	{ 0x7ecb001b, "__per_cpu_offset" },
	{ 0x16624d6e, "__cpu_online_mask" },
	{ 0x17de3d5, "nr_cpu_ids" },
	{ 0xd8a17f84, "kutrace_traceblock_per_cpu" },
	{ 0xfb578fc5, "memset" },
	{ 0xb456a31f, "kutrace_tracing" },
	{ 0x8344ac91, "kutrace_pid_filter" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "4C1F73C8C54096163A53386");
