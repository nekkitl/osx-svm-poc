#include "../svm-kext/provider/amdvm_host_mtrr.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static amdvm_host_mtrr_state_t
make_wb_state(void)
{
	amdvm_host_mtrr_state_t state;
	memset(&state, 0, sizeof(state));
	state.phys_bits = 48;
	state.variable_count = AMDVM_HOST_MTRR_VARIABLE_MAX;
	state.default_type = AMDVM_HOST_MTRR_DEFAULT_ENABLE |
	    AMDVM_HOST_MTRR_WB;
	return state;
}

static void
test_amd_tom2(void)
{
	amdvm_host_mtrr_state_t state = make_wb_state();
	uint8_t type = 0xff;
	state.amd_extended = 1;
	state.amd_syscfg = AMDVM_HOST_MTRR_AMD_TOM2_ENABLE |
	    AMDVM_HOST_MTRR_AMD_TOM2_FORCE_WB;
	state.amd_top_mem = UINT64_C(0xc0000000);
	state.amd_top_mem2 = UINT64_C(0x400000000); /* synthetic 16 GiB cutoff */
	state.fixed_supported = 1;
	state.default_type = UINT64_C(0xc00); /* observed default UC, E+FE */
	for (unsigned i = 0; i < AMDVM_HOST_MTRR_FIXED_COUNT; ++i) {
		state.fixed[i] = UINT64_C(0x0606060606060606);
	}
	assert(amdvm_host_mtrr_validate(&state) == 0);
	/* Expected values follow APM 3.2.1, not the variable-range matcher. */
	const struct { uint64_t pa; uint8_t expected; } cases[] = {
		{ UINT64_C(0x1000), AMDVM_HOST_MTRR_WB },
		{ UINT64_C(0xfffff000), AMDVM_HOST_MTRR_UC },
		{ UINT64_C(0x100000000), AMDVM_HOST_MTRR_WB },
		{ UINT64_C(0x1a65eb000), AMDVM_HOST_MTRR_WB },
		{ UINT64_C(0x2eddf5000), AMDVM_HOST_MTRR_WB },
		{ UINT64_C(0x3fffff000), AMDVM_HOST_MTRR_WB },
		{ UINT64_C(0x400000000), AMDVM_HOST_MTRR_UC },
	};
	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		assert(amdvm_host_mtrr_effective_type(&state, cases[i].pa,
		    &type) == 0 && type == cases[i].expected);
	}

	/* Neither high RAM nor TOP_MEM2 alone establishes cacheability. */
	for (unsigned flags = 0; flags < 3; ++flags) {
		state.amd_syscfg = (flags & 1 ? AMDVM_HOST_MTRR_AMD_TOM2_ENABLE : 0) |
		    (flags & 2 ? AMDVM_HOST_MTRR_AMD_TOM2_FORCE_WB : 0);
		assert(amdvm_host_mtrr_effective_type(&state, cases[4].pa,
		    &type) == 0 && type == AMDVM_HOST_MTRR_UC);
	}
	state.amd_syscfg = AMDVM_HOST_MTRR_AMD_TOM2_ENABLE |
	    AMDVM_HOST_MTRR_AMD_TOM2_FORCE_WB;
	state.default_type &= ~AMDVM_HOST_MTRR_DEFAULT_ENABLE;
	assert(amdvm_host_mtrr_effective_type(&state, cases[4].pa,
	    &type) == 0 && type == AMDVM_HOST_MTRR_UC);
	state.default_type |= AMDVM_HOST_MTRR_DEFAULT_ENABLE;

	/* Explicit 1 GiB variable range at 4 GiB overrides TOM2 default WB. */
	state.variable_mask[0] = UINT64_C(0x0000ffffc0000000) |
	    AMDVM_HOST_MTRR_MASK_VALID;
	const uint8_t override_types[] = { AMDVM_HOST_MTRR_UC,
	    AMDVM_HOST_MTRR_WC, AMDVM_HOST_MTRR_WT, AMDVM_HOST_MTRR_WP,
	    AMDVM_HOST_MTRR_WB };
	for (unsigned i = 0; i < sizeof(override_types); ++i) {
		state.variable_base[0] = UINT64_C(0x100000000) | override_types[i];
		assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x100001000),
		    &type) == 0 && type == override_types[i]);
	}
	state.variable_base[0] = UINT64_C(0x100000000) | AMDVM_HOST_MTRR_WC;
	state.variable_base[1] = UINT64_C(0x100000000) | AMDVM_HOST_MTRR_WT;
	state.variable_mask[1] = state.variable_mask[0];
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x100001000),
	    &type) != 0); /* undefined overlap still fails closed */
	memset(state.variable_mask, 0, sizeof(state.variable_mask));
	memset(state.variable_base, 0, sizeof(state.variable_base));

	/* TOP_MEM2 low bits are architecturally ignored, not part of the cutoff. */
	state.amd_top_mem2 |= UINT64_C(0x7fffff);
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x400000000),
	    &type) == 0 && type == AMDVM_HOST_MTRR_UC);
	amdvm_host_mtrr_state_t changed = state;
	assert(amdvm_host_mtrr_equal(&state, &changed));
	changed.amd_syscfg ^= AMDVM_HOST_MTRR_AMD_TOM2_ENABLE;
	assert(!amdvm_host_mtrr_equal(&state, &changed));
	changed = state;
	changed.amd_top_mem2 += UINT64_C(0x800000);
	assert(!amdvm_host_mtrr_equal(&state, &changed));
	changed = state;
	changed.amd_top_mem += UINT64_C(0x800000);
	assert(!amdvm_host_mtrr_equal(&state, &changed));
	changed = state;
	changed.amd_extended = 0;
	assert(!amdvm_host_mtrr_equal(&state, &changed));
	assert(amdvm_host_mtrr_validate(&changed) != 0);
	changed = state;
	changed.phys_bits = 32; /* cutoff outside implemented physical space */
	assert(amdvm_host_mtrr_validate(&changed) != 0);
}

int
main(void)
{
	amdvm_host_mtrr_state_t state = make_wb_state();
	uint8_t type = 0xff;

	assert(amdvm_host_mtrr_validate(&state) == 0);
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x100000),
	    &type) == 0 && type == AMDVM_HOST_MTRR_WB);

	/* A 256 MiB WB range overrides a UC default for matching HPAs. */
	state.default_type = AMDVM_HOST_MTRR_DEFAULT_ENABLE | AMDVM_HOST_MTRR_UC;
	state.variable_base[0] = UINT64_C(0x40000000) | AMDVM_HOST_MTRR_WB;
	state.variable_mask[0] = UINT64_C(0x0000fffff0000000) |
	    AMDVM_HOST_MTRR_MASK_VALID;
	assert(amdvm_host_mtrr_validate(&state) == 0);
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x40001000),
	    &type) == 0 && type == AMDVM_HOST_MTRR_WB);
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x50000000),
	    &type) == 0 && type == AMDVM_HOST_MTRR_UC);

	/* An undefined WB/WC overlap fails closed. */
	state.variable_base[1] = UINT64_C(0x40000000) | AMDVM_HOST_MTRR_WC;
	state.variable_mask[1] = state.variable_mask[0];
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x40001000),
	    &type) != 0);
	state.variable_base[0] = UINT64_C(0x40000000) | AMDVM_HOST_MTRR_WB;
	state.variable_base[1] = UINT64_C(0x40000000) | AMDVM_HOST_MTRR_WT;
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x40001000),
	    &type) == 0 && type == AMDVM_HOST_MTRR_WT);

	/* Fixed MTRRs take priority below 1 MiB when enabled. */
	state.fixed_supported = 1;
	state.default_type |= AMDVM_HOST_MTRR_FIXED_ENABLE;
	for (unsigned i = 0; i < AMDVM_HOST_MTRR_FIXED_COUNT; ++i) {
		state.fixed[i] = UINT64_C(0x0606060606060606);
	}
	state.fixed[0] = UINT64_C(0x0000000000000000);
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x1000),
	    &type) == 0 && type == AMDVM_HOST_MTRR_UC);
	assert(amdvm_host_mtrr_effective_type(&state, UINT64_C(0x100000),
	    &type) == 0 && type == AMDVM_HOST_MTRR_UC);

	state.variable_mask[0] |= UINT64_C(0x1000); /* noncontiguous mask */
	assert(amdvm_host_mtrr_validate(&state) != 0);
	test_amd_tom2();
	puts("amdvm host MTRR helper tests (generic + AMD TOM2): PASS");
	return 0;
}
