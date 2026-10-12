#include "../svm-kext/provider/amdvm_vmcb.h"
#include "../svm-kext/amdvm_probe_abi.h"

#include <assert.h>
#include <stdint.h>

int
main(void)
{
	amdvm_vmcb_t vmcb;
	uint32_t external_irq = AMDVM_VCPU_EVENTINJ_VALID | 0x40u;
	uint32_t guest_gp = AMDVM_VCPU_EVENTINJ_VALID |
	    AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
	    AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP;
	uint64_t encoded;

	assert(amdvm_vmcb_validate_event_injection(0, 0) == 0);
	assert(amdvm_vmcb_validate_event_injection(0, 1) != 0);
	assert(amdvm_vmcb_validate_event_injection(external_irq, 0) == 0);
	assert(amdvm_vmcb_validate_event_injection(guest_gp, 0) == 0);

	assert(amdvm_vmcb_validate_event_injection(external_irq, 1) != 0);
	assert(amdvm_vmcb_validate_event_injection(
	    external_irq | AMDVM_VCPU_EVENTINJ_ERROR_VALID, 0) != 0);
	assert(amdvm_vmcb_validate_event_injection(
	    guest_gp & ~AMDVM_VCPU_EVENTINJ_ERROR_VALID, 0) != 0);
	assert(amdvm_vmcb_validate_event_injection(
	    guest_gp, 1) != 0);
	assert(amdvm_vmcb_validate_event_injection(
	    AMDVM_VCPU_EVENTINJ_VALID |
	    AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION | 6u, 0) != 0);
	assert(amdvm_vmcb_validate_event_injection(
	    guest_gp | (1u << 12), 0) != 0);
	assert(amdvm_vmcb_validate_event_injection(
	    AMDVM_VCPU_EVENTINJ_VALID | (2u << AMDVM_VCPU_EVENTINJ_TYPE_SHIFT) |
	    2u, 0) != 0);

	amdvm_vmcb_clear(&vmcb);
	encoded = ((uint64_t)0 << 32) | guest_gp;
	amdvm_vmcb_set_u64(&vmcb, AMDVM_VMCB_EVENT_INJECTION, encoded);
	assert(amdvm_vmcb_get_u64(&vmcb, AMDVM_VMCB_EVENT_INJECTION) == encoded);

	return 0;
}
