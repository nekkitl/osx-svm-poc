#include <assert.h>
#include <stdio.h>

#include "../svm-kext/provider/amdvm_vcpu_lifecycle.h"

int
main(void)
{
	assert(amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_READY, false, false));
	assert(amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_MEMORY_MAPPED,
	    false, false));
	assert(amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_MEMORY_MAPPED,
	    true, false));
	assert(amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_HALTED, true, true));
	assert(!amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_HALTED, true, false));
	assert(!amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_HALTED, false, true));
	assert(!amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_READY, true, false));
	assert(!amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_LEASED, true, true));
	assert(!amdvm_vcpu_state_can_run(AMDVM_VCPU_STATE_DESTROYING, true, true));
	puts("amdvm vCPU lifecycle RUN gate: PASS");
	return 0;
}
