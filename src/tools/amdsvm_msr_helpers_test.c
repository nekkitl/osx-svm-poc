#include "../qemu/target/i386/amdsvm/amdsvm-msr.h"

#include <assert.h>
#include <string.h>

static void test_mtrr_decode(void)
{
	AMDVMMTRRRegister reg;
	uint32_t msr;

	for (msr = 0x200u; msr <= 0x20fu; msr++) {
		assert(amdsvm_msr_decode_mtrr(msr, &reg));
		assert(reg.index == (msr - 0x200u) / 2u);
		assert(reg.kind == ((msr & 1u) ? AMDVM_MTRR_VARIABLE_MASK :
		                    AMDVM_MTRR_VARIABLE_BASE));
	}
	assert(amdsvm_msr_decode_mtrr(0x250u, &reg));
	assert(reg.kind == AMDVM_MTRR_FIXED_64K && reg.index == 0);
	for (msr = 0x258u; msr <= 0x259u; msr++) {
		assert(amdsvm_msr_decode_mtrr(msr, &reg));
		assert(reg.kind == AMDVM_MTRR_FIXED_16K);
		assert(reg.index == msr - 0x258u + 1u);
	}
	for (msr = 0x268u; msr <= 0x26fu; msr++) {
		assert(amdsvm_msr_decode_mtrr(msr, &reg));
		assert(reg.kind == AMDVM_MTRR_FIXED_4K);
		assert(reg.index == msr - 0x268u + 3u);
	}
	assert(amdsvm_msr_decode_mtrr(0x2ffu, &reg));
	assert(reg.kind == AMDVM_MTRR_DEFAULT_TYPE);
	assert(amdsvm_msr_decode_mtrr(0xfeu, &reg));
	assert(reg.kind == AMDVM_MTRR_CAPABILITY);
	assert(!amdsvm_msr_decode_mtrr(0x251u, &reg));
	assert(!amdsvm_msr_decode_mtrr(0x25au, &reg));
	assert(!amdsvm_msr_decode_mtrr(0x270u, &reg));
	assert(!amdsvm_msr_decode_mtrr(0xc0010000u, &reg));
}

static void test_compat_msrs(void)
{
	uint32_t msr;

	for (msr = 0xc0010000u; msr <= 0xc0010003u; msr++) {
		assert(amdsvm_msr_is_tcg_compat_amd(msr));
	}
	assert(amdsvm_msr_is_tcg_compat_amd(0xc0010007u));
	assert(amdsvm_msr_is_tcg_compat_amd(0xc0010010u));
	assert(amdsvm_msr_is_tcg_compat_amd(0xc0010015u));
	assert(amdsvm_msr_is_tcg_compat_amd(0xc0010055u));
	assert(amdsvm_msr_is_tcg_compat_amd(0xc0010114u));
	assert(amdsvm_msr_is_tcg_compat_amd(0xc0011029u));
	assert(!amdsvm_msr_is_tcg_compat_amd(0xc0010004u));
	assert(!amdsvm_msr_is_tcg_compat_amd(0xc0019999u));
}

static void test_mtrr_value_validation(void)
{
	uint8_t combined;

	assert(amdsvm_mtrr_type_valid(AMDVM_MTRR_TYPE_UC));
	assert(amdsvm_mtrr_type_valid(AMDVM_MTRR_TYPE_WB));
	assert(!amdsvm_mtrr_type_valid(2));
	assert(amdsvm_mtrr_fixed_value_valid(UINT64_C(0x0606060606060606)));
	assert(!amdsvm_mtrr_fixed_value_valid(UINT64_C(0x0206060606060606)));
	assert(amdsvm_mtrr_default_value_valid(AMDVM_MTRR_TYPE_WB | 0xc00u));
	assert(!amdsvm_mtrr_default_value_valid(UINT64_C(0x1000)));
	assert(!amdsvm_mtrr_default_value_valid(2));

	/* 16 MiB variable range at 0x10000000 on a 36-bit guest. */
	assert(amdsvm_mtrr_base_value_valid(UINT64_C(0x10000006), 36));
	assert(amdsvm_mtrr_mask_value_valid(UINT64_C(0x0000000fff000800), 36));
	assert(amdsvm_mtrr_base_matches_mask(UINT64_C(0x10000006),
	                                     UINT64_C(0x0000000fff000800), 36));
	assert(!amdsvm_mtrr_base_matches_mask(UINT64_C(0x10800006),
	                                      UINT64_C(0x0000000fff000800), 36));
	assert(!amdsvm_mtrr_mask_value_valid(UINT64_C(0x0000000ff5000800), 36));
	assert(!amdsvm_mtrr_mask_value_valid(UINT64_C(0x0000001fff000800), 36));
	assert(amdsvm_mtrr_mask_value_valid(AMDVM_MTRR_MASK_VALID, 36));
	assert(amdsvm_mtrr_base_matches_mask(AMDVM_MTRR_TYPE_WB,
	                                     AMDVM_MTRR_MASK_VALID, 36));
	/* Alpine TCG trace: 2 GiB PCI hole starting at the 2 GiB boundary. */
	assert(amdsvm_mtrr_base_value_valid(UINT64_C(0x80000000), 40));
	assert(amdsvm_mtrr_mask_value_valid(UINT64_C(0xff80000800), 40));
	assert(amdsvm_mtrr_base_matches_mask(UINT64_C(0x80000000),
	                                     UINT64_C(0xff80000800), 40));
	assert(!amdsvm_mtrr_base_value_valid(UINT64_C(0x10000002), 36));
	assert(!amdsvm_mtrr_base_value_valid(UINT64_C(0x10000006), 11));

	assert(amdsvm_mtrr_types_combine(AMDVM_MTRR_TYPE_UC,
	                                 AMDVM_MTRR_TYPE_WB, &combined));
	assert(combined == AMDVM_MTRR_TYPE_UC);
	assert(amdsvm_mtrr_types_combine(AMDVM_MTRR_TYPE_WB,
	                                 AMDVM_MTRR_TYPE_WT, &combined));
	assert(combined == AMDVM_MTRR_TYPE_WT);
	assert(!amdsvm_mtrr_types_combine(AMDVM_MTRR_TYPE_WB,
	                                 AMDVM_MTRR_TYPE_WC, &combined));
}

static void test_mtrr_effective_type(void)
{
	AMDVMMTRRState state = { 0 };
	uint8_t type;
	uint64_t wb_fixed = UINT64_C(0x0606060606060606);
	uint64_t range_mask = UINT64_C(0x0000000fff000800);

	state.phys_bits = 36;
	state.fixed_supported = true;
	assert(amdsvm_mtrr_effective_type(&state, 0x1000, &type));
	assert(type == AMDVM_MTRR_TYPE_UC); /* Reset has MTRRs disabled. */

	state.default_type = AMDVM_MTRR_DEFAULT_ENABLE | AMDVM_MTRR_TYPE_WB;
	state.variable[0].base = UINT64_C(0x10000000) | AMDVM_MTRR_TYPE_WB;
	state.variable[0].mask = range_mask;
	assert(amdsvm_mtrr_effective_type(&state, 0x10000000, &type));
	assert(type == AMDVM_MTRR_TYPE_WB);
	assert(amdsvm_mtrr_effective_type(&state, 0x10ffffff, &type));
	assert(type == AMDVM_MTRR_TYPE_WB);
	assert(amdsvm_mtrr_effective_type(&state, 0x11000000, &type));
	assert(type == AMDVM_MTRR_TYPE_WB); /* Default fallback. */

	state.variable[1].base = UINT64_C(0x10000000) | AMDVM_MTRR_TYPE_WT;
	state.variable[1].mask = range_mask;
	assert(amdsvm_mtrr_effective_type(&state, 0x10001000, &type));
	assert(type == AMDVM_MTRR_TYPE_WT); /* Defined WB + WT overlap. */
	state.variable[1].base = UINT64_C(0x10000000) | AMDVM_MTRR_TYPE_WC;
	assert(!amdsvm_mtrr_effective_type(&state, 0x10001000, &type));

	state.variable[1].mask = 0; /* Remove the overlapping second range. */
	state.default_type |= AMDVM_MTRR_FIXED_ENABLE;
	memcpy(state.fixed, (uint64_t[AMDVM_MTRR_FIXED_COUNT]){
		wb_fixed, wb_fixed, wb_fixed, wb_fixed, wb_fixed, wb_fixed,
		wb_fixed, wb_fixed, wb_fixed, wb_fixed, wb_fixed,
	}, sizeof(state.fixed));
	state.fixed[2] &= ~UINT64_C(0xff);
	assert(amdsvm_mtrr_effective_type(&state, 0xa1000, &type));
	assert(type == AMDVM_MTRR_TYPE_UC);
	assert(amdsvm_mtrr_effective_type(&state, 0xa2000, &type));
	assert(type == AMDVM_MTRR_TYPE_WB);
	assert(!amdsvm_mtrr_effective_type(&state, UINT64_C(1) << 36, &type));

	/* The captured qemu64 boot mask describes the 2-4 GiB PCI hole. */
	memset(&state, 0, sizeof(state));
	state.phys_bits = 40;
	state.default_type = AMDVM_MTRR_DEFAULT_ENABLE | AMDVM_MTRR_TYPE_WB;
	state.variable[0].base = UINT64_C(0x80000000);
	state.variable[0].mask = UINT64_C(0xff80000800);
	assert(amdsvm_mtrr_effective_type(&state, 0x807ff000, &type));
	assert(type == AMDVM_MTRR_TYPE_UC);
	assert(amdsvm_mtrr_effective_type(&state, 0x80800000, &type));
	assert(type == AMDVM_MTRR_TYPE_UC);
	assert(amdsvm_mtrr_effective_type(&state, UINT64_C(0x100000000), &type));
	assert(type == AMDVM_MTRR_TYPE_WB);
}

static void test_exit_validation(void)
{
	assert(amdsvm_msr_exit_direction_valid(0, true, false));
	assert(amdsvm_msr_exit_direction_valid(1, false, true));
	assert(!amdsvm_msr_exit_direction_valid(0, false, true));
	assert(!amdsvm_msr_exit_direction_valid(1, true, false));
	assert(!amdsvm_msr_exit_direction_valid(2, true, false));
	assert(!amdsvm_msr_exit_direction_valid(0, true, true));

	assert(amdsvm_msr_next_rip_valid(0x1000, 2, true, true, 0x1002));
	assert(!amdsvm_msr_next_rip_valid(0x1000, 2, true, true, 0x1003));
	assert(!amdsvm_msr_next_rip_valid(0x1000, 0, true, true, 0x1000));
	assert(!amdsvm_msr_next_rip_valid(0x1000, 16, true, true, 0x1010));
	assert(!amdsvm_msr_next_rip_valid(UINT64_MAX, 1, true, true, 0));
	assert(amdsvm_msr_next_rip_valid(0xfffffffeu, 2, false, true, 0));
	assert(amdsvm_msr_next_rip_valid(0xfffeu, 2, false, false, 0));
	assert(!amdsvm_msr_next_rip_valid(0xfffeu, 2, false, false, 0x10000));
}

int main(void)
{
	test_mtrr_decode();
	test_mtrr_value_validation();
	test_mtrr_effective_type();
	test_compat_msrs();
	test_exit_validation();
	return 0;
}
