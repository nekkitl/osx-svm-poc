#include "amdvm_host_mtrr.h"

#include <stddef.h>

static int
amdvm_host_mtrr_type_valid(uint8_t type)
{
	return type == AMDVM_HOST_MTRR_UC || type == AMDVM_HOST_MTRR_WC ||
	    type == AMDVM_HOST_MTRR_WT || type == AMDVM_HOST_MTRR_WP ||
	    type == AMDVM_HOST_MTRR_WB;
}

static uint64_t
amdvm_host_mtrr_address_mask(uint8_t phys_bits)
{
	return phys_bits >= 12 && phys_bits <= 52 ?
	    ((UINT64_C(1) << phys_bits) - 1u) & ~UINT64_C(0xfff) : 0;
}

static int
amdvm_host_mtrr_fixed_valid(uint64_t value)
{
	unsigned i;
	for (i = 0; i < 8u; ++i) {
		if (!amdvm_host_mtrr_type_valid((uint8_t)(value >> (i * 8u)))) {
			return 0;
		}
	}
	return 1;
}

static int
amdvm_host_mtrr_combine(uint8_t first, uint8_t second, uint8_t *result)
{
	if (first == second) {
		*result = first;
		return 1;
	}
	if (first == AMDVM_HOST_MTRR_UC || second == AMDVM_HOST_MTRR_UC) {
		*result = AMDVM_HOST_MTRR_UC;
		return 1;
	}
	if ((first == AMDVM_HOST_MTRR_WB && second == AMDVM_HOST_MTRR_WT) ||
	    (first == AMDVM_HOST_MTRR_WT && second == AMDVM_HOST_MTRR_WB)) {
		*result = AMDVM_HOST_MTRR_WT;
		return 1;
	}
	return 0;
}

int
amdvm_host_mtrr_validate(const amdvm_host_mtrr_state_t *state)
{
	uint64_t address_mask;
	unsigned i;
	if (state == NULL || state->phys_bits < 32 || state->phys_bits > 52 ||
	    state->variable_count == 0 ||
	    state->variable_count > AMDVM_HOST_MTRR_VARIABLE_MAX ||
	    state->fixed_supported > 1 || state->amd_extended > 1 ||
	    state->reserved != 0 ||
	    (state->default_type & ~UINT64_C(0xcff)) != 0 ||
	    !amdvm_host_mtrr_type_valid((uint8_t)state->default_type) ||
	    ((state->default_type & AMDVM_HOST_MTRR_FIXED_ENABLE) != 0 &&
	    state->fixed_supported == 0)) {
		return -1;
	}
	if ((!state->amd_extended && (state->amd_syscfg != 0 ||
	    state->amd_top_mem != 0 || state->amd_top_mem2 != 0)) ||
	    (state->amd_extended &&
	    ((state->amd_top_mem & AMDVM_HOST_MTRR_AMD_TOP_MEM_MASK) >
	    (UINT64_C(1) << state->phys_bits) ||
	    (state->amd_top_mem2 & AMDVM_HOST_MTRR_AMD_TOP_MEM_MASK) >
	    (UINT64_C(1) << state->phys_bits)))) {
		return -1;
	}
	address_mask = amdvm_host_mtrr_address_mask(state->phys_bits);
	if (address_mask == 0) return -1;
	if (state->fixed_supported) {
		for (i = 0; i < AMDVM_HOST_MTRR_FIXED_COUNT; ++i) {
			if (!amdvm_host_mtrr_fixed_valid(state->fixed[i])) return -1;
		}
	}
	for (i = 0; i < state->variable_count; ++i) {
		uint64_t base = state->variable_base[i];
		uint64_t mask = state->variable_mask[i];
		uint64_t address_bits = mask & address_mask;
		if ((base & ~(address_mask | UINT64_C(0xff))) != 0 ||
		    (mask & ~(address_mask | AMDVM_HOST_MTRR_MASK_VALID)) != 0 ||
		    !amdvm_host_mtrr_type_valid((uint8_t)base)) {
			return -1;
		}
		if ((mask & AMDVM_HOST_MTRR_MASK_VALID) == 0) continue;
		if (address_bits == 0) {
			if ((base & address_mask) != 0) return -1;
			continue;
		}
		uint64_t shifted = address_bits >> 12;
		while ((shifted & 1u) == 0) shifted >>= 1;
		if ((shifted & (shifted + 1u)) != 0) return -1;
		uint64_t low_mask = address_bits == address_mask ? 0 :
		    (~address_bits & address_mask);
		if (((low_mask >> 12) & ((low_mask >> 12) + 1u)) != 0 ||
		    (base & low_mask) != 0) {
			return -1;
		}
	}
	return 0;
}

static int
amdvm_host_mtrr_fixed_type(const amdvm_host_mtrr_state_t *state,
	uint64_t address, uint8_t *type)
{
	unsigned reg, slot;
	if (address >= UINT64_C(0x100000) || !state->fixed_supported ||
	    type == NULL) return -1;
	if (address < UINT64_C(0x80000)) {
		reg = 0;
		slot = (unsigned)(address >> 16);
	} else if (address < UINT64_C(0xa0000)) {
		reg = 1;
		slot = (unsigned)((address - UINT64_C(0x80000)) >> 13);
	} else if (address < UINT64_C(0xc0000)) {
		reg = 2;
		slot = (unsigned)((address - UINT64_C(0xa0000)) >> 13);
	} else {
		reg = 3u + (unsigned)((address - UINT64_C(0xc0000)) >> 15);
		slot = (unsigned)((address >> 12) & 7u);
	}
	*type = (uint8_t)(state->fixed[reg] >> (slot * 8u));
	return amdvm_host_mtrr_type_valid(*type) ? 0 : -1;
}

int
amdvm_host_mtrr_effective_type(const amdvm_host_mtrr_state_t *state,
	uint64_t physical_address, uint8_t *type)
{
	uint64_t address_mask;
	uint8_t matched_type = 0;
	int matched = 0;
	unsigned i;
	if (type == NULL || amdvm_host_mtrr_validate(state) != 0 ||
	    physical_address >= (UINT64_C(1) << state->phys_bits)) {
		return -1;
	}
	if ((state->default_type & AMDVM_HOST_MTRR_DEFAULT_ENABLE) == 0) {
		*type = AMDVM_HOST_MTRR_UC;
		return 0;
	}
	if ((state->default_type & AMDVM_HOST_MTRR_FIXED_ENABLE) != 0 &&
	    physical_address < UINT64_C(0x100000)) {
		return amdvm_host_mtrr_fixed_type(state, physical_address, type);
	}
	address_mask = amdvm_host_mtrr_address_mask(state->phys_bits);
	for (i = 0; i < state->variable_count; ++i) {
		uint64_t mask = state->variable_mask[i];
		uint8_t candidate;
		if ((mask & AMDVM_HOST_MTRR_MASK_VALID) == 0 ||
		    (physical_address & address_mask & mask) !=
		    (state->variable_base[i] & address_mask & mask)) {
			continue;
		}
		candidate = (uint8_t)state->variable_base[i];
		if (!matched) {
			matched_type = candidate;
			matched = 1;
		} else if (!amdvm_host_mtrr_combine(matched_type, candidate,
		    &matched_type)) {
			return -1;
		}
	}
	/* APM vol. 2, 3.2.1: TOM2 force-WB changes the default only.
	 * Explicit variable MTRRs retain priority; global MTRR E was checked above.
	 * Do not infer WB merely from a high PA or from TOP_MEM2 alone. */
	uint64_t tom2_flags = AMDVM_HOST_MTRR_AMD_TOM2_ENABLE |
	    AMDVM_HOST_MTRR_AMD_TOM2_FORCE_WB;
	uint8_t default_type = (uint8_t)state->default_type;
	if (state->amd_extended &&
	    (state->amd_syscfg & tom2_flags) == tom2_flags &&
	    physical_address >= UINT64_C(0x100000000) &&
	    physical_address <
	    (state->amd_top_mem2 & AMDVM_HOST_MTRR_AMD_TOP_MEM_MASK)) {
		default_type = AMDVM_HOST_MTRR_WB;
	}
	*type = matched ? matched_type : default_type;
	return 0;
}

int
amdvm_host_mtrr_equal(const amdvm_host_mtrr_state_t *first,
	const amdvm_host_mtrr_state_t *second)
{
	unsigned i;
	if (first == NULL || second == NULL ||
	    first->default_type != second->default_type ||
	    first->amd_syscfg != second->amd_syscfg ||
	    first->amd_top_mem != second->amd_top_mem ||
	    first->amd_top_mem2 != second->amd_top_mem2 ||
	    first->amd_extended != second->amd_extended ||
	    first->phys_bits != second->phys_bits ||
	    first->variable_count != second->variable_count ||
	    first->fixed_supported != second->fixed_supported ||
	    first->reserved != second->reserved) {
		return 0;
	}
	for (i = 0; i < AMDVM_HOST_MTRR_FIXED_COUNT; ++i) {
		if (first->fixed[i] != second->fixed[i]) return 0;
	}
	for (i = 0; i < AMDVM_HOST_MTRR_VARIABLE_MAX; ++i) {
		if (first->variable_base[i] != second->variable_base[i] ||
		    first->variable_mask[i] != second->variable_mask[i]) return 0;
	}
	return 1;
}
