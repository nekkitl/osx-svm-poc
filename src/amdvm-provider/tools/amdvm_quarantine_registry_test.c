#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../provider/amdvm_quarantine.h"

typedef struct FakeDescriptor {
	unsigned int failures_remaining;
	unsigned int unpin_successes;
} FakeDescriptor;

static int
fake_unpin(amdvm_pinned_user_region_t *region, void *opaque)
{
	FakeDescriptor *descriptor = region->memory_descriptor;
	(void)opaque;
	assert(descriptor != NULL);
	if (descriptor->failures_remaining != 0) {
		--descriptor->failures_remaining;
		return -1;
	}
	++descriptor->unpin_successes;
	memset(region, 0, sizeof(*region));
	return 0;
}

static void
test_orphan_retry_and_slot_accounting(void)
{
	amdvm_quarantine_registry_t registry = { 0 };
	FakeDescriptor first = { .failures_remaining = 100 };
	FakeDescriptor second = { 0 };
	amdvm_pinned_user_region_t ranges[2] = {
		{ &first, (void *)0x11, 0x1000, 0x2000 },
		{ &second, (void *)0x22, 0x4000, 0x3000 }
	};
	uint32_t slot, reserved_slots[AMDVM_QUARANTINE_MAX_SLOTS];
	uint64_t generation, reserved_generations[AMDVM_QUARANTINE_MAX_SLOTS];
	size_t contexts, range_count;
	uint64_t bytes;
	size_t i;

	assert(amdvm_quarantine_registry_reserve(&registry, &slot, &generation,
	    fake_unpin, NULL) == 0);
	assert(amdvm_quarantine_registry_transfer(&registry, slot, generation,
	    ranges, 2) == 0);
	assert(ranges[0].memory_descriptor == NULL);
	assert(ranges[1].memory_descriptor == NULL);
	assert(registry.slots[slot].ranges[0].memory_descriptor == &first);
	assert(registry.slots[slot].ranges[0].owning_task == (void *)0x11);
	assert(registry.slots[slot].ranges[0].user_address == 0x1000);
	assert(registry.slots[slot].ranges[0].size == 0x2000);
	assert(amdvm_quarantine_registry_count(&registry) == 1);
	amdvm_quarantine_registry_stats(&registry, &contexts, &range_count, &bytes);
	assert(contexts == 1 && range_count == 2 && bytes == 0x5000);

	/* Close the client while completion still fails: registry becomes owner. */
	assert(amdvm_quarantine_registry_forget(&registry, slot, generation,
	    fake_unpin, NULL) == 0);
	assert(registry.slots[slot].state == AMDVM_QUARANTINE_ORPHANED);
	assert(registry.slots[slot].owner_gone == 1);

	/* Four occupied slots are a hard resource cap; a fifth reserve fails. */
	for (i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS - 1; ++i) {
		assert(amdvm_quarantine_registry_reserve(&registry,
		    &reserved_slots[i], &reserved_generations[i],
		    fake_unpin, NULL) == 0);
	}
	assert(amdvm_quarantine_registry_reserve(&registry, &slot, &generation,
	    fake_unpin, NULL) != 0);

	/* Once complete can succeed, retry releases both records and frees orphan. */
	first.failures_remaining = 0;
	amdvm_quarantine_registry_retry(&registry, fake_unpin, NULL);
	assert(first.unpin_successes == 1);
	assert(second.unpin_successes == 1);
	assert(amdvm_quarantine_registry_count(&registry) == 0);
	amdvm_quarantine_registry_stats(&registry, &contexts, &range_count, &bytes);
	assert(contexts == 0 && range_count == 0 && bytes == 0);

	assert(amdvm_quarantine_registry_reserve(&registry, &slot, &generation,
	    fake_unpin, NULL) == 0);
	assert(amdvm_quarantine_registry_release(&registry, slot, generation) == 0);
	for (i = 0; i < AMDVM_QUARANTINE_MAX_SLOTS - 1; ++i) {
		assert(amdvm_quarantine_registry_release(&registry, reserved_slots[i],
		    reserved_generations[i]) == 0);
	}
	puts("amdvm quarantine registry: transfer, orphan retry, and slot cap PASS");
}

int
main(void)
{
	test_orphan_retry_and_slot_accounting();
	return 0;
}
