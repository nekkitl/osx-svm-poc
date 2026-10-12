#ifndef AMDVM_HLT_CONTEXT_H
#define AMDVM_HLT_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#include "amdvm_npt.h"
#include "amdvm_host_mtrr.h"
#include "amdvm_physmem.h"
#include "amdvm_quarantine.h"
#include "amdvm_svm_owner.h"
#include "amdvm_vmrun.h"
#include "../amdvm_probe_abi.h"

#define AMDVM_HLT_CONTEXT_QUARANTINE_SLOTS AMDVM_QUARANTINE_MAX_SLOTS

#define AMDVM_HLT_CONTEXT_PAGES 11u
#define AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES AMDVM_VCPU_MEMORY_MAX_RANGES

/*
 * Kernel-owned, fixed one-page smoke guest. Initialize the structure to zero
 * before calling init; never pass any of these buffers or mappings from user
 * space. Init prepares resources only. Run is compile-time gated before SVM
 * MSR changes; bypassing that guard does not prove the transition safe.
 */
typedef struct amdvm_hlt_context {
	amdvm_physmem_region_t backing;
	amdvm_physmem_region_t npt_backing;
	amdvm_physmem_region_t hsave;
	amdvm_npt_t npt;
	amdvm_svm_lease_t lease;
	amdvm_pinned_user_region_t guest_ranges[AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES];
	uint64_t guest_range_gpa[AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES];
	uint64_t guest_range_size[AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES];
	uint32_t guest_range_flags[AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES];
	amdvm_vmcb_t *host_vmcb;
	amdvm_vmcb_t *guest_vmcb;
	amdvm_guest_gprs_t *guest_gprs;
	void *host_xstate;
	void *guest_xstate;
	uint8_t *guest_payload;
	uint64_t host_vmcb_pa;
	uint64_t guest_vmcb_pa;
	uint64_t host_xstate_pa;
	uint64_t guest_xstate_pa;
	uint64_t guest_payload_pa;
	uint64_t guest_payload_gpa;
	uint64_t hsave_pa;
	uint64_t guest_page_count;
	size_t guest_range_count;
	size_t guest_payload_offset;
	uint64_t initial_rip;
	uint64_t host_pat_msr;
	uint8_t memory_types[AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES];
	uint8_t memory_pat_indexes[AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES];
	uint32_t memory_type_count;
	uint64_t architectural_guest_pat;
	uint32_t execution_pat_active;
	amdvm_host_mtrr_state_t host_mtrr;
	uint32_t guest_memory_mapped;
	uint32_t host_pat_valid;
	uint32_t host_mtrr_valid;
	uint32_t failed;
	uint32_t quarantine_slot;
	uint64_t quarantine_generation;
	uint32_t quarantined;
	uint32_t initialized;
	volatile uint32_t busy;
} amdvm_hlt_context_t;

enum amdvm_hlt_context_status {
	AMDVM_HLT_CONTEXT_OK = 0,
	AMDVM_HLT_CONTEXT_INVALID = -1,
	AMDVM_HLT_CONTEXT_ALLOC_FAILED = -2,
	AMDVM_HLT_CONTEXT_NPT_FAILED = -3,
	AMDVM_HLT_CONTEXT_VMCB_FAILED = -4,
	AMDVM_HLT_CONTEXT_SVM_ACQUIRE_FAILED = -5,
	AMDVM_HLT_CONTEXT_SVM_RELEASE_FAILED = -6,
	AMDVM_HLT_CONTEXT_LEASE_ACTIVE = -7,
	AMDVM_HLT_CONTEXT_RUN_GATED = -8,
	AMDVM_HLT_CONTEXT_UNEXPECTED_EXIT = -9,
	AMDVM_HLT_CONTEXT_INVALID_GUEST = -10,
	AMDVM_HLT_CONTEXT_VMRUN_FAILED = -11,
	AMDVM_HLT_CONTEXT_FPU_PREFLIGHT_REQUIRED = -12,
	AMDVM_HLT_CONTEXT_UNPIN_FAILED = -13,
	AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED = -14,
	AMDVM_HLT_CONTEXT_NPT_ROLLBACK_FAILED = -15
};

#ifdef __cplusplus
extern "C" {
#endif

int amdvm_hlt_context_init(amdvm_hlt_context_t *context);
int amdvm_hlt_context_prepare_guest_pat(amdvm_hlt_context_t *context);
void amdvm_hlt_context_restore_guest_pat(amdvm_hlt_context_t *context);
int amdvm_hlt_context_init_qemu_reset_vector_v1(
	amdvm_hlt_context_t *context);
int amdvm_hlt_context_map_user_ranges(amdvm_hlt_context_t *context,
	const amdvm_vcpu_memory_range_t *ranges, size_t range_count, void *task,
	size_t *npt_page_count);
int amdvm_hlt_context_set_memory_types(amdvm_hlt_context_t *context,
	const amdvm_vcpu_set_memory_types_request_t *request,
	size_t *updated_page_count);
/* Run only on the synchronous caller's CPU while preemption and interrupts
 * are disabled, after XNU's lazy FPU state has been restored and verified. */
int amdvm_hlt_context_run_once(amdvm_hlt_context_t *context,
	int *owner_status, amdvm_vmrun_status_t *vmrun_status,
	amdvm_vmexit_snapshot_t *exit_snapshot);
/* Continue the saved guest state and return any raw hardware VMEXIT. */
int amdvm_hlt_context_run(amdvm_hlt_context_t *context,
	int *owner_status, amdvm_vmrun_status_t *vmrun_status,
	amdvm_vmexit_snapshot_t *exit_snapshot);
int amdvm_hlt_context_destroy(amdvm_hlt_context_t *context);
/* Persistent process-independent accounting for failed user-page cleanup. */
size_t amdvm_hlt_context_quarantine_count(void);
size_t amdvm_hlt_context_retry_quarantined(void);
int amdvm_hlt_context_forget_quarantined(amdvm_hlt_context_t *context);
void amdvm_hlt_context_quarantine_stats(size_t *context_count,
	size_t *range_count, uint64_t *guest_bytes);

#ifdef __cplusplus
}
#endif

#endif
