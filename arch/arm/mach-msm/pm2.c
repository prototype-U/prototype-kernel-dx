/* arch/arm/mach-msm/pm2.c
 *
 * MSM Power Management Routines
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2008-2012 The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/pm_qos.h>
#include <linux/suspend.h>
#include <linux/reboot.h>
#include <linux/io.h>
#include <linux/tick.h>
#include <linux/memory.h>
#include <mach/msm_iomap.h>
#include <mach/system.h>
#include <mach/board.h>
#include <asm/system.h>
#ifdef CONFIG_CPU_V7
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#endif
#ifdef CONFIG_CACHE_L2X0
#include <asm/hardware/cache-l2x0.h>
#endif
#ifdef CONFIG_VFP
#include <asm/vfp.h>
#endif
#include <mach/htc_util.h>

#ifdef CONFIG_MSM_MEMORY_LOW_POWER_MODE_SUSPEND_DEEP_POWER_DOWN
#include <mach/msm_migrate_pages.h>
#endif
#include <mach/socinfo.h>
#include <mach/proc_comm.h>
#include <asm/smp_scu.h>
#include <linux/console.h>
#include "smd_private.h"
#include "smd_rpcrouter.h"
#include "acpuclock.h"
#include "clock.h"
#include "idle.h"
#include "irq.h"
#include "gpio.h"
#include "timer.h"
#include "pm.h"
#include "spm.h"
#include "sirc.h"
#include "pm-boot.h"
#include "devices-msm7x2xa.h"


enum {
	MSM_PM_DEBUG_SUSPEND = BIT(0),
	MSM_PM_DEBUG_POWER_COLLAPSE = BIT(1),
	MSM_PM_DEBUG_STATE = BIT(2),
	MSM_PM_DEBUG_CLOCK = BIT(3),
	MSM_PM_DEBUG_RESET_VECTOR = BIT(4),
	MSM_PM_DEBUG_SMSM_STATE = BIT(5),
	MSM_PM_DEBUG_IDLE = BIT(6),
	MSM_PM_DEBUG_WAKEUP_REASON = BIT(8),
	MSM_PM_DEBUG_HOTPLUG = BIT(7),
	MSM_PM_DEBUG_IDLE_CLOCK = BIT(11),
};

#define RESTART_REASON_OEM_BASE		0x6f656d00
#define RESTART_REASON_RIL_FATAL	(RESTART_REASON_OEM_BASE | 0x99)
extern void wait_rmt_final_call_back(int timeout);

DEFINE_PER_CPU(int, power_collapsed);

static int msm_pm_debug_mask;
static int msm_pm_debug_mask = MSM_PM_DEBUG_CLOCK | MSM_PM_DEBUG_WAKEUP_REASON | MSM_PM_DEBUG_HOTPLUG;
module_param_named(
	debug_mask, msm_pm_debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP
);

#define MSM_PM_DPRINTK(mask, level, message, ...) \
	do { \
		if ((mask) & msm_pm_debug_mask) \
			printk(level message, ## __VA_ARGS__); \
	} while (0)

#define MSM_PM_DEBUG_PRINT_STATE(tag) \
	do { \
		MSM_PM_DPRINTK(MSM_PM_DEBUG_STATE, \
			KERN_INFO, "%s: " \
			"APPS_CLK_SLEEP_EN %x, APPS_PWRDOWN %x, " \
			"SMSM_POWER_MASTER_DEM %x, SMSM_MODEM_STATE %x, " \
			"SMSM_APPS_DEM %x\n", \
			tag, \
			__raw_readl(APPS_CLK_SLEEP_EN), \
			__raw_readl(APPS_PWRDOWN), \
			smsm_get_state(SMSM_POWER_MASTER_DEM), \
			smsm_get_state(SMSM_MODEM_STATE), \
			smsm_get_state(SMSM_APPS_DEM)); \
	} while (0)

#define MSM_PM_DEBUG_PRINT_SLEEP_INFO(from_idle) \
	do { \
		if (msm_pm_debug_mask & MSM_PM_DEBUG_SMSM_STATE) \
			smsm_print_sleep_info(msm_pm_smem_data->sleep_time, \
				msm_pm_smem_data->resources_used, \
				msm_pm_smem_data->irq_mask, \
				msm_pm_smem_data->wakeup_reason, \
				msm_pm_smem_data->pending_irqs); \
		if (msm_pm_debug_mask & MSM_PM_DEBUG_WAKEUP_REASON && !from_idle) \
			printk(KERN_INFO "SMEM_APPS_DEM_SLAVE_DATA: " \
				"%ds %x %x %x %x %x %x %x %s %x\n", \
				msm_pm_smem_data->sleep_time, \
				msm_pm_smem_data->irq_mask, \
				msm_pm_smem_data->resources_used, \
				msm_pm_smem_data->reserved1, \
				msm_pm_smem_data->wakeup_reason, \
				msm_pm_smem_data->pending_irqs, \
				msm_pm_smem_data->rpc_prog, \
				msm_pm_smem_data->rpc_proc, \
				msm_pm_smem_data->smd_port_name, \
				msm_pm_smem_data->reserved2); \
	} while (0)



static int msm_pm_idle_sleep_min_time = CONFIG_MSM7X00A_IDLE_SLEEP_MIN_TIME;
module_param_named(
	idle_sleep_min_time, msm_pm_idle_sleep_min_time,
	int, S_IRUGO | S_IWUSR | S_IWGRP
);

enum {
	MSM_PM_MODE_ATTR_SUSPEND,
	MSM_PM_MODE_ATTR_IDLE,
	MSM_PM_MODE_ATTR_LATENCY,
	MSM_PM_MODE_ATTR_RESIDENCY,
	MSM_PM_MODE_ATTR_NR,
};

static char *msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_NR] = {
	[MSM_PM_MODE_ATTR_SUSPEND] = "suspend_enabled",
	[MSM_PM_MODE_ATTR_IDLE] = "idle_enabled",
	[MSM_PM_MODE_ATTR_LATENCY] = "latency",
	[MSM_PM_MODE_ATTR_RESIDENCY] = "residency",
};

static char *msm_pm_sleep_mode_labels[MSM_PM_SLEEP_MODE_NR] = {
	[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_SUSPEND] = " ",
	[MSM_PM_SLEEP_MODE_POWER_COLLAPSE] = "power_collapse",
	[MSM_PM_SLEEP_MODE_RAMP_DOWN_AND_WAIT_FOR_INTERRUPT] =
		"ramp_down_and_wfi",
	[MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT] = "wfi",
	[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_NO_XO_SHUTDOWN] =
		"power_collapse_no_xo_shutdown",
	[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_STANDALONE] =
		"standalone_power_collapse",
};

static struct msm_pm_platform_data *msm_pm_modes;
static struct msm_pm_irq_calls *msm_pm_irq_extns;
static struct msm_pm_cpr_ops *msm_cpr_ops;

struct msm_pm_kobj_attribute {
	unsigned int cpu;
	struct kobj_attribute ka;
};

#define GET_CPU_OF_ATTR(attr) \
	(container_of(attr, struct msm_pm_kobj_attribute, ka)->cpu)

struct msm_pm_sysfs_sleep_mode {
	struct kobject *kobj;
	struct attribute_group attr_group;
	struct attribute *attrs[MSM_PM_MODE_ATTR_NR + 1];
	struct msm_pm_kobj_attribute kas[MSM_PM_MODE_ATTR_NR];
};

#define BOOT_LOCK_TIMEOUT_NORMAL	(60 * HZ)
#define BOOT_LOCK_TIMEOUT_SHORT 	(10 * HZ)
static void do_expire_boot_lock(struct work_struct *work)
{
	enable_hlt();
	pr_info("Release 'boot-time' no_halt_lock\n");
}
static DECLARE_DELAYED_WORK(work_expire_boot_lock, do_expire_boot_lock);

static ssize_t msm_pm_mode_attr_show(
	struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int ret = -EINVAL;
	int i;

	for (i = 0; i < MSM_PM_SLEEP_MODE_NR; i++) {
		struct kernel_param kp;
		unsigned int cpu;
		struct msm_pm_platform_data *mode;

		if (msm_pm_sleep_mode_labels[i] == NULL)
			continue;

		if (strcmp(kobj->name, msm_pm_sleep_mode_labels[i]))
			continue;

		cpu = GET_CPU_OF_ATTR(attr);
		mode = &msm_pm_modes[MSM_PM_MODE(cpu, i)];

		if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_SUSPEND])) {
			u32 arg = mode->suspend_enabled;
			kp.arg = &arg;
			ret = param_get_ulong(buf, &kp);
		} else if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_IDLE])) {
			u32 arg = mode->idle_enabled;
			kp.arg = &arg;
			ret = param_get_ulong(buf, &kp);
		} else if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_LATENCY])) {
			u32 arg = mode->latency;
			kp.arg = &arg;
			ret = param_get_ulong(buf, &kp);
		} else if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_RESIDENCY])) {
			u32 arg = mode->residency;
			kp.arg = &arg;
			ret = param_get_ulong(buf, &kp);
		}

		break;
	}

	if (ret > 0) {
		strlcat(buf, "\n", PAGE_SIZE);
		ret++;
	}

	return ret;
}

static ssize_t msm_pm_mode_attr_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret = -EINVAL;
	int i;

	for (i = 0; i < MSM_PM_SLEEP_MODE_NR; i++) {
		struct kernel_param kp;
		unsigned int cpu;
		struct msm_pm_platform_data *mode;

		if (msm_pm_sleep_mode_labels[i] == NULL)
			continue;

		if (strcmp(kobj->name, msm_pm_sleep_mode_labels[i]))
			continue;

		cpu = GET_CPU_OF_ATTR(attr);
		mode = &msm_pm_modes[MSM_PM_MODE(cpu, i)];

		if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_SUSPEND])) {
			kp.arg = &mode->suspend_enabled;
			ret = param_set_byte(buf, &kp);
		} else if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_IDLE])) {
			kp.arg = &mode->idle_enabled;
			ret = param_set_byte(buf, &kp);
		} else if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_LATENCY])) {
			kp.arg = &mode->latency;
			ret = param_set_ulong(buf, &kp);
		} else if (!strcmp(attr->attr.name,
			msm_pm_mode_attr_labels[MSM_PM_MODE_ATTR_RESIDENCY])) {
			kp.arg = &mode->residency;
			ret = param_set_ulong(buf, &kp);
		}

		break;
	}

	return ret ? ret : count;
}

 
static int __init msm_pm_mode_sysfs_add_cpu(
	unsigned int cpu, struct kobject *modes_kobj)
{
	char cpu_name[8];
	struct kobject *cpu_kobj;
	struct msm_pm_sysfs_sleep_mode *mode = NULL;
	int i, j, k;
	int ret;

	snprintf(cpu_name, sizeof(cpu_name), "cpu%u", cpu);
	cpu_kobj = kobject_create_and_add(cpu_name, modes_kobj);
	if (!cpu_kobj) {
		pr_err("%s: cannot create %s kobject\n", __func__, cpu_name);
		ret = -ENOMEM;
		goto mode_sysfs_add_cpu_exit;
	}

	for (i = 0; i < MSM_PM_SLEEP_MODE_NR; i++) {
		int idx = MSM_PM_MODE(cpu, i);

		if ((!msm_pm_modes[idx].suspend_supported) &&
				(!msm_pm_modes[idx].idle_supported))
			continue;

		mode = kzalloc(sizeof(*mode), GFP_KERNEL);
		if (!mode) {
			pr_err("%s: cannot allocate memory for attributes\n",
				__func__);
			ret = -ENOMEM;
			goto mode_sysfs_add_cpu_exit;
		}

		mode->kobj = kobject_create_and_add(
				msm_pm_sleep_mode_labels[i], cpu_kobj);
		if (!mode->kobj) {
			pr_err("%s: cannot create kobject\n", __func__);
			ret = -ENOMEM;
			goto mode_sysfs_add_cpu_exit;
		}

		for (k = 0, j = 0; k < MSM_PM_MODE_ATTR_NR; k++) {
			if ((k == MSM_PM_MODE_ATTR_IDLE) &&
				!msm_pm_modes[idx].idle_supported)
				continue;
			if ((k == MSM_PM_MODE_ATTR_SUSPEND) &&
			     !msm_pm_modes[idx].suspend_supported)
				continue;
			mode->kas[j].cpu = cpu;
			mode->kas[j].ka.attr.mode = 0644;
			mode->kas[j].ka.show = msm_pm_mode_attr_show;
			mode->kas[j].ka.store = msm_pm_mode_attr_store;
			mode->kas[j].ka.attr.name = msm_pm_mode_attr_labels[k];
			mode->attrs[j] = &mode->kas[j].ka.attr;
			j++;
		}
		mode->attrs[j] = NULL;

		mode->attr_group.attrs = mode->attrs;
		ret = sysfs_create_group(mode->kobj, &mode->attr_group);
		if (ret) {
			printk(KERN_ERR
				"%s: cannot create kobject attribute group\n",
				__func__);
			goto mode_sysfs_add_cpu_exit;
		}
	}

	ret = 0;

mode_sysfs_add_cpu_exit:
	if (ret) {
		if (mode && mode->kobj)
			kobject_del(mode->kobj);
		kfree(mode);
	}

	return ret;
}

static int __init msm_pm_mode_sysfs_add(void)
{
	struct kobject *module_kobj = NULL;
	struct kobject *modes_kobj = NULL;
	unsigned int cpu;
	int ret;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj) {
		printk(KERN_ERR "%s: cannot find kobject for module %s\n",
			__func__, KBUILD_MODNAME);
		ret = -ENOENT;
		goto mode_sysfs_add_exit;
	}

	modes_kobj = kobject_create_and_add("modes", module_kobj);
	if (!modes_kobj) {
		printk(KERN_ERR "%s: cannot create modes kobject\n", __func__);
		ret = -ENOMEM;
		goto mode_sysfs_add_exit;
	}

	for_each_possible_cpu(cpu) {
		ret = msm_pm_mode_sysfs_add_cpu(cpu, modes_kobj);
		if (ret)
			goto mode_sysfs_add_exit;
	}

	ret = 0;

mode_sysfs_add_exit:
	return ret;
}

s32 msm_cpuidle_get_deep_idle_latency(void)
{
	int i = MSM_PM_MODE(0, MSM_PM_SLEEP_MODE_POWER_COLLAPSE_NO_XO_SHUTDOWN);
	return msm_pm_modes[i].latency - 1;
}

void __init msm_pm_set_platform_data(
	struct msm_pm_platform_data *data, int count)
{
	BUG_ON(MSM_PM_SLEEP_MODE_NR * num_possible_cpus() > count);
	msm_pm_modes = data;
}

void __init msm_pm_set_irq_extns(struct msm_pm_irq_calls *irq_calls)
{
	
	BUG_ON(irq_calls == NULL || irq_calls->irq_pending == NULL ||
		irq_calls->idle_sleep_allowed == NULL ||
		irq_calls->enter_sleep1 == NULL ||
		irq_calls->enter_sleep2 == NULL ||
		irq_calls->exit_sleep1 == NULL ||
		irq_calls->exit_sleep2 == NULL ||
		irq_calls->exit_sleep3 == NULL);

	msm_pm_irq_extns = irq_calls;
}

void __init msm_pm_set_cpr_ops(struct msm_pm_cpr_ops *ops)
{
	msm_cpr_ops = ops;
}

enum {
	SLEEP_LIMIT_NONE = 0,
	SLEEP_LIMIT_NO_TCXO_SHUTDOWN = 2,
	SLEEP_LIMIT_MASK = 0x03,
};

static uint32_t msm_pm_sleep_limit = SLEEP_LIMIT_NONE;
#ifdef CONFIG_MSM_MEMORY_LOW_POWER_MODE
enum {
	SLEEP_RESOURCE_MEMORY_BIT0 = 0x0200,
	SLEEP_RESOURCE_MEMORY_BIT1 = 0x0010,
};
#endif



#if defined(CONFIG_ARCH_MSM7X30)
#define APPS_CLK_SLEEP_EN (MSM_APCS_GCC_BASE + 0x020)
#define APPS_PWRDOWN      (MSM_ACC0_BASE + 0x01c)
#define APPS_SECOP        (MSM_TCSR_BASE + 0x038)
#define APPS_STANDBY_CTL  NULL
#else
#define APPS_CLK_SLEEP_EN (MSM_CSR_BASE + 0x11c)
#define APPS_PWRDOWN      (MSM_CSR_BASE + 0x440)
#define APPS_STANDBY_CTL  (MSM_CSR_BASE + 0x108)
#define APPS_SECOP	  NULL
#endif

static void msm_pm_config_hw_before_power_down(void)
{
	if (cpu_is_msm7x30() || cpu_is_msm8x55()) {
		__raw_writel(4, APPS_SECOP);
	} else if (cpu_is_msm7x27()) {
		__raw_writel(0x1f, APPS_CLK_SLEEP_EN);
	} else if (cpu_is_msm7x27a() || cpu_is_msm7x27aa() ||
		   cpu_is_msm7x25a() || cpu_is_msm7x25aa() ||
		   cpu_is_msm7x25ab()) {
		__raw_writel(0x7, APPS_CLK_SLEEP_EN);
	} else if (cpu_is_qsd8x50()) {
		__raw_writel(0x1f, APPS_CLK_SLEEP_EN);
		mb();
		__raw_writel(0, APPS_STANDBY_CTL);
	}
	mb();
	__raw_writel(1, APPS_PWRDOWN);
	mb();
}

static void msm_pm_configure_top_csr(void)
{
	uint32_t bit_pos[][6] = {
		
		{17, 15, 13, 16, 14, 17},
		
		{22, 20, 18, 21, 19, 22},
	};
	uint32_t mpa5_cfg_ctl[2] = {0x30, 0x48};
	void __iomem *base_ptr;
	unsigned int value = 0;
	unsigned int cpu;
	int i;

	
	msm_spm_reinit();

	for_each_possible_cpu(cpu) {
		
		if (!cpu)
			continue;

		base_ptr = core_reset_base(cpu);
		if (!base_ptr)
			return;

		
		__raw_writel(0x3, base_ptr);
		mb();

		for (i = 0; i < 5; i++) {
			value = __raw_readl(MSM_CFG_CTL_BASE +
							mpa5_cfg_ctl[cpu/2]);
			value |= BIT(bit_pos[cpu%2][i]);
			__raw_writel(value,  MSM_CFG_CTL_BASE +
							mpa5_cfg_ctl[cpu/2]);
			mb();
		}

		
		value = __raw_readl(MSM_CFG_CTL_BASE +
						mpa5_cfg_ctl[cpu/2]);
		value &= ~BIT(bit_pos[cpu%2][i]);
		__raw_writel(value,  MSM_CFG_CTL_BASE +
						mpa5_cfg_ctl[cpu/2]);
		mb();

		__raw_writel(0x0, base_ptr);
		mb();
	}
}

static void msm_pm_config_hw_after_power_up(void)
{

	if (cpu_is_msm7x30() || cpu_is_msm8x55()) {
		__raw_writel(0, APPS_SECOP);
		mb();
		__raw_writel(0, APPS_PWRDOWN);
		mb();
		msm_spm_reinit();
	} else if (cpu_is_msm8625() || cpu_is_msm8625q()) {
		__raw_writel(0, APPS_PWRDOWN);
		mb();

		if (per_cpu(power_collapsed, 1)) {
			scu_enable(MSM_SCU_BASE);
			msm_pm_configure_top_csr();
		}
	} else {
		__raw_writel(0, APPS_PWRDOWN);
		mb();
		__raw_writel(0, APPS_CLK_SLEEP_EN);
		mb();
	}
}

static void msm_pm_config_hw_before_swfi(void)
{
	if (cpu_is_qsd8x50()) {
		__raw_writel(0x1f, APPS_CLK_SLEEP_EN);
		mb();
	} else if (cpu_is_msm7x27()) {
		__raw_writel(0x0f, APPS_CLK_SLEEP_EN);
		mb();
	} else if (cpu_is_msm7x27a() || cpu_is_msm7x27aa() ||
		   cpu_is_msm7x25a() || cpu_is_msm7x25aa() ||
		   cpu_is_msm7x25ab()) {
		__raw_writel(0x7, APPS_CLK_SLEEP_EN);
		mb();
	}
}

static void msm_pm_timeout(void)
{
#if defined(CONFIG_MSM_PM_TIMEOUT_RESET_CHIP)
	printk(KERN_EMERG "[K] %s(): resetting chip\n", __func__);
	msm_proc_comm(PCOM_RESET_CHIP_IMM, NULL, NULL);
#elif defined(CONFIG_MSM_PM_TIMEOUT_RESET_MODEM)
	printk(KERN_EMERG "[K] %s(): resetting modem\n", __func__);
	msm_proc_comm_reset_modem_now();
#elif defined(CONFIG_MSM_PM_TIMEOUT_HALT)
	printk(KERN_EMERG "[K] %s(): halting\n", __func__);
#endif
	for (;;)
		;
}



struct msm_pm_polled_group {
	uint32_t group_id;

	uint32_t bits_all_set;
	uint32_t bits_all_clear;
	uint32_t bits_any_set;
	uint32_t bits_any_clear;

	uint32_t value_read;
};

static inline bool msm_pm_all_set(uint32_t source, uint32_t flag)
{
	return (source & flag) == flag;
}

static inline bool msm_pm_any_set(uint32_t source, uint32_t flag)
{
	return !flag || (source & flag);
}

static inline bool msm_pm_all_clear(uint32_t source, uint32_t flag)
{
	return (~source & flag) == flag;
}

static inline bool msm_pm_any_clear(uint32_t source, uint32_t flag)
{
	return !flag || (~source & flag);
}

static int msm_pm_poll_state(int nr_grps, struct msm_pm_polled_group *grps)
{
	int i, k;

	for (i = 0; i < 2500000; i++) {
		for (k = 0; k < nr_grps; k++) {
			bool all_set, all_clear;
			bool any_set, any_clear;

			grps[k].value_read = smsm_get_state(grps[k].group_id);

			all_set = msm_pm_all_set(grps[k].value_read,
					grps[k].bits_all_set);
			all_clear = msm_pm_all_clear(grps[k].value_read,
					grps[k].bits_all_clear);
			any_set = msm_pm_any_set(grps[k].value_read,
					grps[k].bits_any_set);
			any_clear = msm_pm_any_clear(grps[k].value_read,
					grps[k].bits_any_clear);

			if (all_set && all_clear && (any_set || any_clear))
				return k;
		}
		udelay(1);
	}

	printk(KERN_ERR "[K] %s failed:\n", __func__);
	for (k = 0; k < nr_grps; k++)
		printk(KERN_ERR "(%x, %x, %x, %x) %x\n",
			grps[k].bits_all_set, grps[k].bits_all_clear,
			grps[k].bits_any_set, grps[k].bits_any_clear,
			grps[k].value_read);

	return -ETIMEDOUT;
}



#define SCLK_HZ (32768)
#define MSM_PM_SLEEP_TICK_LIMIT (0x6DDD000)

#ifdef CONFIG_MSM_SLEEP_TIME_OVERRIDE
static int msm_pm_sleep_time_override;
module_param_named(sleep_time_override,
	msm_pm_sleep_time_override, int, S_IRUGO | S_IWUSR | S_IWGRP);
#endif

static uint32_t msm_pm_max_sleep_time;

static int64_t msm_pm_convert_and_cap_time(int64_t time_ns, int64_t limit)
{
	do_div(time_ns, NSEC_PER_SEC / SCLK_HZ);
	return (time_ns > limit) ? limit : time_ns;
}

void msm_pm_set_max_sleep_time(int64_t max_sleep_time_ns)
{
	unsigned long flags;

	local_irq_save(flags);
	if (max_sleep_time_ns == 0) {
		msm_pm_max_sleep_time = 0;
	} else {
		msm_pm_max_sleep_time = (uint32_t)msm_pm_convert_and_cap_time(
			max_sleep_time_ns, MSM_PM_SLEEP_TICK_LIMIT);

		if (msm_pm_max_sleep_time == 0)
			msm_pm_max_sleep_time = 1;
	}

	MSM_PM_DPRINTK(MSM_PM_DEBUG_SUSPEND, KERN_INFO,
		"%s(): Requested %lld ns Giving %u sclk ticks\n", __func__,
		max_sleep_time_ns, msm_pm_max_sleep_time);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(msm_pm_set_max_sleep_time);



#define DEM_MASTER_BITS_PER_CPU             6

#define DEM_MASTER_SMSM_RUN \
	(0x01UL << (DEM_MASTER_BITS_PER_CPU * SMSM_APPS_STATE))
#define DEM_MASTER_SMSM_RSA \
	(0x02UL << (DEM_MASTER_BITS_PER_CPU * SMSM_APPS_STATE))
#define DEM_MASTER_SMSM_PWRC_EARLY_EXIT \
	(0x04UL << (DEM_MASTER_BITS_PER_CPU * SMSM_APPS_STATE))
#define DEM_MASTER_SMSM_SLEEP_EXIT \
	(0x08UL << (DEM_MASTER_BITS_PER_CPU * SMSM_APPS_STATE))
#define DEM_MASTER_SMSM_READY \
	(0x10UL << (DEM_MASTER_BITS_PER_CPU * SMSM_APPS_STATE))
#define DEM_MASTER_SMSM_SLEEP \
	(0x20UL << (DEM_MASTER_BITS_PER_CPU * SMSM_APPS_STATE))

#define DEM_SLAVE_SMSM_RUN                  (0x0001)
#define DEM_SLAVE_SMSM_PWRC                 (0x0002)
#define DEM_SLAVE_SMSM_PWRC_DELAY           (0x0004)
#define DEM_SLAVE_SMSM_PWRC_EARLY_EXIT      (0x0008)
#define DEM_SLAVE_SMSM_WFPI                 (0x0010)
#define DEM_SLAVE_SMSM_SLEEP                (0x0020)
#define DEM_SLAVE_SMSM_SLEEP_EXIT           (0x0040)
#define DEM_SLAVE_SMSM_MSGS_REDUCED         (0x0080)
#define DEM_SLAVE_SMSM_RESET                (0x0100)
#define DEM_SLAVE_SMSM_PWRC_SUSPEND         (0x0200)



#define DEM_MAX_PORT_NAME_LEN (20)

struct msm_pm_smem_t {
	uint32_t sleep_time;
	uint32_t irq_mask;
	uint32_t resources_used;
	uint32_t reserved1;

	uint32_t wakeup_reason;
	uint32_t pending_irqs;
	uint32_t rpc_prog;
	uint32_t rpc_proc;
	char     smd_port_name[DEM_MAX_PORT_NAME_LEN];
	uint32_t reserved2;
};


static struct msm_pm_smem_t *msm_pm_smem_data;
static atomic_t msm_pm_init_done = ATOMIC_INIT(0);

static int msm_pm_modem_busy(void)
{
	if (!(smsm_get_state(SMSM_POWER_MASTER_DEM) & DEM_MASTER_SMSM_READY)) {
		MSM_PM_DPRINTK(MSM_PM_DEBUG_POWER_COLLAPSE,
			KERN_INFO, "%s(): master not ready\n", __func__);
		return -EBUSY;
	}

	return 0;
}

static int msm_pm_power_collapse
	(bool from_idle, uint32_t sleep_delay, uint32_t sleep_limit)
{
	struct msm_pm_polled_group state_grps[2];
	unsigned long saved_acpuclk_rate;
	int collapsed = 0;
	int ret;
	int val;
	int modem_early_exit = 0;

	MSM_PM_DPRINTK(MSM_PM_DEBUG_SUSPEND|MSM_PM_DEBUG_POWER_COLLAPSE,
		KERN_INFO, "%s(): idle %d, delay %u, limit %u\n", __func__,
		(int)from_idle, sleep_delay, sleep_limit);

	if (!(smsm_get_state(SMSM_POWER_MASTER_DEM) & DEM_MASTER_SMSM_READY)) {
		MSM_PM_DPRINTK(
			MSM_PM_DEBUG_SUSPEND | MSM_PM_DEBUG_POWER_COLLAPSE,
			KERN_INFO, "%s(): master not ready\n", __func__);
		ret = -EBUSY;
		goto power_collapse_bail;
	}

	memset(msm_pm_smem_data, 0, sizeof(*msm_pm_smem_data));

	if (cpu_is_msm8625() || cpu_is_msm8625q()) {
		
		ret = msm_spm_set_low_power_mode(MSM_SPM_MODE_POWER_COLLAPSE,
									false);
		WARN_ON(ret);
	}

	if (msm_cpr_ops)
		msm_cpr_ops->cpr_suspend();

	msm_pm_irq_extns->enter_sleep1(true, from_idle,
						&msm_pm_smem_data->irq_mask);

	if (((!from_idle) && (MSM_PM_DEBUG_CLOCK & msm_pm_debug_mask)) ||
			((from_idle) && (MSM_PM_DEBUG_IDLE_CLOCK & msm_pm_debug_mask))) {
			clock_debug_print_enabled();
	}

	msm_pm_smem_data->sleep_time = sleep_delay;
	msm_pm_smem_data->resources_used = sleep_limit;

	saved_acpuclk_rate = acpuclk_power_collapse();

	if (!from_idle)
		MSM_PM_DPRINTK(MSM_PM_DEBUG_CLOCK, KERN_INFO,
			"%s(): change clock rate (old rate = %lu)\n", __func__,
			saved_acpuclk_rate);

	if (saved_acpuclk_rate == 0) {
		ret = -EAGAIN;
		goto acpu_set_clock_fail;
	}

	msm_sirc_enter_sleep();
	msm_gpio_enter_sleep(from_idle);

	

	if (from_idle)
		smsm_change_state(SMSM_APPS_DEM, DEM_SLAVE_SMSM_RUN,
			DEM_SLAVE_SMSM_PWRC);
	else
		smsm_change_state(SMSM_APPS_DEM, DEM_SLAVE_SMSM_RUN,
			DEM_SLAVE_SMSM_PWRC | DEM_SLAVE_SMSM_PWRC_SUSPEND);

	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): PWRC");
	MSM_PM_DEBUG_PRINT_SLEEP_INFO(from_idle);

	memset(state_grps, 0, sizeof(state_grps));
	state_grps[0].group_id = SMSM_POWER_MASTER_DEM;
	state_grps[0].bits_all_set = DEM_MASTER_SMSM_RSA;
	state_grps[1].group_id = SMSM_MODEM_STATE;
	state_grps[1].bits_all_set = SMSM_RESET;

	ret = msm_pm_poll_state(ARRAY_SIZE(state_grps), state_grps);

	if (ret < 0) {
		printk(KERN_EMERG "[K] %s(): power collapse entry "
			"timed out waiting for Modem's response\n", __func__);
		goto power_collapse_early_exit;
	}

	if (ret == 1) {
		MSM_PM_DPRINTK(
			MSM_PM_DEBUG_SUSPEND|MSM_PM_DEBUG_POWER_COLLAPSE,
			KERN_INFO,
			"%s(): msm_pm_poll_state detected Modem reset\n",
			__func__);
		goto power_collapse_early_exit;
	}

	

	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): PWRC RSA");

	ret = msm_pm_irq_extns->enter_sleep2(true, from_idle);
	if (ret < 0) {
		MSM_PM_DPRINTK(
			MSM_PM_DEBUG_SUSPEND|MSM_PM_DEBUG_POWER_COLLAPSE,
			KERN_INFO,
			"%s(): msm_irq_enter_sleep2 aborted, %d\n", __func__,
			ret);
		goto power_collapse_early_exit;
	}

	msm_pm_config_hw_before_power_down();
	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): pre power down");

	msm_pm_boot_config_before_pc(smp_processor_id(),
			virt_to_phys(msm_pm_collapse_exit));

#ifdef CONFIG_VFP
	if (from_idle)
		vfp_pm_suspend();
#endif

#ifdef CONFIG_CACHE_L2X0
	if (!cpu_is_msm8625() && !cpu_is_msm8625q())
		l2cc_suspend();
	else
		apps_power_collapse = 1;
#endif
	if (!from_idle)
		printk(KERN_INFO "[K][R] suspend end\n");
	msm_pm_irq_extns->read_active_irq();
	collapsed = msm_pm_collapse();

	if (cpu_is_msm8625() || cpu_is_msm8625q()) {
		int cpu;
		val = __raw_readl(MSM_CFG_CTL_BASE + 0x38);
		
		if (cpu_is_msm8625q()) {			
			if (val != 0x000F0002) {
				for_each_possible_cpu(cpu) {
					if (!cpu)
						continue;
					per_cpu(power_collapsed, cpu) = 1;
				}
				power_collapsed = 1;
				 __raw_writel(0x000F0002,
						 (MSM_CFG_CTL_BASE + 0x38));
			} else
				modem_early_exit = 1;
		} else {
			if (val != 0x00030002) {
				for_each_possible_cpu(cpu) {
					if (!cpu)
						continue;
					per_cpu(power_collapsed, cpu) = 1;
				}
				power_collapsed = 1;
				 __raw_writel(0x00030002,
						 (MSM_CFG_CTL_BASE + 0x38));
			} else
				modem_early_exit = 1;
		}
	}
	if (!from_idle)
		printk(KERN_INFO "[K][R] resume start\n");
#ifdef CONFIG_CACHE_L2X0
	if (!cpu_is_msm8625() && !cpu_is_msm8625q())		
		l2cc_resume();
	else
		apps_power_collapse = 0;
#endif

	msm_pm_boot_config_after_pc(smp_processor_id());

	if (collapsed) {
#ifdef CONFIG_VFP
		if (from_idle)
			vfp_pm_resume();
#endif
		cpu_init();
		local_fiq_enable();
	}

	MSM_PM_DPRINTK(MSM_PM_DEBUG_SUSPEND | MSM_PM_DEBUG_POWER_COLLAPSE,
		KERN_INFO,
		"%s(): msm_pm_collapse returned %d\n", __func__, collapsed);

	msm_pm_irq_extns->exit_sleep1(msm_pm_smem_data->irq_mask,
		msm_pm_smem_data->wakeup_reason,
		msm_pm_smem_data->pending_irqs);

	msm_pm_config_hw_after_power_up();
	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): post power up");

	memset(state_grps, 0, sizeof(state_grps));
	state_grps[0].group_id = SMSM_POWER_MASTER_DEM;
	state_grps[0].bits_any_set =
		DEM_MASTER_SMSM_RSA | DEM_MASTER_SMSM_PWRC_EARLY_EXIT;
	state_grps[1].group_id = SMSM_MODEM_STATE;
	state_grps[1].bits_all_set = SMSM_RESET;

	ret = msm_pm_poll_state(ARRAY_SIZE(state_grps), state_grps);

	if (ret < 0) {
		printk(KERN_EMERG "[K] %s(): power collapse exit "
			"timed out waiting for Modem's response\n", __func__);
		goto power_collapse_early_exit;
	}

	if (ret == 1) {
		MSM_PM_DPRINTK(
			MSM_PM_DEBUG_SUSPEND|MSM_PM_DEBUG_POWER_COLLAPSE,
			KERN_INFO,
			"%s(): msm_pm_poll_state detected Modem reset\n",
			__func__);
		goto power_collapse_early_exit;
	}

	
	if (collapsed && !modem_early_exit) {
		
	} else {
		goto power_collapse_early_exit;
	}

	

	smsm_change_state(SMSM_APPS_DEM,
		DEM_SLAVE_SMSM_PWRC | DEM_SLAVE_SMSM_PWRC_SUSPEND,
		DEM_SLAVE_SMSM_WFPI);

	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): WFPI");

	memset(state_grps, 0, sizeof(state_grps));
	state_grps[0].group_id = SMSM_POWER_MASTER_DEM;
	state_grps[0].bits_all_set = DEM_MASTER_SMSM_RUN;
	state_grps[1].group_id = SMSM_MODEM_STATE;
	state_grps[1].bits_all_set = SMSM_RESET;

	ret = msm_pm_poll_state(ARRAY_SIZE(state_grps), state_grps);

	if (ret < 0) {
		printk(KERN_EMERG "[K] %s(): power collapse WFPI "
			"timed out waiting for Modem's response\n", __func__);
		
		
	}

	if (ret == 1) {
		MSM_PM_DPRINTK(
			MSM_PM_DEBUG_SUSPEND|MSM_PM_DEBUG_POWER_COLLAPSE,
			KERN_INFO,
			"%s(): msm_pm_poll_state detected Modem reset\n",
			__func__);
		ret = -EAGAIN;
		goto power_collapse_restore_gpio_bail;
	}

	if (!from_idle)
		MSM_PM_DPRINTK(MSM_PM_DEBUG_CLOCK, KERN_INFO,
			"%s(): restore clock rate to %lu\n", __func__,
			saved_acpuclk_rate);

	if (acpuclk_set_rate(smp_processor_id(), saved_acpuclk_rate,
			SETRATE_PC) < 0)
		printk(KERN_ERR "%s(): failed to restore clock rate(%lu)\n",
			__func__, saved_acpuclk_rate);

	

	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): WFPI RUN");
	MSM_PM_DEBUG_PRINT_SLEEP_INFO(from_idle);

	msm_pm_irq_extns->exit_sleep2(msm_pm_smem_data->irq_mask,
		msm_pm_smem_data->wakeup_reason,
		msm_pm_smem_data->pending_irqs);
	msm_pm_irq_extns->exit_sleep3(msm_pm_smem_data->irq_mask,
		msm_pm_smem_data->wakeup_reason,
		msm_pm_smem_data->pending_irqs);
	msm_gpio_exit_sleep();
	msm_sirc_exit_sleep();

	smsm_change_state(SMSM_APPS_DEM,
		DEM_SLAVE_SMSM_WFPI, DEM_SLAVE_SMSM_RUN);

	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): RUN");
	MSM_PM_DEBUG_PRINT_SLEEP_INFO(from_idle);

	smd_sleep_exit();

	if (cpu_is_msm8625() || cpu_is_msm8625q()) {
		ret = msm_spm_set_low_power_mode(MSM_SPM_MODE_CLOCK_GATING,
									false);
		WARN_ON(ret);
	}

	if (msm_cpr_ops)
		msm_cpr_ops->cpr_resume();

	return 0;

power_collapse_early_exit:
	

	smsm_change_state(SMSM_APPS_DEM,
		DEM_SLAVE_SMSM_PWRC | DEM_SLAVE_SMSM_PWRC_SUSPEND,
		DEM_SLAVE_SMSM_PWRC_EARLY_EXIT);

	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): EARLY_EXIT");

	memset(state_grps, 0, sizeof(state_grps));
	state_grps[0].group_id = SMSM_POWER_MASTER_DEM;
	state_grps[0].bits_all_set = DEM_MASTER_SMSM_PWRC_EARLY_EXIT;
	state_grps[1].group_id = SMSM_MODEM_STATE;
	state_grps[1].bits_all_set = SMSM_RESET;

	ret = msm_pm_poll_state(ARRAY_SIZE(state_grps), state_grps);
	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): EARLY_EXIT EE");

	if (ret < 0)
		printk(KERN_EMERG "[K] %s(): power collapse EARLY_EXIT "
			"timed out waiting for Modem's response\n", __func__);

	if (ret == 1) {
		MSM_PM_DPRINTK(
			MSM_PM_DEBUG_SUSPEND|MSM_PM_DEBUG_POWER_COLLAPSE,
			KERN_INFO,
			"%s(): msm_pm_poll_state detected Modem reset\n",
			__func__);
	}

	

	ret = -EAGAIN;

power_collapse_restore_gpio_bail:
	msm_gpio_exit_sleep();
	msm_sirc_exit_sleep();

	
	smsm_change_state(SMSM_APPS_DEM,
		DEM_SLAVE_SMSM_PWRC | DEM_SLAVE_SMSM_PWRC_SUSPEND |
		DEM_SLAVE_SMSM_PWRC_EARLY_EXIT | DEM_SLAVE_SMSM_WFPI,
		DEM_SLAVE_SMSM_RUN);

	MSM_PM_DEBUG_PRINT_STATE("msm_pm_power_collapse(): RUN");

	MSM_PM_DPRINTK(MSM_PM_DEBUG_CLOCK, KERN_INFO,
		"%s(): restore clock rate to %lu\n", __func__,
		saved_acpuclk_rate);
	if (acpuclk_set_rate(smp_processor_id(), saved_acpuclk_rate,
			SETRATE_PC) < 0)
		printk(KERN_ERR "%s(): failed to restore clock rate(%lu)\n",
			__func__, saved_acpuclk_rate);

	if (collapsed)
		smd_sleep_exit();

acpu_set_clock_fail:
	if (msm_cpr_ops && from_idle)
		msm_cpr_ops->cpr_resume();

power_collapse_bail:
	if (cpu_is_msm8625() || cpu_is_msm8625q()) {
		ret = msm_spm_set_low_power_mode(MSM_SPM_MODE_CLOCK_GATING,
									false);
		WARN_ON(ret);
	}

	return ret;
}

static int __ref msm_pm_power_collapse_standalone(bool from_idle)
{
	int collapsed = 0;
	int ret;
	void *entry;

	MSM_PM_DPRINTK(MSM_PM_DEBUG_SUSPEND|MSM_PM_DEBUG_POWER_COLLAPSE,
		KERN_INFO, "%s()\n", __func__);

	ret = msm_spm_set_low_power_mode(MSM_SPM_MODE_POWER_COLLAPSE, false);
	WARN_ON(ret);

	entry = (!smp_processor_id() || from_idle) ?
			msm_pm_collapse_exit : msm_secondary_startup;

	msm_pm_boot_config_before_pc(smp_processor_id(),
						virt_to_phys(entry));

#ifdef CONFIG_VFP
	vfp_pm_suspend();
#endif

#ifdef CONFIG_CACHE_L2X0
	if (!cpu_is_msm8625() && !cpu_is_msm8625q())
		l2cc_suspend();
#endif

	msm_pm_irq_extns->read_active_irq();
	collapsed = msm_pm_collapse();

	if (!from_idle)
		printk(KERN_INFO "[K][R] resume start\n");
#ifdef CONFIG_CACHE_L2X0
	if (!cpu_is_msm8625() && !cpu_is_msm8625q())
		l2cc_resume();
#endif

	msm_pm_boot_config_after_pc(smp_processor_id());

	if (collapsed) {
#ifdef CONFIG_VFP
		vfp_pm_resume();
#endif
		cpu_init();
		local_fiq_enable();
	}

	MSM_PM_DPRINTK(MSM_PM_DEBUG_SUSPEND | MSM_PM_DEBUG_POWER_COLLAPSE,
		KERN_INFO,
		"%s(): msm_pm_collapse returned %d\n", __func__, collapsed);

	ret = msm_spm_set_low_power_mode(MSM_SPM_MODE_CLOCK_GATING, false);
	WARN_ON(ret);

	return !collapsed;
}

static int msm_pm_swfi(bool from_idle, bool ramp_acpu)
{
	unsigned long saved_acpuclk_rate = 0;

	if (ramp_acpu) {
		saved_acpuclk_rate = acpuclk_wait_for_irq();

		if (!from_idle)
			MSM_PM_DPRINTK(MSM_PM_DEBUG_CLOCK, KERN_INFO,
				"%s(): change clock rate (old rate = %lu)\n", __func__,
				saved_acpuclk_rate);

		if (!saved_acpuclk_rate)
			return -EIO;
	}

	if (!cpu_is_msm8625() && !cpu_is_msm8625q())
		msm_pm_config_hw_before_swfi();

	msm_pm_irq_extns->read_active_irq();
	msm_arch_idle();

	if (ramp_acpu) {
		if (!from_idle)
			MSM_PM_DPRINTK(MSM_PM_DEBUG_CLOCK, KERN_INFO,
				"%s(): restore clock rate to %lu\n", __func__,
				saved_acpuclk_rate);

		if (acpuclk_set_rate(smp_processor_id(), saved_acpuclk_rate,
				SETRATE_SWFI) < 0)
			printk(KERN_ERR
				"[K] %s(): failed to restore clock rate(%lu)\n",
				__func__, saved_acpuclk_rate);
	}

	return 0;
}

static int64_t msm_pm_timer_enter_suspend(int64_t *period)
{
	int time = 0;

	time = msm_timer_get_sclk_time(period);
	if (!time)
		pr_err("%s: Unable to read sclk.\n", __func__);
		return time;
}

static int64_t msm_pm_timer_exit_suspend(int64_t time, int64_t period)
{

	if (time != 0) {
		int64_t end_time = msm_timer_get_sclk_time(NULL);
		if (end_time != 0) {
			time = end_time - time;
			if (time < 0)
				time += period;
			} else
				time = 0;
		}
	return time;
}


void arch_idle(void)
{
	bool allow[MSM_PM_SLEEP_MODE_NR];
	uint32_t sleep_limit = SLEEP_LIMIT_NONE;
	enum msm_pm_sleep_mode sleep_mode = MSM_PM_SLEEP_MODE_NR;

	int64_t timer_expiration;
	int latency_qos;
	int ret;
	int i;
	unsigned int cpu;
	int64_t t1;
	static DEFINE_PER_CPU(int64_t, t2);
	int exit_stat;

	if (!atomic_read(&msm_pm_init_done))
		return;

	cpu = smp_processor_id();
	latency_qos = pm_qos_request(PM_QOS_CPU_DMA_LATENCY);
	
	timer_expiration = ktime_to_ns(tick_nohz_get_sleep_length());

	t1 = ktime_to_ns(ktime_get());
	msm_pm_add_stat(MSM_PM_STAT_NOT_IDLE, t1 - __get_cpu_var(t2));
	msm_pm_add_stat(MSM_PM_STAT_REQUESTED_IDLE, timer_expiration);
	exit_stat = MSM_PM_STAT_IDLE_SPIN;

	for (i = 0; i < ARRAY_SIZE(allow); i++)
		allow[i] = true;

	if (num_online_cpus() > 1 ||
		(timer_expiration < msm_pm_idle_sleep_min_time) ||
		!msm_pm_irq_extns->idle_sleep_allowed()) {
		allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE] = false;
		allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_NO_XO_SHUTDOWN] = false;
		allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_STANDALONE] = false;
	}

	for (i = 0; i < ARRAY_SIZE(allow); i++) {
		struct msm_pm_platform_data *mode =
					&msm_pm_modes[MSM_PM_MODE(cpu, i)];
		if (!mode->idle_supported || !mode->idle_enabled ||
			mode->latency >= latency_qos ||
			mode->residency * 1000ULL >= timer_expiration)
			allow[i] = false;
	}

	if (allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE] ||
		allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_NO_XO_SHUTDOWN]) {
		uint32_t wait_us = CONFIG_MSM_IDLE_WAIT_ON_MODEM;
		while (msm_pm_modem_busy() && wait_us) {
			if (wait_us > 100) {
				udelay(100);
				wait_us -= 100;
			} else {
				udelay(wait_us);
				wait_us = 0;
			}
		}

		if (msm_pm_modem_busy()) {
			allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE] = false;
			allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_NO_XO_SHUTDOWN]
				= false;
		}
	}

	MSM_PM_DPRINTK(MSM_PM_DEBUG_IDLE, KERN_INFO,
		"%s(): latency qos %d, next timer %lld, sleep limit %u\n",
		__func__, latency_qos, timer_expiration, sleep_limit);

	for (i = 0; i < ARRAY_SIZE(allow); i++)
		MSM_PM_DPRINTK(MSM_PM_DEBUG_IDLE, KERN_INFO,
			"%s(): allow %s: %d\n", __func__,
			msm_pm_sleep_mode_labels[i], (int)allow[i]);

	if (allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE] ||
		allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_NO_XO_SHUTDOWN]) {
		int64_t next_timer_exp = msm_timer_enter_idle();
		uint32_t sleep_delay;
		bool low_power = false;

		sleep_mode = MSM_PM_SLEEP_MODE_POWER_COLLAPSE;
		sleep_delay = (uint32_t) msm_pm_convert_and_cap_time(
			next_timer_exp, MSM_PM_SLEEP_TICK_LIMIT);

		if (sleep_delay == 0) 
			sleep_delay = 1;

		if (!allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE])
			sleep_limit = SLEEP_LIMIT_NO_TCXO_SHUTDOWN;

#if defined(CONFIG_MSM_MEMORY_LOW_POWER_MODE_IDLE_ACTIVE)
		sleep_limit |= SLEEP_RESOURCE_MEMORY_BIT1;
#elif defined(CONFIG_MSM_MEMORY_LOW_POWER_MODE_IDLE_RETENTION)
		sleep_limit |= SLEEP_RESOURCE_MEMORY_BIT0;
#endif

		ret = msm_pm_power_collapse(true, sleep_delay, sleep_limit);
		low_power = (ret != -EBUSY && ret != -ETIMEDOUT);
		msm_timer_exit_idle(low_power);

		if (ret)
			exit_stat = MSM_PM_STAT_IDLE_FAILED_POWER_COLLAPSE;
		else {
			exit_stat = MSM_PM_STAT_IDLE_POWER_COLLAPSE;
			msm_pm_sleep_limit = sleep_limit;
		}
	} else if (allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_STANDALONE]) {
		sleep_mode = MSM_PM_SLEEP_MODE_POWER_COLLAPSE_STANDALONE;
		ret = msm_pm_power_collapse_standalone(true);
		exit_stat = ret ?
			MSM_PM_STAT_IDLE_FAILED_STANDALONE_POWER_COLLAPSE :
			MSM_PM_STAT_IDLE_STANDALONE_POWER_COLLAPSE;
	} else if (allow[MSM_PM_SLEEP_MODE_RAMP_DOWN_AND_WAIT_FOR_INTERRUPT]) {
		sleep_mode = MSM_PM_SLEEP_MODE_RAMP_DOWN_AND_WAIT_FOR_INTERRUPT;
		ret = msm_pm_swfi(true, true);
		if (ret)
			while (!msm_pm_irq_extns->irq_pending())
				udelay(1);
		exit_stat = ret ? MSM_PM_STAT_IDLE_SPIN : MSM_PM_STAT_IDLE_WFI;
	} else if (allow[MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT]) {
		sleep_mode = MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT;
		msm_pm_swfi(true, false);

		exit_stat = MSM_PM_STAT_IDLE_WFI;
	} else {
		while (!msm_pm_irq_extns->irq_pending())
			udelay(1);
		exit_stat = MSM_PM_STAT_IDLE_SPIN;
	}

	__get_cpu_var(t2) = ktime_to_ns(ktime_get());
	msm_pm_add_stat(exit_stat, __get_cpu_var(t2) - t1);
	htc_idle_stat_add(sleep_mode, (u32)(t2 - t1)/1000);

}

static int msm_pm_enter(suspend_state_t state)
{
	bool allow[MSM_PM_SLEEP_MODE_NR];
	uint32_t sleep_limit = SLEEP_LIMIT_NONE;
	int ret = -EPERM;
	int i;
	int64_t period = 0;
	int64_t time = 0;

	
	if (smp_processor_id()) {
		__WARN();
		goto suspend_exit;
	}

	time = msm_pm_timer_enter_suspend(&period);

	if (board_mfg_mode() == 4) {
		gpio_set_diag_gpio_table(
			(unsigned long *)board_get_mfg_sleep_gpio_table());
		pr_info("power test mode : gpio_set_diag_gpio_table\n");
	}

	MSM_PM_DPRINTK(MSM_PM_DEBUG_SUSPEND, KERN_INFO,
		"%s(): sleep limit %u\n", __func__, sleep_limit);

	for (i = 0; i < ARRAY_SIZE(allow); i++)
		allow[i] = true;

	for (i = 0; i < ARRAY_SIZE(allow); i++) {
		struct msm_pm_platform_data *mode;
		mode = &msm_pm_modes[MSM_PM_MODE(0, i)];
		if (!mode->suspend_supported || !mode->suspend_enabled)
			allow[i] = false;
	}

	if (allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE] ||
		allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_NO_XO_SHUTDOWN]) {
		enum msm_pm_time_stats_id id;


#ifdef CONFIG_MSM_SLEEP_TIME_OVERRIDE
		if (msm_pm_sleep_time_override > 0) {
			int64_t ns;
			ns = NSEC_PER_SEC * (int64_t)msm_pm_sleep_time_override;
			msm_pm_set_max_sleep_time(ns);
			msm_pm_sleep_time_override = 0;
		}
#endif
		if (!allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE])
			sleep_limit = SLEEP_LIMIT_NO_TCXO_SHUTDOWN;

#if defined(CONFIG_MSM_MEMORY_LOW_POWER_MODE_SUSPEND_ACTIVE)
		sleep_limit |= SLEEP_RESOURCE_MEMORY_BIT1;
#elif defined(CONFIG_MSM_MEMORY_LOW_POWER_MODE_SUSPEND_RETENTION)
		sleep_limit |= SLEEP_RESOURCE_MEMORY_BIT0;
#elif defined(CONFIG_MSM_MEMORY_LOW_POWER_MODE_SUSPEND_DEEP_POWER_DOWN)
		if (get_msm_migrate_pages_status() != MEM_OFFLINE)
			sleep_limit |= SLEEP_RESOURCE_MEMORY_BIT0;
#endif

		for (i = 0; i < 30 && msm_pm_modem_busy(); i++)
			udelay(500);

		ret = msm_pm_power_collapse(
			false, msm_pm_max_sleep_time, sleep_limit);

		if (ret)
			id = MSM_PM_STAT_FAILED_SUSPEND;
		else {
			id = MSM_PM_STAT_SUSPEND;
			msm_pm_sleep_limit = sleep_limit;
		}

		time = msm_pm_timer_exit_suspend(time, period);
		msm_pm_add_stat(id, time);
	} else if (allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_STANDALONE]) {
		ret = msm_pm_power_collapse_standalone(false);
	} else if (allow[MSM_PM_SLEEP_MODE_RAMP_DOWN_AND_WAIT_FOR_INTERRUPT]) {
		printk(KERN_INFO "[K][R] suspend end\n");
		ret = msm_pm_swfi(false, true);
		printk(KERN_INFO "[K][R] resume start\n");
		if (ret)
			while (!msm_pm_irq_extns->irq_pending())
				udelay(1);
	} else if (allow[MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT]) {
		printk(KERN_INFO "[K][R] suspend end\n");
		msm_pm_swfi(false, false);
		printk(KERN_INFO "[K][R] resume start\n");
	}

suspend_exit:
	MSM_PM_DPRINTK(MSM_PM_DEBUG_SUSPEND, KERN_INFO,
		"%s(): return %d\n", __func__, ret);

	return ret;
}

static struct platform_suspend_ops msm_pm_ops = {
	.enter = msm_pm_enter,
	.valid = suspend_valid_only_mem,
};

static bool console_flushed;

void msm_pm_flush_console(void)
{
	if (console_flushed)
		return;
	console_flushed = true;

	printk(KERN_EMERG "\n");
	printk(KERN_EMERG "[K] Restarting %s\n", linux_banner);
	if (console_trylock()) {
		console_unlock();
		return;
	}

	mdelay(50);

	local_irq_disable();
	if (!console_trylock())
		printk(KERN_EMERG "[K] msm_restart: Console was locked! Busting\n");
	else
		printk(KERN_EMERG "[K] msm_restart: Console was locked!\n");
	console_unlock();
}

void msm_pm_cpu_enter_lowpower(unsigned int cpu)
{
	bool allow[MSM_PM_SLEEP_MODE_NR];
	int i;

	for (i = 0; i < MSM_PM_SLEEP_MODE_NR; i++) {
		struct msm_pm_platform_data *mode;

		mode = &msm_pm_modes[MSM_PM_MODE(cpu, i)];
		allow[i] = mode->suspend_supported && mode->suspend_enabled;
	}

	MSM_PM_DPRINTK(MSM_PM_DEBUG_HOTPLUG, KERN_INFO,
		"CPU%u: %s: shutting down cpu\n", cpu, __func__);

	if (allow[MSM_PM_SLEEP_MODE_POWER_COLLAPSE_STANDALONE]) {
		msm_pm_power_collapse_standalone(false);
	} else if (allow[MSM_PM_SLEEP_MODE_WAIT_FOR_INTERRUPT]) {
		msm_pm_swfi(true, false);
	} else {
		MSM_PM_DPRINTK(MSM_PM_DEBUG_HOTPLUG, KERN_INFO,
			"CPU%u: %s: shutting down failed!!!\n", cpu, __func__);
	}
}


static uint32_t restart_reason = 0x776655AA;

static void msm_pm_power_off(void)
{
#if 0
	msm_rpcrouter_close();
#endif
#ifdef CONFIG_HTC_OFFMODE_ALARM
        set_offmode_alarm();
        printk(KERN_INFO "[K] msm_pm_power_off:wakeup after %d\r\n", msm_wakeup_after);
        if (msm_wakeup_after)
                msm_proc_comm(PCOM_SET_RTC_ALARM, &msm_wakeup_after, 0);
#endif

	msm_proc_comm(PCOM_POWER_DOWN, 0, 0);
	for (;;)
		;
}
#if 1
static void msm_pm_restart(char str, const char *cmd)
{
	if (console_trylock()) {
		console_unlock();
	}else{
		printk(KERN_EMERG "[K] msm_restart: Console was locked, can't flush console\n");
	}

	pr_info("[K] %s: restart_reason 0x%x, cmd %s\n", __func__, restart_reason, (cmd) ? cmd : "NULL");

	
	if (restart_reason == RESTART_REASON_RIL_FATAL)
		msm_proc_comm(PCOM_RESET_CHIP_IMM, &restart_reason, 0);
	else
		msm_proc_comm(PCOM_RESET_CHIP, &restart_reason, 0);

	printk(KERN_INFO "[K] @@@@@@@@@@@  msm_proc_comm end\r\n");

#if defined(CONFIG_MSM_RMT_STORAGE_SERVER) || defined(CONFIG_MSM_RMT_STORAGE_CLIENT)
	printk(KERN_INFO "[K] from %s\r\n", __func__);
	wait_rmt_final_call_back(10);
	printk(KERN_INFO "[K] back %s\r\n", __func__);
	
	mdelay(2000);
#else
	
	mdelay(10000);
#endif

	printk(KERN_INFO "[K] @@@@@@@@@@@  enter msm_pm_flush_console\r\n");
	msm_pm_flush_console();
	printk(KERN_INFO "[K] @@@@@@@@@@@  rise PS_hold start\r\n");

	
	if (msm_hw_reset_hook) {
		printk(KERN_INFO "[K] %s : Do HW_RESET by APP not by RADIO\r\n", __func__);
		msm_hw_reset_hook();
	}

	for (;;)
		;
}
#endif
static int msm_reboot_call
	(struct notifier_block *this, unsigned long code, void *_cmd)
{
	if ((code == SYS_RESTART) && _cmd) {
		char *cmd = _cmd;
		if (!strcmp(cmd, "bootloader")) {
			restart_reason = 0x77665500;
		} else if (!strcmp(cmd, "recovery")) {
			restart_reason = 0x77665502;
		} else if (!strcmp(cmd, "eraseflash")) {
			restart_reason = 0x776655EF;
		} else if (!strncmp(cmd, "oem-", 4)) {
			unsigned code = simple_strtoul(cmd + 4, 0, 16) & 0xff;
			restart_reason = 0x6f656d00 | code;
		} else {
			restart_reason = 0x77665501;
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block msm_reboot_notifier = {
	.notifier_call = msm_reboot_call,
};

static void __init boot_lock_nohalt(void)
{
	int nohalt_timeout = 0;

	
	switch (board_mfg_mode()) {
	case 0: 
	case 1: 
	case 2: 
		nohalt_timeout = BOOT_LOCK_TIMEOUT_NORMAL;
		break;
	case 3: 
	case 4: 
	case 5: 
	default:
		nohalt_timeout = BOOT_LOCK_TIMEOUT_SHORT;
		break;
	}
	disable_hlt();

	schedule_delayed_work(&work_expire_boot_lock, nohalt_timeout);
	pr_info("Acquire 'boot-time' no_halt_lock %ds\n", nohalt_timeout / HZ);
}


static int __init msm_pm_init(void)
{
	int ret;
	int val;
	enum msm_pm_time_stats_id enable_stats[] = {
		MSM_PM_STAT_REQUESTED_IDLE,
		MSM_PM_STAT_IDLE_SPIN,
		MSM_PM_STAT_IDLE_WFI,
		MSM_PM_STAT_IDLE_STANDALONE_POWER_COLLAPSE,
		MSM_PM_STAT_IDLE_FAILED_STANDALONE_POWER_COLLAPSE,
		MSM_PM_STAT_IDLE_POWER_COLLAPSE,
		MSM_PM_STAT_IDLE_FAILED_POWER_COLLAPSE,
		MSM_PM_STAT_SUSPEND,
		MSM_PM_STAT_FAILED_SUSPEND,
		MSM_PM_STAT_NOT_IDLE,
	};

#ifdef CONFIG_CPU_V7
	pgd_t *pc_pgd;
	pmd_t *pmd;
	unsigned long pmdval;
	unsigned long exit_phys;

	exit_phys = virt_to_phys(msm_pm_collapse_exit);

	
	pc_pgd = pgd_alloc(&init_mm);
	if (!pc_pgd)
		return -ENOMEM;
	pmd = pmd_offset(pud_offset(pc_pgd + pgd_index(exit_phys), exit_phys),
			 exit_phys);
	pmdval = (exit_phys & PGDIR_MASK) |
		     PMD_TYPE_SECT | PMD_SECT_AP_WRITE;
	pmd[0] = __pmd(pmdval);
	pmd[1] = __pmd(pmdval + (1 << (PGDIR_SHIFT - 1)));

	msm_saved_state_phys =
		allocate_contiguous_ebi_nomap(CPU_SAVED_STATE_SIZE *
					      num_possible_cpus(), 4);
	if (!msm_saved_state_phys)
		return -ENOMEM;
	msm_saved_state = ioremap_nocache(msm_saved_state_phys,
					  CPU_SAVED_STATE_SIZE *
					  num_possible_cpus());
	if (!msm_saved_state)
		return -ENOMEM;

	pmd[2] = __pmd(pmdval + (2 << (PGDIR_SHIFT - 1)));
	flush_pmd_entry(pmd);
	msm_pm_pc_pgd = virt_to_phys(pc_pgd);
	clean_caches((unsigned long)&msm_pm_pc_pgd, sizeof(msm_pm_pc_pgd),
		     virt_to_phys(&msm_pm_pc_pgd));
#endif

	pm_power_off = msm_pm_power_off;
	arm_pm_restart = msm_pm_restart;
	register_reboot_notifier(&msm_reboot_notifier);

	msm_pm_smem_data = smem_alloc(SMEM_APPS_DEM_SLAVE_DATA,
		sizeof(*msm_pm_smem_data));
	if (msm_pm_smem_data == NULL) {
		printk(KERN_ERR "[K] %s: failed to get smsm_data\n", __func__);
		return -ENODEV;
	}

	ret = msm_timer_init_time_sync(msm_pm_timeout);
	if (ret)
		return ret;

	ret = smsm_change_intr_mask(SMSM_POWER_MASTER_DEM, 0xFFFFFFFF, 0);
	if (ret) {
		printk(KERN_ERR "[K] %s: failed to clear interrupt mask, %d\n",
			__func__, ret);
		return ret;
	}

	if (cpu_is_msm8625() || cpu_is_msm8625q()) {
		target_type = TARGET_IS_8625;
		clean_caches((unsigned long)&target_type, sizeof(target_type),
				virt_to_phys(&target_type));

		if (cpu_is_msm8625q())
			val = 0x000F0002;
		else
			val = 0x00030002;

		__raw_writel(val, (MSM_CFG_CTL_BASE + 0x38));

		l2x0_base_addr = MSM_L2CC_BASE;
	}

#ifdef CONFIG_MSM_MEMORY_LOW_POWER_MODE
	msm_pm_smem_data->wakeup_reason = 1;
	smsm_change_state(SMSM_APPS_DEM, 0, DEM_SLAVE_SMSM_RUN);
#endif

	BUG_ON(msm_pm_modes == NULL);

	suspend_set_ops(&msm_pm_ops);

	msm_pm_mode_sysfs_add();
	msm_pm_add_stats(enable_stats, ARRAY_SIZE(enable_stats));

	atomic_set(&msm_pm_init_done, 1);
	boot_lock_nohalt();
	return 0;
}

late_initcall_sync(msm_pm_init);
