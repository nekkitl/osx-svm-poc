/* Regression test for stale quarantine owners after slot reuse. */
#include "../svm-kext/provider/amdvm_quarantine_owner.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

enum {
	SLOT_FREE = 0,
	SLOT_ACTIVE = 1,
};

int
main(void)
{
	uint32_t slot_state = SLOT_FREE;
	uint64_t slot_generation = 0;
	const uint64_t owner_a = 41;
	const uint64_t owner_b = 42;

	/* A owns slot 0 and releases it successfully. */
	slot_state = SLOT_ACTIVE;
	slot_generation = owner_a;
	assert(amdvm_quarantine_owner_release(&slot_state, &slot_generation,
	    SLOT_ACTIVE, SLOT_FREE, owner_a) == 0);
	assert(slot_state == SLOT_FREE && slot_generation == 0);

	/* B reuses slot 0; a repeated destroy(A) must not release B's slot. */
	slot_state = SLOT_ACTIVE;
	slot_generation = owner_b;
	assert(amdvm_quarantine_owner_release(&slot_state, &slot_generation,
	    SLOT_ACTIVE, SLOT_FREE, owner_a) == -1);
	assert(slot_state == SLOT_ACTIVE && slot_generation == owner_b);
	assert(amdvm_quarantine_owner_release(&slot_state, &slot_generation,
	    SLOT_ACTIVE, SLOT_FREE, owner_b) == 0);
	assert(slot_state == SLOT_FREE && slot_generation == 0);

	puts("amdvm quarantine stale-owner reuse regression: PASS");
	return 0;
}
