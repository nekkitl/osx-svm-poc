#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "amdvm_probe_abi.h"

static int
amdvm_run_query(io_connect_t connection)
{
	amdvm_probe_result_t result;
	size_t outputSize = sizeof(result);
	IOReturn status;

	memset(&result, 0, sizeof(result));
	status = IOConnectCallStructMethod(connection, AMDVM_PROBE_SELECTOR_QUERY,
	    NULL, 0, &result, &outputSize);
	if (status != kIOReturnSuccess) {
		fprintf(stderr, "query failed: 0x%08x\n", status);
		return 1;
	}
	if (result.abi_version != AMDVM_PROBE_ABI_VERSION) {
		fprintf(stderr, "ABI mismatch: got %u\n", result.abi_version);
		return 1;
	}
	printf("abi_version=%u\n", result.abi_version);
	printf("logical_cpus_seen=%u\n", result.logical_cpus_seen);
	printf("svm_cpuid_cpus=%u\n", result.svm_cpuid_cpus);
	printf("npt_cpuid_cpus=%u\n", result.npt_cpuid_cpus);
	printf("nrip_save_cpus=%u\n", result.nrip_save_cpus);
	printf("vm_cr_svmdis_cpus=%u\n", result.vm_cr_svmdis_cpus);
	printf("vm_cr_lock_cpus=%u\n", result.vm_cr_lock_cpus);
	printf("efer_svme_cpus=%u\n", result.efer_svme_cpus);
	printf("hsave_nonzero_cpus=%u\n", result.hsave_nonzero_cpus);
	printf("hsave_valid_cpus=%u\n", result.hsave_valid_cpus);
	printf("cpu_index_overflow=%u\n", result.cpu_index_overflow);
	printf("quarantined_memory_contexts=%u\n",
	    result.quarantined_memory_contexts);
	printf("quarantined_memory_ranges=%u\n",
	    result.quarantined_memory_ranges);
	printf("quarantined_guest_bytes=%llu\n",
	    (unsigned long long)result.quarantined_guest_bytes);
	for (uint32_t i = 0; i < AMDVM_PROBE_MAX_CPUS; i++) {
		const amdvm_probe_cpu_state_t *cpu = &result.cpus[i];
		if ((cpu->flags & AMDVM_CPU_PRESENT) == 0) {
			continue;
		}
		printf("cpu[%u]: flags=0x%08x VM_CR=0x%016" PRIx64
		    " EFER=0x%016" PRIx64 " VM_HSAVE_PA=0x%016" PRIx64 "\n",
		    cpu->cpu_index, cpu->flags, cpu->vm_cr, cpu->efer,
		    cpu->hsave_pa);
	}
	return 0;
}

static int
amdvm_run_hlt_preflight(io_connect_t connection)
{
	amdvm_hlt_preflight_result_t result;
	size_t outputSize = sizeof(result);
	IOReturn status;
	memset(&result, 0, sizeof(result));
	status = IOConnectCallStructMethod(connection,
	    AMDVM_PROBE_SELECTOR_HLT_PREFLIGHT, NULL, 0, &result, &outputSize);
	if (status != kIOReturnSuccess) {
		fprintf(stderr, "HLT preflight failed: 0x%08x\n", status);
		return 1;
	}
	if (result.abi_version != AMDVM_HLT_PREFLIGHT_ABI_VERSION) {
		fprintf(stderr, "HLT preflight ABI mismatch: got %u\n",
		    result.abi_version);
		return 1;
	}
	printf("init_status=%d\n", result.init_status);
	printf("destroy_status=%d\n", result.destroy_status);
	printf("flags=0x%08x\n", result.flags);
	printf("svm_msrs_touched=%s\n",
	    (result.flags & AMDVM_HLT_PREFLIGHT_SVM_UNTOUCHED) ? "no" : "unknown");
	printf("vmrun_executed=%s\n",
	    (result.flags & AMDVM_HLT_PREFLIGHT_VMRUN_NOT_EXECUTED) ? "no" : "unknown");
	return result.init_status == 0 && result.destroy_status == 0 &&
	    (result.flags & (AMDVM_HLT_PREFLIGHT_ALLOCATIONS_VALID |
	    AMDVM_HLT_PREFLIGHT_NPT_READY | AMDVM_HLT_PREFLIGHT_HLT_PAYLOAD |
	    AMDVM_HLT_PREFLIGHT_VMCB_PAGES_VALID |
	    AMDVM_HLT_PREFLIGHT_SVM_UNTOUCHED |
	    AMDVM_HLT_PREFLIGHT_VMRUN_NOT_EXECUTED |
	    AMDVM_HLT_PREFLIGHT_DESTROYED)) ==
	    (AMDVM_HLT_PREFLIGHT_ALLOCATIONS_VALID |
	    AMDVM_HLT_PREFLIGHT_NPT_READY | AMDVM_HLT_PREFLIGHT_HLT_PAYLOAD |
	    AMDVM_HLT_PREFLIGHT_VMCB_PAGES_VALID |
	    AMDVM_HLT_PREFLIGHT_SVM_UNTOUCHED |
	    AMDVM_HLT_PREFLIGHT_VMRUN_NOT_EXECUTED |
	    AMDVM_HLT_PREFLIGHT_DESTROYED) ? 0 : 1;
}

static int
amdvm_run_hlt_fpu_preflight(io_connect_t connection)
{
	amdvm_hlt_fpu_preflight_result_t result;
	size_t outputSize = sizeof(result);
	IOReturn status;
	const uint32_t expected = AMDVM_HLT_FPU_PREFLIGHT_LOCKED_CPU |
	    AMDVM_HLT_FPU_PREFLIGHT_XSAVE_OK |
	    AMDVM_HLT_FPU_PREFLIGHT_TS_CLEARED |
	    AMDVM_HLT_FPU_PREFLIGHT_CPU_STABLE |
	    AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE |
	    AMDVM_HLT_FPU_PREFLIGHT_INTERRUPTS_RESTORED |
	    AMDVM_HLT_FPU_PREFLIGHT_VMRUN_NOT_EXECUTED |
	    AMDVM_HLT_FPU_PREFLIGHT_CONTEXT_DESTROYED;

	memset(&result, 0, sizeof(result));
	/* Fault in this caller's PCB state before the kext's CPU-pinning lock. */
	__asm__ volatile("pxor %%xmm0, %%xmm0" : : : "xmm0");
	status = IOConnectCallStructMethod(connection,
	    AMDVM_PROBE_SELECTOR_HLT_FPU_PREFLIGHT, NULL, 0,
	    &result, &outputSize);
	if (status != kIOReturnSuccess) {
		fprintf(stderr, "HLT FPU preflight failed: 0x%08x\n", status);
		return 1;
	}
	if (result.abi_version != AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION) {
		fprintf(stderr, "HLT FPU preflight ABI mismatch: got %u\n",
		    result.abi_version);
		return 1;
	}
	printf("init_status=%d\n", result.init_status);
	printf("first_xsave_status=%d\n", result.first_xsave_status);
	printf("second_xsave_status=%d\n", result.second_xsave_status);
	printf("destroy_status=%d\n", result.destroy_status);
	printf("flags=0x%08x\n", result.flags);
	printf("cpu_before=%u\n", result.cpu_before);
	printf("cpu_after=%u\n", result.cpu_after);
	printf("cr0_ts_before=%u\n", result.ts_before);
	printf("cr0_ts_after=%u\n", result.ts_after);
	printf("cr0_em_before=%u\n", result.em_before);
	printf("cr0_em_after=%u\n", result.em_after);
	printf("xcr0=0x%016" PRIx64 "\n", result.xcr0);
	printf("svm_msrs_touched=no\n");
	printf("vmrun_executed=no\n");
	return result.init_status == 0 && result.first_xsave_status == 0 &&
	    result.second_xsave_status == 0 && result.destroy_status == 0 &&
	    result.flags == expected ? 0 : 1;
}

static int
amdvm_run_hlt_once(io_connect_t connection)
{
	amdvm_hlt_run_request_t request;
	amdvm_hlt_run_result_t result;
	size_t outputSize = sizeof(result);
	IOReturn status;
	uint32_t expectedFlags = AMDVM_HLT_RUN_CONTEXT_INITIALIZED |
	    AMDVM_HLT_RUN_CALLED | AMDVM_HLT_RUN_CONTEXT_DESTROYED;

	memset(&request, 0, sizeof(request));
	request.abi_version = AMDVM_HLT_RUN_REQUEST_ABI_VERSION;
	request.flags = AMDVM_HLT_RUN_CONFIRM_NO_KDP_WATCHPOINTS;
	memset(&result, 0, sizeof(result));
	/* Trigger XNU's lazy restore before entering the kext, so a first-use PCB
	 * allocation cannot happen later while the kernel holds a spin lock. XMM0
	 * is caller-saved; the kernel independently repeats XSAVE/#NM validation. */
	__asm__ volatile("pxor %%xmm0, %%xmm0" : : : "xmm0");
	status = IOConnectCallStructMethod(connection, AMDVM_PROBE_SELECTOR_HLT_RUN,
	    &request, sizeof(request), &result, &outputSize);
	if (status != kIOReturnSuccess) {
		fprintf(stderr, "HLT run request failed: 0x%08x\n", status);
		return 1;
	}
	if (result.abi_version != AMDVM_HLT_RUN_ABI_VERSION) {
		fprintf(stderr, "HLT run ABI mismatch: got %u\n", result.abi_version);
		return 1;
	}
	printf("context_status=%d\n", result.context_status);
	printf("owner_status=%d\n", result.owner_status);
	printf("vmrun_status=%d\n", result.vmrun_status);
	printf("destroy_status=%d\n", result.destroy_status);
	printf("flags=0x%08x\n", result.flags);
	printf("exit_code=0x%016" PRIx64 "\n", result.exit_code);
	printf("exit_info1=0x%016" PRIx64 "\n", result.exit_info1);
	printf("exit_info2=0x%016" PRIx64 "\n", result.exit_info2);
	printf("next_rip=0x%016" PRIx64 "\n", result.next_rip);
	printf("guest_rip=0x%016" PRIx64 "\n", result.guest_rip);
	printf("guest_rflags=0x%016" PRIx64 "\n", result.guest_rflags);
	if (result.context_status != 0 || result.owner_status != 0 ||
	    result.vmrun_status != 0 || result.destroy_status != 0 ||
	    result.exit_code != AMDVM_HLT_EXPECTED_EXIT ||
	    result.flags != expectedFlags) {
		return 1;
	}
	return 0;
}

static int
amdvm_vcpu_lifecycle_call(io_connect_t connection, uint32_t selector,
	const void *input, size_t inputSize,
	amdvm_vcpu_lifecycle_result_t *result)
{
	size_t outputSize = sizeof(*result);
	IOReturn status;

	memset(result, 0, sizeof(*result));
	status = IOConnectCallStructMethod(connection, selector, input, inputSize,
	    result, &outputSize);
	if (status != kIOReturnSuccess || outputSize != sizeof(*result) ||
	    result->abi_version != AMDVM_VCPU_ABI_VERSION ||
	    result->context_status != 0) {
		fprintf(stderr, "vCPU selector %u failed: io=0x%08x size=%zu abi=%u context=%d\n",
		    selector, status, outputSize, result->abi_version,
		    result->context_status);
		return 1;
	}
	return 0;
}

static bool
amdvm_probe_svm_state_equal(const amdvm_probe_result_t *left,
	const amdvm_probe_result_t *right)
{
	uint32_t i;

	if (left->logical_cpus_seen != right->logical_cpus_seen ||
	    left->svm_cpuid_cpus != right->svm_cpuid_cpus ||
	    left->npt_cpuid_cpus != right->npt_cpuid_cpus) {
		return false;
	}
	for (i = 0; i < AMDVM_PROBE_MAX_CPUS; ++i) {
		const amdvm_probe_cpu_state_t *a = &left->cpus[i];
		const amdvm_probe_cpu_state_t *b = &right->cpus[i];

		if (a->cpu_index != b->cpu_index || a->flags != b->flags ||
		    a->vm_cr != b->vm_cr || a->efer != b->efer ||
		    a->hsave_pa != b->hsave_pa) {
			return false;
		}
	}
	return true;
}

/* Exercise the real IOKit ABI without entering VMRUN. */
static int
amdvm_run_vcpu_ram_smoke(io_connect_t connection, bool execute_guest)
{
	amdvm_probe_result_t query;
	amdvm_probe_result_t after;
	amdvm_vcpu_create_request_t create = { 0 };
	amdvm_vcpu_set_reset_rom_request_t rom = { 0 };
	amdvm_vcpu_memory_map_request_t map = { 0 };
	amdvm_vcpu_memory_map_result_t mapResult = { 0 };
	amdvm_vcpu_set_memory_types_request_t memoryTypes = { 0 };
	amdvm_vcpu_lifecycle_result_t memoryTypesResult = { 0 };
	amdvm_vcpu_kick_request_t kick = { 0 };
	amdvm_vcpu_run_request_t run = { 0 };
	amdvm_vcpu_run_result_t runResult = { 0 };
	amdvm_vcpu_lifecycle_result_t lifecycle = { 0 };
	void *ramPage = NULL;
	size_t outputSize;
	IOReturn status;
	bool created = false;
	bool destroyed = false;
	bool safeToFreeRam = true;
	bool guestSmokePassed = false;
	int ret = 1;

	outputSize = sizeof(query);
	memset(&query, 0, sizeof(query));
	status = IOConnectCallStructMethod(connection, AMDVM_PROBE_SELECTOR_QUERY,
	    NULL, 0, &query, &outputSize);
	if (status != kIOReturnSuccess || outputSize != sizeof(query) ||
	    query.abi_version != AMDVM_PROBE_ABI_VERSION) {
		fprintf(stderr, "ABI negotiation failed: io=0x%08x size=%zu abi=%u\n",
		    status, outputSize, query.abi_version);
		goto cleanup;
	}
	printf("abi_version=%u\n", query.abi_version);

	/* This preflight touches XSAVE only; its ABI explicitly forbids VMRUN. */
	if (amdvm_run_hlt_fpu_preflight(connection) != 0) {
		goto cleanup;
	}

	if (posix_memalign(&ramPage, 4096u, 4096u) != 0) {
		fprintf(stderr, "RAM test page allocation failed\n");
		goto cleanup;
	}
	memset(ramPage, 0xa5, 4096u);
	if (execute_guest) ((uint8_t *)ramPage)[0] = 0x41u;

	create.abi_version = AMDVM_VCPU_ABI_VERSION;
	create.guest_profile = AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1;
	if (amdvm_vcpu_lifecycle_call(connection,
	    AMDVM_PROBE_SELECTOR_VCPU_CREATE, &create, sizeof(create),
	    &lifecycle) != 0) {
		goto cleanup;
	}
	created = true;
	printf("vcpu_create=ok\n");

	rom.abi_version = AMDVM_VCPU_ABI_VERSION;
	if (execute_guest) {
		/* MOV AL,[DS:0] ; OUT 0xe9,AL ; HLT. DS.base points at GPA 0x1000. */
		static const uint8_t code[] = {0xa0, 0x00, 0x00, 0xe6, 0xe9, 0xf4};
		memcpy(&rom.page[AMDVM_VCPU_RESET_ROM_OFFSET], code, sizeof(code));
	} else {
		rom.page[AMDVM_VCPU_RESET_ROM_OFFSET] = AMDVM_VCPU_TEST_ROM_HLT;
	}
	if (amdvm_vcpu_lifecycle_call(connection,
	    AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM, &rom, sizeof(rom),
	    &lifecycle) != 0) {
		goto cleanup;
	}

	map.abi_version = AMDVM_VCPU_ABI_VERSION;
	map.range_count = 1;
	map.ranges[0].guest_physical_address = 0x1000u;
	map.ranges[0].user_virtual_address = (uint64_t)(uintptr_t)ramPage;
	map.ranges[0].size = 4096u;
	map.ranges[0].flags = AMDVM_VCPU_MEMORY_READ | AMDVM_VCPU_MEMORY_WRITE;
	outputSize = sizeof(mapResult);
	status = IOConnectCallStructMethod(connection,
	    AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY, &map, sizeof(map),
	    &mapResult, &outputSize);
	if (status != kIOReturnSuccess || outputSize != sizeof(mapResult) ||
	    mapResult.abi_version != AMDVM_VCPU_ABI_VERSION ||
	    mapResult.context_status != 0 || mapResult.range_count != 1 ||
	    mapResult.guest_page_count != 1 || mapResult.npt_page_count == 0) {
		fprintf(stderr, "RAM pin/NPT map failed: io=0x%08x size=%zu abi=%u context=%d pages=%llu npt=%llu\n",
		    status, outputSize, mapResult.abi_version,
		    mapResult.context_status,
		    (unsigned long long)mapResult.guest_page_count,
		    (unsigned long long)mapResult.npt_page_count);
		goto cleanup;
	}
	printf("ram_pin=ok guest_pages=%llu npt_pages=%llu\n",
	    (unsigned long long)mapResult.guest_page_count,
	    (unsigned long long)mapResult.npt_page_count);

	if (execute_guest) {
		memoryTypes.abi_version = AMDVM_VCPU_ABI_VERSION;
		memoryTypes.range_count = 2;
		memoryTypes.ranges[0].guest_physical_address = 0x1000u;
		memoryTypes.ranges[0].size = 4096u;
		memoryTypes.ranges[0].memory_type = 0; /* UC */
		memoryTypes.ranges[1].guest_physical_address =
		    AMDVM_VCPU_RESET_ROM_GPA;
		memoryTypes.ranges[1].size = 4096u;
		memoryTypes.ranges[1].memory_type = 0; /* UC */
		if (amdvm_vcpu_lifecycle_call(connection,
		    AMDVM_PROBE_SELECTOR_VCPU_SET_MEMORY_TYPES, &memoryTypes,
		    sizeof(memoryTypes), &memoryTypesResult) != 0 ||
		    memoryTypesResult.reserved[0] != 2u) {
			fprintf(stderr, "UC memory-type commit failed: updated=%u\n",
			    memoryTypesResult.reserved[0]);
			goto cleanup;
		}
		printf("memory_types=UC pages=2 execution_gpat_expected=0\n");
	}

	if (execute_guest) {
		/* Real-mode DS:0 translates to GPA 0x1000 through the mapped page. */
		run.abi_version = AMDVM_VCPU_ABI_VERSION;
		run.flags = AMDVM_VCPU_RUN_CONFIRM_NO_KDP_WATCHPOINTS;
		run.state.abi_version = AMDVM_X86_STATE_ABI_VERSION;
		run.state.rip = AMDVM_VCPU_RESET_VECTOR_RIP;
		run.state.rflags = 2u;
		run.state.segments[AMDVM_X86_SEG_CS].selector = 0xf000u;
		run.state.segments[AMDVM_X86_SEG_CS].attributes = 0x9bu;
		run.state.segments[AMDVM_X86_SEG_CS].limit = 0xffffu;
		run.state.segments[AMDVM_X86_SEG_CS].base = AMDVM_VCPU_RESET_CS_BASE;
		run.state.segments[AMDVM_X86_SEG_DS].selector = 0x100u;
		run.state.segments[AMDVM_X86_SEG_DS].attributes = 0x93u;
		run.state.segments[AMDVM_X86_SEG_DS].limit = 0xffffu;
		run.state.segments[AMDVM_X86_SEG_DS].base = 0x1000u;
		run.state.segments[AMDVM_X86_SEG_ES] = run.state.segments[AMDVM_X86_SEG_DS];
		run.state.segments[AMDVM_X86_SEG_SS] = run.state.segments[AMDVM_X86_SEG_DS];
		run.state.segments[AMDVM_X86_SEG_FS] = run.state.segments[AMDVM_X86_SEG_DS];
		run.state.segments[AMDVM_X86_SEG_GS] = run.state.segments[AMDVM_X86_SEG_DS];
		run.state.segments[AMDVM_X86_SEG_GDTR].limit = 0xffffu;
		run.state.segments[AMDVM_X86_SEG_LDTR].limit = 0xffffu;
		run.state.segments[AMDVM_X86_SEG_IDTR].limit = 0xffffu;
		run.state.segments[AMDVM_X86_SEG_TR].attributes = 0x8bu;
		run.state.segments[AMDVM_X86_SEG_TR].limit = 0xffffu;
		run.state.cr[0] = UINT64_C(0x60000010);
		run.state.dr[6] = UINT64_C(0xffff0ff0);
		run.state.dr[7] = 0x400u;
		run.state.pat = UINT64_C(0x0007040600070406);
		run.state.xcr0 = 1u;
		run.state.xstate_size = AMDVM_X86_XSTATE_MAX_SIZE;
		run.state.xstate[160] = 0x5au;
		outputSize = sizeof(runResult);
		status = IOConnectCallStructMethod(connection,
		    AMDVM_PROBE_SELECTOR_VCPU_RUN, &run, sizeof(run),
		    &runResult, &outputSize);
		if (status != kIOReturnSuccess || outputSize != sizeof(runResult) ||
		    runResult.abi_version != AMDVM_VCPU_ABI_VERSION ||
		    !(runResult.flags & AMDVM_VCPU_RUN_CALLED) ||
		    (runResult.flags & AMDVM_VCPU_RUN_LEASE_RETAINED) ||
		    runResult.context_status != 0 || runResult.owner_status != 0 ||
		    runResult.vmrun_status != 0 ||
		    runResult.exit_code != AMDVM_VCPU_EXPECTED_IOIO_EXIT ||
		    runResult.next_rip != AMDVM_VCPU_RESET_VECTOR_RIP + 5u ||
		    (runResult.exit_info1 & 1u) != 0 || /* OUT, not IN */
		    (runResult.exit_info1 & (1u << 2)) != 0 || /* no string I/O */
		    (runResult.exit_info1 & (1u << 3)) != 0 || /* no REP */
		    ((runResult.exit_info1 >> 4) & 7u) != 1u || /* byte width */
		    ((runResult.exit_info1 >> 16) & 0xffffu) != 0xe9u ||
		    (runResult.gpr[0] & 0xffu) != 0x41u ||
		    runResult.state.pat != run.state.pat) {
			fprintf(stderr, "RAM UC read/IO exit failed: io=0x%08x exit=0x%llx info=0x%llx AL=0x%02llx RIP=0x%llx NRIP=0x%llx PAT=0x%llx\n",
			    status, (unsigned long long)runResult.exit_code,
			    (unsigned long long)runResult.exit_info1,
			    (unsigned long long)(runResult.gpr[0] & 0xffu),
			    (unsigned long long)runResult.rip,
			    (unsigned long long)runResult.next_rip,
			    (unsigned long long)runResult.state.pat);
			goto cleanup;
		}
		printf("vmexit_ioio=0x7b read_back=0x%02llx guest_pat_preserved=yes\n",
		    (unsigned long long)(runResult.gpr[0] & 0xffu));
		run.state = runResult.state;
		run.state.rip = runResult.next_rip;
		outputSize = sizeof(runResult);
		memset(&runResult, 0, sizeof(runResult));
		status = IOConnectCallStructMethod(connection,
		    AMDVM_PROBE_SELECTOR_VCPU_RUN, &run, sizeof(run),
		    &runResult, &outputSize);
		if (status != kIOReturnSuccess || outputSize != sizeof(runResult) ||
		    !(runResult.flags & AMDVM_VCPU_RUN_CALLED) ||
		    (runResult.flags & AMDVM_VCPU_RUN_LEASE_RETAINED) ||
		    runResult.context_status != 0 || runResult.owner_status != 0 ||
		    runResult.vmrun_status != 0 ||
		    runResult.exit_code != AMDVM_HLT_EXPECTED_EXIT ||
		    runResult.state.pat != run.state.pat) {
			fprintf(stderr, "bounded HLT exit failed: io=0x%08x exit=0x%llx status=%d/%d/%d\n",
			    status, (unsigned long long)runResult.exit_code,
			    runResult.context_status, runResult.owner_status,
			    runResult.vmrun_status);
			goto cleanup;
		}
		printf("vmexit_hlt=0x78 guest_pat_preserved=yes\n");
		guestSmokePassed = true;
		ret = 0;
		goto cleanup;
	}

	/*
	 * Construct the QEMU reset profile, then make the provider return through
	 * its pre-entry stale-generation path. The full state crosses IOUserClient
	 * and is echoed back, but neither the VMCB loader nor VMRUN is reached.
	 */
	run.abi_version = AMDVM_VCPU_ABI_VERSION;
	run.flags = AMDVM_VCPU_RUN_CONFIRM_NO_KDP_WATCHPOINTS;
	run.state.abi_version = AMDVM_X86_STATE_ABI_VERSION;
	run.state.gpr[0] = UINT64_C(0x0123456789abcdef);
	run.state.rip = AMDVM_VCPU_RESET_VECTOR_RIP;
	run.state.rflags = 2u;
	run.state.segments[AMDVM_X86_SEG_CS].selector = 0xf000u;
	run.state.segments[AMDVM_X86_SEG_CS].attributes = 0x9bu;
	run.state.segments[AMDVM_X86_SEG_CS].limit = 0xffffu;
	run.state.segments[AMDVM_X86_SEG_CS].base = AMDVM_VCPU_RESET_CS_BASE;
	run.state.segments[AMDVM_X86_SEG_DS].attributes = 0x93u;
	run.state.segments[AMDVM_X86_SEG_DS].limit = 0xffffu;
	run.state.segments[AMDVM_X86_SEG_ES] =
	    run.state.segments[AMDVM_X86_SEG_DS];
	run.state.segments[AMDVM_X86_SEG_SS] =
	    run.state.segments[AMDVM_X86_SEG_DS];
	run.state.segments[AMDVM_X86_SEG_FS] =
	    run.state.segments[AMDVM_X86_SEG_DS];
	run.state.segments[AMDVM_X86_SEG_GS] =
	    run.state.segments[AMDVM_X86_SEG_DS];
	run.state.segments[AMDVM_X86_SEG_GDTR].limit = 0xffffu;
	run.state.segments[AMDVM_X86_SEG_LDTR].limit = 0xffffu;
	run.state.segments[AMDVM_X86_SEG_IDTR].limit = 0xffffu;
	run.state.segments[AMDVM_X86_SEG_TR].attributes = 0x8bu;
	run.state.segments[AMDVM_X86_SEG_TR].limit = 0xffffu;
	run.state.cr[0] = UINT64_C(0x60000010);
	run.state.dr[6] = UINT64_C(0xffff0ff0);
	run.state.dr[7] = 0x400u;
	run.state.pat = UINT64_C(0x0007040600070406);
	run.state.xcr0 = 1u;
	run.state.xstate_size = AMDVM_X86_XSTATE_MAX_SIZE;
	run.state.xstate[160] = 0x5au;

	kick.abi_version = AMDVM_VCPU_ABI_VERSION;
	kick.generation = 1u;
	status = IOConnectCallStructMethod(connection,
	    AMDVM_PROBE_SELECTOR_VCPU_KICK, &kick, sizeof(kick), NULL, NULL);
	if (status != kIOReturnSuccess) {
		fprintf(stderr, "pre-entry kick setup failed: 0x%08x\n", status);
		goto cleanup;
	}

	outputSize = sizeof(runResult);
	memset(&runResult, 0, sizeof(runResult));
	status = IOConnectCallStructMethod(connection, AMDVM_PROBE_SELECTOR_VCPU_RUN,
	    &run, sizeof(run), &runResult, &outputSize);
	if (status != kIOReturnSuccess || outputSize != sizeof(runResult) ||
	    runResult.abi_version != AMDVM_VCPU_ABI_VERSION ||
	    runResult.flags != AMDVM_VCPU_RUN_KICKED ||
	    runResult.exit_code != UINT64_MAX ||
	    runResult.context_status != 0 || runResult.owner_status != 0 ||
	    runResult.vmrun_status != 0 ||
	    memcmp(&runResult.state, &run.state, sizeof(run.state)) != 0) {
		fprintf(stderr, "no-entry CPU-state transfer failed: io=0x%08x size=%zu abi=%u flags=0x%x exit=0x%llx\n",
		    status, outputSize, runResult.abi_version, runResult.flags,
		    (unsigned long long)runResult.exit_code);
		goto cleanup;
	}
	printf("cpu_state_transport=roundtrip-no-entry\n");
	printf("vmrun_executed=no\n");

	if (amdvm_vcpu_lifecycle_call(connection,
	    AMDVM_PROBE_SELECTOR_VCPU_DESTROY, NULL, 0, &lifecycle) != 0) {
		goto cleanup;
	}
	destroyed = true;
	created = false;
	printf("ram_unpin=completed-by-destroy\n");
	printf("vcpu_destroy=ok\n");
	ret = execute_guest ? (guestSmokePassed ? 0 : 1) : 0;

cleanup:
	if (created && !destroyed) {
		if (amdvm_vcpu_lifecycle_call(connection,
		    AMDVM_PROBE_SELECTOR_VCPU_DESTROY, NULL, 0, &lifecycle) != 0) {
			fprintf(stderr, "cleanup destroy failed; retain RAM until IOServiceClose\n");
			safeToFreeRam = false;
			ret = 1;
		} else {
			destroyed = true;
			created = false;
			printf("cleanup_destroy=ok\n");
		}
	}
	if (destroyed) {
		outputSize = sizeof(after);
		memset(&after, 0, sizeof(after));
		status = IOConnectCallStructMethod(connection, AMDVM_PROBE_SELECTOR_QUERY,
		    NULL, 0, &after, &outputSize);
		if (status != kIOReturnSuccess || outputSize != sizeof(after) ||
		    after.abi_version != AMDVM_PROBE_ABI_VERSION ||
		    !amdvm_probe_svm_state_equal(&query, &after)) {
			fprintf(stderr, "post-destroy host SVM state comparison failed\n");
			ret = 1;
		} else {
			printf("host_svm_state_unchanged=yes\n");
		}
	}
	if (safeToFreeRam) {
		free(ramPage);
	}
	return ret;
}

int
main(int argc, char **argv)
{
	io_service_t service;
	io_connect_t connection = IO_OBJECT_NULL;
	IOReturn status;
	int result;
	if ((argc == 2 && strcmp(argv[1], "--hlt-preflight") != 0 &&
	    strcmp(argv[1], "--hlt-fpu-preflight") != 0 &&
	    strcmp(argv[1], "--vcpu-no-vmrun-smoke") != 0) ||
	    (argc == 3 &&
	    ((strcmp(argv[1], "--hlt-run") != 0 &&
	    strcmp(argv[1], "--bounded-pat-ram-smoke") != 0) ||
	    strcmp(argv[2], "--confirm-no-kdp-watchpoints") != 0)) || argc > 3) {
		fprintf(stderr, "usage: %s [--hlt-preflight|--hlt-fpu-preflight|"
	    "--vcpu-no-vmrun-smoke|--bounded-pat-ram-smoke "
	    "--confirm-no-kdp-watchpoints|--hlt-run "
		    "--confirm-no-kdp-watchpoints]\n", argv[0]);
		return 2;
	}
	service = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching("AMDVMProbeService"));
	if (service == IO_OBJECT_NULL) {
		fprintf(stderr, "AMDVMProbeService not found\n");
		return 1;
	}
	status = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	if (status != kIOReturnSuccess) {
		fprintf(stderr, "IOServiceOpen failed: 0x%08x\n", status);
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "--hlt-preflight") == 0) {
		result = amdvm_run_hlt_preflight(connection);
	} else if (argc == 2 && strcmp(argv[1], "--hlt-fpu-preflight") == 0) {
		result = amdvm_run_hlt_fpu_preflight(connection);
	} else if (argc == 3 && strcmp(argv[1], "--hlt-run") == 0 &&
	    strcmp(argv[2], "--confirm-no-kdp-watchpoints") == 0) {
		result = amdvm_run_hlt_once(connection);
	} else if (argc == 2 && strcmp(argv[1], "--vcpu-no-vmrun-smoke") == 0) {
		result = amdvm_run_vcpu_ram_smoke(connection, false);
	} else if (argc == 3 && strcmp(argv[1], "--bounded-pat-ram-smoke") == 0 &&
	    strcmp(argv[2], "--confirm-no-kdp-watchpoints") == 0) {
		result = amdvm_run_vcpu_ram_smoke(connection, true);
	} else {
		result = amdvm_run_query(connection);
	}
	status = IOServiceClose(connection);
	if (status != kIOReturnSuccess) {
		fprintf(stderr, "IOServiceClose failed: 0x%08x\n", status);
		return 1;
	}
	if (argc == 2 && strcmp(argv[1], "--vcpu-no-vmrun-smoke") == 0) {
		printf("io_service_close=ok\n");
	}
	return result;
}
