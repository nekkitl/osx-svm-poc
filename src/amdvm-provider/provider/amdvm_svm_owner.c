#include "amdvm_svm_owner.h"

#include <string.h>

#define AMDVM_MSR_EFER UINT32_C(0xc0000080)
#define AMDVM_MSR_VM_CR UINT32_C(0xc0010114)
#define AMDVM_MSR_VM_HSAVE_PA UINT32_C(0xc0010117)
#define AMDVM_VM_CR_SVMDIS (UINT64_C(1) << 4)
#define AMDVM_CPUID_SVM (1u << 2)
#define AMDVM_CPUID_NPT (1u << 0)
#define AMDVM_SVM_HSAVE_PA_MASK UINT64_C(0x000ffffffffff000)
#define AMDVM_PAGE_MASK (AMDVM_PAGE_SIZE - 1u)

extern int cpu_number(void);

static uint64_t
amdvm_rdmsr(uint32_t msr)
{
	uint32_t low, high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64_t)high << 32) | low;
}

static void
amdvm_wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t low = (uint32_t)value;
	uint32_t high = (uint32_t)(value >> 32);
	__asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
}

static uint64_t
amdvm_irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory", "cc");
	return flags;
}

static void
amdvm_irq_restore(uint64_t flags)
{
	__asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static int
amdvm_svm_capable_current_cpu(void)
{
	uint32_t a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000000u), "c"(0));
	if (a < 0x8000000au) {
		return 0;
	}
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x80000001u), "c"(0));
	if ((c & AMDVM_CPUID_SVM) == 0) {
		return 0;
	}
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
	    : "a"(0x8000000au), "c"(0));
	return (d & AMDVM_CPUID_NPT) != 0;
}

int
amdvm_svm_owner_acquire_current_cpu(amdvm_svm_lease_t *lease,
	const amdvm_physmem_region_t *hsave_region)
{
	uint64_t flags, vm_cr, efer, original_hsave_pa, hsave_pa;
	int cpu;

	if (lease == NULL || hsave_region == NULL || lease->active != 0 ||
	    hsave_region->size != AMDVM_PAGE_SIZE ||
	    amdvm_physmem_validate(hsave_region) != 0) {
		return AMDVM_SVM_OWNER_INVALID;
	}
	cpu = cpu_number();
	if (cpu < 0) {
		return AMDVM_SVM_OWNER_INVALID;
	}
	if (!amdvm_svm_capable_current_cpu()) {
		return AMDVM_SVM_OWNER_UNSUPPORTED;
	}
	hsave_pa = hsave_region->physical_address;
	if ((hsave_pa & AMDVM_PAGE_MASK) != 0 ||
	    (hsave_pa & ~AMDVM_SVM_HSAVE_PA_MASK) != 0 || hsave_pa == 0) {
		return AMDVM_SVM_OWNER_INVALID;
	}

	/* Keep the read-check-write sequence indivisible from local IRQ handlers. */
	flags = amdvm_irq_save();
	vm_cr = amdvm_rdmsr(AMDVM_MSR_VM_CR);
	efer = amdvm_rdmsr(AMDVM_MSR_EFER);
	original_hsave_pa = amdvm_rdmsr(AMDVM_MSR_VM_HSAVE_PA);
	if ((vm_cr & AMDVM_VM_CR_SVMDIS) != 0) {
		amdvm_irq_restore(flags);
		return AMDVM_SVM_OWNER_DISABLED_BY_FIRMWARE;
	}
	if ((efer & AMDVM_EFER_SVME) != 0 || original_hsave_pa != 0) {
		amdvm_irq_restore(flags);
		return AMDVM_SVM_OWNER_BUSY;
	}

	lease->original_efer = efer;
	lease->original_hsave_pa = original_hsave_pa;
	lease->owned_hsave_pa = hsave_pa;
	lease->cpu_index = (uint32_t)cpu;
	/* Publish HSAVE before enabling SVM, matching the safe ordering. */
	amdvm_wrmsr(AMDVM_MSR_VM_HSAVE_PA, hsave_pa);
	amdvm_wrmsr(AMDVM_MSR_EFER, efer | AMDVM_EFER_SVME);
	if (amdvm_rdmsr(AMDVM_MSR_VM_HSAVE_PA) != hsave_pa ||
	    (amdvm_rdmsr(AMDVM_MSR_EFER) & AMDVM_EFER_SVME) == 0) {
		amdvm_wrmsr(AMDVM_MSR_EFER, efer);
		amdvm_wrmsr(AMDVM_MSR_VM_HSAVE_PA, original_hsave_pa);
		if (amdvm_rdmsr(AMDVM_MSR_VM_HSAVE_PA) != original_hsave_pa ||
		    (amdvm_rdmsr(AMDVM_MSR_EFER) & AMDVM_EFER_SVME) !=
		    (efer & AMDVM_EFER_SVME)) {
			lease->active = 1;
			amdvm_irq_restore(flags);
			return AMDVM_SVM_OWNER_ROLLBACK_FAILED;
		}
		amdvm_irq_restore(flags);
		memset(lease, 0, sizeof(*lease));
		return AMDVM_SVM_OWNER_VERIFY_FAILED;
	}
	lease->active = 1;
	amdvm_irq_restore(flags);
	return AMDVM_SVM_OWNER_OK;
}

int
amdvm_svm_owner_release_current_cpu(amdvm_svm_lease_t *lease)
{
	uint64_t flags, efer, hsave_pa;
	int status = AMDVM_SVM_OWNER_OK;

	if (lease == NULL || lease->active == 0) {
		return AMDVM_SVM_OWNER_INVALID;
	}
	if (cpu_number() != (int)lease->cpu_index) {
		return AMDVM_SVM_OWNER_CPU_MISMATCH;
	}
	flags = amdvm_irq_save();
	efer = amdvm_rdmsr(AMDVM_MSR_EFER);
	hsave_pa = amdvm_rdmsr(AMDVM_MSR_VM_HSAVE_PA);
	if (hsave_pa != lease->owned_hsave_pa ||
	    (efer & AMDVM_EFER_SVME) == 0) {
		amdvm_irq_restore(flags);
		return AMDVM_SVM_OWNER_STATE_CHANGED;
	}
	/* Disable SVM before detaching the HSAVE page. */
	amdvm_wrmsr(AMDVM_MSR_EFER,
	    (efer & ~AMDVM_EFER_SVME) | (lease->original_efer & AMDVM_EFER_SVME));
	amdvm_wrmsr(AMDVM_MSR_VM_HSAVE_PA, lease->original_hsave_pa);
	if ((amdvm_rdmsr(AMDVM_MSR_EFER) & AMDVM_EFER_SVME) !=
	    (lease->original_efer & AMDVM_EFER_SVME) ||
	    amdvm_rdmsr(AMDVM_MSR_VM_HSAVE_PA) != lease->original_hsave_pa) {
		status = AMDVM_SVM_OWNER_VERIFY_FAILED;
	}
	amdvm_irq_restore(flags);
	if (status == AMDVM_SVM_OWNER_OK) {
		memset(lease, 0, sizeof(*lease));
	}
	return status;
}
