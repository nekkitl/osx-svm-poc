#include "amdvm_quarantine.h"
#include "amdvm_quarantine_owner.h"

#include <string.h>

static void
amdvm_quarantine_lock(amdvm_quarantine_registry_t *registry)
{
	while (__sync_lock_test_and_set(&registry->lock, 1u) != 0) {
		while (registry->lock != 0) {
			__asm__ volatile("pause");
		}
	}
}

static void
amdvm_quarantine_unlock(amdvm_quarantine_registry_t *registry)
{
	__sync_lock_release(&registry->lock);
}

static int
amdvm_quarantine_slot_counted(uint32_t state)
{
	return state == AMDVM_QUARANTINE_FAILED ||
	    state == AMDVM_QUARANTINE_RETRYING ||
	    state == AMDVM_QUARANTINE_ORPHANED ||
	    state == AMDVM_QUARANTINE_RETRYING_ORPHAN;
}

void
amdvm_quarantine_registry_retry(amdvm_quarantine_registry_t *registry,
	amdvm_quarantine_unpin_fn unpin, void *opaque)
{
	size_t slot_index;
	if (registry == NULL || unpin == NULL) return;
	for (slot_index = 0; slot_index < AMDVM_QUARANTINE_MAX_SLOTS;
	    ++slot_index) {
		amdvm_quarantine_slot_t *slot = &registry->slots[slot_index];
		size_t i;
		int failed = 0;
		int was_orphaned;

		amdvm_quarantine_lock(registry);
		if (slot->state != AMDVM_QUARANTINE_FAILED &&
		    slot->state != AMDVM_QUARANTINE_ORPHANED) {
			amdvm_quarantine_unlock(registry);
			continue;
		}
		was_orphaned = slot->state == AMDVM_QUARANTINE_ORPHANED;
		slot->state = was_orphaned ? AMDVM_QUARANTINE_RETRYING_ORPHAN :
		    AMDVM_QUARANTINE_RETRYING;
		amdvm_quarantine_unlock(registry);

		/* Descriptor operations must never run under the registry spinlock. */
		for (i = 0; i < slot->range_count; ++i) {
			amdvm_pinned_user_region_t candidate;
			void *descriptor;
			amdvm_quarantine_lock(registry);
			candidate = slot->ranges[i];
			amdvm_quarantine_unlock(registry);
			if (candidate.memory_descriptor == NULL) continue;
			descriptor = candidate.memory_descriptor;
			if (unpin(&candidate, opaque) != 0) {
				failed = 1;
				break;
			}
			amdvm_quarantine_lock(registry);
			if (slot->ranges[i].memory_descriptor == descriptor) {
				memset(&slot->ranges[i], 0, sizeof(slot->ranges[i]));
			}
			amdvm_quarantine_unlock(registry);
		}

		amdvm_quarantine_lock(registry);
		if (failed) {
			slot->state = slot->owner_gone ? AMDVM_QUARANTINE_ORPHANED :
			    AMDVM_QUARANTINE_FAILED;
		} else if (slot->owner_gone) {
			memset(slot, 0, sizeof(*slot));
		} else {
			slot->range_count = 0;
			slot->state = AMDVM_QUARANTINE_CLEANED;
		}
		amdvm_quarantine_unlock(registry);
	}
}

int
amdvm_quarantine_registry_reserve(amdvm_quarantine_registry_t *registry,
	uint32_t *slot_index, uint64_t *generation,
	amdvm_quarantine_unpin_fn unpin, void *opaque)
{
	size_t i;
	if (registry == NULL || slot_index == NULL || generation == NULL ||
	    unpin == NULL) return -1;
	amdvm_quarantine_registry_retry(registry, unpin, opaque);
	amdvm_quarantine_lock(registry);
	for (i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS; ++i) {
		if (registry->slots[i].state == AMDVM_QUARANTINE_FREE) {
			if (++registry->generation == 0) ++registry->generation;
			registry->slots[i].state = AMDVM_QUARANTINE_ACTIVE;
			registry->slots[i].generation = registry->generation;
			*slot_index = (uint32_t)i;
			*generation = registry->generation;
			amdvm_quarantine_unlock(registry);
			return 0;
		}
	}
	amdvm_quarantine_unlock(registry);
	return -1;
}

int
amdvm_quarantine_registry_release(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation)
{
	if (registry == NULL || slot_index >= AMDVM_QUARANTINE_MAX_SLOTS) {
		return -1;
	}
	amdvm_quarantine_lock(registry);
	if (amdvm_quarantine_owner_release(&registry->slots[slot_index].state,
	    &registry->slots[slot_index].generation, AMDVM_QUARANTINE_ACTIVE,
	    AMDVM_QUARANTINE_FREE, generation) != 0) {
		amdvm_quarantine_unlock(registry);
		return -1;
	}
	memset(&registry->slots[slot_index], 0,
	    sizeof(registry->slots[slot_index]));
	amdvm_quarantine_unlock(registry);
	return 0;
}

int
amdvm_quarantine_registry_recover(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation,
	amdvm_quarantine_unpin_fn unpin, void *opaque)
{
	if (registry == NULL || slot_index >= AMDVM_QUARANTINE_MAX_SLOTS ||
	    generation == 0 || unpin == NULL) return -1;
	amdvm_quarantine_registry_retry(registry, unpin, opaque);
	amdvm_quarantine_lock(registry);
	if (!amdvm_quarantine_owner_matches(registry->slots[slot_index].state,
	    registry->slots[slot_index].generation, AMDVM_QUARANTINE_CLEANED,
	    generation)) {
		amdvm_quarantine_unlock(registry);
		return -1;
	}
	memset(&registry->slots[slot_index], 0,
	    sizeof(registry->slots[slot_index]));
	amdvm_quarantine_unlock(registry);
	return 0;
}

int
amdvm_quarantine_registry_transfer(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation,
	amdvm_pinned_user_region_t *ranges, size_t range_count)
{
	amdvm_quarantine_slot_t *slot;
	size_t i, pending = 0;
	if (registry == NULL || ranges == NULL ||
	    slot_index >= AMDVM_QUARANTINE_MAX_SLOTS ||
	    range_count > AMDVM_QUARANTINE_MAX_RANGES) return -1;
	for (i = 0; i < range_count; ++i) {
		pending += ranges[i].memory_descriptor != NULL;
	}
	slot = &registry->slots[slot_index];
	amdvm_quarantine_lock(registry);
	if (!amdvm_quarantine_owner_matches(slot->state, slot->generation,
	    AMDVM_QUARANTINE_ACTIVE, generation) ||
	    slot->range_count > AMDVM_QUARANTINE_MAX_RANGES ||
	    pending > AMDVM_QUARANTINE_MAX_RANGES - slot->range_count) {
		amdvm_quarantine_unlock(registry);
		return -1;
	}
	for (i = 0; i < range_count; ++i) {
		if (ranges[i].memory_descriptor != NULL) {
			slot->ranges[slot->range_count++] = ranges[i];
			memset(&ranges[i], 0, sizeof(ranges[i]));
		}
	}
	slot->owner_gone = 0;
	slot->state = AMDVM_QUARANTINE_FAILED;
	amdvm_quarantine_unlock(registry);
	return 0;
}

int
amdvm_quarantine_registry_forget(amdvm_quarantine_registry_t *registry,
	uint32_t slot_index, uint64_t generation,
	amdvm_quarantine_unpin_fn unpin, void *opaque)
{
	amdvm_quarantine_slot_t *slot;
	if (registry == NULL || slot_index >= AMDVM_QUARANTINE_MAX_SLOTS ||
	    generation == 0 || unpin == NULL) return -1;
	slot = &registry->slots[slot_index];
	amdvm_quarantine_registry_retry(registry, unpin, opaque);
	amdvm_quarantine_lock(registry);
	if (slot->generation != generation) {
		amdvm_quarantine_unlock(registry);
		return -1;
	}
	if (slot->state == AMDVM_QUARANTINE_CLEANED) {
		slot->owner_gone = 1;
		memset(slot, 0, sizeof(*slot));
	} else if (slot->state == AMDVM_QUARANTINE_FAILED) {
		slot->state = AMDVM_QUARANTINE_ORPHANED;
		slot->owner_gone = 1;
	} else if (slot->state == AMDVM_QUARANTINE_ORPHANED ||
	    slot->state == AMDVM_QUARANTINE_RETRYING ||
	    slot->state == AMDVM_QUARANTINE_RETRYING_ORPHAN) {
		/* In-flight retry observes owner_gone before choosing final state. */
		slot->owner_gone = 1;
	} else {
		amdvm_quarantine_unlock(registry);
		return -1;
	}
	amdvm_quarantine_unlock(registry);
	return 0;
}

size_t
amdvm_quarantine_registry_count(amdvm_quarantine_registry_t *registry)
{
	size_t i, count = 0;
	if (registry == NULL) return 0;
	amdvm_quarantine_lock(registry);
	for (i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS; ++i) {
		if (amdvm_quarantine_slot_counted(registry->slots[i].state)) {
			++count;
		}
	}
	amdvm_quarantine_unlock(registry);
	return count;
}

void
amdvm_quarantine_registry_stats(amdvm_quarantine_registry_t *registry,
	size_t *context_count, size_t *range_count, uint64_t *guest_bytes)
{
	size_t i, contexts = 0, ranges = 0;
	uint64_t bytes = 0;
	if (registry == NULL) return;
	amdvm_quarantine_lock(registry);
	for (i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS; ++i) {
		amdvm_quarantine_slot_t *slot = &registry->slots[i];
		size_t j;
		if (!amdvm_quarantine_slot_counted(slot->state)) continue;
		++contexts;
		for (j = 0; j < slot->range_count; ++j) {
			if (slot->ranges[j].memory_descriptor != NULL) {
				++ranges;
				bytes = slot->ranges[j].size > UINT64_MAX - bytes ?
				    UINT64_MAX : bytes + slot->ranges[j].size;
			}
		}
	}
	amdvm_quarantine_unlock(registry);
	if (context_count != NULL) *context_count = contexts;
	if (range_count != NULL) *range_count = ranges;
	if (guest_bytes != NULL) *guest_bytes = bytes;
}
