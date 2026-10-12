#include "../svm-kext/provider/amdvm_npt.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct failing_setter {
	size_t calls;
	size_t fail_first;
	size_t fail_second;
} failing_setter_t;

static int
fail_selected_sets(void *opaque, amdvm_npt_t *npt, uint64_t gpa,
	uint8_t pat_index)
{
	failing_setter_t *state = (failing_setter_t *)opaque;
	++state->calls;
	if (state->calls == state->fail_first ||
	    state->calls == state->fail_second) return AMDVM_NPT_CONFLICT;
	int rc = amdvm_npt_set_4k_pat_index(npt, gpa, pat_index);
	return rc;
}

static void
assert_index(const amdvm_npt_t *npt, uint64_t gpa, uint8_t expected)
{
	uint8_t actual = 0xff;
	assert(amdvm_npt_get_4k_pat_index(npt, gpa, &actual) == AMDVM_NPT_OK);
	assert(actual == expected);
}

int
main(void)
{
	/* The observed machine has all-WB hPAT. Reset MTRRs request uniform UC;
	 * reset gPAT has no WC slot, so exact gPAT composition can represent it. */
	uint8_t types[2] = {0, 0}, indexes[2];
	uint64_t execution = UINT64_MAX;
	assert(amdvm_npt_plan_pat(UINT64_C(0x0606060606060606),
	    UINT64_C(0x0007040600070406), types, 2, indexes, &execution) == AMDVM_NPT_OK);
	assert(execution == 0 && indexes[0] == 0 && indexes[1] == 0);
	/* Uniform WB must preserve architectural UC-/WT/WP/PAT behavior. */
	types[0] = types[1] = 6;
	assert(amdvm_npt_plan_pat(UINT64_C(0x0606060606060606),
	    UINT64_C(0x0007040600070406), types, 2, indexes, &execution) == AMDVM_NPT_OK);
	assert(execution == UINT64_C(0x0007040600070406));
	/* One common gPAT cannot make the same guest WB slot both UC and WB. */
	types[0] = 0;
	assert(amdvm_npt_plan_pat(UINT64_C(0x0606060606060606),
	    UINT64_C(0x0007040600070406), types, 2, indexes, &execution) != AMDVM_NPT_OK);
	/* WC and WC+ are distinct; do not claim exact equivalence by conflating them. */
	types[0] = types[1] = 0;
	assert(amdvm_npt_plan_pat(UINT64_C(0x0606060606060606),
	    UINT64_C(0x0101010101010101), types, 2, indexes, &execution) != AMDVM_NPT_OK);
	/* A native host UC slot permits the direct plan, including a guest WC slot. */
	assert(amdvm_npt_plan_pat(UINT64_C(0x0007040600070406),
	    UINT64_C(0x0101010101010101), types, 2, indexes, &execution) == AMDVM_NPT_OK);
	assert(execution == UINT64_C(0x0101010101010101));
	assert(indexes[0] == 3 && indexes[1] == 3);
	assert(amdvm_npt_plan_pat(UINT64_C(0x0606060606060606),
	    UINT64_C(0x0202020202020202), types, 2, indexes, &execution) != AMDVM_NPT_OK);
	/* Independent APM table oracle for every uniform guest/target type pair. */
	static const uint8_t architectural_types[] = {0,7,1,5,4,6};
	static const uint8_t expected[6][6] = {
	    {0,0,0,0,0,0}, {0,7,1,0,0,0}, {1,1,1,8,8,8},
	    {0,0,0,5,0,5}, {0,0,0,0,4,4}, {0,0,1,5,4,6}
	};
	for (unsigned g = 0; g < 6; ++g) {
	    for (unsigned target = 0; target < 6; ++target) {
	        if (target == 1) continue; /* UC- is a PAT type, not a guest MTRR type. */
	        uint64_t uniform = UINT64_C(0x0101010101010101) * architectural_types[g];
	        types[0] = types[1] = architectural_types[target];
	        int plan = amdvm_npt_plan_pat(UINT64_C(0x0606060606060606),
	            uniform, types, 2, indexes, &execution);
	        if (plan == AMDVM_NPT_OK) {
	            assert(indexes[0] == 0 && indexes[1] == 0);
	            for (unsigned slot = 0; slot < 8; ++slot) {
	                unsigned candidate;
	                for (candidate = 0; candidate < 6; ++candidate)
	                    if (architectural_types[candidate] ==
	                        (uint8_t)(execution >> (slot * 8))) break;
	                assert(candidate < 6);
	                assert(expected[candidate][5] == expected[g][target]);
	            }
	        } else {
	            for (unsigned candidate = 0; candidate < 6; ++candidate)
	                assert(expected[candidate][5] != expected[g][target]);
	        }
	    }
	}
	const uint64_t guest_gpa = UINT64_C(0x200000);
	const amdvm_npt_pat_range_t range = {
		.guest_gpa = guest_gpa,
		.size = 2u * AMDVM_NPT_PAGE_SIZE,
		.pat_index = 5u,
		.reserved = {0}
	};
	uint8_t old_indexes[2] = {0xff, 0xff};
	uint8_t *pool = aligned_alloc(AMDVM_NPT_PAGE_SIZE,
	    4u * AMDVM_NPT_PAGE_SIZE);
	amdvm_npt_t npt = {0};
	failing_setter_t setter = {0};
	assert(pool != NULL);
	assert(amdvm_npt_init(&npt, pool, UINT64_C(0x100000), 4u, 48u, 48u) ==
	    AMDVM_NPT_OK);
	assert(amdvm_npt_map_4k(&npt, guest_gpa, UINT64_C(0x2000000),
	    AMDVM_NPT_READ | AMDVM_NPT_WRITE) == AMDVM_NPT_OK);
	assert(amdvm_npt_map_4k(&npt, guest_gpa + AMDVM_NPT_PAGE_SIZE,
	    UINT64_C(0x2001000), AMDVM_NPT_READ | AMDVM_NPT_WRITE) ==
	    AMDVM_NPT_OK);

	/* An apply failure restores every leaf that had already changed. */
	setter.fail_first = 2u;
	int rc = amdvm_npt_update_pat_ranges_atomic(&npt, &range, 1u, old_indexes,
	    2u, fail_selected_sets, &setter);
	assert(rc == AMDVM_NPT_CONFLICT);
	assert_index(&npt, guest_gpa, 0u);
	assert_index(&npt, guest_gpa + AMDVM_NPT_PAGE_SIZE, 0u);

	/* A failed rollback is distinguished so the owning vCPU can be poisoned. */
	setter.calls = 0;
	setter.fail_first = 2u;
	setter.fail_second = 3u;
	assert(amdvm_npt_update_pat_ranges_atomic(&npt, &range, 1u, old_indexes,
	    2u, fail_selected_sets, &setter) == AMDVM_NPT_ROLLBACK_FAILED);
	assert_index(&npt, guest_gpa, 5u);
	assert_index(&npt, guest_gpa + AMDVM_NPT_PAGE_SIZE, 0u);

	/* The high PAT bit also round-trips through ordinary leaf lookup. */
	assert(amdvm_npt_set_4k_pat_index(&npt, guest_gpa, 0u) == AMDVM_NPT_OK);
	setter.calls = 0;
	setter.fail_first = 0;
	setter.fail_second = 0;
	assert(amdvm_npt_update_pat_ranges_atomic(&npt, &range, 1u, old_indexes,
	    2u, fail_selected_sets, &setter) == AMDVM_NPT_OK);
	assert_index(&npt, guest_gpa, 5u);
	assert_index(&npt, guest_gpa + AMDVM_NPT_PAGE_SIZE, 5u);

	free(pool);
	puts("amdvm NPT PAT transaction tests: PASS");
	return 0;
}
