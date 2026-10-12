#include "../qemu/target/i386/amdsvm/amdsvm-event.h"

#include <assert.h>
#include <string.h>

int
main(void)
{
	AMDVMEventState pending = { 0 };
	amdvm_vcpu_run_request_t run;
	uint32_t gp = AMDVM_VCPU_EVENTINJ_VALID |
	    AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION |
	    AMDVM_VCPU_EVENTINJ_ERROR_VALID | AMDVM_VCPU_EXCEPTION_GP;
	uint32_t irq = AMDVM_VCPU_EVENTINJ_VALID | 0x41u;

	assert(amdsvm_event_queue_exception(&pending, gp, 0) == 0);
	assert(amdsvm_event_queue_exception(&pending, gp, 0) != 0);
	pending.irq_pending = true;
	pending.irq_vector = 0x41u;

	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.event_injection == gp);
	assert(run.event_injection_error_code == 0);

	/* Hardware reports the same event in EXITINTINFO: retain and retry it. */
	assert(amdsvm_event_complete(&pending, gp) == 0);
	assert(pending.exception_pending);
	assert(pending.irq_pending);
	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.event_injection == gp);

	/* No EXITINTINFO means #GP was consumed; the IRQ remains queued. */
	assert(amdsvm_event_complete(&pending, 0) == 0);
	assert(!pending.exception_pending);
	assert(pending.irq_pending);
	memset(&run, 0, sizeof(run));
	amdsvm_event_prepare(&pending, &run);
	assert(run.event_injection == irq);

	/* A mismatched interrupted event fails closed and retains the IRQ. */
	assert(amdsvm_event_complete(&pending,
	    AMDVM_VCPU_EVENTINJ_VALID | 0x42u) != 0);
	assert(pending.irq_pending);
	assert(amdsvm_event_complete(&pending, irq) == 0);
	assert(pending.irq_pending);
	assert(amdsvm_event_complete(&pending, 0) == 0);
	assert(!pending.irq_pending);

	/* EXITINTINFO without a valid event, or with no queued event, is invalid. */
	assert(amdsvm_event_complete(&pending, 1u) != 0);
	assert(amdsvm_event_complete(&pending, irq) != 0);

	return 0;
}
