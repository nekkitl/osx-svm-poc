#include "amdvm_physmem.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/task_ref.h>
#include <string.h>
#include <limits.h>

#if defined(__x86_64__)
static_assert(sizeof(IOByteCount) == sizeof(IOByteCount64),
	"x86_64 kext must use the kernel IOByteCount ABI");
#endif

#define AMDVM_PHYSMEM_PA_MASK UINT64_C(0x000ffffffffff000)
#define AMDVM_PHYSMEM_PAGE_MASK (AMDVM_PAGE_SIZE - 1u)

extern task_t kernel_task;

static IOBufferMemoryDescriptor *
amdvm_physmem_descriptor(const amdvm_physmem_region_t *region)
{
	return region == nullptr ? nullptr :
	    static_cast<IOBufferMemoryDescriptor *>(region->memory_descriptor);
}

extern "C" int
amdvm_physmem_validate(const amdvm_physmem_region_t *region)
{
	IOBufferMemoryDescriptor *descriptor = amdvm_physmem_descriptor(region);
	uintptr_t base;

	if (descriptor == nullptr || region->virtual_address == nullptr ||
	    region->size == 0 ||
	    ((uintptr_t)region->virtual_address & AMDVM_PHYSMEM_PAGE_MASK) != 0 ||
	    (region->physical_address & AMDVM_PHYSMEM_PAGE_MASK) != 0 ||
	    (region->physical_address & ~AMDVM_PHYSMEM_PA_MASK) != 0 ||
	    (region->size & AMDVM_PHYSMEM_PAGE_MASK) != 0 ||
	    region->physical_segment_length < region->size ||
	    descriptor->getBytesNoCopy() != region->virtual_address ||
	    descriptor->getLength() < region->size) {
		return -1;
	}
	base = (uintptr_t)region->virtual_address;
	if (region->size > UINTPTR_MAX - base ||
	    region->size - 1u > UINT64_MAX - region->physical_address ||
	    region->size - 1u > AMDVM_PHYSMEM_PA_MASK - region->physical_address) {
		return -1;
	}
	return 0;
}

extern "C" int
amdvm_physmem_resolve(const amdvm_physmem_region_t *region,
	const void *virtual_address, size_t length, uint64_t *physical_address)
{
	uintptr_t base, address;
	size_t offset;

	if (virtual_address == nullptr || physical_address == nullptr || length == 0 ||
	    amdvm_physmem_validate(region) != 0) {
		return -1;
	}
	base = (uintptr_t)region->virtual_address;
	address = (uintptr_t)virtual_address;
	if (address < base) {
		return -1;
	}
	offset = (size_t)(address - base);
	if (offset > region->size || length > region->size - offset ||
	    offset > UINT64_MAX - region->physical_address) {
		return -1;
	}
	*physical_address = region->physical_address + offset;
	if (*physical_address > AMDVM_PHYSMEM_PA_MASK ||
	    length - 1u > AMDVM_PHYSMEM_PA_MASK - *physical_address) {
		return -1;
	}
	return 0;
}

extern "C" int
amdvm_physmem_alloc_contiguous(amdvm_physmem_region_t *region, size_t size)
{
	IOBufferMemoryDescriptor *descriptor;
	IOBufferMemoryDescriptor *owned_descriptor;
	IOByteCount segment_length = 0;
	addr64_t physical_address;
	amdvm_physmem_region_t candidate = {};

	if (region == nullptr || size == 0 || size > UINT32_MAX ||
	    (size & AMDVM_PHYSMEM_PAGE_MASK) != 0) {
		return -1;
	}
	region->virtual_address = nullptr;
	region->memory_descriptor = nullptr;
	region->physical_address = 0;
	region->physical_segment_length = 0;
	region->size = 0;

	/* MapperNone makes the segment addresses host physical addresses. */
	descriptor = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIOMemoryHostPhysicallyContiguous | kIOMemoryMapperNone,
	    size, AMDVM_PAGE_SIZE);
	if (descriptor == nullptr) {
		return -1;
	}
	owned_descriptor = descriptor;
	candidate.memory_descriptor = owned_descriptor;
	candidate.virtual_address = owned_descriptor->getBytesNoCopy();
	candidate.size = size;
	physical_address = owned_descriptor->getPhysicalSegment(0,
	    &segment_length, kIOMemoryMapperNone);
	candidate.physical_address = physical_address;
	candidate.physical_segment_length = segment_length;
	if (candidate.virtual_address == nullptr || physical_address == 0 ||
	    segment_length < size ||
	    (physical_address & AMDVM_PHYSMEM_PAGE_MASK) != 0 ||
	    (physical_address & ~AMDVM_PHYSMEM_PA_MASK) != 0 ||
	    size - 1u > AMDVM_PHYSMEM_PA_MASK - physical_address) {
		if (candidate.virtual_address != nullptr) {
			memset(candidate.virtual_address, 0, size);
		}
		owned_descriptor->release();
		return -1;
	}
	memset(candidate.virtual_address, 0, size);
	if (amdvm_physmem_validate(&candidate) != 0) {
		owned_descriptor->release();
		return -1;
	}
	*region = candidate;
	return 0;
}

extern "C" void
amdvm_physmem_free(amdvm_physmem_region_t *region)
{
	IOBufferMemoryDescriptor *descriptor = amdvm_physmem_descriptor(region);
	if (descriptor == nullptr) {
		return;
	}
	if (region->virtual_address != nullptr && region->size != 0) {
		memset(region->virtual_address, 0, region->size);
	}
	descriptor->release();
	region->virtual_address = nullptr;
	region->memory_descriptor = nullptr;
	region->physical_address = 0;
	region->physical_segment_length = 0;
	region->size = 0;
}

extern "C" uint64_t
amdvm_physmem_page_pa(const amdvm_physmem_region_t *region, size_t page_index)
{
	uint64_t physical_address;
	const uint8_t *virtual_address;

	if (region == nullptr || region->memory_descriptor == nullptr ||
	    page_index >= region->size / AMDVM_PAGE_SIZE) {
		return 0;
	}
	virtual_address = (const uint8_t *)region->virtual_address +
	    page_index * AMDVM_PAGE_SIZE;
	if (amdvm_physmem_resolve(region, virtual_address, AMDVM_PAGE_SIZE,
	    &physical_address) != 0) {
		return 0;
	}
	return physical_address;
}

extern "C" int
amdvm_physmem_pin_user_region(amdvm_pinned_user_region_t *region,
	uint64_t user_address, size_t size, void *task)
{
	IOMemoryDescriptor *descriptor;
	task_t owning_task;
	if (region == nullptr || task == nullptr || user_address == 0 || size == 0 ||
	    (user_address & AMDVM_PHYSMEM_PAGE_MASK) != 0 ||
	    (size & AMDVM_PHYSMEM_PAGE_MASK) != 0 ||
	    size > UINT32_MAX || user_address > UINT64_MAX - size ||
	    region->memory_descriptor != nullptr) {
		return -1;
	}
	owning_task = (task_t)task;
	task_reference(owning_task);
	descriptor = IOMemoryDescriptor::withAddressRange(user_address, size,
	    kIODirectionInOut, owning_task);
	if (descriptor == nullptr) {
		task_deallocate(owning_task);
		return -1;
	}
	if (descriptor->getLength() != size ||
	    descriptor->prepare(kIODirectionInOut) != kIOReturnSuccess) {
		descriptor->release();
		task_deallocate(owning_task);
		return -1;
	}
	region->memory_descriptor = descriptor;
	region->owning_task = owning_task;
	region->user_address = user_address;
	region->size = size;
	return 0;
}

extern "C" int
amdvm_physmem_user_page_pa(const amdvm_pinned_user_region_t *region,
	size_t page_index, uint8_t host_phys_bits, uint64_t *physical_address)
{
	IOMemoryDescriptor *descriptor = region == nullptr ? nullptr :
	    static_cast<IOMemoryDescriptor *>(region->memory_descriptor);
	IOByteCount segment_length = 0;
	uint64_t address, physical_limit;
	if (descriptor == nullptr || physical_address == nullptr ||
	    host_phys_bits < 32 || host_phys_bits > 52 ||
	    page_index >= region->size / AMDVM_PAGE_SIZE) {
		return -1;
	}
	address = descriptor->getPhysicalSegment(
	    (IOByteCount)(page_index * AMDVM_PAGE_SIZE), &segment_length,
	    kIOMemoryMapperNone);
	physical_limit = UINT64_C(1) << host_phys_bits;
	if (address == 0 || (address & AMDVM_PHYSMEM_PAGE_MASK) != 0 ||
	    segment_length < AMDVM_PAGE_SIZE || address >= physical_limit ||
	    physical_limit - address < AMDVM_PAGE_SIZE ||
	    (address & ~AMDVM_PHYSMEM_PA_MASK) != 0) {
		return -1;
	}
	*physical_address = address;
	return 0;
}

extern "C" int
amdvm_physmem_unpin_user_region(amdvm_pinned_user_region_t *region)
{
	IOMemoryDescriptor *descriptor = region == nullptr ? nullptr :
	    static_cast<IOMemoryDescriptor *>(region->memory_descriptor);
	if (descriptor == nullptr) {
		return region == nullptr ? -1 : 0;
	}
	if (descriptor->complete(kIODirectionInOut) != kIOReturnSuccess) {
		/*
		 * Completion state is unknown: retain the descriptor and all of its
		 * metadata. The caller must quarantine this reference and prohibit RUN;
		 * this return value alone does not prove that pages remain wired.
		 */
		return -1;
	}
	descriptor->release();
	if (region->owning_task != nullptr) {
		task_deallocate((task_t)region->owning_task);
	}
	region->memory_descriptor = nullptr;
	region->owning_task = nullptr;
	region->user_address = 0;
	region->size = 0;
	return 0;
}
