#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task_ref.h>
#include "../provider/amdvm_physmem.h"

static unsigned int gTaskReferences;
static unsigned int gTaskReleases;
static unsigned int gDescriptorReleases;

task_t kernel_task = reinterpret_cast<task_t>(0x1);

void task_reference(task_t task)
{
	assert(task != nullptr);
	++gTaskReferences;
}

void task_deallocate(task_t task)
{
	assert(task != nullptr);
	++gTaskReleases;
}

class FaultInjectDescriptor : public IOMemoryDescriptor {
public:
	unsigned int complete_calls = 0;
	IOReturn complete(IODirection) override
	{
		++complete_calls;
		return complete_calls == 1 ? -1 : kIOReturnSuccess;
	}
	void release() override
	{
		++gDescriptorReleases;
	}
};

IOReturn IOMemoryDescriptor::complete(IODirection) { return kIOReturnSuccess; }
IOReturn IOMemoryDescriptor::prepare(IODirection) { return kIOReturnSuccess; }
addr64_t IOMemoryDescriptor::getPhysicalSegment(IOByteCount,
	IOByteCount *length, uint32_t)
{
	if (length) *length = 4096;
	return 0x1000;
}
IOByteCount IOMemoryDescriptor::getLength() { return 4096; }
void IOMemoryDescriptor::release() { ++gDescriptorReleases; }
IOMemoryDescriptor *IOMemoryDescriptor::withAddressRange(uint64_t, size_t,
	IODirection, void *)
{
	return nullptr;
}
IOBufferMemoryDescriptor *IOBufferMemoryDescriptor::inTaskWithOptions(void *,
	uint32_t, size_t, size_t)
{
	return nullptr;
}
void *IOBufferMemoryDescriptor::getBytesNoCopy() { return nullptr; }

int main()
{
	FaultInjectDescriptor descriptor;
	int task_token;
	amdvm_pinned_user_region_t region = {
		&descriptor, &task_token, 0x4000, 0x2000
	};

	assert(amdvm_physmem_unpin_user_region(&region) == -1);
	assert(descriptor.complete_calls == 1);
	assert(region.memory_descriptor == &descriptor);
	assert(region.owning_task == &task_token);
	assert(region.user_address == 0x4000);
	assert(region.size == 0x2000);
	assert(gDescriptorReleases == 0);
	assert(gTaskReleases == 0);

	assert(amdvm_physmem_unpin_user_region(&region) == 0);
	assert(descriptor.complete_calls == 2);
	assert(region.memory_descriptor == nullptr);
	assert(region.owning_task == nullptr);
	assert(region.user_address == 0);
	assert(region.size == 0);
	assert(gDescriptorReleases == 1);
	assert(gTaskReleases == 1);
	puts("amdvm_physmem_unpin_test: failed complete retains ownership; retry releases once");
	return 0;
}
