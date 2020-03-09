/* Copyright (c) 2014, The Linux Foundation. All rights reserved.
 * Copyright (c) 2013 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* MSM ARMv8 CPU Operations
 * Based on arch/arm64/kernel/smp_spin_table.c
 */

#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/qcom_scm.h>
#include <linux/smp.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>

/* CPU power domain register offsets */
#define CPU_PWR_CTL			0x4
#define CPU_PWR_GATE_CTL		0x14

/* L2 power domain register offsets */
#define L2_PWR_CTL_OVERRIDE		0xc
#define L2_PWR_CTL			0x14
#define L2_PWR_STATUS			0x18
#define L2_CORE_CBCR			0x58

extern void secondary_holding_pen(void);

extern volatile unsigned long secondary_holding_pen_release;

static DEFINE_RAW_SPINLOCK(boot_lock);
static DEFINE_PER_CPU(int, cold_boot_done);

static void write_pen_release(u64 val)
{
	void *start = (void *)&secondary_holding_pen_release;
	unsigned long size = sizeof(secondary_holding_pen_release);

	secondary_holding_pen_release = val;
	smp_wmb();
	__flush_dcache_area(start, size);
}

static int secondary_pen_release(unsigned int cpu)
{
	unsigned long timeout;

	/*
	 * Set synchronisation state between this boot processor
	 * and the secondary one
	 */
	raw_spin_lock(&boot_lock);
	write_pen_release(cpu_logical_map(cpu));

	/*
	 * Wake up CPU with a sev (send event) instruction)
	 */
	sev();

	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		if (secondary_holding_pen_release == INVALID_HWID)
			break;
		udelay(10);
	}
	raw_spin_unlock(&boot_lock);

	return secondary_holding_pen_release != INVALID_HWID ? -ENOSYS : 0;
}

static int __init msm_cpu_init_once(void)
{
	int ret;

	ret = qcom_scm_mc_boot_available();
	if (ret == 0) {
		pr_err("%s: Multi-cluster boot unavailable\n", __func__);
		return -ENOSYS;
	}

	return 0;
}

static int __init msm_cpu_init(unsigned int cpu)
{
	static int initialized = 0;
	int ret;

	if (initialized == 0) {
		ret = msm_cpu_init_once();
		if (ret) {
			pr_err("%s: one time initialization failed\n",
			       __func__);
			return ret;
		}

		initialized = 1;
	}

	return 0;
}

static int __init msm_cpu_prepare(unsigned int cpu)
{
	u64 mpidr_el1 = cpu_logical_map(cpu);
	int ret;

	if (mpidr_el1 & ~MPIDR_HWID_BITMASK) {
		pr_err("CPU%d: Failed to set boot address\n", cpu);
		return -ENOSYS;
	}

	ret = qcom_scm_set_cold_boot_addr_mc(secondary_holding_pen,
				BIT(MPIDR_AFFINITY_LEVEL(mpidr_el1, 0)),
				BIT(MPIDR_AFFINITY_LEVEL(mpidr_el1, 1)),
				BIT(MPIDR_AFFINITY_LEVEL(mpidr_el1, 2)));
	if (ret) {
		pr_warn("CPU%d: Failed to set boot address\n", cpu);
		return -ENOSYS;
	}

	/* Mark CPU0 cold boot flag as done */
	if (per_cpu(cold_boot_done, 0) == false)
		per_cpu(cold_boot_done, 0) = true;

	return 0;
}

static int msm_power_on_l2_cache(struct device_node *pd_node, int cpu)
{
	void __iomem *pd_base;

	pd_base = of_iomap(pd_node, 0);
	if (!pd_base)
		return -ENOMEM;

	/* Skip power-on sequence if l2 cache is already powered up */
	if (readl_relaxed(pd_base + L2_PWR_STATUS) & BIT(9))
		goto done;

	/* Close L2/SCU Logic GDHS and power up the cache */
	writel_relaxed(0x10D700, pd_base + L2_PWR_CTL);

	/* Assert PRESETDBGn */
	writel_relaxed(0x400000, pd_base + L2_PWR_CTL_OVERRIDE);
	mb();
	udelay(2);

	/* De-assert L2/SCU memory Clamp */
	writel_relaxed(0x101700, pd_base + L2_PWR_CTL);

	/* Wakeup L2/SCU RAMs by deasserting sleep signals */
	writel_relaxed(0x101703, pd_base + L2_PWR_CTL);
	mb();
	udelay(2);

	/* Enable clocks via SW_CLK_EN */
	writel_relaxed(0x01, pd_base + L2_CORE_CBCR);

	/* De-assert L2/SCU logic clamp */
	writel_relaxed(0x101603, pd_base + L2_PWR_CTL);
	mb();
	udelay(2);

	/* De-assert PRESSETDBg */
	writel_relaxed(0x0, pd_base + L2_PWR_CTL_OVERRIDE);

	/* De-assert L2/SCU Logic reset */
	writel_relaxed(0x100203, pd_base + L2_PWR_CTL);
	mb();
	udelay(54);

	/* Turn on the PMIC_APC */
	writel_relaxed(0x10100203, pd_base + L2_PWR_CTL);

	/* Set H/W clock control for the cpu CBC block */
	writel_relaxed(0x03, pd_base + L2_CORE_CBCR);
	mb();

done:
	iounmap(pd_base);
	return 0;
}

static int msm_unclamp_secondary_arm_cpu(unsigned int cpu)
{
	struct device_node *cpu_node, *acc_node, *l2_node, *l2pd_node;
	void __iomem *acc_base;
	int ret;

	cpu_node = of_get_cpu_node(cpu, NULL);
	if (!cpu_node)
		return -ENODEV;

	acc_node = of_parse_phandle(cpu_node, "qcom,acc", 0);
	if (!acc_node) {
		ret = -ENODEV;
		goto put_cpu;
	}

	l2_node = of_parse_phandle(cpu_node, "next-level-cache", 0);
	if (!l2_node) {
		ret = -ENODEV;
		goto put_acc;
	}

	l2pd_node = of_parse_phandle(l2_node, "power-domain", 0);
	if (!l2pd_node) {
		ret = -ENODEV;
		goto put_l2;
	}

	/* Ensure L2-cache of the CPU is powered on before
	 * unclamping cpu power rails.
	 */
	ret = msm_power_on_l2_cache(l2pd_node, cpu);
	if (ret) {
		pr_err("L2 cache power up failed for CPU%d\n", cpu);
		goto put_l2pd;
	}

	acc_base = of_iomap(acc_node, 0);
	if (!acc_base) {
		ret = -ENOMEM;
		goto put_l2pd;
	}

	/* Assert Reset on cpu-n */
	writel_relaxed(0x00000033, acc_base + CPU_PWR_CTL);
	mb();

	/* Program skew to 16 X0 clock cycles */
	writel_relaxed(0x10000001, acc_base + CPU_PWR_GATE_CTL);
	mb();
	udelay(2);

	/* De-assert coremem clamp */
	writel_relaxed(0x00000031, acc_base + CPU_PWR_CTL);
	mb();

	/* Close coremem array gdhs */
	writel_relaxed(0x00000039, acc_base + CPU_PWR_CTL);
	mb();
	udelay(2);

	/* De-assert cpu-n clamp */
	writel_relaxed(0x00020038, acc_base + CPU_PWR_CTL);
	mb();
	udelay(2);

	/* De-assert cpu-n reset */
	writel_relaxed(0x00020008, acc_base + CPU_PWR_CTL);
	mb();

	/* Assert PWRDUP signal on core-n */
	writel_relaxed(0x00020088, acc_base + CPU_PWR_CTL);
	mb();

	/* Secondary CPU-N is now alive */
	iounmap(acc_base);

put_l2pd:
	of_node_put(l2pd_node);
put_l2:
	of_node_put(l2_node);
put_acc:
	of_node_put(acc_node);
put_cpu:
	of_node_put(cpu_node);
	return ret;
}

static int msm_cpu_boot(unsigned int cpu)
{
	int ret = 0;

	if (per_cpu(cold_boot_done, cpu) == false) {
		ret = msm_unclamp_secondary_arm_cpu(cpu);
		if (ret)
			return ret;

		per_cpu(cold_boot_done, cpu) = true;
	}

	return secondary_pen_release(cpu);
}

static void msm_cpu_postboot(void)
{
	/*
	 * Let the primary processor know we're out of the pen.
	 */
	write_pen_release(INVALID_HWID);

	/*
	 * Synchronise with the boot thread.
	 */
	raw_spin_lock(&boot_lock);
	raw_spin_unlock(&boot_lock);
}

const struct cpu_operations qcom_kpss_acc_v2_ops = {
	.name		= "qcom,kpss-acc-v2",
	.cpu_init	= msm_cpu_init,
	.cpu_prepare	= msm_cpu_prepare,
	.cpu_boot	= msm_cpu_boot,
	.cpu_postboot	= msm_cpu_postboot,
};
