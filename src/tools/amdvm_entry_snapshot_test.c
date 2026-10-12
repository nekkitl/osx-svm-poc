/* Offline production-path test: stage QEMU RUN, walk real NPT bytes, snapshot. */
#include "../svm-kext/provider/amdvm_vmcb.h"
#include "../svm-kext/provider/amdvm_npt.h"
#include "../svm-kext/provider/amdvm_vmrun.h"
#include "../svm-kext/amdvm_probe_abi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
amdvm_vmrun_validate_guest_xstate(const void *area, size_t size,
	uint64_t xcr0)
{
	(void)area;
	(void)size;
	(void)xcr0;
	return -1; /* This test uses the fixed legacy microguest request. */
}

int
main(void)
{
	const uint8_t rom[] = {
		AMDVM_VCPU_TEST_ROM_CPUID_0, AMDVM_VCPU_TEST_ROM_CPUID_1,
		AMDVM_VCPU_TEST_ROM_MOV_AL_IMM8, AMDVM_VCPU_TEST_ROM_DEBUG_BYTE,
		AMDVM_VCPU_TEST_ROM_OUT_IMM8_AL, AMDVM_VCPU_TEST_ROM_DEBUG_PORT,
		AMDVM_VCPU_TEST_ROM_HLT
	};
	const uint64_t npt_pa = UINT64_C(0x300000);
	const uint64_t backing_pa = UINT64_C(0x500000);
	amdvm_vmcb_t *vmcb = aligned_alloc(AMDVM_PAGE_SIZE, sizeof(*vmcb));
	uint8_t *npt_pool = aligned_alloc(AMDVM_NPT_PAGE_SIZE,
	    8u * AMDVM_NPT_PAGE_SIZE);
	uint8_t *backing = aligned_alloc(AMDVM_NPT_PAGE_SIZE, AMDVM_NPT_PAGE_SIZE);
	void *guest_xstate = aligned_alloc(AMDVM_NPT_PAGE_SIZE,
	    AMDVM_X86_XSTATE_MAX_SIZE);
	amdvm_npt_t npt = {0};
	amdvm_vcpu_run_request_t request = {0};
	amdvm_vcpu_entry_snapshot_t snapshot = {0};
	uint64_t gprs[15] = {0};
	uint64_t *xstate_bv = (uint64_t *)((uint8_t *)guest_xstate + 512u);
	assert(vmcb != NULL && npt_pool != NULL && backing != NULL &&
	    guest_xstate != NULL);
	memset(backing, 0, AMDVM_NPT_PAGE_SIZE);
	memcpy(backing + AMDVM_VCPU_RESET_ROM_OFFSET, rom, sizeof(rom));
	memset(guest_xstate, 0, AMDVM_X86_XSTATE_MAX_SIZE);
	memset((uint8_t *)guest_xstate, 0x3c, 512u);
	*xstate_bv = 7;
	assert(amdvm_npt_init(&npt, npt_pool, npt_pa, 8u, 48u, 48u) ==
	    AMDVM_NPT_OK);
	assert(amdvm_npt_map_4k(&npt, AMDVM_VCPU_RESET_ROM_GPA, backing_pa,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK);
	assert(amdvm_vmcb_configure_probe(vmcb, npt_pa, UINT64_C(0x400000),
	    UINT64_C(0x403000), 1u) == 0);
	assert(amdvm_vmcb_configure_qemu_reset_vector_v1(vmcb) == 0);
	request.rip = AMDVM_VCPU_RESET_VECTOR_RIP;
	request.rflags = 2;
	request.gpr[0] = 0; /* CPUID leaf 0 */
	request.gpr[1] = UINT64_C(0x11111111); /* CPUID input RCX */
	assert(amdvm_vmcb_stage_run_request(vmcb, gprs, guest_xstate,
	    &request, 0) == 0);
	assert(amdvm_vmcb_capture_entry_snapshot(vmcb, &npt,
	    AMDVM_VCPU_RESET_ROM_GPA, AMDVM_VCPU_RESET_ROM_OFFSET,
	    backing, backing_pa, AMDVM_NPT_PAGE_SIZE, gprs, 7, 0, 0,
	    &snapshot) == 0);
	assert(snapshot.guest_physical_address == AMDVM_VCPU_RESET_VECTOR_LINEAR);
	assert(snapshot.host_physical_address ==
	    backing_pa + AMDVM_VCPU_RESET_ROM_OFFSET);
	assert(memcmp(snapshot.entry_bytes, rom, sizeof(rom)) == 0);
	assert(snapshot.rip == AMDVM_VCPU_RESET_VECTOR_RIP);
	assert(snapshot.cs_selector == 0xf000u && snapshot.cs_base ==
	    AMDVM_VCPU_RESET_CS_BASE && snapshot.cs_limit == 0xffffu);
	assert(snapshot.cr0 == 0x60000010ull && snapshot.cr3 == 0 &&
	    snapshot.cr4 == 0 && snapshot.efer == AMDVM_EFER_SVME);
	if ((snapshot.flags & (AMDVM_VCPU_ENTRY_TRANSLATION_VALID |
	    AMDVM_VCPU_ENTRY_BACKING_BYTES_VALID | AMDVM_VCPU_ENTRY_CPUID_INTERCEPT |
	    AMDVM_VCPU_ENTRY_NPT_ENABLED)) !=
	    (AMDVM_VCPU_ENTRY_TRANSLATION_VALID |
	    AMDVM_VCPU_ENTRY_BACKING_BYTES_VALID | AMDVM_VCPU_ENTRY_CPUID_INTERCEPT |
	    AMDVM_VCPU_ENTRY_NPT_ENABLED)) {
		fprintf(stderr, "entry flags=0x%x nested=0x%x misc1=0x%x\n",
		    snapshot.flags, snapshot.nested_paging_control,
		    snapshot.intercept_misc1);
		abort();
	}
	assert(snapshot.event_injection == 0);
	assert(snapshot.exit_code_sentinel == AMDVM_SVM_EXIT_NOT_RUN);
	assert(snapshot.host_xcr0 == 7 && snapshot.guest_xcr0 == 0);
	assert(snapshot.gpr[0] == request.gpr[0] &&
	    snapshot.gpr[1] == request.gpr[1]);
	puts("PASS: production RUN staging snapshot matches the actual NPT leaf, backing bytes at reset offset 0xff0, CPUID intercept, reset VMCB state and pre-entry sentinels; no VMRUN executed.");
	free(guest_xstate);
	free(backing);
	free(npt_pool);
	free(vmcb);
	return 0;
}
