#include "amdvm_vmrun.h"
#include "amdvm_xstate.h"

#include <stdint.h>

#define AMDVM_VMRUN_PAGE_MASK UINT64_C(0xfff)
#define AMDVM_VMRUN_XSTATE_SIZE AMDVM_PAGE_SIZE
#define AMDVM_VMRUN_PA_MASK UINT64_C(0x000ffffffffff000)
#define AMDVM_CR0_TS (UINT64_C(1) << 3)
#define AMDVM_CR0_EM (UINT64_C(1) << 2)
#define AMDVM_CPUID_AMD_IBPB (1u << 12)

extern int amdvm_vmrun_once_asm(uint64_t host_vmcb_pa,
	uint64_t guest_vmcb_pa, amdvm_vmcb_t *guest_vmcb,
	amdvm_guest_gprs_t *guest_gprs, void *host_xstate_area,
	void *guest_xstate_area);

static int amdvm_xstate_supported(void);

static uint64_t
amdvm_current_xcr0(void)
{
	uint32_t low, high;
	__asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
	return ((uint64_t)high << 32) | low;
}

amdvm_vmrun_status_t
amdvm_vmrun_validate_guest_xstate(const void *xstate_area, size_t xstate_size,
	uint64_t guest_xcr0)
{
	uint64_t host_xcr0;
	if (xstate_area == NULL || xstate_size != AMDVM_VMRUN_XSTATE_SIZE ||
	    !amdvm_xstate_supported()) {
		return AMDVM_VMRUN_XSTATE_UNSUPPORTED;
	}
	host_xcr0 = amdvm_current_xcr0();
	if ((guest_xcr0 & 1u) == 0 || (guest_xcr0 & ~host_xcr0) != 0 ||
	    amdvm_xstate_validate_standard(xstate_area, xstate_size,
	    guest_xcr0) != 0) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	return AMDVM_VMRUN_SUCCESS;
}

static uint32_t
amdvm_vmcb_intercept_misc2(const amdvm_vmcb_t *vmcb)
{
	const uint8_t *p = &vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC2];
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t
amdvm_vmcb_intercept_u16(const amdvm_vmcb_t *vmcb, uint32_t offset)
{
	return (uint16_t)((uint16_t)vmcb->bytes[offset] |
	    ((uint16_t)vmcb->bytes[offset + 1u] << 8));
}

static int
amdvm_ibpb_supported(void)
{
	uint32_t max_extended, a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(max_extended), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000000u), "c"(0));
	if (max_extended < 0x80000008u) {
		return 0;
	}
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000008u), "c"(0));
	return (b & AMDVM_CPUID_AMD_IBPB) != 0;
}

amdvm_vmrun_status_t
amdvm_vmrun_xstate_capture_current_thread(void *xstate_area,
	uint64_t *xcr0_out, uint64_t *cr0_before_out, uint64_t *cr0_after_out)
{
	uint64_t cr0_before, cr0_after, xcr0;
	uint32_t eax, edx;

	if (xstate_area == NULL || xcr0_out == NULL || cr0_before_out == NULL ||
	    cr0_after_out == NULL ||
	    ((uintptr_t)xstate_area & (AMDVM_PAGE_SIZE - 1u)) != 0 ||
	    !amdvm_xstate_supported()) {
		return AMDVM_VMRUN_XSTATE_UNSUPPORTED;
	}
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0_before));
	*cr0_before_out = cr0_before;
	*cr0_after_out = cr0_before;
	if (cr0_before & AMDVM_CR0_EM) {
		return AMDVM_VMRUN_FPU_NOT_READY;
	}
	xcr0 = amdvm_current_xcr0();
	if ((xcr0 & 1u) == 0) {
		return AMDVM_VMRUN_XSTATE_UNSUPPORTED;
	}
	eax = (uint32_t)xcr0;
	edx = (uint32_t)(xcr0 >> 32);
	/*
	 * If XNU left CR0.TS set for lazy ownership, this architectural XSAVE
	 * raises #NM. XNU's normal thread-context handler restores the PCB state;
	 * the retried XSAVE records it without changing the live register state.
	 * The caller must pin this thread to its CPU and provide a wired area.
	 */
	__asm__ volatile("xsave64 (%0)" : : "r"(xstate_area), "a"(eax),
	    "d"(edx) : "memory");
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0_after));
	*xcr0_out = xcr0;
	*cr0_after_out = cr0_after;
	if (cr0_after & (AMDVM_CR0_TS | AMDVM_CR0_EM)) {
		return AMDVM_VMRUN_FPU_NOT_READY;
	}
	return AMDVM_VMRUN_SUCCESS;
}

static int
amdvm_xstate_supported(void)
{
	uint32_t max_basic, a, b, c, d;
	uint64_t cr4;

	__asm__ volatile("cpuid" : "=a"(max_basic), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0), "c"(0));
	if (max_basic < 0x0du) {
		return 0;
	}
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(1), "c"(0));
	if ((c & ((1u << 26) | (1u << 27))) !=
	    ((1u << 26) | (1u << 27))) {
		return 0;
	}
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
	if ((cr4 & (UINT64_C(1) << 18)) == 0) {
		return 0;
	}
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x0d), "c"(0));
	return c >= 576u && c <= AMDVM_VMRUN_XSTATE_SIZE;
}

static int
amdvm_vmcb_address_valid(const amdvm_physmem_region_t *backing_region,
	amdvm_vmcb_t *vmcb, uint64_t physical_address)
{
	uint64_t resolved_address;

	if (vmcb == NULL || ((uintptr_t)vmcb & AMDVM_VMRUN_PAGE_MASK) != 0 ||
	    (physical_address & AMDVM_VMRUN_PAGE_MASK) != 0 ||
	    (physical_address & ~AMDVM_VMRUN_PA_MASK) != 0 ||
	    amdvm_physmem_resolve(backing_region, vmcb, AMDVM_PAGE_SIZE,
	    &resolved_address) != 0) {
		return 0;
	}
	return resolved_address == physical_address;
}

static int
amdvm_ranges_overlap(uintptr_t first, size_t first_size,
	uintptr_t second, size_t second_size)
{
	uintptr_t first_end, second_end;
	if (first > UINTPTR_MAX - first_size ||
	    second > UINTPTR_MAX - second_size) {
		return 1;
	}
	first_end = first + first_size;
	second_end = second + second_size;
	return first < second_end && second < first_end;
}

amdvm_vmrun_status_t
amdvm_vmrun_once(const amdvm_physmem_region_t *backing_region,
	amdvm_vmcb_t *host_vmcb, uint64_t host_vmcb_pa,
	amdvm_vmcb_t *guest_vmcb, uint64_t guest_vmcb_pa,
	amdvm_guest_gprs_t *guest_gprs, void *host_xstate_area,
	void *guest_xstate_area, amdvm_vmexit_snapshot_t *exit_snapshot)
{
	uintptr_t gprs_address;
	uint64_t gprs_pa, host_xstate_pa, guest_xstate_pa;
	uintptr_t host_xstate_address, guest_xstate_address;
	uint64_t xcr0, captured_xcr0, capture_cr0_before, capture_cr0_after;
	amdvm_vmrun_status_t capture_status;

#if !defined(AMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN)
	/*
	 * Fail closed until entry preconditions and residual fault paths are
	 * reviewed. This opt-in only bypasses the guard; it is not a safety proof.
	 */
	(void)host_vmcb;
	(void)host_vmcb_pa;
	(void)backing_region;
	(void)guest_vmcb;
	(void)guest_vmcb_pa;
	(void)guest_gprs;
	(void)host_xstate_area;
	(void)guest_xstate_area;
	(void)exit_snapshot;
	(void)captured_xcr0;
	(void)capture_cr0_before;
	(void)capture_cr0_after;
	(void)capture_status;
	return AMDVM_VMRUN_ENTRY_GATED;
#endif

	if (!amdvm_vmcb_address_valid(backing_region, host_vmcb, host_vmcb_pa) ||
	    !amdvm_vmcb_address_valid(backing_region, guest_vmcb, guest_vmcb_pa) ||
	    host_vmcb == guest_vmcb || host_vmcb_pa == guest_vmcb_pa ||
	    guest_gprs == NULL || ((uintptr_t)guest_gprs & 7u) != 0 ||
	    exit_snapshot == NULL ||
	    host_xstate_area == NULL || guest_xstate_area == NULL ||
	    ((uintptr_t)host_xstate_area & (AMDVM_PAGE_SIZE - 1u)) != 0 ||
	    ((uintptr_t)guest_xstate_area & (AMDVM_PAGE_SIZE - 1u)) != 0) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	gprs_address = (uintptr_t)guest_gprs;
	host_xstate_address = (uintptr_t)host_xstate_area;
	guest_xstate_address = (uintptr_t)guest_xstate_area;
	if ((gprs_address & AMDVM_VMRUN_PAGE_MASK) + sizeof(*guest_gprs) >
	    AMDVM_VMRUN_PAGE_MASK + 1u ||
	    amdvm_ranges_overlap(host_xstate_address, AMDVM_VMRUN_XSTATE_SIZE,
	    guest_xstate_address, AMDVM_VMRUN_XSTATE_SIZE) ||
	    amdvm_ranges_overlap(gprs_address, sizeof(*guest_gprs),
	    (uintptr_t)host_vmcb, sizeof(*host_vmcb)) ||
	    amdvm_ranges_overlap(gprs_address, sizeof(*guest_gprs),
	    (uintptr_t)guest_vmcb, sizeof(*guest_vmcb)) ||
	    amdvm_ranges_overlap(host_xstate_address, AMDVM_VMRUN_XSTATE_SIZE,
	    (uintptr_t)host_vmcb, sizeof(*host_vmcb)) ||
	    amdvm_ranges_overlap(host_xstate_address, AMDVM_VMRUN_XSTATE_SIZE,
	    (uintptr_t)guest_vmcb, sizeof(*guest_vmcb)) ||
	    amdvm_ranges_overlap(host_xstate_address, AMDVM_VMRUN_XSTATE_SIZE,
	    gprs_address, sizeof(*guest_gprs)) ||
	    amdvm_ranges_overlap(guest_xstate_address, AMDVM_VMRUN_XSTATE_SIZE,
	    (uintptr_t)host_vmcb, sizeof(*host_vmcb)) ||
	    amdvm_ranges_overlap(guest_xstate_address, AMDVM_VMRUN_XSTATE_SIZE,
	    (uintptr_t)guest_vmcb, sizeof(*guest_vmcb)) ||
	    amdvm_ranges_overlap(guest_xstate_address, AMDVM_VMRUN_XSTATE_SIZE,
	    gprs_address, sizeof(*guest_gprs))) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	if (amdvm_ranges_overlap((uintptr_t)exit_snapshot,
	    sizeof(*exit_snapshot), (uintptr_t)host_vmcb, sizeof(*host_vmcb)) ||
	    amdvm_ranges_overlap((uintptr_t)exit_snapshot, sizeof(*exit_snapshot),
	    (uintptr_t)guest_vmcb, sizeof(*guest_vmcb)) ||
	    amdvm_ranges_overlap((uintptr_t)exit_snapshot, sizeof(*exit_snapshot),
	    gprs_address, sizeof(*guest_gprs)) ||
	    amdvm_ranges_overlap((uintptr_t)exit_snapshot, sizeof(*exit_snapshot),
	    host_xstate_address, AMDVM_VMRUN_XSTATE_SIZE) ||
	    amdvm_ranges_overlap((uintptr_t)exit_snapshot, sizeof(*exit_snapshot),
	    guest_xstate_address, AMDVM_VMRUN_XSTATE_SIZE)) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	if (amdvm_physmem_resolve(backing_region, guest_gprs,
	    sizeof(*guest_gprs), &gprs_pa) != 0 ||
	    amdvm_physmem_resolve(backing_region, host_xstate_area,
	    AMDVM_VMRUN_XSTATE_SIZE, &host_xstate_pa) != 0 ||
	    amdvm_physmem_resolve(backing_region, guest_xstate_area,
	    AMDVM_VMRUN_XSTATE_SIZE, &guest_xstate_pa) != 0 ||
	    (gprs_pa & ~AMDVM_VMRUN_PAGE_MASK) == host_vmcb_pa ||
	    (gprs_pa & ~AMDVM_VMRUN_PAGE_MASK) == guest_vmcb_pa ||
	    (host_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) == host_vmcb_pa ||
	    (host_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) == guest_vmcb_pa ||
	    (guest_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) == host_vmcb_pa ||
	    (guest_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) == guest_vmcb_pa ||
	    (host_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) ==
	    (guest_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) ||
	    (host_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) ==
	    (gprs_pa & ~AMDVM_VMRUN_PAGE_MASK) ||
	    (guest_xstate_pa & ~AMDVM_VMRUN_PAGE_MASK) ==
	    (gprs_pa & ~AMDVM_VMRUN_PAGE_MASK)) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	if ((amdvm_vmcb_intercept_misc2(guest_vmcb) &
	    AMDVM_SVM_INTERCEPT_XSETBV) == 0) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	if (!amdvm_vmcb_external_interrupt_exit_enabled(guest_vmcb)) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	/*
	 * A guest may clear RFLAGS.IF, including at reset. Require V_INTR_MASKING
	 * so that this cannot also mask physical host IPIs needed to return control
	 * to XNU (e.g. a pmap TLB shootdown).
	 */
	if ((guest_vmcb->bytes[AMDVM_VMCB_INTERRUPT_CONTROL + 3u] &
	    (AMDVM_VMCB_V_INTR_MASKING >> 24)) == 0) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	if ((amdvm_vmcb_intercept_u16(guest_vmcb,
	    AMDVM_VMCB_INTERCEPT_CR_WRITE) & ((1u << 0) | (1u << 4))) !=
	    ((1u << 0) | (1u << 4)) ||
	    amdvm_vmcb_intercept_u16(guest_vmcb,
	    AMDVM_VMCB_INTERCEPT_DR_READ) != UINT16_MAX ||
	    amdvm_vmcb_intercept_u16(guest_vmcb,
	    AMDVM_VMCB_INTERCEPT_DR_WRITE) != UINT16_MAX ||
	    (amdvm_vmcb_get_u64(guest_vmcb, AMDVM_VMCB_GUEST_DR7) &
	    (UINT64_C(1) << 13)) != 0) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}

	/* Stay on this CPU; this entry is only for the synchronous user-client path. */
	if (!amdvm_xstate_supported()) {
		return AMDVM_VMRUN_XSTATE_UNSUPPORTED;
	}
	xcr0 = amdvm_current_xcr0();
	if ((xcr0 & 1u) == 0 ||
	    amdvm_xstate_validate_standard(guest_xstate_area,
	    AMDVM_VMRUN_XSTATE_SIZE, xcr0) != 0) {
		return AMDVM_VMRUN_INVALID_ARGUMENT;
	}
	if (!amdvm_ibpb_supported()) {
		return AMDVM_VMRUN_SPECULATION_UNSUPPORTED;
	}
	/*
	 * XNU may set CR0.TS again after the IOUserClient's initial preflight.
	 * Capture at the last C boundary before the assembly world switch. With
	 * the caller pinned and local interrupts disabled, XSAVE raises the normal
	 * XNU #NM path if TS is set; XNU restores this thread's PCB state and the
	 * retried XSAVE records that live state. Never clear TS with CLTS or read
	 * foreign/stale SIMD registers. A set EM bit, changed XCR0, or unsuccessful
	 * post-capture state remains a hard failure. Assembly independently checks
	 * CR0.TS/EM once more before CLGI.
	 */
	capture_status = amdvm_vmrun_xstate_capture_current_thread(host_xstate_area,
	    &captured_xcr0, &capture_cr0_before, &capture_cr0_after);
	if (capture_status != AMDVM_VMRUN_SUCCESS) {
		return capture_status;
	}
	if (captured_xcr0 != xcr0 ||
	    (capture_cr0_after & (AMDVM_CR0_TS | AMDVM_CR0_EM)) != 0) {
		return AMDVM_VMRUN_FPU_NOT_READY;
	}
	int asm_status = amdvm_vmrun_once_asm(host_vmcb_pa, guest_vmcb_pa, guest_vmcb,
	    guest_gprs, host_xstate_area, guest_xstate_area);
	if (asm_status == AMDVM_VMRUN_SUCCESS) {
		__sync_synchronize();
		exit_snapshot->code = amdvm_vmcb_get_u64(guest_vmcb,
		    AMDVM_VMCB_EXIT_CODE);
		exit_snapshot->info1 = amdvm_vmcb_get_u64(guest_vmcb,
		    AMDVM_VMCB_EXIT_INFO1);
		exit_snapshot->info2 = amdvm_vmcb_get_u64(guest_vmcb,
		    AMDVM_VMCB_EXIT_INFO2);
		exit_snapshot->next_rip = amdvm_vmcb_get_u64(guest_vmcb,
		    AMDVM_VMCB_NEXT_RIP);
		exit_snapshot->guest_rip = amdvm_vmcb_get_u64(guest_vmcb,
		    AMDVM_VMCB_GUEST_RIP);
		exit_snapshot->guest_rflags = amdvm_vmcb_get_u64(guest_vmcb,
		    AMDVM_VMCB_GUEST_RFLAGS);
		exit_snapshot->instruction_length =
		    guest_vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_LENGTH];
		for (size_t decodeIndex = 0;
		    decodeIndex < AMDVM_VMCB_DECODE_ASSIST_MAX; ++decodeIndex) {
			exit_snapshot->instruction_bytes[decodeIndex] =
			    guest_vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_BYTES +
			    decodeIndex];
		}
	}
	return (amdvm_vmrun_status_t)asm_status;
}
