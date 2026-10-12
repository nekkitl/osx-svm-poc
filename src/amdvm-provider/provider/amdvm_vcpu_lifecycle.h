#ifndef AMDVM_VCPU_LIFECYCLE_H
#define AMDVM_VCPU_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

enum amdvm_vcpu_lifecycle_state {
	AMDVM_VCPU_STATE_EMPTY = 0,
	AMDVM_VCPU_STATE_READY = 1,
	AMDVM_VCPU_STATE_RUNNING = 2,
	AMDVM_VCPU_STATE_LEASED = 3,
	AMDVM_VCPU_STATE_DESTROYING = 4,
	AMDVM_VCPU_STATE_HALTED = 5,
	AMDVM_VCPU_STATE_FAILED = 6,
	AMDVM_VCPU_STATE_MEMORY_MAPPED = 7
};

/*
 * A mapped context can run repeatedly. Resuming a halted full-state vCPU
 * requires an event to wake HLT; legacy fixed-test requests cannot resume it.
 */
static inline bool
amdvm_vcpu_state_can_run(uint32_t state, bool full_state, bool event_pending)
{
	if (full_state) {
		return state == AMDVM_VCPU_STATE_MEMORY_MAPPED ||
		    (state == AMDVM_VCPU_STATE_HALTED && event_pending);
	}
	return state == AMDVM_VCPU_STATE_READY ||
	    state == AMDVM_VCPU_STATE_MEMORY_MAPPED;
}

#endif
