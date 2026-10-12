#ifndef AMDVM_QUARANTINE_OWNER_H
#define AMDVM_QUARANTINE_OWNER_H

#include <stdint.h>

/*
 * Shared ownership check for the quarantine registry and its userspace
 * regression test. The caller must hold the registry lock while applying a
 * successful release to the full slot.
 */
static inline int
amdvm_quarantine_owner_matches(uint32_t state, uint64_t slot_generation,
	uint32_t expected_state, uint64_t owner_generation)
{
	return owner_generation != 0 && state == expected_state &&
	    slot_generation == owner_generation;
}

static inline int
amdvm_quarantine_owner_release(uint32_t *state, uint64_t *slot_generation,
	uint32_t expected_state, uint32_t free_state,
	uint64_t owner_generation)
{
	if (state == 0 || slot_generation == 0 ||
	    !amdvm_quarantine_owner_matches(*state, *slot_generation,
	    expected_state, owner_generation)) {
		return -1;
	}
	*state = free_state;
	*slot_generation = 0;
	return 0;
}

#endif /* AMDVM_QUARANTINE_OWNER_H */
