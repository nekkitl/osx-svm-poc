#include "amdvm_vmcb.h"
#include "amdvm_npt.h"
#include "amdvm_vmrun.h"
#include "amdvm_xstate.h"
#include "../amdvm_probe_abi.h"

#include <string.h>

#define AMDVM_GUEST_CR0_ET       (1ull << 4)
#define AMDVM_GUEST_DR6_RESET    0xffff0ff0ull
#define AMDVM_GUEST_DR7_RESET    0x00000400ull
#define AMDVM_GUEST_RFLAGS_FIXED 0x00000002ull

/* VMCB segment attributes use descriptor bits 11:0; bits 15:12 are reserved. */
#define AMDVM_SEG_ATTR_CODE_16       0x09bu
#define AMDVM_SEG_ATTR_DATA_16       0x093u
#define AMDVM_SEG_ATTR_BUSY_TSS_32   0x08bu
#define AMDVM_SEG_ATTR_RESERVED      0xf000u
#define AMDVM_CR0_PE                 (1ull << 0)
#define AMDVM_CR0_NW                 (1ull << 29)
#define AMDVM_CR0_CD                 (1ull << 30)
#define AMDVM_CR0_PG                 (1ull << 31)
#define AMDVM_CR4_PAE                (1ull << 5)
#define AMDVM_EFER_LME               (1ull << 8)
#define AMDVM_RFLAGS_RESERVED_MASK   ((1ull << 3) | (1ull << 5) | \
	(1ull << 15) | (0xffffffffffc00000ull))

static const uint32_t amdvm_x86_segment_offsets[AMDVM_X86_SEGMENT_COUNT] = {
	AMDVM_VMCB_GUEST_ES, AMDVM_VMCB_GUEST_CS, AMDVM_VMCB_GUEST_SS,
	AMDVM_VMCB_GUEST_DS, AMDVM_VMCB_GUEST_FS, AMDVM_VMCB_GUEST_GS,
	AMDVM_VMCB_GUEST_GDTR, AMDVM_VMCB_GUEST_LDTR,
	AMDVM_VMCB_GUEST_IDTR, AMDVM_VMCB_GUEST_TR
};

static void
amdvm_vmcb_set_segment(amdvm_vmcb_t *vmcb, uint32_t offset,
	uint16_t selector, uint16_t attributes, uint32_t limit, uint64_t base)
{
	amdvm_vmcb_set_u16(vmcb, offset, selector);
	amdvm_vmcb_set_u16(vmcb, offset + 2u, attributes);
	amdvm_vmcb_set_u32(vmcb, offset + 4u, limit);
	amdvm_vmcb_set_u64(vmcb, offset + 8u, base);
}

void
amdvm_vmcb_clear(amdvm_vmcb_t *vmcb)
{
	if (vmcb != NULL) {
		memset(vmcb->bytes, 0, sizeof(vmcb->bytes));
	}
}

void
amdvm_vmcb_set_u16(amdvm_vmcb_t *vmcb, uint32_t offset, uint16_t value)
{
	if (vmcb != NULL && offset <= AMDVM_VMCB_SIZE - sizeof(value)) {
		memcpy(&vmcb->bytes[offset], &value, sizeof(value));
	}
}

void
amdvm_vmcb_set_u32(amdvm_vmcb_t *vmcb, uint32_t offset, uint32_t value)
{
	if (vmcb != NULL && offset <= AMDVM_VMCB_SIZE - sizeof(value)) {
		memcpy(&vmcb->bytes[offset], &value, sizeof(value));
	}
}

void
amdvm_vmcb_set_u64(amdvm_vmcb_t *vmcb, uint32_t offset, uint64_t value)
{
	if (vmcb != NULL && offset <= AMDVM_VMCB_SIZE - sizeof(value)) {
		memcpy(&vmcb->bytes[offset], &value, sizeof(value));
	}
}

uint64_t
amdvm_vmcb_get_u64(const amdvm_vmcb_t *vmcb, uint32_t offset)
{
	uint64_t value = 0;
	if (vmcb != NULL && offset <= AMDVM_VMCB_SIZE - sizeof(value)) {
		memcpy(&value, &vmcb->bytes[offset], sizeof(value));
	}
	return value;
}

int
amdvm_vmcb_external_interrupt_exit_enabled(const amdvm_vmcb_t *vmcb)
{
	uint32_t intercepts;
	if (vmcb == NULL) {
		return 0;
	}
	memcpy(&intercepts, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1],
	    sizeof(intercepts));
	return (intercepts & AMDVM_SVM_INTERCEPT_INTR) != 0;
}

int
amdvm_vmcb_validate_event_injection(uint32_t event_injection,
	uint32_t error_code)
{
	uint32_t event_type;
	uint32_t vector;
	int error_code_valid;

	if (event_injection == 0) {
		return error_code == 0 ? 0 : -1;
	}
	if ((event_injection & AMDVM_VCPU_EVENTINJ_VALID) == 0 ||
	    (event_injection & AMDVM_VCPU_EVENTINJ_RESERVED_MASK) != 0) {
		return -1;
	}

	event_type = event_injection & AMDVM_VCPU_EVENTINJ_TYPE_MASK;
	vector = event_injection & 0xffu;
	error_code_valid =
	    (event_injection & AMDVM_VCPU_EVENTINJ_ERROR_VALID) != 0;
	if (event_type == AMDVM_VCPU_EVENTINJ_TYPE_EXTERNAL) {
		return !error_code_valid && error_code == 0 ? 0 : -1;
	}
	if (event_type == AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION &&
	    vector == AMDVM_VCPU_EXCEPTION_GP && error_code_valid &&
	    error_code == 0) {
		return 0;
	}
	return -1;
}

int
amdvm_vmcb_validate_x86_state_v1(
	const struct amdvm_x86_guest_state_v1 *state)
{
	size_t i;
	if (state == NULL ||
	    state->abi_version != AMDVM_X86_STATE_ABI_VERSION ||
	    (state->interruptibility &
	    ~AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW) != 0 || state->cr[1] != 0 ||
	    (state->rflags & 2u) == 0 ||
	    (state->rflags & AMDVM_RFLAGS_RESERVED_MASK) != 0 ||
	    ((state->cr[0] & AMDVM_CR0_NW) != 0 &&
	    (state->cr[0] & AMDVM_CR0_CD) == 0) ||
	    ((state->cr[0] & AMDVM_CR0_PG) != 0 &&
	    (state->cr[0] & AMDVM_CR0_PE) == 0) ||
	    ((state->efer & AMDVM_EFER_LME) != 0 &&
	    (state->cr[4] & AMDVM_CR4_PAE) == 0) ||
	    /* SVME belongs to the provider's execution state, not guest-visible EFER. */
	    (state->efer & AMDVM_EFER_SVME) != 0 ||
	    (state->dr[7] & (1ull << 13)) != 0 ||
	    (state->xcr0 & 1ull) == 0 ||
	    ((state->xcr0 & (1ull << 2)) != 0 &&
	    (state->xcr0 & (1ull << 1)) == 0) ||
	    state->xstate_size != AMDVM_X86_XSTATE_MAX_SIZE ||
	    state->xstate_reserved != 0 ||
	    amdvm_vmrun_validate_guest_xstate(state->xstate,
	    state->xstate_size, state->xcr0) != AMDVM_VMRUN_SUCCESS) {
		return -1;
	}
	for (i = 0; i < AMDVM_X86_SEGMENT_COUNT; ++i) {
		if ((state->segments[i].attributes & AMDVM_SEG_ATTR_RESERVED) != 0) {
			return -1;
		}
	}
	return 0;
}

int
amdvm_vmcb_load_x86_state_v1(amdvm_vmcb_t *vmcb,
	const struct amdvm_x86_guest_state_v1 *state, uint64_t gprs[15],
	void *guest_xstate, size_t guest_xstate_size)
{
	size_t i;
	if (vmcb == NULL || gprs == NULL || guest_xstate == NULL ||
	    amdvm_vmcb_validate_x86_state_v1(state) != 0) {
		return -1;
	}
	/* Copy and validate XSTATE before mutating the VMCB or GPR frame. */
	if (amdvm_xstate_copy_standard(guest_xstate, guest_xstate_size,
	    state->xstate, state->xstate_size, state->xcr0) != 0) {
		return -1;
	}

	for (i = 0; i < AMDVM_X86_SEGMENT_COUNT; ++i) {
		const amdvm_x86_segment_state_t *segment = &state->segments[i];
		uint32_t offset = amdvm_x86_segment_offsets[i];
		amdvm_vmcb_set_u16(vmcb, offset, segment->selector);
		amdvm_vmcb_set_u16(vmcb, offset + 2u, segment->attributes);
		amdvm_vmcb_set_u32(vmcb, offset + 4u, segment->limit);
		amdvm_vmcb_set_u64(vmcb, offset + 8u, segment->base);
	}
	vmcb->bytes[AMDVM_VMCB_GUEST_CPL] =
	    (uint8_t)((state->segments[AMDVM_X86_SEG_CS].attributes >> 5) & 3u);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR0, state->cr[0]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR2, state->cr[2]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR3, state->cr[3]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR4, state->cr[4]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_EFER,
	    state->efer | AMDVM_EFER_SVME);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR6, state->dr[6]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR7, state->dr[7]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS, state->rflags);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RIP, state->rip);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_INTERRUPT_STATE,
	    (state->interruptibility & AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW) != 0 ?
	    AMDVM_VMCB_INTERRUPT_SHADOW : 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RSP, state->gpr[4]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RAX, state->gpr[0]);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_STAR, state->star);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_LSTAR, state->lstar);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CSTAR, state->cstar);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SFMASK, state->sfmask);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_KERNEL_GS_BASE,
	    state->kernel_gs_base);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SYSENTER_CS,
	    state->sysenter_cs);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SYSENTER_ESP,
	    state->sysenter_esp);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_SYSENTER_EIP,
	    state->sysenter_eip);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_PAT, state->pat);

	/* QEMU and VMRUN use different GPR order; RSP lives in the VMCB. */
	gprs[0] = state->gpr[0];  /* RAX */
	gprs[1] = state->gpr[3];  /* RBX */
	gprs[2] = state->gpr[1];  /* RCX */
	gprs[3] = state->gpr[2];  /* RDX */
	gprs[4] = state->gpr[6];  /* RSI */
	gprs[5] = state->gpr[7];  /* RDI */
	gprs[6] = state->gpr[5];  /* RBP */
	for (i = 8; i < AMDVM_X86_GPR_COUNT; ++i) {
		gprs[i - 1u] = state->gpr[i];
	}
	return 0;
}

int
amdvm_vmcb_stage_run_request(amdvm_vmcb_t *vmcb, uint64_t gprs[15],
	void *guest_xstate, const struct amdvm_vcpu_run_request *request,
	int full_state)
{
	size_t i;
	if (vmcb == NULL || gprs == NULL || guest_xstate == NULL ||
	    request == NULL ||
	    amdvm_vmcb_validate_event_injection(request->event_injection,
	    request->event_injection_error_code) != 0) {
		return -1;
	}
	if (full_state) {
		if (request->state.abi_version != AMDVM_X86_STATE_ABI_VERSION ||
		    amdvm_vmcb_load_x86_state_v1(vmcb, &request->state, gprs,
		    guest_xstate, AMDVM_X86_XSTATE_MAX_SIZE) != 0) {
			return -1;
		}
	} else {
		if (request->state.abi_version != 0 || request->rflags != 2u ||
		    request->event_injection != 0 ||
		    request->event_injection_error_code != 0) {
			return -1;
		}
		for (i = 0; i < sizeof(request->state); ++i) {
			if (((const uint8_t *)&request->state)[i] != 0) return -1;
		}
		for (i = 0; i < 15u; ++i) gprs[i] = request->gpr[i];
		amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RIP, request->rip);
		amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS, request->rflags);
	}
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EVENT_INJECTION,
	    ((uint64_t)request->event_injection_error_code << 32) |
	    request->event_injection);
	/* Hardware outputs must not carry a stale event from the prior entry. */
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_INT_INFO, 0);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_EXIT_INT_INFO_ERROR, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_CODE, AMDVM_SVM_EXIT_NOT_RUN);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_INFO1, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_EXIT_INFO2, 0);
	vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_LENGTH] = 0;
	memset(&vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_BYTES], 0xcc,
	    AMDVM_VMCB_DECODE_ASSIST_MAX);
	return 0;
}

int
amdvm_vmcb_capture_entry_snapshot(const amdvm_vmcb_t *vmcb,
	const struct amdvm_npt *npt, uint64_t guest_gpa_page,
	size_t entry_offset,
	const void *backing_virtual_address, uint64_t backing_physical_address,
	size_t backing_size, const uint64_t gprs[15], uint64_t host_xcr0,
	uint64_t guest_xcr0, uint32_t guest_xstate_size,
	struct amdvm_vcpu_entry_snapshot *snapshot)
{
	amdvm_npt_translation_t translation = {0};
	uint64_t backing_end, entry_hpa, offset;
	uint32_t nested, misc1;
	if (vmcb == NULL || npt == NULL || backing_virtual_address == NULL ||
	    gprs == NULL || snapshot == NULL ||
	    amdvm_npt_translate_4k(npt, guest_gpa_page, &translation) != AMDVM_NPT_OK ||
	    (translation.permissions & (AMDVM_NPT_READ | AMDVM_NPT_EXECUTE)) !=
	    (AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) ||
	    entry_offset > AMDVM_NPT_PAGE_SIZE - 16u ||
	    backing_size > UINT64_MAX - backing_physical_address) {
		return -1;
	}
	backing_end = backing_physical_address + backing_size;
	entry_hpa = translation.host_physical_address + entry_offset;
	if (entry_hpa < backing_physical_address || entry_hpa >= backing_end) {
		return -1;
	}
	offset = entry_hpa - backing_physical_address;
	if (offset > backing_size || backing_size - (size_t)offset < 16u) {
		return -1;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->guest_physical_address = guest_gpa_page + entry_offset;
	snapshot->host_physical_address = entry_hpa;
	snapshot->npt_leaf_entry = translation.leaf_entry;
	snapshot->npt_root = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_NPT_ROOT);
	snapshot->cs_selector = (uint16_t)amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_GUEST_CS);
	memcpy(&snapshot->cs_attributes,
	    &vmcb->bytes[AMDVM_VMCB_GUEST_CS + 2u], sizeof(snapshot->cs_attributes));
	memcpy(&snapshot->cs_limit,
	    &vmcb->bytes[AMDVM_VMCB_GUEST_CS + 4u], sizeof(snapshot->cs_limit));
	snapshot->cs_base = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CS + 8u);
	snapshot->rip = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RIP);
	snapshot->rflags = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS);
	snapshot->cr0 = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CR0);
	snapshot->cr3 = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CR3);
	snapshot->cr4 = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CR4);
	snapshot->efer = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_EFER);
	snapshot->event_injection = amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_EVENT_INJECTION);
	snapshot->exit_code_sentinel = amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_EXIT_CODE);
	snapshot->nested_paging_control = (uint32_t)amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_NESTED_PAGING);
	{
		uint64_t asid_tlb = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_TLB_CONTROL);
		snapshot->asid = (uint32_t)asid_tlb;
	}
	memcpy(&misc1, &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1], sizeof(misc1));
	snapshot->intercept_misc1 = misc1;
	memcpy(&snapshot->intercept_misc2,
	    &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC2],
	    sizeof(snapshot->intercept_misc2));
	snapshot->host_xcr0 = host_xcr0;
	snapshot->guest_xcr0 = guest_xcr0;
	snapshot->guest_xstate_size = guest_xstate_size;
	memcpy(snapshot->entry_bytes,
	    (const uint8_t *)backing_virtual_address + (size_t)offset,
	    sizeof(snapshot->entry_bytes));
	memcpy(snapshot->gpr, gprs, sizeof(snapshot->gpr));
	snapshot->flags = AMDVM_VCPU_ENTRY_TRANSLATION_VALID |
	    AMDVM_VCPU_ENTRY_BACKING_BYTES_VALID;
	if (misc1 & AMDVM_SVM_INTERCEPT_CPUID) {
		snapshot->flags |= AMDVM_VCPU_ENTRY_CPUID_INTERCEPT;
	}
	nested = snapshot->nested_paging_control;
	if (nested & AMDVM_VMCB_NPT_ENABLE) {
		snapshot->flags |= AMDVM_VCPU_ENTRY_NPT_ENABLED;
	}
	return 0;
}

int
amdvm_vmcb_store_x86_state_v1(const amdvm_vmcb_t *vmcb,
	struct amdvm_x86_guest_state_v1 *state, const uint64_t gprs[15],
	const void *guest_xstate, size_t guest_xstate_size)
{
	size_t i;
	if (vmcb == NULL || state == NULL || gprs == NULL || guest_xstate == NULL ||
	    state->abi_version != AMDVM_X86_STATE_ABI_VERSION ||
	    (state->interruptibility &
	    ~AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW) != 0) {
		return -1;
	}
	/* Validate/copy XSTATE before partially updating the architectural state. */
	if (guest_xstate_size != state->xstate_size ||
	    amdvm_vmrun_validate_guest_xstate(guest_xstate, guest_xstate_size,
	    state->xcr0) != AMDVM_VMRUN_SUCCESS ||
	    amdvm_xstate_copy_standard(state->xstate, state->xstate_size,
	    guest_xstate, guest_xstate_size, state->xcr0) != 0) {
		return -1;
	}
	for (i = 0; i < AMDVM_X86_SEGMENT_COUNT; ++i) {
		amdvm_x86_segment_state_t *segment = &state->segments[i];
		uint32_t offset = amdvm_x86_segment_offsets[i];
		memcpy(&segment->selector, &vmcb->bytes[offset], sizeof(uint16_t));
		memcpy(&segment->attributes, &vmcb->bytes[offset + 2u], sizeof(uint16_t));
		memcpy(&segment->limit, &vmcb->bytes[offset + 4u], sizeof(uint32_t));
		memcpy(&segment->base, &vmcb->bytes[offset + 8u], sizeof(uint64_t));
	}
	state->gpr[0] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RAX);
	state->gpr[1] = gprs[2];
	state->gpr[2] = gprs[3];
	state->gpr[3] = gprs[1];
	state->gpr[4] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RSP);
	state->gpr[5] = gprs[6];
	state->gpr[6] = gprs[4];
	state->gpr[7] = gprs[5];
	for (i = 8; i < AMDVM_X86_GPR_COUNT; ++i) {
		state->gpr[i] = gprs[i - 1u];
	}
	state->rip = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RIP);
	state->rflags = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS);
	state->interruptibility =
	    (amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_INTERRUPT_STATE) &
	    AMDVM_VMCB_INTERRUPT_SHADOW) != 0 ?
	    AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW : 0;
	state->cr[0] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CR0);
	state->cr[2] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CR2);
	state->cr[3] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CR3);
	state->cr[4] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CR4);
	/* Never leak the provider's SVM enable bit into QEMU guest state. */
	state->efer = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_EFER) &
	    ~AMDVM_EFER_SVME;
	state->dr[6] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_DR6);
	state->dr[7] = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_DR7);
	state->star = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_STAR);
	state->lstar = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_LSTAR);
	state->cstar = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_CSTAR);
	state->sfmask = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_SFMASK);
	state->kernel_gs_base = amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_GUEST_KERNEL_GS_BASE);
	state->sysenter_cs = amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_GUEST_SYSENTER_CS);
	state->sysenter_esp = amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_GUEST_SYSENTER_ESP);
	state->sysenter_eip = amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_GUEST_SYSENTER_EIP);
	state->pat = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_GUEST_PAT);
	return 0;
}

int
amdvm_vmcb_configure_probe(amdvm_vmcb_t *vmcb, uint64_t npt_root_pa,
	uint64_t iopm_pa, uint64_t msrpm_pa, uint32_t asid)
{
	uint32_t misc1, misc2;
	uint64_t asid_tlb;
	if (vmcb == NULL || asid == 0 ||
	    (npt_root_pa & (AMDVM_PAGE_SIZE - 1u)) != 0 || npt_root_pa == 0 ||
	    (iopm_pa & (AMDVM_PAGE_SIZE - 1u)) != 0 || iopm_pa == 0 ||
	    (msrpm_pa & (AMDVM_PAGE_SIZE - 1u)) != 0 || msrpm_pa == 0) {
		return -1;
	}
	amdvm_vmcb_clear(vmcb);

	/*
	 * Start with restrictive traps. This first-run profile is for a tiny
	 * one-page guest probe; it deliberately favors safety over exit count.
	 */
	amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_CR_READ, UINT16_MAX);
	amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_CR_WRITE, UINT16_MAX);
	amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_DR_READ, UINT16_MAX);
	amdvm_vmcb_set_u16(vmcb, AMDVM_VMCB_INTERCEPT_DR_WRITE, UINT16_MAX);
	/* Deliver guest #PF through the guest IDT; NPT violations remain NPF exits. */
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERCEPT_EXCEPTIONS,
	    UINT32_MAX & ~(UINT32_C(1) << AMDVM_VCPU_EXCEPTION_PF));
	/* Start the probe with no inherited fault address or debug breakpoints. */
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR2, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR6, 0xffff0ff0u);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR7, 0x400u);

	misc1 = AMDVM_SVM_INTERCEPT_INTR | AMDVM_SVM_INTERCEPT_NMI |
	    AMDVM_SVM_INTERCEPT_SMI | AMDVM_SVM_INTERCEPT_INIT |
	    AMDVM_SVM_INTERCEPT_RDTSC | AMDVM_SVM_INTERCEPT_RDPMC |
	    AMDVM_SVM_INTERCEPT_CPUID | AMDVM_SVM_INTERCEPT_RSM |
	    AMDVM_SVM_INTERCEPT_INVD | AMDVM_SVM_INTERCEPT_PAUSE |
	    AMDVM_SVM_INTERCEPT_HLT | AMDVM_SVM_INTERCEPT_SHUTDOWN |
	    AMDVM_SVM_INTERCEPT_IOIO | AMDVM_SVM_INTERCEPT_MSR;
	misc2 = AMDVM_SVM_INTERCEPT_VMRUN | AMDVM_SVM_INTERCEPT_VMMCALL |
	    AMDVM_SVM_INTERCEPT_VMLOAD | AMDVM_SVM_INTERCEPT_VMSAVE |
	    AMDVM_SVM_INTERCEPT_STGI | AMDVM_SVM_INTERCEPT_CLGI |
	    AMDVM_SVM_INTERCEPT_SKINIT | AMDVM_SVM_INTERCEPT_RDTSCP |
	    AMDVM_SVM_INTERCEPT_WBINVD | AMDVM_SVM_INTERCEPT_MONITOR |
	    AMDVM_SVM_INTERCEPT_MWAIT | AMDVM_SVM_INTERCEPT_XSETBV |
	    AMDVM_SVM_INTERCEPT_RDPRU | AMDVM_SVM_INTERCEPT_EFER_WRITE_TRAP;
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERCEPT_MISC1, misc1);
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERCEPT_MISC2, misc2);
	/*
	 * Keep guest RFLAGS.IF from masking physical host interrupts while SVM is
	 * active. With this bit clear, a guest that leaves IF=0 can also block the
	 * host's physical INTR path, including an XNU TLB-shootdown IPI. Guest IF
	 * continues to control virtual interrupt delivery.
	 */
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_INTERRUPT_CONTROL,
	    AMDVM_VMCB_V_INTR_MASKING);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_IOPM_PA, iopm_pa);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_MSRPM_PA, msrpm_pa);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_NESTED_PAGING,
	    AMDVM_VMCB_NPT_ENABLE);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_NPT_ROOT, npt_root_pa);
	/* Flush all tagged translations on first entry; never trust stale ASIDs. */
	asid_tlb = (uint64_t)asid |
	    ((uint64_t)AMDVM_SVM_TLB_FLUSH_ALL << 32);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_TLB_CONTROL, asid_tlb);
	/* No VMCB state group may be treated as clean on first entry. */
	amdvm_vmcb_set_u32(vmcb, AMDVM_VMCB_CLEAN_BITS, 0);
	return 0;
}

int
amdvm_vmcb_configure_realmode_test_v1(amdvm_vmcb_t *vmcb)
{
	if (vmcb == NULL) {
		return -1;
	}

	/*
	 * Keep the caller's control area intact, but make the complete save area
	 * deterministic for REALMODE_TEST_V1. This is a fixed test profile at GPA
	 * 0, not QEMU's architectural reset vector. The companion intercept profile
	 * traps CPUID, I/O, HLT, and every exception; NPT maps GPA 0 to the private
	 * RX test-payload page.
	 */
	memset(&vmcb->bytes[AMDVM_VMCB_GUEST_STATE], 0,
	    AMDVM_VMCB_SIZE - AMDVM_VMCB_GUEST_STATE);

	/* Real-mode code and data caches with fully expanded base/limit fields. */
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_ES, 0, AMDVM_SEG_ATTR_DATA_16,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_CS, 0, AMDVM_SEG_ATTR_CODE_16,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_SS, 0, AMDVM_SEG_ATTR_DATA_16,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_DS, 0, AMDVM_SEG_ATTR_DATA_16,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_FS, 0, AMDVM_SEG_ATTR_DATA_16,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_GS, 0, AMDVM_SEG_ATTR_DATA_16,
	    0xffffu, 0);

	/* Fixed descriptor-table and task-register caches for this profile. */
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_GDTR, 0, 0, 0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_IDTR, 0, 0, 0x03ffu, 0);
	/* TR is never a null segment; seed a valid busy 32-bit TSS cache. */
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_TR, 0x18u,
	    AMDVM_SEG_ATTR_BUSY_TSS_32, 0x67u, 0);
	vmcb->bytes[AMDVM_VMCB_GUEST_CPL] = 0;

	/*
	 * AMD requires guest EFER.SVME even for a guest that never executes SVM.
	 * The uniform WB PAT keeps the single private payload page cacheable.
	 */
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_EFER, AMDVM_EFER_SVME);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_PAT, AMDVM_GUEST_PAT_ALL_WB);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR0, AMDVM_GUEST_CR0_ET);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR3, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR4, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR2, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR6, AMDVM_GUEST_DR6_RESET);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR7, AMDVM_GUEST_DR7_RESET);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS, AMDVM_GUEST_RFLAGS_FIXED);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RIP, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RSP, 0x800u);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RAX, 0);
	return 0;
}

int
amdvm_vmcb_configure_qemu_reset_vector_v1(amdvm_vmcb_t *vmcb)
{
	if (vmcb == NULL) {
		return -1;
	}

	/* Preserve the restrictive control/intercept area configured by the caller. */
	memset(&vmcb->bytes[AMDVM_VMCB_GUEST_STATE], 0,
	    AMDVM_VMCB_SIZE - AMDVM_VMCB_GUEST_STATE);

	/* Values mirror QEMU target/i386/cpu.c::x86_cpu_reset_hold(). */
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_CS, 0xf000u,
	    AMDVM_SEG_ATTR_CODE_16, 0xffffu, AMDVM_VCPU_RESET_CS_BASE);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_DS, 0,
	    AMDVM_SEG_ATTR_DATA_16, 0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_ES, 0,
	    AMDVM_SEG_ATTR_DATA_16, 0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_SS, 0,
	    AMDVM_SEG_ATTR_DATA_16, 0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_FS, 0,
	    AMDVM_SEG_ATTR_DATA_16, 0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_GS, 0,
	    AMDVM_SEG_ATTR_DATA_16, 0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_GDTR, 0, 0,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_LDTR, 0, 0x082u,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_IDTR, 0, 0,
	    0xffffu, 0);
	amdvm_vmcb_set_segment(vmcb, AMDVM_VMCB_GUEST_TR, 0, 0x08bu,
	    0xffffu, 0);
	vmcb->bytes[AMDVM_VMCB_GUEST_CPL] = 0;

	/*
	 * QEMU resets EFER to zero. AMD SVM requires SVME in the hardware-loaded
	 * guest EFER; the MSR intercept must expose the virtual QEMU value (zero).
	 */
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_EFER, AMDVM_EFER_SVME);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_PAT, AMDVM_QEMU_RESET_PAT);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR0, 0x60000010ull);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR2, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR3, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_CR4, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR6, AMDVM_GUEST_DR6_RESET);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_DR7, AMDVM_GUEST_DR7_RESET);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RFLAGS, AMDVM_GUEST_RFLAGS_FIXED);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RIP,
	    AMDVM_VCPU_RESET_VECTOR_RIP);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RSP, 0);
	amdvm_vmcb_set_u64(vmcb, AMDVM_VMCB_GUEST_RAX, 0);
	return 0;
}

int
amdvm_svm_init_all_intercept_bitmaps(void *iopm, size_t iopm_size,
	void *msrpm, size_t msrpm_size)
{
	if (iopm == NULL || msrpm == NULL ||
	    iopm_size < AMDVM_IOPM_SIZE || msrpm_size < AMDVM_MSRPM_SIZE) {
		return -1;
	}
	memset(iopm, 0xff, AMDVM_IOPM_SIZE);
	memset(msrpm, 0xff, AMDVM_MSRPM_SIZE);
	return 0;
}
