/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <i386/cpu_data.h>
#include <i386/cpuid.h>
#include <i386/lapic.h>
#include <i386/proc_reg.h>
#include <kern/assert.h> /* static_assert, assert */
#include <kern/monotonic.h>
#include <x86_64/monotonic.h>
#include <sys/errno.h>
#include <sys/monotonic.h>

/*
 * Sanity check the compiler.
 */

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif /* !defined(__has_builtin) */
#if !__has_builtin(__builtin_ia32_rdpmc)
#error requires __builtin_ia32_rdpmc builtin
#endif /* !__has_builtin(__builtin_ia32_rdpmc) */

#pragma mark core counters

bool mt_core_supported = false;

/*
 * PMC[0-2]_{RD,WR} allow reading and writing the fixed PMCs.
 *
 * There are separate defines for access type because the read side goes through
 * the rdpmc instruction, which has a different counter encoding than the msr
 * path.
 */
#define PMC_FIXED_RD(CTR) ((UINT64_C(1) << 30) | (CTR))
#define PMC_FIXED_WR(CTR) (MSR_IA32_PERF_FIXED_CTR0 + (CTR))
#define PMC0_RD PMC_FIXED_RD(0)
#define PMC0_WR PMC_FIXED_WR(0)
#define PMC1_RD PMC_FIXED_RD(1)
#define PMC1_WR PMC_FIXED_WR(1)
#define PMC2_RD PMC_FIXED_RD(2)
#define PMC2_WR PMC_FIXED_WR(2)

struct mt_cpu *
mt_cur_cpu(void)
{
	return &current_cpu_datap()->cpu_monotonic;
}

uint64_t
mt_core_snap(unsigned int ctr)
{
	if (!mt_core_supported) {
		return 0;
	}

	switch (ctr) {
	case 0:
		return __builtin_ia32_rdpmc(PMC0_RD);
	case 1:
		return __builtin_ia32_rdpmc(PMC1_RD);
	case 2:
		return __builtin_ia32_rdpmc(PMC2_RD);
	default:
		panic("monotonic: invalid core counter read: %u", ctr);
		__builtin_trap();
	}
}

void
mt_core_set_snap(unsigned int ctr, uint64_t count)
{
	if (!mt_core_supported) {
		return;
	}

	switch (ctr) {
	case 0:
		wrmsr64(PMC0_WR, count);
		break;
	case 1:
		wrmsr64(PMC1_WR, count);
		break;
	case 2:
		wrmsr64(PMC2_WR, count);
		break;
	default:
		panic("monotonic: invalid core counter write: %u", ctr);
		__builtin_trap();
	}
}

/*
 * FIXED_CTR_CTRL controls which rings fixed counters are enabled in and if they
 * deliver PMIs.
 *
 * Each fixed counters has 4 bits: [0:1] controls which ring it's enabled in,
 * [2] counts all hardware threads in each logical core (we don't want this),
 * and [3] enables PMIs on overflow.
 */

#define FIXED_CTR_CTRL 0x38d

/*
 * Fixed counters are enabled in all rings, so hard-code this register state to
 * enable in all rings and deliver PMIs.
 */
#define FIXED_CTR_CTRL_INIT (0x888 | 0x333)

/*
 * GLOBAL_CTRL controls which counters are enabled -- the high 32-bits control
 * the fixed counters and the lower half is for the configurable counters.
 */

#define GLOBAL_CTRL 0x38f

/*
 * Fixed counters are always enabled -- and there are three of them.
 */
#define GLOBAL_CTRL_FIXED_EN (((UINT64_C(1) << 3) - 1) << 32)

/*
 * GLOBAL_STATUS reports the state of counters, like those that have overflowed.
 */
#define GLOBAL_STATUS 0x38e

#define CTR_MAX ((UINT64_C(1) << 48) - 1)
#define CTR_FIX_POS(CTR) ((UINT64_C(1) << (CTR)) << 32)

#define GLOBAL_OVF 0x390

static void
core_down(cpu_data_t *cpu)
{
	if (!mt_core_supported) {
		return;
	}

	assert(ml_get_interrupts_enabled() == FALSE);

	wrmsr64(GLOBAL_CTRL, 0);
	mt_mtc_update_fixed_counts(&cpu->cpu_monotonic, NULL, NULL);
}

static void
core_up(cpu_data_t *cpu)
{
	struct mt_cpu *mtc;

	if (!mt_core_supported) {
		return;
	}

	assert(ml_get_interrupts_enabled() == FALSE);

	mtc = &cpu->cpu_monotonic;

	for (int i = 0; i < MT_CORE_NFIXED; i++) {
		mt_core_set_snap(i, mtc->mtc_snaps[i]);
	}
	wrmsr64(FIXED_CTR_CTRL, FIXED_CTR_CTRL_INIT);
	wrmsr64(GLOBAL_CTRL, GLOBAL_CTRL_FIXED_EN);
}

void
mt_cpu_down(cpu_data_t *cpu)
{
	core_down(cpu);
}

void
mt_cpu_up(cpu_data_t *cpu)
{
	boolean_t intrs_en;
	intrs_en = ml_set_interrupts_enabled(FALSE);
	core_up(cpu);
	ml_set_interrupts_enabled(intrs_en);
}

static int
mt_pmi_x86_64(x86_saved_state_t *state)
{
	uint64_t status;
	struct mt_cpu *mtc;
	bool fixed_ovf = false;

	assert(ml_get_interrupts_enabled() == FALSE);
	mtc = mt_cur_cpu();
	status = rdmsr64(GLOBAL_STATUS);

	(void)atomic_fetch_add_explicit(&mt_pmis, 1, memory_order_relaxed);

	for (int i = 0; i < MT_CORE_NFIXED; i++) {
		if (status & CTR_FIX_POS(i)) {
			fixed_ovf = true;
			uint64_t prior;

			prior = CTR_MAX - mtc->mtc_snaps[i];
			assert(prior <= CTR_MAX);
			prior += 1; /* wrapped */

			mtc->mtc_counts[i] += prior;
			mtc->mtc_snaps[i] = 0;
			mt_mtc_update_count(mtc, i);
		}
	}

	/* if any of the configurable counters overflowed, tell kpc */
	if (status & ((UINT64_C(1) << 4) - 1)) {
		extern void kpc_pmi_handler(x86_saved_state_t *state);
		kpc_pmi_handler(state);
	}
	return 0;
}

void
mt_init(void)
{
	uint32_t cpuinfo[4];

	do_cpuid(0xA, cpuinfo);

	if ((cpuinfo[0] & 0xff) >= 2) {
		lapic_set_pmi_func((i386_intr_func_t)mt_pmi_x86_64);
		mt_core_supported = true;
	}
}

static int
core_init(void)
{
	return ENOTSUP;
}

#pragma mark common hooks

const struct monotonic_dev monotonic_devs[] = {
	[0] = {
		.mtd_name = "monotonic/core",
		.mtd_init = core_init
	}
};

static_assert(
		(sizeof(monotonic_devs) / sizeof(monotonic_devs[0])) == MT_NDEVS,
		"MT_NDEVS macro should be same as the length of monotonic_devs");
