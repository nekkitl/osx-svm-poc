#ifndef AMDVM_PHYSMEM_H
#define AMDVM_PHYSMEM_H

#include <stddef.h>
#include <stdint.h>

#include "amdvm_vmcb.h"

typedef struct amdvm_physmem_region {
	void *virtual_address;
	void *memory_descriptor;
	uint64_t physical_address;
	uint64_t physical_segment_length;
	size_t size;
} amdvm_physmem_region_t;

/* A user virtual range wired by an IOMemoryDescriptor for scatter/gather NPT. */
typedef struct amdvm_pinned_user_region {
	void *memory_descriptor;
	void *owning_task;
	uint64_t user_address;
	size_t size;
} amdvm_pinned_user_region_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate wired, page-aligned, physically contiguous kernel memory. */
int amdvm_physmem_alloc_contiguous(amdvm_physmem_region_t *region,
	size_t size);
void amdvm_physmem_free(amdvm_physmem_region_t *region);

/* Validate the descriptor-backed region and resolve a contained VA to its PA. */
int amdvm_physmem_validate(const amdvm_physmem_region_t *region);
int amdvm_physmem_resolve(const amdvm_physmem_region_t *region,
	const void *virtual_address, size_t length, uint64_t *physical_address);
uint64_t amdvm_physmem_page_pa(const amdvm_physmem_region_t *region,
	size_t page_index);

/* Pin a page-aligned client range until unpin; task is the owning QEMU task. */
int amdvm_physmem_pin_user_region(amdvm_pinned_user_region_t *region,
	uint64_t user_address, size_t size, void *task);
int amdvm_physmem_user_page_pa(const amdvm_pinned_user_region_t *region,
	size_t page_index, uint8_t host_phys_bits, uint64_t *physical_address);
int amdvm_physmem_unpin_user_region(amdvm_pinned_user_region_t *region);

#ifdef __cplusplus
}
#endif

#endif
