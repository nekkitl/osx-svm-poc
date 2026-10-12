#ifndef AMDVM_VMRUN_H
#define AMDVM_VMRUN_H

#include <stddef.h>
#include <stdint.h>
#include "amdvm_vmcb.h"
#include "amdvm_physmem.h"
#include "amdvm_vmrun_asm.h"

typedef struct amdvm_guest_gprs {
	uint64_t rax; /* VMRUN exchanges RAX through the VMCB. */
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rsi;
	uint64_t rdi;
	uint64_t rbp;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
} amdvm_guest_gprs_t;

/* Raw architectural exit plus the guest state needed by a state-based caller.
 * Valid only when amdvm_vmrun_once() returns AMDVM_VMRUN_SUCCESS. */
typedef struct amdvm_vmexit_snapshot {
	uint64_t code;
	uint64_t info1;
	uint64_t info2;
	uint64_t guest_rip;
	uint64_t guest_rflags;
	uint64_t next_rip;
	uint8_t instruction_length;
	uint8_t instruction_bytes[AMDVM_VMCB_DECODE_ASSIST_MAX];
} amdvm_vmexit_snapshot_t;

typedef enum amdvm_vmrun_status {
	AMDVM_VMRUN_SUCCESS = 0,
	AMDVM_VMRUN_INVALID_ARGUMENT = -1,
	AMDVM_VMRUN_DEBUGGER_ACTIVE = -2,
	AMDVM_VMRUN_XSTATE_UNSUPPORTED = -3,
	AMDVM_VMRUN_SPECULATION_UNSUPPORTED = -4,
	AMDVM_VMRUN_ENTRY_GATED = -5,
	AMDVM_VMRUN_FPU_NOT_READY = -6
} amdvm_vmrun_status_t;

_Static_assert(offsetof(amdvm_guest_gprs_t, rax) == 0, "GPR ABI rax");
_Static_assert(offsetof(amdvm_guest_gprs_t, rbx) == 8, "GPR ABI rbx");
_Static_assert(offsetof(amdvm_guest_gprs_t, rcx) == 16, "GPR ABI rcx");
_Static_assert(offsetof(amdvm_guest_gprs_t, rdx) == 24, "GPR ABI rdx");
_Static_assert(offsetof(amdvm_guest_gprs_t, rsi) == 32, "GPR ABI rsi");
_Static_assert(offsetof(amdvm_guest_gprs_t, rdi) == 40, "GPR ABI rdi");
_Static_assert(offsetof(amdvm_guest_gprs_t, rbp) == 48, "GPR ABI rbp");
_Static_assert(offsetof(amdvm_guest_gprs_t, r8) == 56, "GPR ABI r8");
_Static_assert(offsetof(amdvm_guest_gprs_t, r9) == 64, "GPR ABI r9");
_Static_assert(offsetof(amdvm_guest_gprs_t, r10) == 72, "GPR ABI r10");
_Static_assert(offsetof(amdvm_guest_gprs_t, r11) == 80, "GPR ABI r11");
_Static_assert(offsetof(amdvm_guest_gprs_t, r12) == 88, "GPR ABI r12");
_Static_assert(offsetof(amdvm_guest_gprs_t, r13) == 96, "GPR ABI r13");
_Static_assert(offsetof(amdvm_guest_gprs_t, r14) == 104, "GPR ABI r14");
_Static_assert(offsetof(amdvm_guest_gprs_t, r15) == 112, "GPR ABI r15");
_Static_assert(sizeof(amdvm_guest_gprs_t) == 120, "GPR ABI size");
_Static_assert(sizeof(amdvm_vmexit_snapshot_t) == 64, "VMEXIT snapshot ABI size");
_Static_assert(AMDVM_VMRUN_VMCB_GUEST_RAX_OFFSET == AMDVM_VMCB_GUEST_RAX,
	"assembly VMCB RAX offset");
_Static_assert(AMDVM_VMRUN_FRAME_TEMP_RDX + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_HOST_CR2, "VMRUN frame scratch overlap");
_Static_assert(AMDVM_VMRUN_FRAME_XSTATE_AREA + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_XCR0, "VMRUN frame XSTATE pointer overlap");
_Static_assert(AMDVM_VMRUN_FRAME_XCR0 + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_TEMP_RDX, "VMRUN frame XCR0 overlap");
_Static_assert(AMDVM_VMRUN_FRAME_HOST_CR2 + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_GUEST_GPRS, "VMRUN frame CR2 overlap");
_Static_assert(AMDVM_VMRUN_FRAME_GUEST_GPRS + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_GUEST_VA, "VMRUN frame GPR pointer overlap");
_Static_assert(AMDVM_VMRUN_FRAME_GUEST_VA + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_GUEST_PA, "VMRUN frame guest VA overlap");
_Static_assert(AMDVM_VMRUN_FRAME_GUEST_PA + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_HOST_PA, "VMRUN frame guest PA overlap");
_Static_assert(AMDVM_VMRUN_FRAME_HOST_PA + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_SIZE, "VMRUN frame host PA outside frame");
_Static_assert(AMDVM_VMRUN_FRAME_GUEST_XSTATE + sizeof(uint64_t) <=
	AMDVM_VMRUN_FRAME_SIZE, "VMRUN frame guest XSTATE outside frame");
_Static_assert((AMDVM_VMRUN_FRAME_SIZE & 15u) == 0,
	"VMRUN frame preserves stack alignment");

/*
 * One bounded entry/exit wrapper. Caller must own SVM on the current CPU and
 * invoke this only from the synchronous administrator-only userclient thread
 * while a supported IOSimpleLock pins the CPU and local interrupts are
 * disabled. Before entry, use amdvm_vmrun_xstate_capture_current_thread() on
 * this thread so XNU's normal #NM path restores its PCB state; do not clear
 * CR0.TS directly. This is a fixed-HLT experiment, not a general guest runner.
 * XNU does not
 * export its KDP watchpoint state, so operators must not invoke the experiment
 * while a kernel debugger or KDP hardware watchpoints are active. Caller also
 * provides private host/guest VMCBs, guest GPR state, and distinct,
 * kernel-owned 4 KiB host_xstate_area and guest_xstate_area pages. Both XSTATE
 * pages must be page-aligned, pinned, mapped, exclusive, and excluded from the
 * guest NPT. guest_xstate_area must contain a valid standard XSAVE header
 * (an all-zero page is the architectural initial state). The wrapper saves
 * host XSTATE, restores guest XSTATE, executes VMRUN, saves guest XSTATE, and
 * restores host XSTATE. This first ABI uses the current CPU XCR0 for both
 * contexts and rejects VMCBs that do not intercept XSETBV. This does not make
 * arbitrary guest code safe and must not be called for an untrusted or mutable
 * payload.
 */
#ifdef __cplusplus
extern "C" {
#endif
amdvm_vmrun_status_t amdvm_vmrun_once(
	const amdvm_physmem_region_t *backing_region,
	amdvm_vmcb_t *host_vmcb,
	uint64_t host_vmcb_pa, amdvm_vmcb_t *guest_vmcb,
	uint64_t guest_vmcb_pa, amdvm_guest_gprs_t *guest_gprs,
	void *host_xstate_area, void *guest_xstate_area,
	amdvm_vmexit_snapshot_t *exit_snapshot);

/* Save current-thread XSTATE to wired storage; XSAVE may invoke XNU's #NM
 * lazy-restore path. Does not touch SVM state or modify CR0/PCB directly. */
amdvm_vmrun_status_t amdvm_vmrun_xstate_capture_current_thread(
	void *xstate_area, uint64_t *xcr0_out, uint64_t *cr0_before_out,
	uint64_t *cr0_after_out);
/* Validate a QEMU standard-format XSAVE image against host and guest XCR0. */
amdvm_vmrun_status_t amdvm_vmrun_validate_guest_xstate(
	const void *xstate_area, size_t xstate_size, uint64_t guest_xcr0);
#ifdef __cplusplus
}
#endif

#endif
