#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <mach/mach_time.h>
#include "amdvm_probe_abi.h"
#include "provider/amdvm_hlt_context.h"
#include "provider/amdvm_kick_logic.h"
#include "provider/amdvm_vcpu_lifecycle.h"
#include "provider/amdvm_vmcb.h"

static amdvm_hlt_context_t gHltContext = {};
static volatile uint32_t gHltContextBusy = 0;
static volatile uint32_t gHltFpuPreflightPassed = 0;
static volatile uint32_t gVcpuContextCount = 0;
static volatile uint32_t gVcpuLeaseCount = 0;

class AMDVMObjectRetainGuard {
public:
	explicit AMDVMObjectRetainGuard(OSObject *object) : fObject(object)
	{
		fObject->retain();
	}
	~AMDVMObjectRetainGuard()
	{
		fObject->release();
	}
private:
	OSObject *fObject;
};

/* IOUserClient places structures larger than io_struct_inband_t in an
 * IOMemoryDescriptor. Never dereference structureInput for those requests. */
class AMDVMStructureInput {
public:
	AMDVMStructureInput() : fBytes(nullptr), fOwned(nullptr), fSize(0) {}
	~AMDVMStructureInput()
	{
		if (fOwned != nullptr) {
			IOFree(fOwned, fSize);
		}
	}
	bool init(IOExternalMethodArguments *arguments, size_t expectedSize)
	{
		if (arguments == nullptr) return false;
		if (arguments->structureInputDescriptor != nullptr) {
			IOMemoryDescriptor *descriptor = arguments->structureInputDescriptor;
			if (arguments->structureInputSize != 0 ||
			    descriptor->getLength() != expectedSize) return false;
			fOwned = IOMalloc(expectedSize);
			if (fOwned == nullptr) return false;
			fSize = expectedSize;
			IOReturn prepareStatus = descriptor->prepare();
			if (prepareStatus != kIOReturnSuccess) {
				IOFree(fOwned, fSize);
				fOwned = nullptr;
				fSize = 0;
				return false;
			}
			IOByteCount copied = descriptor->readBytes(0, fOwned, expectedSize);
			IOReturn completeStatus = descriptor->complete();
			if (copied != expectedSize || completeStatus != kIOReturnSuccess) {
				IOFree(fOwned, fSize);
				fOwned = nullptr;
				fSize = 0;
				return false;
			}
			fBytes = fOwned;
			return true;
		}
		if (arguments->structureInput == nullptr ||
		    arguments->structureInputSize != expectedSize) return false;
		fBytes = arguments->structureInput;
		return true;
	}
	const void *bytes() const { return fBytes; }
private:
	const void *fBytes;
	void *fOwned;
	size_t fSize;
};

/* Copy large output structures through the prepared OOL descriptor supplied
 * by IOUserClient. Small results continue to use the inline structure buffer. */
class AMDVMStructureOutput {
public:
	AMDVMStructureOutput() : fBytes(nullptr), fSize(0) {}
	~AMDVMStructureOutput()
	{
		if (fBytes != nullptr) IOFree(fBytes, fSize);
	}
	bool init(size_t size)
	{
		fBytes = IOMalloc(size);
		if (fBytes == nullptr) return false;
		fSize = size;
		bzero(fBytes, size);
		return true;
	}
	void *bytes() const { return fBytes; }
	IOReturn copyTo(IOExternalMethodArguments *arguments)
	{
		if (arguments == nullptr) return kIOReturnBadArgument;
		if (arguments->structureOutputDescriptor != nullptr) {
			IOMemoryDescriptor *descriptor = arguments->structureOutputDescriptor;
			if (descriptor->getLength() < fSize) return kIOReturnBadArgument;
			IOReturn prepareStatus = descriptor->prepare();
			if (prepareStatus != kIOReturnSuccess) return prepareStatus;
			IOByteCount copied = descriptor->writeBytes(0, fBytes, fSize);
			IOReturn completeStatus = descriptor->complete();
			if (copied != fSize || completeStatus != kIOReturnSuccess) {
				return kIOReturnError;
			}
			arguments->structureOutputDescriptorSize = (uint32_t)fSize;
			return kIOReturnSuccess;
		}
		if (arguments->structureOutput == nullptr ||
		    arguments->structureOutputSize < fSize) return kIOReturnBadArgument;
		bcopy(fBytes, arguments->structureOutput, fSize);
		arguments->structureOutputSize = (uint32_t)fSize;
		return kIOReturnSuccess;
	}
private:
	void *fBytes;
	size_t fSize;
};

/* Kernel collections still require kmod_info for an IOKit service kext.
 * These hooks acknowledge module lifecycle; service startup stays with IOKit. */
extern "C" {
kern_return_t _start(kmod_info_t *info, void *data);
kern_return_t _stop(kmod_info_t *info, void *data);
KMOD_EXPLICIT_DECL(AMDVMProbe, "0.1.0", _start, _stop)

kern_return_t
_start(kmod_info_t *info, void *data)
{
	(void)info;
	(void)data;
	return KERN_SUCCESS;
}

kern_return_t
_stop(kmod_info_t *info, void *data)
{
	(void)info;
	(void)data;
	/* Never unload code while SVM ownership or backing pages may be retained. */
	if (__sync_val_compare_and_swap(&gHltContextBusy, 0u, 0u) != 0 ||
	    gHltContext.lease.active != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.npt_backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr ||
	    amdvm_hlt_context_quarantine_count() != 0 ||
	    __sync_val_compare_and_swap(&gVcpuContextCount, 0u, 0u) != 0 ||
	    __sync_val_compare_and_swap(&gVcpuLeaseCount, 0u, 0u) != 0) {
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}
}

static const uint32_t kMsrVmCr = 0xc0010114u;
static const uint32_t kMsrEfer = 0xc0000080u;
static const uint32_t kMsrVmHsavePa = 0xc0010117u;
static const uint64_t kVmCrLock = 1ull << 3;
static const uint64_t kVmCrSvmdis = 1ull << 4;
static const uint64_t kEferSvme = 1ull << 12;

extern "C" int cpu_number(void);
extern "C" void mp_rendezvous(void (*setup)(void *),
	void (*action)(void *), void (*teardown)(void *), void *arg);
#if defined(AMDVM_PATCHED_XNU)
/* Experimental API available only in the matching patched XNU profile. */
typedef volatile uint64_t amdvm_xnu_cpumask_t;
extern "C" kern_return_t mp_cpus_kick_bounded(
	amdvm_xnu_cpumask_t cpus, uint64_t timeout_ns);
static const uint64_t kAmdvmKickTimeoutNs = 10000000ull;
#elif !defined(AMDVM_STOCK_XNU)
#error Select exactly one AMDVM_XNU_PROFILE for this kernel provider
#endif

static inline uint64_t
amdvm_rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void
amdvm_cpuid(uint32_t leaf, uint32_t subleaf,
    uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
	    : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
	    : "a"(leaf), "c"(subleaf));
}

static void
amdvm_query_cpu(void *opaque)
{
	amdvm_probe_result_t *result = (amdvm_probe_result_t *)opaque;
	uint32_t a, b, c, d;
	uint32_t maxExtended;
	uint64_t vmCr, efer, hsave;
	int cpuIndex = cpu_number();
	amdvm_probe_cpu_state_t *cpuState;

	__sync_fetch_and_add(&result->logical_cpus_seen, 1u);
	if (cpuIndex < 0 || cpuIndex >= (int)AMDVM_PROBE_MAX_CPUS) {
		__sync_fetch_and_add(&result->cpu_index_overflow, 1u);
		return;
	}
	cpuState = &result->cpus[cpuIndex];
	cpuState->cpu_index = (uint32_t)cpuIndex;
	cpuState->flags = AMDVM_CPU_PRESENT;
	amdvm_cpuid(0x80000000u, 0, &maxExtended, &b, &c, &d);
	if (maxExtended < 0x80000001u) {
		return;
	}
	amdvm_cpuid(0x80000001u, 0, &a, &b, &c, &d);
	if ((c & (1u << 2)) == 0) {
		return;
	}
	__sync_fetch_and_add(&result->svm_cpuid_cpus, 1u);
	cpuState->flags |= AMDVM_CPU_SVM;
	if (maxExtended < 0x8000000au) {
		goto read_svm_msrs;
	}
	amdvm_cpuid(0x8000000au, 0, &a, &b, &c, &d);
	if (d & (1u << 0)) {
		__sync_fetch_and_add(&result->npt_cpuid_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_NPT;
	}
	if (d & (1u << 3)) {
		__sync_fetch_and_add(&result->nrip_save_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_NRIP_SAVE;
	}

read_svm_msrs:
	vmCr = amdvm_rdmsr(kMsrVmCr);
	efer = amdvm_rdmsr(kMsrEfer);
	hsave = amdvm_rdmsr(kMsrVmHsavePa);
	cpuState->vm_cr = vmCr;
	cpuState->efer = efer;
	cpuState->hsave_pa = hsave;
	if (vmCr & kVmCrSvmdis) {
		__sync_fetch_and_add(&result->vm_cr_svmdis_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_VMCR_SVMDIS;
	}
	if (vmCr & kVmCrLock) {
		__sync_fetch_and_add(&result->vm_cr_lock_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_VMCR_LOCK;
	}
	if (efer & kEferSvme) {
		__sync_fetch_and_add(&result->efer_svme_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_EFER_SVME;
	}
	if (hsave != 0) {
		__sync_fetch_and_add(&result->hsave_nonzero_cpus, 1u);
		cpuState->flags |= AMDVM_CPU_HSAVE_NONZERO;
		if ((hsave & 0xfffu) == 0) {
			__sync_fetch_and_add(&result->hsave_valid_cpus, 1u);
			cpuState->flags |= AMDVM_CPU_HSAVE_ALIGNED;
		}
	}
}

class AMDVMProbeUserClient : public IOUserClient {
	using super = IOUserClient;
	OSDeclareDefaultStructors(AMDVMProbeUserClient)

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties = nullptr) APPLE_KEXT_OVERRIDE;
	IOReturn query(IOExternalMethodArguments *arguments);
	IOReturn hltPreflight(IOExternalMethodArguments *arguments);
	IOReturn hltFpuPreflight(IOExternalMethodArguments *arguments);
	IOReturn hltRun(IOExternalMethodArguments *arguments);
	IOReturn vcpuCreate(IOExternalMethodArguments *arguments);
	IOReturn vcpuDiagnostic(IOExternalMethodArguments *arguments);
	IOReturn vcpuControlDiagnostic(IOExternalMethodArguments *arguments);
	IOReturn vcpuSetResetRom(IOExternalMethodArguments *arguments);
	IOReturn vcpuMapMemory(IOExternalMethodArguments *arguments);
	IOReturn vcpuSetMemoryTypes(IOExternalMethodArguments *arguments);
	IOReturn vcpuRun(IOExternalMethodArguments *arguments);
	IOReturn vcpuKick(IOExternalMethodArguments *arguments);
	IOReturn vcpuDestroy(IOExternalMethodArguments *arguments);
	IOReturn externalMethod(uint32_t selector,
	    IOExternalMethodArguments *arguments,
	    IOExternalMethodDispatch *dispatch = nullptr,
	    OSObject *target = nullptr,
	    void *reference = nullptr) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;

private:
	void *fSecurityID;
	amdvm_hlt_context_t fVcpuContext;
	volatile uint32_t fVcpuBusy;
	volatile uint32_t fVcpuActiveRuns;
	volatile uint32_t fVcpuRunCpu;
	volatile uint64_t fVcpuRunGeneration;
	volatile uint64_t fVcpuKickGeneration;
	uint32_t fVcpuState;
	uint64_t fVcpuExpectedRip;
	uint32_t fVcpuGuestProfile;
	uint32_t fVcpuResetRomInstalled;
	uint32_t fVcpuContextTracked;
	uint32_t fVcpuLeaseTracked;
};

class AMDVMProbeService : public IOService {
	using super = IOService;
	OSDeclareDefaultStructors(AMDVMProbeService)

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AMDVMProbeService, IOService)
OSDefineMetaClassAndStructors(AMDVMProbeUserClient, IOUserClient)

static IOReturn
amdvm_query_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->query(arguments);
}

static IOReturn
amdvm_hlt_preflight_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->hltPreflight(arguments);
}

static IOReturn
amdvm_hlt_fpu_preflight_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->hltFpuPreflight(arguments);
}

static IOReturn
amdvm_hlt_run_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->hltRun(arguments);
}

static IOReturn
amdvm_vcpu_create_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuCreate(arguments);
}

static IOReturn
amdvm_vcpu_diagnostic_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuDiagnostic(arguments);
}

static IOReturn
amdvm_vcpu_control_diagnostic_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuControlDiagnostic(arguments);
}

static IOReturn
amdvm_vcpu_set_reset_rom_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuSetResetRom(arguments);
}

static IOReturn
amdvm_vcpu_run_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuRun(arguments);
}

static IOReturn
amdvm_vcpu_kick_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuKick(arguments);
}

static IOReturn
amdvm_vcpu_map_memory_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuMapMemory(arguments);
}

static IOReturn
amdvm_vcpu_set_memory_types_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuSetMemoryTypes(arguments);
}

static IOReturn
amdvm_vcpu_destroy_action(OSObject *target, void *reference,
	IOExternalMethodArguments *arguments)
{
	AMDVMProbeUserClient *client = (AMDVMProbeUserClient *)target;
	(void)reference;
	return client->vcpuDestroy(arguments);
}

IOReturn
AMDVMProbeUserClient::query(IOExternalMethodArguments *arguments)
{
	amdvm_probe_result_t result = {};
	IOReturn privilege;
	if (arguments == nullptr || arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	result.abi_version = AMDVM_PROBE_ABI_VERSION;
	mp_rendezvous(nullptr, amdvm_query_cpu, nullptr, &result);
	(void)amdvm_hlt_context_retry_quarantined();
	{
		size_t contexts, ranges;
		amdvm_hlt_context_quarantine_stats(&contexts, &ranges,
		    &result.quarantined_guest_bytes);
		result.quarantined_memory_contexts = (uint32_t)contexts;
		result.quarantined_memory_ranges = (uint32_t)ranges;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::hltPreflight(IOExternalMethodArguments *arguments)
{
	amdvm_hlt_preflight_result_t result = {};
	IOReturn privilege;
	int status;
	uint64_t backing_base, backing_end;

	if (arguments == nullptr || arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&gHltContextBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_HLT_PREFLIGHT_ABI_VERSION;
	result.init_status = AMDVM_HLT_CONTEXT_INVALID;
	result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
	if (gHltContext.initialized != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&gHltContextBusy);
		return kIOReturnBusy;
	}

	status = amdvm_hlt_context_init(&gHltContext);
	result.init_status = status;
	result.flags |= AMDVM_HLT_PREFLIGHT_SVM_UNTOUCHED |
	    AMDVM_HLT_PREFLIGHT_VMRUN_NOT_EXECUTED;
	if (status == AMDVM_HLT_CONTEXT_OK) {
		backing_base = gHltContext.backing.physical_address;
		backing_end = backing_base + gHltContext.backing.size;
		if (amdvm_physmem_validate(&gHltContext.backing) == 0 &&
		    amdvm_physmem_validate(&gHltContext.hsave) == 0) {
			result.flags |= AMDVM_HLT_PREFLIGHT_ALLOCATIONS_VALID;
		}
		if (gHltContext.npt.pool != nullptr &&
		    gHltContext.npt.pool_pa == backing_base &&
		    amdvm_npt_validate_single_4k(&gHltContext.npt, 0,
		    gHltContext.guest_payload_pa,
		    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK &&
		    (gHltContext.npt.pool_pa & (AMDVM_PAGE_SIZE - 1u)) == 0) {
			result.flags |= AMDVM_HLT_PREFLIGHT_NPT_READY;
		}
		if (gHltContext.guest_payload != nullptr &&
		    gHltContext.guest_payload[0] == 0xf4u &&
		    gHltContext.guest_payload_pa >= backing_base &&
		    gHltContext.guest_payload_pa < backing_end) {
			result.flags |= AMDVM_HLT_PREFLIGHT_HLT_PAYLOAD;
		}
		if (gHltContext.host_vmcb != nullptr &&
		    gHltContext.guest_vmcb != nullptr &&
		    gHltContext.host_vmcb_pa >= backing_base &&
		    gHltContext.host_vmcb_pa < backing_end &&
		    gHltContext.guest_vmcb_pa >= backing_base &&
		    gHltContext.guest_vmcb_pa < backing_end &&
		    (gHltContext.host_vmcb_pa & (AMDVM_PAGE_SIZE - 1u)) == 0 &&
		    (gHltContext.guest_vmcb_pa & (AMDVM_PAGE_SIZE - 1u)) == 0) {
			result.flags |= AMDVM_HLT_PREFLIGHT_VMCB_PAGES_VALID;
		}
		result.destroy_status = amdvm_hlt_context_destroy(
		    &gHltContext);
		if (result.destroy_status == AMDVM_HLT_CONTEXT_OK) {
			result.flags |= AMDVM_HLT_PREFLIGHT_DESTROYED;
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&gHltContextBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::hltFpuPreflight(IOExternalMethodArguments *arguments)
{
	amdvm_hlt_fpu_preflight_result_t result = {};
	IOSimpleLock *runLock = nullptr;
	thread_t initialThread = THREAD_NULL;
	IOInterruptState interruptState = FALSE;
	uint64_t firstCr0Before = 0, firstCr0After = 0;
	uint64_t secondCr0Before = 0, secondCr0After = 0;
	IOReturn privilege;
	int firstStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int secondStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int initialCpu = -1;
	int finalCpu = -1;
	boolean_t interruptsWereEnabled;

	if (arguments == nullptr || arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&gHltContextBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION;
	result.init_status = AMDVM_HLT_CONTEXT_INVALID;
	result.first_xsave_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	result.second_xsave_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
	result.flags = AMDVM_HLT_FPU_PREFLIGHT_VMRUN_NOT_EXECUTED;
	if (gHltContext.initialized != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&gHltContextBusy);
		return kIOReturnBusy;
	}

	result.init_status = amdvm_hlt_context_init(&gHltContext);
	if (result.init_status == AMDVM_HLT_CONTEXT_OK) {
		runLock = IOSimpleLockAlloc();
		if (runLock != nullptr) {
			initialThread = current_thread();
			interruptsWereEnabled = ml_get_interrupts_enabled();
			if (interruptsWereEnabled) {
				firstStatus = amdvm_vmrun_xstate_capture_current_thread(
				    gHltContext.host_xstate, &result.xcr0,
				    &firstCr0Before, &firstCr0After);
				result.ts_before = (uint32_t)((firstCr0Before >> 3) & 1u);
				result.em_before = (uint32_t)((firstCr0Before >> 2) & 1u);
				if (firstStatus == AMDVM_VMRUN_SUCCESS) {
					result.flags |= AMDVM_HLT_FPU_PREFLIGHT_XSAVE_OK;
				}
				if (firstStatus == AMDVM_VMRUN_SUCCESS) {
					IOSimpleLockLock(runLock);
					result.flags |= AMDVM_HLT_FPU_PREFLIGHT_LOCKED_CPU;
					initialCpu = cpu_number();
					result.cpu_before = (uint32_t)initialCpu;
					if (initialThread == current_thread()) {
						result.flags |= AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE;
					}
					interruptState = ml_set_interrupts_enabled(FALSE);
					secondStatus = amdvm_vmrun_xstate_capture_current_thread(
					    gHltContext.host_xstate, &result.xcr0,
					    &secondCr0Before, &secondCr0After);
					result.ts_after = (uint32_t)((secondCr0After >> 3) & 1u);
					result.em_after = (uint32_t)((secondCr0After >> 2) & 1u);
					if (secondStatus == AMDVM_VMRUN_SUCCESS) {
						result.flags |= AMDVM_HLT_FPU_PREFLIGHT_TS_CLEARED;
					}
					if (initialThread == current_thread()) {
						result.flags |= AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE;
					} else {
						result.flags &= ~AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE;
					}
					finalCpu = cpu_number();
					result.cpu_after = (uint32_t)finalCpu;
					if (initialCpu == finalCpu) {
						result.flags |= AMDVM_HLT_FPU_PREFLIGHT_CPU_STABLE;
					}
					(void)ml_set_interrupts_enabled(interruptState);
					result.flags |= AMDVM_HLT_FPU_PREFLIGHT_INTERRUPTS_RESTORED;
					IOSimpleLockUnlock(runLock);
				}
			}
			IOSimpleLockFree(runLock);
		}
		result.first_xsave_status = firstStatus;
		result.second_xsave_status = secondStatus;
		result.destroy_status = amdvm_hlt_context_destroy(&gHltContext);
		if (result.destroy_status == AMDVM_HLT_CONTEXT_OK) {
			result.flags |= AMDVM_HLT_FPU_PREFLIGHT_CONTEXT_DESTROYED;
		}
		const uint32_t expectedFlags =
		    AMDVM_HLT_FPU_PREFLIGHT_LOCKED_CPU |
		    AMDVM_HLT_FPU_PREFLIGHT_XSAVE_OK |
		    AMDVM_HLT_FPU_PREFLIGHT_TS_CLEARED |
		    AMDVM_HLT_FPU_PREFLIGHT_CPU_STABLE |
		    AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE |
		    AMDVM_HLT_FPU_PREFLIGHT_INTERRUPTS_RESTORED |
		    AMDVM_HLT_FPU_PREFLIGHT_VMRUN_NOT_EXECUTED |
		    AMDVM_HLT_FPU_PREFLIGHT_CONTEXT_DESTROYED;
		if (result.first_xsave_status == AMDVM_VMRUN_SUCCESS &&
		    result.second_xsave_status == AMDVM_VMRUN_SUCCESS &&
		    result.destroy_status == AMDVM_HLT_CONTEXT_OK &&
		    result.flags == expectedFlags) {
			__sync_lock_test_and_set(&gHltFpuPreflightPassed, 1u);
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&gHltContextBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::hltRun(IOExternalMethodArguments *arguments)
{
	amdvm_hlt_run_result_t result = {};
	const amdvm_hlt_run_request_t *request;
	IOSimpleLock *runLock = nullptr;
	thread_t initialThread = THREAD_NULL;
	IOInterruptState interruptState = FALSE;
	boolean_t interruptsWereEnabled;
	uint64_t xcr0 = 0, cr0Before = 0, cr0After = 0;
	amdvm_vmrun_status_t vmrunStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	amdvm_vmexit_snapshot_t exitSnapshot = {};
	IOReturn privilege;
	int status, xstateStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int cpuBefore = -1;

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(amdvm_hlt_run_request_t) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_hlt_run_request_t *)arguments->structureInput;
	if (request->abi_version != AMDVM_HLT_RUN_REQUEST_ABI_VERSION ||
	    request->flags != AMDVM_HLT_RUN_CONFIRM_NO_KDP_WATCHPOINTS) {
		return kIOReturnNotPermitted;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (__sync_val_compare_and_swap(&gHltFpuPreflightPassed, 0u, 0u) == 0) {
		result.abi_version = AMDVM_HLT_RUN_ABI_VERSION;
		result.context_status = AMDVM_HLT_CONTEXT_FPU_PREFLIGHT_REQUIRED;
		result.owner_status = AMDVM_SVM_OWNER_INVALID;
		result.vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
		result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
		bcopy(&result, arguments->structureOutput, sizeof(result));
		arguments->structureOutputSize = sizeof(result);
		return kIOReturnSuccess;
	}
	if (!__sync_bool_compare_and_swap(&gHltContextBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_HLT_RUN_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	result.owner_status = AMDVM_SVM_OWNER_INVALID;
	result.vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	result.destroy_status = AMDVM_HLT_CONTEXT_INVALID;
	if (gHltContext.initialized != 0 ||
	    gHltContext.backing.memory_descriptor != nullptr ||
	    gHltContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&gHltContextBusy);
		return kIOReturnBusy;
	}

	status = amdvm_hlt_context_init(&gHltContext);
	result.context_status = status;
	if (status == AMDVM_HLT_CONTEXT_OK) {
		result.flags |= AMDVM_HLT_RUN_CONTEXT_INITIALIZED |
		    AMDVM_HLT_RUN_CALLED;
		result.owner_status = AMDVM_SVM_OWNER_INVALID;
		result.vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
#if defined(AMDVM_SVM_RUN_EXPERIMENTAL_OPT_IN)
		runLock = IOSimpleLockAlloc();
		if (runLock == nullptr) {
			result.vmrun_status = AMDVM_VMRUN_XSTATE_UNSUPPORTED;
			result.context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
		} else {
			initialThread = current_thread();
			interruptsWereEnabled = ml_get_interrupts_enabled();
			if (interruptsWereEnabled) {
				/*
				 * The initial XSAVE can take XNU's lazy #NM restore in this
				 * thread context. Replace that snapshot after IOSimpleLock has
				 * disabled preemption. Keep that lock through VM entry, VMEXIT,
				 * and the wrapper's host-XSTATE restore; local interrupts are
				 * disabled over the same pinned-CPU interval.
				 */
				xstateStatus = amdvm_vmrun_xstate_capture_current_thread(
				    gHltContext.host_xstate, &xcr0,
				    &cr0Before, &cr0After);
				if (xstateStatus == AMDVM_VMRUN_SUCCESS) {
					IOSimpleLockLock(runLock);
					cpuBefore = cpu_number();
					interruptState = ml_set_interrupts_enabled(FALSE);
					if (initialThread == current_thread()) {
						xstateStatus = amdvm_vmrun_xstate_capture_current_thread(
						    gHltContext.host_xstate, &xcr0,
						    &cr0Before, &cr0After);
					} else {
						xstateStatus = AMDVM_VMRUN_FPU_NOT_READY;
					}
					if (xstateStatus == AMDVM_VMRUN_SUCCESS &&
					    cpuBefore == cpu_number() &&
					    initialThread == current_thread()) {
						result.context_status = amdvm_hlt_context_run_once(
						    &gHltContext, &result.owner_status,
						    &vmrunStatus, &exitSnapshot);
						result.vmrun_status = (int32_t)vmrunStatus;
						result.exit_code = exitSnapshot.code;
						result.exit_info1 = exitSnapshot.info1;
						result.exit_info2 = exitSnapshot.info2;
						result.next_rip = exitSnapshot.next_rip;
						result.guest_rip = exitSnapshot.guest_rip;
						result.guest_rflags = exitSnapshot.guest_rflags;
					} else {
						result.vmrun_status = xstateStatus;
						result.context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
					}
					(void)ml_set_interrupts_enabled(interruptState);
					IOSimpleLockUnlock(runLock);
				}
			} else {
				result.vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
				result.context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
			}
			IOSimpleLockFree(runLock);
		}
#else
		result.vmrun_status = AMDVM_VMRUN_ENTRY_GATED;
		result.context_status = AMDVM_HLT_CONTEXT_RUN_GATED;
#endif
		if (gHltContext.lease.active != 0) {
			result.flags |= AMDVM_HLT_RUN_LEASE_RETAINED;
		} else {
			result.destroy_status = amdvm_hlt_context_destroy(
			    &gHltContext);
			if (result.destroy_status == AMDVM_HLT_CONTEXT_OK) {
				result.flags |= AMDVM_HLT_RUN_CONTEXT_DESTROYED;
			}
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&gHltContextBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuCreate(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_lifecycle_result_t result = {};
	const amdvm_vcpu_create_request_t *request;
	IOReturn privilege;
	int status;
	static const uint8_t expectedCode[AMDVM_VCPU_TEST_ROM_SIZE] = {
		AMDVM_VCPU_TEST_ROM_CPUID_0,
		AMDVM_VCPU_TEST_ROM_CPUID_1,
		AMDVM_VCPU_TEST_ROM_MOV_AL_IMM8,
		AMDVM_VCPU_TEST_ROM_DEBUG_BYTE,
		AMDVM_VCPU_TEST_ROM_OUT_IMM8_AL,
		AMDVM_VCPU_TEST_ROM_DEBUG_PORT,
		AMDVM_VCPU_TEST_ROM_HLT
	};
	bool fixedCodeProfile;
	bool firmwareProfile;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(*request) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_create_request_t *)arguments->structureInput;
	fixedCodeProfile = request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_REALMODE_TEST_V1 ||
	    request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1;
	firmwareProfile = request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    (!fixedCodeProfile && !firmwareProfile) || request->reserved != 0 ||
	    (fixedCodeProfile && (request->code_size != sizeof(expectedCode) ||
	    bcmp(request->code, expectedCode, sizeof(expectedCode)) != 0)) ||
	    (firmwareProfile && request->code_size != 0)) {
		return kIOReturnNotPermitted;
	}
	if (firmwareProfile) {
		for (size_t i = 0; i < sizeof(request->code); ++i) {
			if (request->code[i] != 0) {
				return kIOReturnNotPermitted;
			}
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (fVcpuContext.initialized != 0 ||
	    fVcpuContext.backing.memory_descriptor != nullptr ||
	    fVcpuContext.npt_backing.memory_descriptor != nullptr ||
	    fVcpuContext.hsave.memory_descriptor != nullptr) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnBusy;
	}
	status = request->guest_profile ==
	    AMDVM_VCPU_GUEST_PROFILE_REALMODE_TEST_V1 ?
	    amdvm_hlt_context_init(&fVcpuContext) :
	    amdvm_hlt_context_init_qemu_reset_vector_v1(&fVcpuContext);
	result.context_status = status;
	if (status == AMDVM_HLT_CONTEXT_OK) {
		/* Fixed smoke payload, or a placeholder until QEMU supplies firmware. */
		if (fixedCodeProfile &&
		    (fVcpuContext.guest_payload_offset > AMDVM_PAGE_SIZE ||
		    request->code_size > AMDVM_PAGE_SIZE -
		    fVcpuContext.guest_payload_offset)) {
			result.context_status = AMDVM_HLT_CONTEXT_INVALID_GUEST;
			(void)amdvm_hlt_context_destroy(&fVcpuContext);
			bcopy(&result, arguments->structureOutput, sizeof(result));
			arguments->structureOutputSize = sizeof(result);
			__sync_lock_release(&fVcpuBusy);
			return kIOReturnSuccess;
		}
		if (fixedCodeProfile) {
			bcopy(request->code,
			    (uint8_t *)fVcpuContext.guest_payload +
			    fVcpuContext.guest_payload_offset,
			    request->code_size);
		}
		__sync_fetch_and_add(&gVcpuContextCount, 1u);
		fVcpuContextTracked = 1;
		fVcpuExpectedRip = fVcpuContext.initial_rip;
		fVcpuGuestProfile = request->guest_profile;
		fVcpuResetRomInstalled = fixedCodeProfile ? 1u : 0u;
		fVcpuState = AMDVM_VCPU_STATE_READY;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuDiagnostic(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_diagnostic_result_t result = {};
	IOReturn privilege;
	uint32_t csOffset = AMDVM_VMCB_GUEST_CS;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInputSize != 0 ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (fVcpuContext.initialized == 0 || fVcpuContext.guest_vmcb == nullptr ||
	    fVcpuContext.guest_payload == nullptr || fVcpuResetRomInstalled == 0 ||
	    (fVcpuState != AMDVM_VCPU_STATE_READY &&
	     fVcpuState != AMDVM_VCPU_STATE_MEMORY_MAPPED) ||
	    fVcpuActiveRuns != 0 || fVcpuContext.lease.active != 0 ||
	    fVcpuContext.guest_payload_offset != AMDVM_VCPU_RESET_ROM_OFFSET) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}

	result.context_status = AMDVM_HLT_CONTEXT_OK;
	result.guest_profile = fVcpuGuestProfile;
	result.guest_physical_address = AMDVM_VCPU_RESET_ROM_GPA;
	result.rom_offset = AMDVM_VCPU_RESET_ROM_OFFSET;
	bcopy(fVcpuContext.guest_payload + AMDVM_VCPU_RESET_ROM_OFFSET,
	    result.entry_bytes, sizeof(result.entry_bytes));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset],
	    &result.cs_selector, sizeof(result.cs_selector));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset + 2],
	    &result.cs_attributes, sizeof(result.cs_attributes));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset + 4],
	    &result.cs_limit, sizeof(result.cs_limit));
	bcopy(&fVcpuContext.guest_vmcb->bytes[csOffset + 8],
	    &result.cs_base, sizeof(result.cs_base));
	result.rip = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_RIP);
	result.rflags = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_RFLAGS);
	result.cr0 = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_CR0);
	result.efer = amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
	    AMDVM_VMCB_GUEST_EFER);
	result.flags |= AMDVM_VCPU_DIAGNOSTIC_ROM_INSTALLED;
	if (amdvm_npt_validate_4k_mapping(&fVcpuContext.npt,
	    fVcpuContext.guest_payload_gpa, fVcpuContext.guest_payload_pa,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK) {
		result.flags |= AMDVM_VCPU_DIAGNOSTIC_NPT_RX_VALID;
	}
	if (result.cs_selector == 0xf000u &&
	    result.cs_attributes == 0x009bu && result.cs_limit == 0xffffu &&
	    result.cs_base == AMDVM_VCPU_RESET_CS_BASE &&
	    result.rip == AMDVM_VCPU_RESET_VECTOR_RIP &&
	    result.rflags == 2u && result.cr0 == 0x60000010ull &&
	    result.efer == AMDVM_EFER_SVME) {
		result.flags |= AMDVM_VCPU_DIAGNOSTIC_RESET_STATE;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuControlDiagnostic(
	IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_control_diagnostic_result_t result = {};
	const amdvm_vmcb_t *vmcb;
	uint32_t nested_paging;
	uint32_t misc1;
	uint32_t misc2;
	uint32_t exceptions;
	uint32_t interrupt_control;
	uint64_t npt_root;
	uint64_t iopm;
	uint64_t msrpm;
	uint64_t asid_tlb;
	IOReturn privilege;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInputSize != 0 ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (fVcpuContext.initialized == 0 || fVcpuContext.guest_vmcb == nullptr ||
	    fVcpuResetRomInstalled == 0 ||
	    (fVcpuState != AMDVM_VCPU_STATE_READY &&
	     fVcpuState != AMDVM_VCPU_STATE_MEMORY_MAPPED) ||
	    fVcpuActiveRuns != 0 || fVcpuContext.lease.active != 0) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}

	vmcb = fVcpuContext.guest_vmcb;
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_READ],
	    &result.intercept_cr_read, sizeof(result.intercept_cr_read));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_CR_WRITE],
	    &result.intercept_cr_write, sizeof(result.intercept_cr_write));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_DR_READ],
	    &result.intercept_dr_read, sizeof(result.intercept_dr_read));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_DR_WRITE],
	    &result.intercept_dr_write, sizeof(result.intercept_dr_write));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_EXCEPTIONS], &exceptions,
	    sizeof(exceptions));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC1], &misc1,
	    sizeof(misc1));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERCEPT_MISC2], &misc2,
	    sizeof(misc2));
	bcopy(&vmcb->bytes[AMDVM_VMCB_INTERRUPT_CONTROL], &interrupt_control,
	    sizeof(interrupt_control));
	nested_paging = (uint32_t)amdvm_vmcb_get_u64(vmcb,
	    AMDVM_VMCB_NESTED_PAGING);
	npt_root = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_NPT_ROOT);
	iopm = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_IOPM_PA);
	msrpm = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_MSRPM_PA);
	asid_tlb = amdvm_vmcb_get_u64(vmcb, AMDVM_VMCB_TLB_CONTROL);

	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_OK;
	result.intercept_exceptions = exceptions;
	result.intercept_misc1 = misc1;
	result.intercept_misc2 = misc2;
	result.interrupt_control = interrupt_control;
	result.nested_paging_control = nested_paging;
	result.asid = (uint32_t)asid_tlb;
	result.tlb_control = (uint32_t)(asid_tlb >> 32);
	if (misc1 & AMDVM_SVM_INTERCEPT_CPUID) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_CPUID_INTERCEPT;
	}
	if (exceptions & (1u << 6)) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_UD_INTERCEPT;
	}
	if (misc1 & AMDVM_SVM_INTERCEPT_HLT) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_HLT_INTERCEPT;
	}
	if (misc1 & AMDVM_SVM_INTERCEPT_IOIO) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_IOIO_INTERCEPT;
	}
	if (misc1 & AMDVM_SVM_INTERCEPT_INTR) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_INTR_INTERCEPT;
	}
	if (interrupt_control & AMDVM_VMCB_V_INTR_MASKING) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_V_INTR_MASKING;
	}
	if (nested_paging & AMDVM_VMCB_NPT_ENABLE) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_NPT_ENABLED;
	}
	if (npt_root != 0 &&
	    (npt_root & (AMDVM_PAGE_SIZE - 1u)) == 0) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_NPT_ROOT_VALID;
	}
	if (iopm != 0 && (iopm & (AMDVM_PAGE_SIZE - 1u)) == 0) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_IOPM_VALID;
	}
	if (msrpm != 0 && (msrpm & (AMDVM_PAGE_SIZE - 1u)) == 0) {
		result.flags |= AMDVM_VCPU_CONTROL_DIAG_MSRPM_VALID;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuSetResetRom(IOExternalMethodArguments *arguments)
{
	const amdvm_vcpu_set_reset_rom_request_t *request;
	AMDVMStructureInput input;
	amdvm_vcpu_lifecycle_result_t result = {};
	IOReturn privilege;
	if (arguments == nullptr || !input.init(arguments, sizeof(*request)) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_set_reset_rom_request_t *)
	    input.bytes();
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->reserved != 0) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (fVcpuState != AMDVM_VCPU_STATE_READY ||
	    fVcpuGuestProfile != AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1 ||
	    fVcpuResetRomInstalled != 0 || fVcpuContext.guest_payload == nullptr ||
	    fVcpuContext.guest_payload_offset != AMDVM_VCPU_RESET_ROM_OFFSET) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	bcopy(request->page, fVcpuContext.guest_payload,
	    AMDVM_VCPU_RESET_ROM_SIZE);
	fVcpuResetRomInstalled = 1;
	result.context_status = AMDVM_HLT_CONTEXT_OK;
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuKick(IOExternalMethodArguments *arguments)
{
	const amdvm_vcpu_kick_request_t *request;
	IOReturn privilege;
	uint64_t current;
	uint64_t previous;
#if defined(AMDVM_PATCHED_XNU)
	uint32_t runCpu;
	uint64_t runGeneration;
	bool waitForRunCompletion = false;
#endif
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(*request)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_kick_request_t *)arguments->structureInput;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->reserved != 0 || request->generation == 0) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}

	current = __sync_val_compare_and_swap(&fVcpuKickGeneration, 0u, 0u);
	while (amdvm_generation_newer(request->generation, current)) {
		previous = __sync_val_compare_and_swap(&fVcpuKickGeneration, current,
		    request->generation);
		if (previous == current) {
			current = request->generation;
			break;
		}
		current = previous;
	}
	if (!amdvm_kick_generation_should_dispatch(request->generation, current)) {
		return kIOReturnSuccess;
	}

	/*
	 * RUN compares this generation around entry and after every VMEXIT. The
	 * stock-XNU profile has no cross-CPU private-kick import: it relies on
	 * AMD SVM's mandatory external-interrupt intercept returning to QEMU on
	 * the next host interrupt. This is not a hard wall-clock latency guarantee;
	 * firmware execution remains gated until target behavior is measured.
	 */
#if defined(AMDVM_PATCHED_XNU)
	/* Retry an equal generation after timeout; a prior send may not have run. */
	runCpu = __sync_val_compare_and_swap(&fVcpuRunCpu, UINT32_MAX, UINT32_MAX);
	runGeneration = __sync_val_compare_and_swap(&fVcpuRunGeneration, 0u, 0u);
	if (runCpu < AMDVM_PROBE_MAX_CPUS &&
	    amdvm_generation_newer(request->generation, runGeneration)) {
		amdvm_xnu_cpumask_t target = 1ull << runCpu;
		kern_return_t kickStatus = mp_cpus_kick_bounded(target,
			kAmdvmKickTimeoutNs);
		if (kickStatus == KERN_OPERATION_TIMED_OUT) {
			return kIOReturnTimeout;
		}
		if (kickStatus != KERN_SUCCESS) {
			return kIOReturnError;
		}
		waitForRunCompletion = true;
	} else if (runCpu >= AMDVM_PROBE_MAX_CPUS &&
	    __sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0) {
		/* KICK raced before CPU publication; RUN will observe the new generation. */
		waitForRunCompletion = true;
	}
	if (waitForRunCompletion) {
		/*
		 * A successful IPI send is not proof that RUN left VMRUN. Wait for
		 * vcpuRun's active reference to drain, under a second finite deadline.
		 * On timeout QEMU must retain the context; destroy rejects active runs.
		 */
		uint64_t runDeadline;
		clock_interval_to_deadline(10, 1000000u, &runDeadline);
		while (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0) {
			if (mach_absolute_time() >= runDeadline) {
				return kIOReturnTimeout;
			}
			IOSleep(1);
		}
	}
#endif
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuRun(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_run_result_t *result;
	const amdvm_vcpu_run_request_t *request;
	AMDVMStructureInput input;
	AMDVMStructureOutput output;
	IOSimpleLock *runLock = nullptr;
	thread_t initialThread = THREAD_NULL;
	IOInterruptState interruptState = FALSE;
	boolean_t interruptsWereEnabled;
	uint64_t xcr0 = 0, cr0Before = 0, cr0After = 0;
	amdvm_vmrun_status_t vmrunStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	amdvm_vmexit_snapshot_t exitSnapshot = {};
	IOReturn privilege;
	int xstateStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
	int cpuBefore = -1;
	uint32_t i;
	uint32_t priorState;
	bool fullState = false;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || !input.init(arguments, sizeof(*request)) ||
	    !output.init(sizeof(*result))) {
		return kIOReturnBadArgument;
	}
	result = (amdvm_vcpu_run_result_t *)output.bytes();
	request = (const amdvm_vcpu_run_request_t *)input.bytes();
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->flags != AMDVM_VCPU_RUN_CONFIRM_NO_KDP_WATCHPOINTS ||
	    amdvm_vmcb_validate_event_injection(request->event_injection,
	    request->event_injection_error_code) != 0) {
		return kIOReturnNotPermitted;
	}
	fullState = request->state.abi_version == AMDVM_X86_STATE_ABI_VERSION;
	if (fullState) {
		if (request->rip != 0 || request->rflags != 0 ||
		    amdvm_vmcb_validate_x86_state_v1(&request->state) != 0) {
			return kIOReturnBadArgument;
		}
		for (i = 0; i < sizeof(request->gpr) / sizeof(request->gpr[0]); ++i) {
			if (request->gpr[i] != 0) return kIOReturnBadArgument;
		}
	} else {
		if (request->event_injection != 0 ||
		    request->state.abi_version != 0 ||
		    request->rflags != 2u) {
			return kIOReturnBadArgument;
		}
		for (i = 0; i < sizeof(request->state); ++i) {
			if (((const uint8_t *)&request->state)[i] != 0) {
				return kIOReturnBadArgument;
			}
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (__sync_val_compare_and_swap(&gHltFpuPreflightPassed, 0u, 0u) == 0) {
		result->abi_version = AMDVM_VCPU_ABI_VERSION;
		result->context_status = AMDVM_HLT_CONTEXT_FPU_PREFLIGHT_REQUIRED;
		result->owner_status = AMDVM_SVM_OWNER_INVALID;
		result->vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
		return output.copyTo(arguments);
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (!fullState && request->rip != fVcpuExpectedRip) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotPermitted;
	}
	priorState = fVcpuState;
	result->abi_version = AMDVM_VCPU_ABI_VERSION;
	result->context_status = AMDVM_HLT_CONTEXT_INVALID;
	result->owner_status = AMDVM_SVM_OWNER_INVALID;
	result->vmrun_status = AMDVM_VMRUN_INVALID_ARGUMENT;
	if (fVcpuContext.initialized == 0 ||
	    fVcpuContext.guest_gprs == nullptr || fVcpuContext.guest_vmcb == nullptr ||
	    !amdvm_vcpu_state_can_run(fVcpuState, fullState,
	    request->event_injection != 0)) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	if (request->kick_generation !=
	    __sync_val_compare_and_swap(&fVcpuKickGeneration, 0u, 0u)) {
		/* QEMU processes the pending event before retrying this same state. */
		result->abi_version = AMDVM_VCPU_ABI_VERSION;
		result->flags = AMDVM_VCPU_RUN_KICKED;
		result->context_status = AMDVM_HLT_CONTEXT_OK;
		result->owner_status = AMDVM_SVM_OWNER_OK;
		result->vmrun_status = AMDVM_VMRUN_SUCCESS;
		result->exit_code = UINT64_MAX;
		if (fullState) {
			result->state = request->state;
		}
		IOReturn outputStatus = output.copyTo(arguments);
		__sync_lock_release(&fVcpuBusy);
		return outputStatus;
	}
	__sync_fetch_and_add(&fVcpuActiveRuns, 1u);
	fVcpuState = AMDVM_VCPU_STATE_RUNNING;
	if (amdvm_vmcb_stage_run_request(fVcpuContext.guest_vmcb,
	    (uint64_t *)fVcpuContext.guest_gprs, fVcpuContext.guest_xstate,
	    request, fullState) != 0) {
		fVcpuState = priorState;
		__sync_fetch_and_sub(&fVcpuActiveRuns, 1u);
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnBadArgument;
	}
	if (fullState) result->state = request->state;
	if (amdvm_hlt_context_prepare_guest_pat(&fVcpuContext) != 0) {
		result->context_status = AMDVM_HLT_CONTEXT_MEMORY_TYPE_UNSUPPORTED;
		result->exit_code = UINT64_MAX;
		fVcpuState = priorState;
		__sync_fetch_and_sub(&fVcpuActiveRuns, 1u);
		IOReturn outputStatus = output.copyTo(arguments);
		__sync_lock_release(&fVcpuBusy);
		return outputStatus;
	}

	if (ml_get_interrupts_enabled()) {
		runLock = IOSimpleLockAlloc();
		if (runLock != nullptr) {
			initialThread = current_thread();
			interruptsWereEnabled = ml_get_interrupts_enabled();
			if (interruptsWereEnabled) {
				xstateStatus = amdvm_vmrun_xstate_capture_current_thread(
				    fVcpuContext.host_xstate, &xcr0,
				    &cr0Before, &cr0After);
				if (xstateStatus == AMDVM_VMRUN_SUCCESS) {
					IOSimpleLockLock(runLock);
					cpuBefore = cpu_number();
					interruptState = ml_set_interrupts_enabled(FALSE);
					if (initialThread == current_thread()) {
						xstateStatus = amdvm_vmrun_xstate_capture_current_thread(
						    fVcpuContext.host_xstate, &xcr0,
						    &cr0Before, &cr0After);
					} else {
						xstateStatus = AMDVM_VMRUN_FPU_NOT_READY;
					}
					if (xstateStatus == AMDVM_VMRUN_SUCCESS &&
					    cpuBefore == cpu_number() &&
					    initialThread == current_thread()) {
						/* Publish generation before CPU so KICK sees a coherent pair. */
						__sync_lock_test_and_set(&fVcpuRunGeneration,
						    request->kick_generation);
						__sync_lock_test_and_set(&fVcpuRunCpu,
						    (uint32_t)cpuBefore);
						if (request->kick_generation !=
						    __sync_val_compare_and_swap(&fVcpuKickGeneration,
						    0u, 0u)) {
							/* A kick before CPU publication needs no VMRUN. */
							result->flags |= AMDVM_VCPU_RUN_KICKED;
							result->context_status = AMDVM_HLT_CONTEXT_OK;
							result->owner_status = AMDVM_SVM_OWNER_OK;
							result->vmrun_status = AMDVM_VMRUN_SUCCESS;
							result->exit_code = UINT64_MAX;
							if (fullState) {
								result->state = request->state;
							}
						} else {
							uint64_t entryGuestXcr0 = fullState ?
							    request->state.xcr0 : 0;
							uint32_t entryGuestXstateSize = fullState ?
							    request->state.xstate_size : 0;
							if (amdvm_vmcb_capture_entry_snapshot(
							    fVcpuContext.guest_vmcb, &fVcpuContext.npt,
							    fVcpuContext.guest_payload_gpa,
							    fVcpuContext.guest_payload_offset,
							    fVcpuContext.backing.virtual_address,
							    fVcpuContext.backing.physical_address,
							    fVcpuContext.backing.size,
							    (const uint64_t *)fVcpuContext.guest_gprs,
							    xcr0, entryGuestXcr0,
							    entryGuestXstateSize, &result->entry) == 0) {
								result->flags |= AMDVM_VCPU_RUN_CALLED;
								result->context_status = amdvm_hlt_context_run(
								    &fVcpuContext, &result->owner_status,
								    &vmrunStatus, &exitSnapshot);
							} else {
								result->context_status =
								    AMDVM_HLT_CONTEXT_INVALID_GUEST;
								vmrunStatus = AMDVM_VMRUN_INVALID_ARGUMENT;
							}
							result->vmrun_status = (int32_t)vmrunStatus;
							result->exit_code = exitSnapshot.code;
							result->exit_info1 = exitSnapshot.info1;
							result->exit_info2 = exitSnapshot.info2;
							result->next_rip = exitSnapshot.next_rip;
							result->instruction_length =
							    exitSnapshot.instruction_length;
							bcopy(exitSnapshot.instruction_bytes,
							    result->instruction_bytes,
							    sizeof(result->instruction_bytes));
							result->rip = exitSnapshot.guest_rip;
							result->rflags = exitSnapshot.guest_rflags;
							result->exit_int_info = (uint32_t)
							    amdvm_vmcb_get_u64(fVcpuContext.guest_vmcb,
							    AMDVM_VMCB_EXIT_INT_INFO);
							bcopy(fVcpuContext.guest_gprs, result->gpr,
							    sizeof(result->gpr));
							if (request->kick_generation !=
							    __sync_val_compare_and_swap(
							    &fVcpuKickGeneration, 0u, 0u)) {
								result->flags |= AMDVM_VCPU_RUN_KICKED;
							}
						}
						/* No new IPI should target this entry after this point. */
						__sync_lock_test_and_set(&fVcpuRunCpu, UINT32_MAX);
						__sync_lock_test_and_set(&fVcpuRunGeneration, 0u);
						if ((result->flags & AMDVM_VCPU_RUN_CALLED) != 0 &&
						    vmrunStatus == AMDVM_VMRUN_SUCCESS) {
							if (fullState) {
								if (amdvm_vmcb_store_x86_state_v1(
								    fVcpuContext.guest_vmcb, &result->state,
								    (const uint64_t *)fVcpuContext.guest_gprs,
								    fVcpuContext.guest_xstate,
								    AMDVM_X86_XSTATE_MAX_SIZE) != 0) {
									result->context_status = AMDVM_HLT_CONTEXT_INVALID_GUEST;
									fVcpuState = AMDVM_VCPU_STATE_FAILED;
								} else {
									result->rip = result->state.rip;
									result->rflags = result->state.rflags;
								if (exitSnapshot.code == AMDVM_HLT_EXPECTED_EXIT) {
									fVcpuState = AMDVM_VCPU_STATE_HALTED;
								} else {
									fVcpuState = priorState == AMDVM_VCPU_STATE_HALTED ?
									    AMDVM_VCPU_STATE_MEMORY_MAPPED : priorState;
									}
								}
							} else if (request->rip == AMDVM_VCPU_TEST_ROM_CPUID_RIP &&
							    exitSnapshot.code == AMDVM_VCPU_EXPECTED_CPUID_EXIT) {
								fVcpuExpectedRip = AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP;
							} else if (request->rip == AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP &&
							    exitSnapshot.code == AMDVM_VCPU_EXPECTED_IOIO_EXIT) {
								fVcpuExpectedRip = AMDVM_VCPU_TEST_ROM_HLT_RIP;
							} else if (request->rip == AMDVM_VCPU_TEST_ROM_HLT_RIP &&
							    exitSnapshot.code == AMDVM_HLT_EXPECTED_EXIT) {
								fVcpuState = AMDVM_VCPU_STATE_HALTED;
							} else {
								fVcpuState = AMDVM_VCPU_STATE_FAILED;
							}
						}
					} else {
						result->vmrun_status = xstateStatus;
						result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
					}
					(void)ml_set_interrupts_enabled(interruptState);
					IOSimpleLockUnlock(runLock);
				}
			} else {
				/* Do not return the untouched INVALID sentinels as a success. */
				xstateStatus = AMDVM_VMRUN_FPU_NOT_READY;
			}
			if (xstateStatus != AMDVM_VMRUN_SUCCESS) {
				result->vmrun_status = xstateStatus;
				result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
			}
			IOSimpleLockFree(runLock);
		} else {
			result->vmrun_status = AMDVM_VMRUN_XSTATE_UNSUPPORTED;
			result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
		}
	} else {
		result->vmrun_status = AMDVM_VMRUN_FPU_NOT_READY;
		result->context_status = AMDVM_HLT_CONTEXT_VMRUN_FAILED;
	}
	amdvm_hlt_context_restore_guest_pat(&fVcpuContext);
	if (fullState) result->state.pat = request->state.pat;
	if (fVcpuContext.lease.active != 0) {
		result->flags |= AMDVM_VCPU_RUN_LEASE_RETAINED;
		if (fVcpuLeaseTracked == 0) {
			fVcpuLeaseTracked = 1;
			__sync_fetch_and_add(&gVcpuLeaseCount, 1u);
		}
	}
	if (fVcpuContext.lease.active != 0) {
		fVcpuState = AMDVM_VCPU_STATE_LEASED;
	} else if (fVcpuState == AMDVM_VCPU_STATE_RUNNING) {
		/* Pre-entry failures leave the same resume point available. */
		fVcpuState = priorState;
	}
	__sync_fetch_and_sub(&fVcpuActiveRuns, 1u);
	IOReturn outputStatus = output.copyTo(arguments);
	__sync_lock_release(&fVcpuBusy);
	return outputStatus;
}

IOReturn
AMDVMProbeUserClient::vcpuMapMemory(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_memory_map_result_t result = {};
	const amdvm_vcpu_memory_map_request_t *request;
	IOReturn privilege;
	size_t nptPageCount = 0;
	size_t i;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || arguments->structureInput == nullptr ||
	    arguments->structureInputSize != sizeof(*request) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_memory_map_request_t *)arguments->structureInput;
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->range_count == 0 ||
	    request->range_count > AMDVM_VCPU_MEMORY_MAX_RANGES ||
	    request->reserved[0] != 0 || request->reserved[1] != 0) {
		return kIOReturnBadArgument;
	}
	for (i = request->range_count; i < AMDVM_VCPU_MEMORY_MAX_RANGES; ++i) {
		const uint8_t *bytes = (const uint8_t *)&request->ranges[i];
		size_t j;
		for (j = 0; j < sizeof(request->ranges[i]); ++j) {
			if (bytes[j] != 0) {
				return kIOReturnBadArgument;
			}
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (fVcpuState != AMDVM_VCPU_STATE_READY ||
	    fVcpuResetRomInstalled == 0 ||
	    fVcpuContext.guest_memory_mapped != 0) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	result.context_status = amdvm_hlt_context_map_user_ranges(
	    &fVcpuContext, request->ranges, request->range_count,
	    current_task(), &nptPageCount);
	if (result.context_status == AMDVM_HLT_CONTEXT_OK) {
		result.range_count = request->range_count;
		result.guest_page_count = fVcpuContext.guest_page_count;
		result.npt_page_count = nptPageCount;
		fVcpuState = AMDVM_VCPU_STATE_MEMORY_MAPPED;
	} else if (result.context_status == AMDVM_HLT_CONTEXT_UNPIN_FAILED) {
		fVcpuState = AMDVM_VCPU_STATE_FAILED;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuSetMemoryTypes(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_lifecycle_result_t result = {};
	const amdvm_vcpu_set_memory_types_request_t *request;
	AMDVMStructureInput input;
	IOReturn privilege;
	size_t pageCount = 0;
	size_t i;
	AMDVMObjectRetainGuard lifetime(this);

	if (arguments == nullptr || !input.init(arguments, sizeof(*request)) ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	request = (const amdvm_vcpu_set_memory_types_request_t *)
	    input.bytes();
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	result.context_status = AMDVM_HLT_CONTEXT_INVALID;
	if (request->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    request->range_count == 0 ||
	    request->range_count > AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES ||
	    request->reserved[0] != 0 || request->reserved[1] != 0) {
		return kIOReturnBadArgument;
	}
	for (i = request->range_count;
	    i < AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES; ++i) {
		const uint8_t *bytes = (const uint8_t *)&request->ranges[i];
		size_t j;
		for (j = 0; j < sizeof(request->ranges[i]); ++j) {
			if (bytes[j] != 0) return kIOReturnBadArgument;
		}
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) return kIOReturnNotPrivileged;
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	if (fVcpuState != AMDVM_VCPU_STATE_MEMORY_MAPPED ||
	    fVcpuContext.guest_memory_mapped == 0 ||
	    fVcpuActiveRuns != 0 || fVcpuContext.lease.active != 0) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnNotReady;
	}
	result.context_status = amdvm_hlt_context_set_memory_types(
	    &fVcpuContext, request, &pageCount);
	if (result.context_status == AMDVM_HLT_CONTEXT_OK) {
		if (pageCount > UINT32_MAX) {
			result.context_status = AMDVM_HLT_CONTEXT_INVALID_GUEST;
		} else {
			result.reserved[0] = (uint32_t)pageCount;
		}
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return kIOReturnSuccess;
}

IOReturn
AMDVMProbeUserClient::vcpuDestroy(IOExternalMethodArguments *arguments)
{
	amdvm_vcpu_lifecycle_result_t result = {};
	uint32_t priorState;
	IOReturn privilege;
	AMDVMObjectRetainGuard lifetime(this);
	if (arguments == nullptr || arguments->structureInputSize != 0 ||
	    arguments->structureOutput == nullptr ||
	    arguments->structureOutputSize < sizeof(result)) {
		return kIOReturnBadArgument;
	}
	privilege = IOUserClient::clientHasPrivilege(fSecurityID,
	    kIOClientPrivilegeAdministrator);
	if (privilege != kIOReturnSuccess) {
		return kIOReturnNotPrivileged;
	}
	if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
		return kIOReturnBusy;
	}
	result.abi_version = AMDVM_VCPU_ABI_VERSION;
	if (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0 ||
	    fVcpuState == AMDVM_VCPU_STATE_RUNNING ||
	    fVcpuState == AMDVM_VCPU_STATE_DESTROYING) {
		__sync_lock_release(&fVcpuBusy);
		return kIOReturnBusy;
	}
	priorState = fVcpuState;
	fVcpuState = AMDVM_VCPU_STATE_DESTROYING;
	result.context_status = amdvm_hlt_context_destroy(&fVcpuContext);
	if (result.context_status == AMDVM_HLT_CONTEXT_OK) {
		fVcpuState = AMDVM_VCPU_STATE_EMPTY;
		fVcpuExpectedRip = 0;
		fVcpuGuestProfile = 0;
		fVcpuResetRomInstalled = 0;
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
	} else if (fVcpuContext.quarantined != 0) {
		fVcpuState = AMDVM_VCPU_STATE_FAILED;
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
	} else {
		fVcpuState = fVcpuContext.lease.active != 0 ?
		    AMDVM_VCPU_STATE_LEASED : priorState;
	}
	bcopy(&result, arguments->structureOutput, sizeof(result));
	arguments->structureOutputSize = sizeof(result);
	__sync_lock_release(&fVcpuBusy);
	return result.context_status == AMDVM_HLT_CONTEXT_OK ?
	    kIOReturnSuccess : kIOReturnError;
}

bool
AMDVMProbeUserClient::initWithTask(task_t owningTask, void *securityID,
	UInt32 type, OSDictionary *properties)
{
	if (type != 0 || !super::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fSecurityID = securityID;
	bzero(&fVcpuContext, sizeof(fVcpuContext));
	fVcpuBusy = 0;
	fVcpuActiveRuns = 0;
	fVcpuRunCpu = UINT32_MAX;
	fVcpuRunGeneration = 0;
	fVcpuKickGeneration = 0;
	fVcpuState = AMDVM_VCPU_STATE_EMPTY;
	fVcpuExpectedRip = 0;
	fVcpuGuestProfile = 0;
	fVcpuResetRomInstalled = 0;
	fVcpuContextTracked = 0;
	fVcpuLeaseTracked = 0;
	return true;
}

IOReturn
AMDVMProbeUserClient::externalMethod(uint32_t selector,
	IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
	OSObject *target, void *reference)
{
	static IOExternalMethodDispatch methods[] = {
		{ amdvm_query_action, 0, 0, 0, sizeof(amdvm_probe_result_t) },
		{ amdvm_hlt_preflight_action, 0, 0, 0,
		    sizeof(amdvm_hlt_preflight_result_t) },
		{ amdvm_hlt_run_action, 0, sizeof(amdvm_hlt_run_request_t), 0,
		    sizeof(amdvm_hlt_run_result_t) },
		{ amdvm_hlt_fpu_preflight_action, 0, 0, 0,
		    sizeof(amdvm_hlt_fpu_preflight_result_t) },
		{ amdvm_vcpu_create_action, 0,
		    sizeof(amdvm_vcpu_create_request_t), 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_run_action, 0, sizeof(amdvm_vcpu_run_request_t), 0,
		    sizeof(amdvm_vcpu_run_result_t) },
		{ amdvm_vcpu_destroy_action, 0, 0, 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_map_memory_action, 0,
		    sizeof(amdvm_vcpu_memory_map_request_t), 0,
		    sizeof(amdvm_vcpu_memory_map_result_t) },
		{ amdvm_vcpu_set_reset_rom_action, 0,
		    sizeof(amdvm_vcpu_set_reset_rom_request_t), 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_kick_action, 0, sizeof(amdvm_vcpu_kick_request_t), 0, 0 },
		{ amdvm_vcpu_set_memory_types_action, 0,
		    sizeof(amdvm_vcpu_set_memory_types_request_t), 0,
		    sizeof(amdvm_vcpu_lifecycle_result_t) },
		{ amdvm_vcpu_diagnostic_action, 0, 0, 0,
		    sizeof(amdvm_vcpu_diagnostic_result_t) },
		{ amdvm_vcpu_control_diagnostic_action, 0, 0, 0,
		    sizeof(amdvm_vcpu_control_diagnostic_result_t) }
	};
	if (selector >= (sizeof(methods) / sizeof(methods[0]))) {
		return kIOReturnUnsupported;
	}
	return super::externalMethod(selector, arguments, &methods[selector],
	    this, nullptr);
}

IOReturn
AMDVMProbeUserClient::clientClose(void)
{
	AMDVMObjectRetainGuard lifetime(this);
	if (fVcpuContext.quarantined != 0) {
		(void)amdvm_hlt_context_forget_quarantined(&fVcpuContext);
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
		fVcpuState = AMDVM_VCPU_STATE_EMPTY;
		fVcpuGuestProfile = 0;
		fVcpuResetRomInstalled = 0;
		terminate();
		return kIOReturnSuccess;
	}
	if (fVcpuContext.initialized != 0 ||
	    fVcpuContext.backing.memory_descriptor != nullptr ||
	    fVcpuContext.npt_backing.memory_descriptor != nullptr ||
	    fVcpuContext.hsave.memory_descriptor != nullptr) {
		if (!__sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
			return kIOReturnBusy;
		}
		if (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) != 0 ||
		    fVcpuState == AMDVM_VCPU_STATE_RUNNING ||
		    fVcpuState == AMDVM_VCPU_STATE_DESTROYING) {
			__sync_lock_release(&fVcpuBusy);
			return kIOReturnBusy;
		}
		uint32_t priorState = fVcpuState;
		fVcpuState = AMDVM_VCPU_STATE_DESTROYING;
		int destroyStatus = amdvm_hlt_context_destroy(&fVcpuContext);
		if (destroyStatus != AMDVM_HLT_CONTEXT_OK &&
		    fVcpuContext.quarantined == 0) {
			fVcpuState = fVcpuContext.lease.active != 0 ?
			    AMDVM_VCPU_STATE_LEASED : priorState;
			__sync_lock_release(&fVcpuBusy);
			return kIOReturnBusy;
		}
		fVcpuState = AMDVM_VCPU_STATE_EMPTY;
		fVcpuExpectedRip = 0;
		fVcpuGuestProfile = 0;
		fVcpuResetRomInstalled = 0;
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
		__sync_lock_release(&fVcpuBusy);
	}
	terminate();
	return kIOReturnSuccess;
}

void
AMDVMProbeUserClient::free(void)
{
	if (fVcpuContext.quarantined != 0) {
		(void)amdvm_hlt_context_forget_quarantined(&fVcpuContext);
		if (fVcpuContextTracked != 0) {
			fVcpuContextTracked = 0;
			__sync_fetch_and_sub(&gVcpuContextCount, 1u);
		}
		if (fVcpuLeaseTracked != 0) {
			fVcpuLeaseTracked = 0;
			__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
		}
	}
	if (fVcpuContext.initialized != 0 ||
	    fVcpuContext.backing.memory_descriptor != nullptr ||
	    fVcpuContext.npt_backing.memory_descriptor != nullptr ||
	    fVcpuContext.hsave.memory_descriptor != nullptr) {
		if (__sync_val_compare_and_swap(&fVcpuActiveRuns, 0u, 0u) == 0 &&
		    __sync_bool_compare_and_swap(&fVcpuBusy, 0u, 1u)) {
			uint32_t priorState = fVcpuState;
			fVcpuState = AMDVM_VCPU_STATE_DESTROYING;
			if (amdvm_hlt_context_destroy(&fVcpuContext) ==
			    AMDVM_HLT_CONTEXT_OK) {
				fVcpuState = AMDVM_VCPU_STATE_EMPTY;
				fVcpuGuestProfile = 0;
				fVcpuResetRomInstalled = 0;
				if (fVcpuContextTracked != 0) {
					fVcpuContextTracked = 0;
					__sync_fetch_and_sub(&gVcpuContextCount, 1u);
				}
				if (fVcpuLeaseTracked != 0) {
					fVcpuLeaseTracked = 0;
					__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
				}
			} else if (fVcpuContext.quarantined != 0) {
				(void)amdvm_hlt_context_forget_quarantined(&fVcpuContext);
				fVcpuState = AMDVM_VCPU_STATE_EMPTY;
				if (fVcpuContextTracked != 0) {
					fVcpuContextTracked = 0;
					__sync_fetch_and_sub(&gVcpuContextCount, 1u);
				}
				if (fVcpuLeaseTracked != 0) {
					fVcpuLeaseTracked = 0;
					__sync_fetch_and_sub(&gVcpuLeaseCount, 1u);
				}
			} else {
				fVcpuState = fVcpuContext.lease.active != 0 ?
				    AMDVM_VCPU_STATE_LEASED : priorState;
			}
			__sync_lock_release(&fVcpuBusy);
		}
	}
	super::free();
}

bool
AMDVMProbeService::start(IOService *provider)
{
	if (!super::start(provider)) {
		return false;
	}
	registerService();
	return true;
}

IOReturn
AMDVMProbeService::newUserClient(task_t owningTask, void *securityID,
	UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
	AMDVMProbeUserClient *client;
	if (handler == nullptr || type != 0) {
		return kIOReturnBadArgument;
	}
	*handler = nullptr;
	client = OSTypeAlloc(AMDVMProbeUserClient);
	if (client == nullptr) {
		return kIOReturnNoMemory;
	}
	if (!client->initWithTask(owningTask, securityID, type, properties) ||
	    !client->attach(this) || !client->start(this)) {
		client->release();
		return kIOReturnError;
	}
	*handler = client;
	return kIOReturnSuccess;
}
