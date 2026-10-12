#ifndef AMDVM_HOST_MTRR_H
#define AMDVM_HOST_MTRR_H

#include <stdint.h>

#define AMDVM_HOST_MTRR_VARIABLE_MAX 8u
#define AMDVM_HOST_MTRR_FIXED_COUNT 11u

#define AMDVM_HOST_MTRR_UC 0u
#define AMDVM_HOST_MTRR_WC 1u
#define AMDVM_HOST_MTRR_WT 4u
#define AMDVM_HOST_MTRR_WP 5u
#define AMDVM_HOST_MTRR_WB 6u

#define AMDVM_HOST_MTRR_DEFAULT_ENABLE (1u << 11)
#define AMDVM_HOST_MTRR_FIXED_ENABLE   (1u << 10)
#define AMDVM_HOST_MTRR_MASK_VALID     (1u << 11)

#define AMDVM_HOST_MTRR_AMD_TOM2_ENABLE (UINT64_C(1) << 21)
#define AMDVM_HOST_MTRR_AMD_TOM2_FORCE_WB (UINT64_C(1) << 22)
/* APM vol. 2, 7.9.4: TOP_MEM{,2} address bits 51:23; other bits ignored. */
#define AMDVM_HOST_MTRR_AMD_TOP_MEM_MASK UINT64_C(0x000fffffff800000)

typedef struct amdvm_host_mtrr_state {
	uint64_t fixed[AMDVM_HOST_MTRR_FIXED_COUNT];
	uint64_t variable_base[AMDVM_HOST_MTRR_VARIABLE_MAX];
	uint64_t variable_mask[AMDVM_HOST_MTRR_VARIABLE_MAX];
	uint64_t default_type;
	uint64_t amd_syscfg;
	uint64_t amd_top_mem;
	uint64_t amd_top_mem2;
	uint8_t phys_bits;
	uint8_t variable_count;
	uint8_t fixed_supported;
	uint8_t amd_extended;
	uint8_t reserved;
} amdvm_host_mtrr_state_t;

int amdvm_host_mtrr_validate(const amdvm_host_mtrr_state_t *state);
int amdvm_host_mtrr_effective_type(const amdvm_host_mtrr_state_t *state,
	uint64_t physical_address, uint8_t *type);
int amdvm_host_mtrr_equal(const amdvm_host_mtrr_state_t *first,
	const amdvm_host_mtrr_state_t *second);

#endif
