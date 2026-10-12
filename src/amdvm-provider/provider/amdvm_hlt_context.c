#include "amdvm_hlt_context.h"
#include "amdvm_quarantine.h"
#include "../amdvm_probe_abi.h"
#include <IOKit/IOLib.h>
#include <i386/machine_routines.h>

#include <string.h>

#define AMDVM_HLT_PAYLOAD_PAGE 0u
#define AMDVM_HLT_HOST_VMCB_PAGE 1u
#define AMDVM_HLT_GUEST_VMCB_PAGE 2u
#define AMDVM_HLT_HOST_XSTATE_PAGE 3u
#define AMDVM_HLT_GUEST_XSTATE_PAGE 4u
#define AMDVM_HLT_IOPM_FIRST_PAGE 5u
#define AMDVM_HLT_IOPM_PAGE_COUNT 3u
#define AMDVM_HLT_MSRPM_FIRST_PAGE 8u
#define AMDVM_HLT_MSRPM_PAGE_COUNT 2u
#define AMDVM_HLT_GPRS_PAGE 10u

#define AMDVM_HLT_NPT_GUEST_PHYS_BITS 32u
#define AMDVM_HLT_NPT_ASID 1u
#define AMDVM_MSR_IA32_PAT UINT32_C(0x277)

extern int cpu_number(void);

static amdvm_quarantine_registry_t gAmdvmQuarantine;

static uint64_t
amdvm_hlt_read_pat(void)
{
	uint32_t low, high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(AMDVM_MSR_IA32_PAT));
	return ((uint64_t)high << 32) | low;
}

static uint64_t
amdvm_hlt_read_msr(uint32_t msr)
{
	uint32_t low, high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

static void
amdvm_hlt_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c,
	uint32_t *d)
{
	uint32_t eax = leaf, ebx, ecx = 0, edx;
	__asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx),
	    "=d"(edx));
	*a = eax;
	*b = ebx;
	*c = ecx;
	*d = edx;
}

/* Read on a stable CPU; RUN rechecks this snapshot on its pinned CPU. */
static int
amdvm_hlt_read_host_mtrr_on_cpu(amdvm_host_mtrr_state_t *state)
{
	static const uint32_t fixed_msrs[AMDVM_HOST_MTRR_FIXED_COUNT] = {
		0x250u, 0x258u, 0x259u, 0x268u, 0x269u, 0x26au,
		0x26bu, 0x26cu, 0x26du, 0x26eu, 0x26fu
	};
	unsigned attempt;
	if (state == NULL) return -1;
	for (attempt = 0; attempt < 3u; ++attempt) {
		int before = cpu_number();
		uint32_t eax, ebx, ecx, edx;
		uint32_t feature_edx;
		uint64_t cap;
		unsigned i;
		memset(state, 0, sizeof(*state));
		amdvm_hlt_cpuid(0u, &eax, &ebx, &ecx, &edx);
		int amd_vendor = ebx == UINT32_C(0x68747541) &&
		    edx == UINT32_C(0x69746e65) && ecx == UINT32_C(0x444d4163);
		amdvm_hlt_cpuid(1u, &eax, &ebx, &ecx, &feature_edx);
		unsigned family = (eax >> 8) & 0xfu;
		if (family == 0xfu) family += (eax >> 20) & 0xffu;
		state->amd_extended = (uint8_t)(amd_vendor && family >= 0xfu);
		(void)ebx;
		(void)ecx;
		if ((feature_edx & (1u << 12)) == 0) return -1;
		amdvm_hlt_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx);
		if (eax < 0x80000008u) return -1;
		amdvm_hlt_cpuid(0x80000008u, &eax, &ebx, &ecx, &edx);
		state->phys_bits = (uint8_t)eax;
		cap = amdvm_hlt_read_msr(0xfeu);
		state->variable_count = (uint8_t)(cap & 0xffu);
		state->fixed_supported = (uint8_t)((cap >> 8) & 1u);
		state->default_type = amdvm_hlt_read_msr(0x2ffu);
		/* AMD family 0Fh+: read-only; never change host cache/routing MSRs. */
		if (state->amd_extended) {
			state->amd_syscfg = amdvm_hlt_read_msr(0xc0010010u);
			state->amd_top_mem = amdvm_hlt_read_msr(0xc001001au);
			state->amd_top_mem2 = amdvm_hlt_read_msr(0xc001001du);
		}
		if (state->variable_count == 0 ||
		    state->variable_count > AMDVM_HOST_MTRR_VARIABLE_MAX) {
			return -1;
		}
		for (i = 0; i < state->variable_count; ++i) {
			state->variable_base[i] = amdvm_hlt_read_msr(0x200u + i * 2u);
			state->variable_mask[i] = amdvm_hlt_read_msr(0x201u + i * 2u);
		}
		if (state->fixed_supported) {
			for (i = 0; i < AMDVM_HOST_MTRR_FIXED_COUNT; ++i) {
				state->fixed[i] = amdvm_hlt_read_msr(fixed_msrs[i]);
			}
		}
		if (before >= 0 && before == cpu_number() &&
		    amdvm_host_mtrr_validate(state) == 0) {
			return 0;
		}
	}
	return -1;
}

static int
amdvm_hlt_read_host_mtrr_stable(amdvm_host_mtrr_state_t *state)
{
	/* A CPU-id check alone cannot exclude migration away and back. This short
	 * capture performs only CPUID, RDMSR and bounded local validation. */
	boolean_t previous = ml_set_interrupts_enabled(FALSE);
	int rc = amdvm_hlt_read_host_mtrr_on_cpu(state);
	(void)ml_set_interrupts_enabled(previous);
	return rc;
}

static void
amdvm_hlt_log_host_mtrr_snapshot(const amdvm_host_mtrr_state_t *state)
{
	IOLog("AMDVM MTRR snapshot amd_ext=%u def=0x%llx syscfg=0x%llx top_mem=0x%llx top_mem2=0x%llx phys_bits=%u var_count=%u\n",
	    state->amd_extended, (unsigned long long)state->default_type,
	    (unsigned long long)state->amd_syscfg,
	    (unsigned long long)state->amd_top_mem,
	    (unsigned long long)state->amd_top_mem2,
	    state->phys_bits, state->variable_count);
	for (unsigned i = 0; i < state->variable_count &&
	    i < AMDVM_HOST_MTRR_VARIABLE_MAX; ++i) {
		IOLog("AMDVM MTRR variable index=%u base=0x%llx mask=0x%llx valid=%u\n",
		    i, (unsigned long long)state->variable_base[i],
		    (unsigned long long)state->variable_mask[i],
		    (unsigned)((state->variable_mask[i] &
		    AMDVM_HOST_MTRR_MASK_VALID) != 0));
	}
}

static int
amdvm_quarantine_unpin(amdvm_pinned_user_region_t *region, void *opaque)
{
	(void)opaque;
	return amdvm_physmem_unpin_user_region(region);
}

static int
amdvm_quarantine_reserve(amdvm_hlt_context_t *context)
{
	return amdvm_quarantine_registry_reserve(&gAmdvmQuarantine,
	    &context->quarantine_slot, &context->quarantine_generation,
	    amdvm_quarantine_unpin, NULL);
}

static int
amdvm_quarantine_release(amdvm_hlt_context_t *context)
{
	return amdvm_quarantine_registry_release(&gAmdvmQuarantine,
	    context->quarantine_slot, context->quarantine_generation);
}

static int
amdvm_quarantine_recover_context(amdvm_hlt_context_t *context)
{
	if (context->quarantined == 0) return 0;
	if (amdvm_quarantine_registry_recover(&gAmdvmQuarantine,
	    context->quarantine_slot, context->quarantine_generation,
	    amdvm_quarantine_unpin, NULL) != 0) {
		return -1;
	}
	memset(context, 0, sizeof(*context));
	context->quarantine_slot = UINT32_MAX;
	return 0;
}

static int
amdvm_quarantine_transfer(amdvm_hlt_context_t *context)
{
	return amdvm_quarantine_registry_transfer(&gAmdvmQuarantine,
	    context->quarantine_slot, context->quarantine_generation,
	    context->guest_ranges, context->guest_range_count);
}

size_t
amdvm_hlt_context_quarantine_count(void)
{
	return amdvm_quarantine_registry_count(&gAmdvmQuarantine);
}

size_t
amdvm_hlt_context_retry_quarantined(void)
{
	amdvm_quarantine_registry_retry(&gAmdvmQuarantine,
	    amdvm_quarantine_unpin, NULL);
	return amdvm_quarantine_registry_count(&gAmdvmQuarantine);
}

int
amdvm_hlt_context_forget_quarantined(amdvm_hlt_context_t *context)
{
	if (context == NULL || context->quarantined == 0 ||
	    amdvm_quarantine_registry_forget(&gAmdvmQuarantine,
	    context->quarantine_slot, context->quarantine_generation,
	    amdvm_quarantine_unpin, NULL) != 0) {
		return -1;
	}
	memset(context, 0, sizeof(*context));
	context->quarantine_slot = UINT32_MAX;
	return 0;
}

void
amdvm_hlt_context_quarantine_stats(size_t *context_count,
	size_t *range_count, uint64_t *guest_bytes)
{
	amdvm_quarantine_registry_stats(&gAmdvmQuarantine, context_count,
	    range_count, guest_bytes);
}

_Static_assert(AMDVM_HLT_GPRS_PAGE + 1u == AMDVM_HLT_CONTEXT_PAGES,
	"HLT context layout must account for every backing page");

static void *
amdvm_hlt_page(amdvm_hlt_context_t *context, size_t page_index)
{
	return (uint8_t *)context->backing.virtual_address +
	    page_index * AMDVM_PAGE_SIZE;
}

static uint64_t
amdvm_hlt_page_pa(const amdvm_hlt_context_t *context, size_t page_index)
{
	return context->backing.physical_address +
	    (uint64_t)page_index * AMDVM_PAGE_SIZE;
}

static int
amdvm_hlt_host_phys_bits(void)
{
	uint32_t max_extended, a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(max_extended), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000000u), "c"(0));
	if (max_extended < 0x80000008u) {
		return 0;
	}
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000008u), "c"(0));
	return (int)(a & 0xffu);
}

static int
amdvm_hlt_context_init_profile(amdvm_hlt_context_t *context,
	int qemu_reset_vector)
{
	uint8_t *iopm, *msrpm;
	uint8_t *payload;
	uint64_t payload_page_pas[1];
	amdvm_npt_range_t initial_npt_range;
	size_t npt_page_count = 0;
	int host_phys_bits;
	int rc;

	if (context == NULL || context->initialized != 0 ||
	    context->backing.memory_descriptor != NULL ||
	    context->npt_backing.memory_descriptor != NULL ||
	    context->hsave.memory_descriptor != NULL) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	if (amdvm_quarantine_recover_context(context) != 0) {
		return AMDVM_HLT_CONTEXT_UNPIN_FAILED;
	}
	memset(context, 0, sizeof(*context));
	context->quarantine_slot = UINT32_MAX;
	if (amdvm_quarantine_reserve(context) != 0) {
		return AMDVM_HLT_CONTEXT_ALLOC_FAILED;
	}
	if (amdvm_physmem_alloc_contiguous(&context->backing,
	    AMDVM_HLT_CONTEXT_PAGES * AMDVM_PAGE_SIZE) != 0) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto fail;
	}
	if (amdvm_physmem_validate(&context->backing) != 0) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto fail;
	}
	if (amdvm_physmem_alloc_contiguous(&context->hsave,
	    AMDVM_PAGE_SIZE) != 0) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto fail;
	}

	host_phys_bits = amdvm_hlt_host_phys_bits();
	if (host_phys_bits < 32 || host_phys_bits > 52) {
		rc = AMDVM_HLT_CONTEXT_INVALID;
		goto fail;
	}
	payload = amdvm_hlt_page(context, AMDVM_HLT_PAYLOAD_PAGE);
	context->guest_payload_pa = amdvm_hlt_page_pa(context,
	    AMDVM_HLT_PAYLOAD_PAGE);
	context->guest_payload_gpa = qemu_reset_vector ?
	    AMDVM_VCPU_RESET_ROM_GPA : 0;
	context->guest_payload_offset = qemu_reset_vector ?
	    AMDVM_VCPU_RESET_ROM_OFFSET : 0;
	context->initial_rip = qemu_reset_vector ?
	    AMDVM_VCPU_RESET_VECTOR_RIP : 0;
	payload_page_pas[0] = context->guest_payload_pa;
	/* Never expose stale kernel bytes through the guest payload page. */
	bzero(payload, AMDVM_PAGE_SIZE);
	/* Seed HLT at the selected entry; create may replace it with the fixed ROM. */
	payload[context->guest_payload_offset] = 0xf4u;
	context->guest_payload = payload;
	initial_npt_range.guest_gpa = context->guest_payload_gpa;
	initial_npt_range.size = AMDVM_PAGE_SIZE;
	rc = amdvm_npt_required_pages(&initial_npt_range, 1,
	    AMDVM_HLT_NPT_GUEST_PHYS_BITS, &npt_page_count);
	if (rc != AMDVM_NPT_OK || npt_page_count == 0 ||
	    npt_page_count > SIZE_MAX / AMDVM_PAGE_SIZE ||
	    amdvm_physmem_alloc_contiguous(&context->npt_backing,
	    npt_page_count * AMDVM_PAGE_SIZE) != 0 ||
	    amdvm_physmem_validate(&context->npt_backing) != 0) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}
	rc = amdvm_npt_init(&context->npt,
	    context->npt_backing.virtual_address,
	    context->npt_backing.physical_address, npt_page_count,
	    AMDVM_HLT_NPT_GUEST_PHYS_BITS, (uint8_t)host_phys_bits);
	if (rc != AMDVM_NPT_OK) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}
	rc = amdvm_npt_map_4k_pages(&context->npt, context->guest_payload_gpa,
	    payload_page_pas, 1,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE);
	if (rc != AMDVM_NPT_OK || amdvm_npt_validate_single_4k(&context->npt,
	    context->guest_payload_gpa, context->guest_payload_pa,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) != AMDVM_NPT_OK) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}

	context->host_vmcb = (amdvm_vmcb_t *)amdvm_hlt_page(context,
	    AMDVM_HLT_HOST_VMCB_PAGE);
	context->guest_vmcb = (amdvm_vmcb_t *)amdvm_hlt_page(context,
	    AMDVM_HLT_GUEST_VMCB_PAGE);
	context->host_vmcb_pa = amdvm_hlt_page_pa(context,
	    AMDVM_HLT_HOST_VMCB_PAGE);
	context->guest_vmcb_pa = amdvm_hlt_page_pa(context,
	    AMDVM_HLT_GUEST_VMCB_PAGE);
	context->host_xstate = amdvm_hlt_page(context,
	    AMDVM_HLT_HOST_XSTATE_PAGE);
	context->guest_xstate = amdvm_hlt_page(context,
	    AMDVM_HLT_GUEST_XSTATE_PAGE);
	context->host_xstate_pa = amdvm_hlt_page_pa(context,
	    AMDVM_HLT_HOST_XSTATE_PAGE);
	context->guest_xstate_pa = amdvm_hlt_page_pa(context,
	    AMDVM_HLT_GUEST_XSTATE_PAGE);
	context->hsave_pa = context->hsave.physical_address;
	context->guest_gprs = (amdvm_guest_gprs_t *)amdvm_hlt_page(context,
	    AMDVM_HLT_GPRS_PAGE);
	iopm = (uint8_t *)amdvm_hlt_page(context, AMDVM_HLT_IOPM_FIRST_PAGE);
	msrpm = (uint8_t *)amdvm_hlt_page(context, AMDVM_HLT_MSRPM_FIRST_PAGE);

	if (amdvm_svm_init_all_intercept_bitmaps(iopm,
	    AMDVM_HLT_IOPM_PAGE_COUNT * AMDVM_PAGE_SIZE, msrpm,
	    AMDVM_HLT_MSRPM_PAGE_COUNT * AMDVM_PAGE_SIZE) != 0) {
		rc = AMDVM_HLT_CONTEXT_INVALID;
		goto fail;
	}
	if (amdvm_vmcb_configure_probe(context->guest_vmcb,
	    amdvm_npt_root_pa(&context->npt), amdvm_hlt_page_pa(context,
	    AMDVM_HLT_IOPM_FIRST_PAGE), amdvm_hlt_page_pa(context,
	    AMDVM_HLT_MSRPM_FIRST_PAGE), AMDVM_HLT_NPT_ASID) != 0 ||
	    (qemu_reset_vector ?
	    amdvm_vmcb_configure_qemu_reset_vector_v1(context->guest_vmcb) :
	    amdvm_vmcb_configure_realmode_test_v1(context->guest_vmcb)) != 0) {
		rc = AMDVM_HLT_CONTEXT_VMCB_FAILED;
		goto fail;
	}
	context->initialized = 1;
	return AMDVM_HLT_CONTEXT_OK;

fail:
	amdvm_physmem_free(&context->npt_backing);
	amdvm_physmem_free(&context->hsave);
	amdvm_physmem_free(&context->backing);
	(void)amdvm_quarantine_release(context);
	memset(context, 0, sizeof(*context));
	return rc;
}

int
amdvm_hlt_context_init(amdvm_hlt_context_t *context)
{
	return amdvm_hlt_context_init_profile(context, 0);
}

int
amdvm_hlt_context_init_qemu_reset_vector_v1(amdvm_hlt_context_t *context)
{
	return amdvm_hlt_context_init_profile(context, 1);
}

static int
amdvm_hlt_pa_is_control_memory(const amdvm_hlt_context_t *context,
	uint64_t physical_address)
{
	const amdvm_physmem_region_t *regions[] = {
		&context->backing, &context->npt_backing, &context->hsave
	};
	size_t i;

	for (i = 0; i < sizeof(regions) / sizeof(regions[0]); ++i) {
		const amdvm_physmem_region_t *region = regions[i];
		if (region->memory_descriptor != NULL &&
		    physical_address >= region->physical_address &&
		    physical_address - region->physical_address < region->size) {
			return 1;
		}
	}
	return 0;
}

static int
amdvm_hlt_pa_is_region(const amdvm_physmem_region_t *region,
	uint64_t physical_address)
{
	return region->memory_descriptor != NULL &&
	    physical_address >= region->physical_address &&
	    physical_address - region->physical_address < region->size;
}

static int
amdvm_hlt_region_mtrr_is_wb(const amdvm_host_mtrr_state_t *mtrr,
	const amdvm_physmem_region_t *region)
{
	size_t page_count, i;
	if (region == NULL || region->memory_descriptor == NULL ||
	    amdvm_physmem_validate(region) != 0) return -1;
	page_count = region->size / AMDVM_PAGE_SIZE;
	for (i = 0; i < page_count; ++i) {
		uint8_t type;
		uint64_t pa = amdvm_physmem_page_pa(region, i);
		if (pa == 0 || amdvm_host_mtrr_effective_type(mtrr, pa, &type) != 0 ||
		    type != AMDVM_HOST_MTRR_WB) return -1;
	}
	return 0;
}

static void
amdvm_hlt_log_mtrr_region_failure(const char *stage,
	const amdvm_host_mtrr_state_t *mtrr,
	const amdvm_physmem_region_t *region)
{
	size_t page_count, i;
	if (region == NULL || region->memory_descriptor == NULL ||
	    amdvm_physmem_validate(region) != 0) {
		IOLog("AMDVM MTRR reject stage=%s region-invalid\n", stage);
		return;
	}
	page_count = region->size / AMDVM_PAGE_SIZE;
	for (i = 0; i < page_count; ++i) {
		uint8_t type = 0xffu;
		uint64_t pa = amdvm_physmem_page_pa(region, i);
		if (pa == 0 || amdvm_host_mtrr_effective_type(mtrr, pa, &type) != 0 ||
		    type != AMDVM_HOST_MTRR_WB) {
			IOLog("AMDVM MTRR reject stage=%s page=%llu pa=0x%llx type=0x%x\n",
			    stage, (unsigned long long)i,
			    (unsigned long long)pa, type);
			return;
		}
	}
	IOLog("AMDVM MTRR reject stage=%s region-check-failed\n", stage);
}

int
amdvm_hlt_context_map_user_ranges(amdvm_hlt_context_t *context,
	const amdvm_vcpu_memory_range_t *ranges, size_t range_count, void *task,
	size_t *npt_page_count_out)
{
	amdvm_pinned_user_region_t pinned[AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES] = {};
	amdvm_npt_range_t npt_ranges[AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES + 1u];
	amdvm_physmem_region_t new_npt_backing = {};
	amdvm_npt_t new_npt = {};
	amdvm_host_mtrr_state_t host_mtrr = {};
	uint64_t total_guest_pages = 0;
	uint64_t *host_pas = NULL;
	size_t npt_page_count = 0;
	size_t host_pa_count = 0;
	size_t npt_range_count;
	size_t i;
	int host_phys_bits;
	int rc = AMDVM_HLT_CONTEXT_INVALID;

	if (context == NULL || ranges == NULL || task == NULL ||
	    npt_page_count_out == NULL || context->initialized == 0 ||
	    context->lease.active != 0 || context->guest_range_count != 0 ||
	    range_count == 0 ||
	    range_count > AMDVM_HLT_CONTEXT_MAX_GUEST_RANGES ||
	    !__sync_bool_compare_and_swap(&context->busy, 0u, 1u)) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	*npt_page_count_out = 0;
	npt_range_count = range_count;

	for (i = 0; i < range_count; ++i) {
		const amdvm_vcpu_memory_range_t *range = &ranges[i];
		uint64_t end;
		uint64_t page_count;

		if (range->reserved != 0 || range->size == 0 ||
		    range->size > SIZE_MAX ||
		    (range->guest_physical_address & (AMDVM_PAGE_SIZE - 1u)) != 0 ||
		    (range->user_virtual_address & (AMDVM_PAGE_SIZE - 1u)) != 0 ||
		    (range->size & (AMDVM_PAGE_SIZE - 1u)) != 0 ||
		    range->guest_physical_address >= (UINT64_C(1) << 48) ||
		    range->size > (UINT64_C(1) << 48) -
		    range->guest_physical_address ||
		    range->user_virtual_address == 0 ||
		    range->user_virtual_address > UINT64_MAX - range->size ||
		    (range->flags & AMDVM_VCPU_MEMORY_READ) == 0 ||
		    (range->flags & ~(AMDVM_VCPU_MEMORY_READ |
		    AMDVM_VCPU_MEMORY_WRITE | AMDVM_VCPU_MEMORY_EXEC)) != 0 ||
		    (i != 0 && range->guest_physical_address <
		    npt_ranges[i - 1u].guest_gpa + npt_ranges[i - 1u].size)) {
			rc = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			goto fail;
		}
		for (size_t j = 0; j < i; ++j) {
			uint64_t prior_start = ranges[j].user_virtual_address;
			uint64_t prior_end = prior_start + ranges[j].size;
			if (range->user_virtual_address < prior_end &&
			    prior_start < range->user_virtual_address + range->size &&
			    (range->flags != (AMDVM_VCPU_MEMORY_READ |
			    AMDVM_VCPU_MEMORY_EXEC) ||
			    ranges[j].flags != (AMDVM_VCPU_MEMORY_READ |
			    AMDVM_VCPU_MEMORY_EXEC))) {
				rc = AMDVM_HLT_CONTEXT_INVALID_GUEST;
				goto fail;
			}
		}
		end = range->guest_physical_address + range->size;
		page_count = range->size / AMDVM_PAGE_SIZE;
		if (page_count > UINT64_MAX - total_guest_pages) {
			rc = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			goto fail;
		}
		total_guest_pages += page_count;
		if (total_guest_pages > AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES /
		    AMDVM_PAGE_SIZE) {
			rc = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			goto fail;
		}
		npt_ranges[i].guest_gpa = range->guest_physical_address;
		npt_ranges[i].size = range->size;
		if (range->guest_physical_address <
		    AMDVM_VCPU_RESET_ROM_GPA + AMDVM_PAGE_SIZE &&
		    AMDVM_VCPU_RESET_ROM_GPA < end) {
			/* The reset page is provider-owned ROM, never caller RAM. */
			rc = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			goto fail;
		}
	}

	/* Preserve the private RX reset page when replacing the initial NPT. */
	{
		size_t insert_at = 0;
		while (insert_at < range_count &&
		    npt_ranges[insert_at].guest_gpa < context->guest_payload_gpa) {
			++insert_at;
		}
		memmove(&npt_ranges[insert_at + 1u], &npt_ranges[insert_at],
		    (range_count - insert_at) * sizeof(npt_ranges[0]));
		npt_ranges[insert_at].guest_gpa = context->guest_payload_gpa;
		npt_ranges[insert_at].size = AMDVM_PAGE_SIZE;
		++npt_range_count;
	}
	rc = amdvm_npt_required_pages(npt_ranges, npt_range_count, 48,
	    &npt_page_count);
	if (rc != AMDVM_NPT_OK || npt_page_count == 0 ||
	    npt_page_count > UINT32_MAX / AMDVM_PAGE_SIZE) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}
	host_phys_bits = amdvm_hlt_host_phys_bits();
	if (host_phys_bits < 32 || host_phys_bits > 52) {
		rc = AMDVM_HLT_CONTEXT_INVALID;
		goto fail;
	}
	int snapshot_status = amdvm_hlt_read_host_mtrr_stable(&host_mtrr);
	amdvm_hlt_log_host_mtrr_snapshot(&host_mtrr);
	if (snapshot_status != 0 ||
	    host_mtrr.phys_bits != (uint8_t)host_phys_bits) {
		IOLog("AMDVM MTRR reject stage=host-snapshot phys_bits=%d mtrr_bits=%u\n",
		    host_phys_bits, host_mtrr.phys_bits);
		rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		goto fail;
	}
	if (amdvm_hlt_region_mtrr_is_wb(&host_mtrr, &context->backing) != 0) {
		amdvm_hlt_log_mtrr_region_failure("context-backing", &host_mtrr,
		    &context->backing);
		rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		goto fail;
	}
	if (amdvm_hlt_region_mtrr_is_wb(&host_mtrr, &context->hsave) != 0) {
		amdvm_hlt_log_mtrr_region_failure("hsave", &host_mtrr,
		    &context->hsave);
		rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		goto fail;
	}

	for (i = 0; i < range_count; ++i) {
		if (amdvm_physmem_pin_user_region(&pinned[i],
		    ranges[i].user_virtual_address, (size_t)ranges[i].size,
		    task) != 0) {
			rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
			goto fail;
		}
	}
	if (total_guest_pages > SIZE_MAX / sizeof(*host_pas)) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto fail;
	}
	host_pa_count = (size_t)total_guest_pages;
	host_pas = (uint64_t *)IOMalloc(host_pa_count * sizeof(*host_pas));
	if (host_pas == NULL) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto fail;
	}
	{
		size_t cursor = 0;
		for (i = 0; i < range_count; ++i) {
			size_t pages = (size_t)(ranges[i].size / AMDVM_PAGE_SIZE);
			size_t page_index;
			for (page_index = 0; page_index < pages; ++page_index) {
				uint8_t host_type = 0xffu;
				int mtrr_status;
				if (amdvm_physmem_user_page_pa(&pinned[i], page_index,
				    (uint8_t)host_phys_bits, &host_pas[cursor]) != 0 ||
				    amdvm_hlt_pa_is_control_memory(context,
				    host_pas[cursor]) != 0) {
					rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
					goto fail;
				}
				mtrr_status = amdvm_host_mtrr_effective_type(&host_mtrr,
				    host_pas[cursor], &host_type);
				if (mtrr_status != 0 || host_type != AMDVM_HOST_MTRR_WB) {
					IOLog("AMDVM MTRR reject stage=guest-page range=%u page=%llu gpa=0x%llx hva=0x%llx pa=0x%llx lookup=%d type=0x%x def=0x%llx var_count=%u fixed=%u phys_bits=%u\n",
					    (unsigned)i, (unsigned long long)page_index,
					    (unsigned long long)(ranges[i].guest_physical_address + page_index * AMDVM_PAGE_SIZE),
					    (unsigned long long)(ranges[i].user_virtual_address + page_index * AMDVM_PAGE_SIZE),
					    (unsigned long long)host_pas[cursor], mtrr_status,
					    host_type, (unsigned long long)host_mtrr.default_type,
					    host_mtrr.variable_count, host_mtrr.fixed_supported,
					    host_mtrr.phys_bits);
					if (host_pas[cursor] >= UINT64_C(0x100000)) {
						uint64_t address_mask =
						    ((UINT64_C(1) << host_mtrr.phys_bits) - 1u) &
						    ~UINT64_C(0xfff);
						for (unsigned m = 0; m < host_mtrr.variable_count; ++m) {
							uint64_t mask = host_mtrr.variable_mask[m];
							if ((mask & AMDVM_HOST_MTRR_MASK_VALID) != 0 &&
							    (host_pas[cursor] & address_mask & mask) ==
							    (host_mtrr.variable_base[m] & address_mask & mask)) {
								IOLog("AMDVM MTRR match index=%u base=0x%llx mask=0x%llx\n",
								    m,
								    (unsigned long long)host_mtrr.variable_base[m],
								    (unsigned long long)mask);
							}
						}
					}
					rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
					goto fail;
				}
				++cursor;
			}
		}
	}
	/*
	 * FlatView aliases are accepted only for immutable RX backing such as BIOS
	 * ROM aliases. Encode the write bit next to each page number so the pure
	 * validator can allow immutable duplicate HPAs without losing policy.
	 */
	{
		size_t cursor = 0;
		for (i = 0; i < range_count; ++i) {
			size_t pages = (size_t)(ranges[i].size / AMDVM_PAGE_SIZE);
			size_t page_index;
			for (page_index = 0; page_index < pages; ++page_index) {
				uint64_t page_number = host_pas[cursor] >> 12;
				uint64_t rom_aliasable = ranges[i].flags ==
				    (AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_EXEC);
				host_pas[cursor++] = (page_number << 1) | rom_aliasable;
			}
		}
	}
	if (amdvm_npt_validate_backing_aliases(host_pas, host_pa_count) !=
	    AMDVM_NPT_OK) {
		rc = AMDVM_HLT_CONTEXT_INVALID_GUEST;
		goto fail;
	}
	if (amdvm_physmem_alloc_contiguous(&new_npt_backing,
	    npt_page_count * AMDVM_PAGE_SIZE) != 0 ||
	    amdvm_physmem_validate(&new_npt_backing) != 0) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto fail;
	}
	if (amdvm_hlt_region_mtrr_is_wb(&host_mtrr, &new_npt_backing) != 0) {
		amdvm_hlt_log_mtrr_region_failure("npt-backing", &host_mtrr,
		    &new_npt_backing);
		rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		goto fail;
	}
	rc = amdvm_npt_init(&new_npt, new_npt_backing.virtual_address,
	    new_npt_backing.physical_address, npt_page_count, 48,
	    (uint8_t)host_phys_bits);
	if (rc != AMDVM_NPT_OK) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}

	for (i = 0; i < range_count; ++i) {
		uint64_t page_count = ranges[i].size / AMDVM_PAGE_SIZE;
		uint64_t page_index;
		uint32_t permissions = AMDVM_NPT_READ;
		if (ranges[i].flags & AMDVM_VCPU_MEMORY_WRITE) {
			permissions |= AMDVM_NPT_WRITE;
		}
		if (ranges[i].flags & AMDVM_VCPU_MEMORY_EXEC) {
			permissions |= AMDVM_NPT_EXECUTE;
		}
		for (page_index = 0; page_index < page_count; ++page_index) {
			uint64_t host_pa;
			if (amdvm_physmem_user_page_pa(&pinned[i],
			    (size_t)page_index, (uint8_t)host_phys_bits,
			    &host_pa) != 0 ||
			    amdvm_hlt_pa_is_control_memory(context, host_pa) ||
			    amdvm_hlt_pa_is_region(&new_npt_backing, host_pa) ||
			    amdvm_npt_map_4k(&new_npt,
			    ranges[i].guest_physical_address +
			    page_index * AMDVM_PAGE_SIZE, host_pa, permissions) !=
			    AMDVM_NPT_OK) {
				rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
				goto fail;
			}
		}
	}
	if (amdvm_npt_map_4k(&new_npt, context->guest_payload_gpa,
	    context->guest_payload_pa, AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) !=
	    AMDVM_NPT_OK) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}
	{
		uint8_t host_type;
		if (amdvm_host_mtrr_effective_type(&host_mtrr,
		    context->guest_payload_pa, &host_type) != 0 ||
		    host_type != AMDVM_HOST_MTRR_WB) {
		IOLog("AMDVM MTRR reject stage=reset-rom pa=0x%llx type=0x%x\n",
			    (unsigned long long)context->guest_payload_pa, host_type);
			rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
			goto fail;
		}
	}
	/* Verify the entire candidate map before changing the VMCB root. */
	if (new_npt.pages_used != npt_page_count) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}
	for (i = 0; i < range_count; ++i) {
		uint64_t page_count = ranges[i].size / AMDVM_PAGE_SIZE;
		uint64_t page_index;
		uint32_t permissions = AMDVM_NPT_READ;
		if (ranges[i].flags & AMDVM_VCPU_MEMORY_WRITE)
			permissions |= AMDVM_NPT_WRITE;
		if (ranges[i].flags & AMDVM_VCPU_MEMORY_EXEC)
			permissions |= AMDVM_NPT_EXECUTE;
		for (page_index = 0; page_index < page_count; ++page_index) {
			uint64_t host_pa;
			if (amdvm_physmem_user_page_pa(&pinned[i],
			    (size_t)page_index, (uint8_t)host_phys_bits, &host_pa) != 0 ||
			    amdvm_npt_validate_4k_mapping(&new_npt,
			    ranges[i].guest_physical_address +
			    page_index * AMDVM_PAGE_SIZE, host_pa, permissions) !=
			    AMDVM_NPT_OK) {
				rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
				goto fail;
			}
		}
	}
	if (amdvm_npt_validate_4k_mapping(&new_npt,
	    context->guest_payload_gpa, context->guest_payload_pa,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) != AMDVM_NPT_OK) {
		rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		goto fail;
	}

	{
		amdvm_physmem_region_t old_npt_backing = context->npt_backing;
		context->npt_backing = new_npt_backing;
		context->npt = new_npt;
		amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_NPT_ROOT,
		    amdvm_npt_root_pa(&context->npt));
		amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_TLB_CONTROL,
		    AMDVM_HLT_NPT_ASID |
		    ((uint64_t)AMDVM_SVM_TLB_FLUSH_ALL << 32));
		new_npt_backing = old_npt_backing;
	}
	IOFree(host_pas, host_pa_count * sizeof(*host_pas));
	host_pas = NULL;
	for (i = 0; i < range_count; ++i) {
		context->guest_ranges[i] = pinned[i];
		context->guest_range_gpa[i] = ranges[i].guest_physical_address;
		context->guest_range_size[i] = ranges[i].size;
		context->guest_range_flags[i] = ranges[i].flags;
		memset(&pinned[i], 0, sizeof(pinned[i]));
	}
	context->guest_range_count = range_count;
	context->guest_page_count = total_guest_pages;
	context->guest_memory_mapped = 1;
	context->host_mtrr = host_mtrr;
	context->host_mtrr_valid = 1;
	IOLog("AMDVM RAM MTRR WB validated ranges=%u guest_pages=%llu npt_pages=%llu\n",
	    (unsigned)range_count, (unsigned long long)total_guest_pages,
	    (unsigned long long)npt_page_count);
	*npt_page_count_out = npt_page_count;
	amdvm_physmem_free(&new_npt_backing);
	rc = AMDVM_HLT_CONTEXT_OK;
	__sync_lock_release(&context->busy);
	return rc;

fail:
	if (rc == AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED) {
		IOLog("AMDVM guest RAM map rejected: MTRR policy status=%d ranges=%u\n",
		    rc, (unsigned)range_count);
	}
	if (host_pas != NULL) {
		IOFree(host_pas, host_pa_count * sizeof(*host_pas));
	}
	amdvm_physmem_free(&new_npt_backing);
	for (i = 0; i < range_count; ++i) {
		if (pinned[i].memory_descriptor != NULL &&
		    amdvm_physmem_unpin_user_region(&pinned[i]) != 0) {
			/* Transfer every unresolved descriptor before the client can die. */
			memcpy(context->guest_ranges, pinned, sizeof(pinned));
			context->guest_range_count = range_count;
			context->initialized = 0;
			rc = AMDVM_HLT_CONTEXT_UNPIN_FAILED;
			break;
		}
	}
	__sync_lock_release(&context->busy);
	return rc;
}

static int
amdvm_hlt_memory_type_valid(uint32_t type)
{
	return type == AMDVM_NPT_MEMTYPE_UC || type == AMDVM_NPT_MEMTYPE_WC ||
	    type == AMDVM_NPT_MEMTYPE_WT || type == AMDVM_NPT_MEMTYPE_WP ||
	    type == AMDVM_NPT_MEMTYPE_WB;
}

/* The update must cover every pinned range and the private reset ROM exactly. */
static int
amdvm_hlt_memory_type_map_complete(const amdvm_hlt_context_t *context,
	const amdvm_vcpu_set_memory_types_request_t *request,
	size_t *page_count_out)
{
	size_t map_index = 0;
	uint64_t cursor = 0;
	int cursor_valid = 0;
	int payload_pending = 1;
	size_t pages = 0;
	uint32_t i;

	for (i = 0; i < request->range_count; ++i) {
		const amdvm_vcpu_memory_type_range_t *range = &request->ranges[i];
		uint64_t expected_start, expected_size, expected_end, range_end;
		int is_payload;

		if (range->size == 0 ||
		    (range->guest_physical_address & (AMDVM_PAGE_SIZE - 1u)) != 0 ||
		    (range->size & (AMDVM_PAGE_SIZE - 1u)) != 0 ||
		    range->guest_physical_address > UINT64_MAX - range->size ||
		    range->reserved != 0 || !amdvm_hlt_memory_type_valid(range->memory_type) ||
		    range->size / AMDVM_PAGE_SIZE > SIZE_MAX - pages) {
			return AMDVM_HLT_CONTEXT_INVALID_GUEST;
		}
		range_end = range->guest_physical_address + range->size;
		if (map_index < context->guest_range_count &&
		    (!payload_pending || context->guest_range_gpa[map_index] <
		    context->guest_payload_gpa)) {
			is_payload = 0;
			expected_start = context->guest_range_gpa[map_index];
			expected_size = context->guest_range_size[map_index];
		} else if (payload_pending) {
			is_payload = 1;
			expected_start = context->guest_payload_gpa;
			expected_size = AMDVM_PAGE_SIZE;
		} else if (map_index < context->guest_range_count) {
			is_payload = 0;
			expected_start = context->guest_range_gpa[map_index];
			expected_size = context->guest_range_size[map_index];
		} else {
			return AMDVM_HLT_CONTEXT_INVALID_GUEST;
		}
		expected_end = expected_start + expected_size;
		if (!cursor_valid) {
			cursor = expected_start;
			cursor_valid = 1;
		}
		if (range->guest_physical_address != cursor || range_end > expected_end) {
			return AMDVM_HLT_CONTEXT_INVALID_GUEST;
		}
		cursor = range_end;
		pages += (size_t)(range->size / AMDVM_PAGE_SIZE);
		if (cursor == expected_end) {
			cursor = 0;
			cursor_valid = 0;
			if (is_payload) {
				payload_pending = 0;
			} else {
				++map_index;
			}
		}
	}
	if (map_index != context->guest_range_count || payload_pending ||
	    cursor_valid ||
	    pages != context->guest_page_count + 1u) {
		return AMDVM_HLT_CONTEXT_INVALID_GUEST;
	}
	*page_count_out = pages;
	return AMDVM_HLT_CONTEXT_OK;
}

int
amdvm_hlt_context_set_memory_types(amdvm_hlt_context_t *context,
	const amdvm_vcpu_set_memory_types_request_t *request,
	size_t *updated_page_count)
{
	uint8_t pat_indexes[AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES];
	amdvm_npt_pat_range_t *npt_pat_ranges = NULL;
	uint8_t *old_pat_indexes = NULL;
	amdvm_host_mtrr_state_t current_host_mtrr;
	size_t page_count = 0;
	uint64_t host_pat;
	uint32_t i;
	int rc;

	if (updated_page_count != NULL) *updated_page_count = 0;
	if (context == NULL || request == NULL || updated_page_count == NULL ||
	    request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->range_count == 0 ||
	    request->range_count > AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES ||
	    request->reserved[0] != 0 || request->reserved[1] != 0 ||
	    context->initialized == 0 || context->guest_memory_mapped == 0 ||
	    context->failed != 0 ||
	    context->lease.active != 0 ||
	    !__sync_bool_compare_and_swap(&context->busy, 0u, 1u)) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	rc = amdvm_hlt_memory_type_map_complete(context, request, &page_count);
	if (rc != AMDVM_HLT_CONTEXT_OK) goto out;
	int snapshot_cpu_before = cpu_number();
	int snapshot_rc = amdvm_hlt_read_host_mtrr_stable(&current_host_mtrr);
	int snapshot_equal = snapshot_rc == 0 &&
	    amdvm_host_mtrr_equal(&context->host_mtrr, &current_host_mtrr);
	if (context->host_mtrr_valid == 0 || snapshot_rc != 0 || !snapshot_equal) {
		IOLog("AMDVM memory-types reject stage=host-snapshot valid=%u capture=%d equal=%d cpu_before=%d cpu_after=%d\n",
		    context->host_mtrr_valid, snapshot_rc, snapshot_equal,
		    snapshot_cpu_before, cpu_number());
		IOLog("AMDVM memory-types saved snapshot follows\n");
		amdvm_hlt_log_host_mtrr_snapshot(&context->host_mtrr);
		IOLog("AMDVM memory-types current snapshot follows\n");
		amdvm_hlt_log_host_mtrr_snapshot(&current_host_mtrr);
		for (unsigned f = 0; f < AMDVM_HOST_MTRR_FIXED_COUNT; ++f) {
			if (context->host_mtrr.fixed[f] != current_host_mtrr.fixed[f]) {
				IOLog("AMDVM memory-types fixed-diff index=%u saved=0x%llx current=0x%llx\n",
				    f, (unsigned long long)context->host_mtrr.fixed[f],
				    (unsigned long long)current_host_mtrr.fixed[f]);
			}
		}
		rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		goto out;
	}
	if (page_count > SIZE_MAX / sizeof(*old_pat_indexes)) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto out;
	}
	old_pat_indexes = (uint8_t *)IOMalloc(page_count * sizeof(*old_pat_indexes));
	if (old_pat_indexes == NULL) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto out;
	}
	npt_pat_ranges = (amdvm_npt_pat_range_t *)IOMalloc(
	    request->range_count * sizeof(*npt_pat_ranges));
	if (npt_pat_ranges == NULL) {
		rc = AMDVM_HLT_CONTEXT_ALLOC_FAILED;
		goto out;
	}
	memset(npt_pat_ranges, 0,
	    request->range_count * sizeof(*npt_pat_ranges));
	host_pat = amdvm_hlt_read_pat();
	uint8_t requested_types[AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES];
	uint64_t execution_pat;
	for (i = 0; i < request->range_count; ++i)
		requested_types[i] = (uint8_t)request->ranges[i].memory_type;
	uint64_t architectural_pat = amdvm_vmcb_get_u64(context->guest_vmcb,
	    AMDVM_VMCB_GUEST_PAT);
	if (amdvm_npt_plan_pat(host_pat, architectural_pat, requested_types,
	    request->range_count, pat_indexes, &execution_pat) != AMDVM_NPT_OK) {
		IOLog("AMDVM memory-types reject stage=pat-composition host_pat=0x%llx guest_pat=0x%llx ranges=%u\n",
		    (unsigned long long)host_pat, (unsigned long long)architectural_pat,
		    request->range_count);
		rc = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		goto out;
	}
	for (i = 0; i < request->range_count; ++i) {
		npt_pat_ranges[i].guest_gpa =
		    request->ranges[i].guest_physical_address;
		npt_pat_ranges[i].size = request->ranges[i].size;
		npt_pat_ranges[i].pat_index = pat_indexes[i];
	}
	rc = amdvm_npt_update_pat_ranges_atomic(&context->npt, npt_pat_ranges,
	    request->range_count, old_pat_indexes, page_count, NULL, NULL);
	if (rc != AMDVM_NPT_OK) {
		amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_TLB_CONTROL,
		    AMDVM_HLT_NPT_ASID |
		    ((uint64_t)AMDVM_SVM_TLB_FLUSH_ALL << 32));
		if (rc == AMDVM_NPT_ROLLBACK_FAILED) {
			context->failed = 1;
			rc = AMDVM_HLT_CONTEXT_NPT_ROLLBACK_FAILED;
		} else {
			rc = AMDVM_HLT_CONTEXT_NPT_FAILED;
		}
		goto out;
	}
	memcpy(context->memory_types, requested_types, request->range_count);
	memcpy(context->memory_pat_indexes, pat_indexes, request->range_count);
	context->memory_type_count = request->range_count;
	IOLog("AMDVM memory-types committed host_pat=0x%llx guest_pat=0x%llx execution_pat=0x%llx pages=%llu\n",
	    (unsigned long long)host_pat, (unsigned long long)architectural_pat,
	    (unsigned long long)execution_pat, (unsigned long long)page_count);
	context->host_pat_msr = host_pat;
	context->host_pat_valid = 1;
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_TLB_CONTROL,
	    AMDVM_HLT_NPT_ASID |
	    ((uint64_t)AMDVM_SVM_TLB_FLUSH_ALL << 32));
	*updated_page_count = page_count;
	rc = AMDVM_HLT_CONTEXT_OK;
out:
	if (old_pat_indexes != NULL) {
		IOFree(old_pat_indexes, page_count * sizeof(*old_pat_indexes));
	}
	if (npt_pat_ranges != NULL) {
		IOFree(npt_pat_ranges,
		    request->range_count * sizeof(*npt_pat_ranges));
	}
	__sync_lock_release(&context->busy);
	return rc;
}

/* Called under the IOUserClient lifecycle lock, after staging architectural
 * state and before any entry snapshot or SVM MSR change. */
int
amdvm_hlt_context_prepare_guest_pat(amdvm_hlt_context_t *context)
{
	uint8_t indexes[AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES];
	uint64_t execution;
	if (!context || context->execution_pat_active) return -1;
	if (!context->guest_memory_mapped) return 0;
	if (!context->host_pat_valid || !context->memory_type_count ||
	    context->memory_type_count > AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES) return -1;
	uint64_t architectural = amdvm_vmcb_get_u64(context->guest_vmcb,
	    AMDVM_VMCB_GUEST_PAT);
	if (amdvm_npt_plan_pat(context->host_pat_msr, architectural,
	    context->memory_types, context->memory_type_count, indexes,
	    &execution) != AMDVM_NPT_OK ||
	    memcmp(indexes, context->memory_pat_indexes, context->memory_type_count))
		return -1;
	context->architectural_guest_pat = architectural;
	context->execution_pat_active = 1;
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_GUEST_PAT, execution);
	/* gPAT affects cached translations even when the NPT leaves did not change. */
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_TLB_CONTROL,
	    AMDVM_HLT_NPT_ASID | ((uint64_t)AMDVM_SVM_TLB_FLUSH_ALL << 32));
	return 0;
}

void
amdvm_hlt_context_restore_guest_pat(amdvm_hlt_context_t *context)
{
	if (context && context->execution_pat_active) {
		amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_GUEST_PAT,
		    context->architectural_guest_pat);
		context->execution_pat_active = 0;
	}
}

int
amdvm_hlt_context_run(amdvm_hlt_context_t *context,
	int *owner_status, amdvm_vmrun_status_t *vmrun_status,
	amdvm_vmexit_snapshot_t *exit_snapshot)
{
	if (context == NULL || owner_status == NULL || vmrun_status == NULL ||
	    exit_snapshot == NULL) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	if (!__sync_bool_compare_and_swap(&context->busy, 0u, 1u)) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	*owner_status = AMDVM_SVM_OWNER_INVALID;
	*vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	memset(exit_snapshot, 0, sizeof(*exit_snapshot));
	if (context->initialized == 0) {
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	if (context->lease.active != 0) {
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_LEASE_ACTIVE;
	}
	if (context->failed != 0) {
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_NPT_FAILED;
	}

#if !defined(AMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN)
	/* Do not acquire SVM or touch its MSRs while entry remains fail-closed. */
	*vmrun_status = AMDVM_VMRUN_ENTRY_GATED;
	__sync_lock_release(&context->busy);
	return AMDVM_HLT_CONTEXT_RUN_GATED;
#else
	int status;

	if (context->guest_memory_mapped != 0 &&
	    (context->host_pat_valid == 0 ||
	    amdvm_hlt_read_pat() != context->host_pat_msr)) {
		/* The leaf PAT selectors are meaningful only under the captured hPAT. */
		*vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
	}
	if (context->guest_memory_mapped != 0) {
		amdvm_host_mtrr_state_t current_host_mtrr;
		if (context->execution_pat_active == 0) {
			__sync_lock_release(&context->busy);
			return AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		}
		if (context->host_mtrr_valid == 0 ||
		    amdvm_hlt_read_host_mtrr_stable(&current_host_mtrr) != 0 ||
		    !amdvm_host_mtrr_equal(&context->host_mtrr,
		    &current_host_mtrr)) {
			/* Never run with an unchecked/different CPU-local MTRR layout. */
			*vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
			__sync_lock_release(&context->busy);
			return AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		}
	}

	/* The caller pins this thread to one CPU and disables local interrupts. */
	status = amdvm_svm_owner_acquire_current_cpu(&context->lease,
	    &context->hsave);
	*owner_status = status;
	if (status != AMDVM_SVM_OWNER_OK) {
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_SVM_ACQUIRE_FAILED;
	}

	/* Reject a stale/unwritten result if VMRUN does not produce a VMEXIT. */
	amdvm_vmcb_set_u64(context->guest_vmcb, AMDVM_VMCB_EXIT_CODE,
	    AMDVM_SVM_EXIT_NOT_RUN);
	/* Zero length means DecodeAssists did not provide valid instruction data. */
	context->guest_vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_LENGTH] = 0;
	for (size_t decodeIndex = 0;
	    decodeIndex < AMDVM_VMCB_DECODE_ASSIST_MAX; ++decodeIndex) {
		context->guest_vmcb->bytes[AMDVM_VMCB_DECODE_ASSIST_BYTES +
		    decodeIndex] = 0xcc;
	}
	*vmrun_status = amdvm_vmrun_once(&context->backing,
	    context->host_vmcb,
	    context->host_vmcb_pa, context->guest_vmcb, context->guest_vmcb_pa,
	    context->guest_gprs, context->host_xstate, context->guest_xstate,
	    exit_snapshot);
	status = amdvm_svm_owner_release_current_cpu(&context->lease);
	*owner_status = status;
	if (status != AMDVM_SVM_OWNER_OK) {
		/* Keep both descriptors alive while the lease may still own HSAVE. */
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_SVM_RELEASE_FAILED;
	}
	__sync_lock_release(&context->busy);
	if (*vmrun_status != AMDVM_VMRUN_SUCCESS) {
		return AMDVM_HLT_CONTEXT_VMRUN_FAILED;
	}
	if (exit_snapshot->code == AMDVM_SVM_EXIT_NOT_RUN ||
	    exit_snapshot->code == UINT64_MAX) {
		return AMDVM_HLT_CONTEXT_INVALID_GUEST;
	}
	return AMDVM_HLT_CONTEXT_OK;
#endif
}

int
amdvm_hlt_context_run_once(amdvm_hlt_context_t *context,
	int *owner_status, amdvm_vmrun_status_t *vmrun_status,
	amdvm_vmexit_snapshot_t *exit_snapshot)
{
	int status = amdvm_hlt_context_run(context, owner_status, vmrun_status,
	    exit_snapshot);
	if (status != AMDVM_HLT_CONTEXT_OK) {
		return status;
	}
	if (exit_snapshot->code != AMDVM_SVM_EXIT_HLT) {
		return AMDVM_HLT_CONTEXT_UNEXPECTED_EXIT;
	}
	return AMDVM_HLT_CONTEXT_OK;
}

int
amdvm_hlt_context_destroy(amdvm_hlt_context_t *context)
{
	if (context == NULL) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	if (context->quarantined != 0) {
		return AMDVM_HLT_CONTEXT_UNPIN_FAILED;
	}
	if (context->initialized == 0) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	if (!__sync_bool_compare_and_swap(&context->busy, 0u, 1u)) {
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	if (context->lease.active != 0) {
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_LEASE_ACTIVE;
	}
	context->initialized = 0;
	for (size_t i = 0; i < context->guest_range_count; ++i) {
		if (amdvm_physmem_unpin_user_region(&context->guest_ranges[i]) != 0) {
			/*
			 * The IOUserClient may be freed immediately after close. Transfer
			 * descriptor references to kext-global quarantine before releasing
			 * the no-longer-runnable NPT/VM-control allocations.
			 */
			if (amdvm_quarantine_transfer(context) != 0) {
				__sync_lock_release(&context->busy);
				return AMDVM_HLT_CONTEXT_UNPIN_FAILED;
			}
			amdvm_physmem_free(&context->npt_backing);
			amdvm_physmem_free(&context->hsave);
			uint32_t quarantine_index = context->quarantine_slot;
			uint64_t quarantine_generation =
			    context->quarantine_generation;
			amdvm_physmem_free(&context->backing);
			memset(context, 0, sizeof(*context));
			context->busy = 1;
			context->quarantined = 1;
			context->quarantine_slot = quarantine_index;
			context->quarantine_generation = quarantine_generation;
			__sync_lock_release(&context->busy);
			return AMDVM_HLT_CONTEXT_UNPIN_FAILED;
		}
	}
	if (amdvm_quarantine_release(context) != 0) {
		__sync_lock_release(&context->busy);
		return AMDVM_HLT_CONTEXT_INVALID;
	}
	context->guest_range_count = 0;
	context->guest_page_count = 0;
	amdvm_physmem_free(&context->npt_backing);
	amdvm_physmem_free(&context->hsave);
	amdvm_physmem_free(&context->backing);
	memset(context, 0, sizeof(*context));
	context->quarantine_slot = UINT32_MAX;
	return AMDVM_HLT_CONTEXT_OK;
}
