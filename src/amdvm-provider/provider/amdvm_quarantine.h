#ifndef AMDVM_QUARANTINE_H
#define AMDVM_QUARANTINE_H

#include <stddef.h>
#include <stdint.h>

#include "../amdvm_probe_abi.h"
#include "amdvm_physmem.h"

#define AMDVM_QUARANTINE_MAX_SLOTS 4u
#define AMDVM_QUARANTINE_MAX_RANGES AMDVM_VCPU_MEMORY_MAX_RANGES

enum amdvm_quarantine_state {
	AMDVM_QUARANTINE_FREE = 0,
	AMDVM_QUARANTINE_ACTIVE = 1,
	AMDVM_QUARANTINE_FAILED = 2,
	AMDVM_QUARANTINE_RETRYING = 3,
	AMDVM_QUARANTINE_CLEANED = 4,
	AMDVM_QUARANTINE_ORPHANED = 5,
	AMDVM_QUARANTINE_RETRYING_ORPHAN = 6
};

typedef struct amdvm_quarantine_slot {
	uint32_t state;
	uint32_t owner_gone;
	uint64_t generation;
	size_t range_count;
	amdvm_pinned_user_region_t ranges[AMDVM_QUARANTINE_MAX_RANGES];
} amdvm_quarantine_slot_t;

typedef struct amdvm_quarantine_registry {
	volatile uint32_t lock;
	uint64_t generation;
	amdvm_quarantine_slot_t slots[AMDVM_QUARANTINE_MAX_SLOTS];
} amdvm_quarantine_registry_t;

typedef int (*amdvm_quarantine_unpin_fn)(
	amdvm_pinned_user_region_t *region, void *opaque);

void amdvm_quarantine_registry_retry(amdvm_quarantine_registry_t *registry,
	amdvm_quarantine_unpin_fn unpin, void *opaque);
int amdvm_quarantine_registry_reserve(amdvm_quarantine_registry_t *registry,
	uint32_t *slot_index, uint64_t *generation,
	amdvm_quarantine_unpin_fn unpin, void *opaque);
int amdvm_quarantine_registry_release(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation);
int amdvm_quarantine_registry_recover(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation,
	amdvm_quarantine_unpin_fn unpin, void *opaque);
int amdvm_quarantine_registry_transfer(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation,
	amdvm_pinned_user_region_t *ranges, size_t range_count);
int amdvm_quarantine_registry_forget(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation,
	amdvm_quarantine_unpin_fn unpin, void *opaque);
size_t amdvm_quarantine_registry_count(amdvm_quarantine_registry_t *registry);
void amdvm_quarantine_registry_stats(amdvm_quarantine_registry_t *registry,
	size_t *context_count, size_t *range_count, uint64_t *guest_bytes);

#endif
