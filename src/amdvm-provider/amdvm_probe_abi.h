#ifndef AMDVM_PROBE_ABI_H
#define AMDVM_PROBE_ABI_H

#include <stdint.h>

#define AMDVM_PROBE_ABI_VERSION 13u
#define AMDVM_PROBE_SELECTOR_QUERY 0u
#define AMDVM_PROBE_SELECTOR_HLT_PREFLIGHT 1u
#define AMDVM_PROBE_SELECTOR_HLT_RUN 2u
#define AMDVM_PROBE_SELECTOR_HLT_FPU_PREFLIGHT 3u
#define AMDVM_PROBE_SELECTOR_VCPU_CREATE 4u
#define AMDVM_PROBE_SELECTOR_VCPU_RUN 5u
#define AMDVM_PROBE_SELECTOR_VCPU_DESTROY 6u
#define AMDVM_PROBE_SELECTOR_VCPU_MAP_MEMORY 7u
#define AMDVM_PROBE_SELECTOR_VCPU_SET_RESET_ROM 8u
#define AMDVM_PROBE_SELECTOR_VCPU_KICK 9u
#define AMDVM_PROBE_SELECTOR_VCPU_SET_MEMORY_TYPES 10u
#define AMDVM_PROBE_SELECTOR_VCPU_DIAGNOSTIC 11u
#define AMDVM_PROBE_SELECTOR_VCPU_CONTROL_DIAGNOSTIC 12u
#define AMDVM_PROBE_MAX_CPUS 64u

#define AMDVM_VCPU_ABI_VERSION 16u
#define AMDVM_VCPU_GUEST_CODE_MAX AMDVM_VCPU_TEST_ROM_SIZE
#define AMDVM_VCPU_RUN_CONFIRM_NO_KDP_WATCHPOINTS (1u << 0)
#define AMDVM_VCPU_RUN_CALLED                    (1u << 0)
#define AMDVM_VCPU_RUN_LEASE_RETAINED            (1u << 1)
#define AMDVM_VCPU_RUN_KICKED                    (1u << 2)
#define AMDVM_VCPU_MEMORY_MAX_RANGES 64u
#define AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES 256u
/* Guest-RAM policy cap. This does not change the memory-map request layout. */
#define AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES (8ull * 1024ull * 1024ull * 1024ull)
/* Keep each pinned IOMemoryDescriptor below UINT32_MAX and page aligned. */
#define AMDVM_VCPU_MEMORY_MAX_RANGE_BYTES \
	((uint64_t)UINT32_MAX & ~UINT64_C(4095))
#define AMDVM_VCPU_MEMORY_READ  (1u << 0)
#define AMDVM_VCPU_MEMORY_WRITE (1u << 1)
#define AMDVM_VCPU_MEMORY_EXEC  (1u << 2)
#define AMDVM_VCPU_TEST_ROM_SIZE 7u
#define AMDVM_VCPU_TEST_ROM_CPUID_0 0x0fu
#define AMDVM_VCPU_TEST_ROM_CPUID_1 0xa2u
#define AMDVM_VCPU_TEST_ROM_MOV_AL_IMM8 0xb0u
#define AMDVM_VCPU_TEST_ROM_DEBUG_BYTE 0x41u
#define AMDVM_VCPU_TEST_ROM_OUT_IMM8_AL 0xe6u
#define AMDVM_VCPU_TEST_ROM_DEBUG_PORT 0xe9u
#define AMDVM_VCPU_TEST_ROM_HLT 0xf4u
#define AMDVM_VCPU_GUEST_PROFILE_REALMODE_TEST_V1 1u
#define AMDVM_VCPU_GUEST_PROFILE_QEMU_RESET_VECTOR_V1 2u
#define AMDVM_VCPU_GUEST_PROFILE_QEMU_FIRMWARE_V1 3u
#define AMDVM_VCPU_RESET_ROM_SIZE 4096u
#define AMDVM_VCPU_RESET_CS_BASE 0xffff0000ull
#define AMDVM_VCPU_RESET_VECTOR_LINEAR \
	(AMDVM_VCPU_RESET_CS_BASE + AMDVM_VCPU_RESET_VECTOR_RIP)
#define AMDVM_VCPU_RESET_ROM_GPA 0xfffff000ull
#define AMDVM_VCPU_RESET_ROM_OFFSET 0xff0u
#define AMDVM_VCPU_RESET_VECTOR_RIP 0xfff0u
#define AMDVM_VCPU_TEST_ROM_CPUID_RIP AMDVM_VCPU_RESET_VECTOR_RIP
#define AMDVM_VCPU_TEST_ROM_CPUID_NEXT_RIP (AMDVM_VCPU_RESET_VECTOR_RIP + 2u)
#define AMDVM_VCPU_TEST_ROM_IO_RIP (AMDVM_VCPU_RESET_VECTOR_RIP + 4u)
#define AMDVM_VCPU_TEST_ROM_IO_NEXT_RIP (AMDVM_VCPU_RESET_VECTOR_RIP + 6u)
#define AMDVM_VCPU_TEST_ROM_HLT_RIP (AMDVM_VCPU_RESET_VECTOR_RIP + 6u)
#define AMDVM_VCPU_EXPECTED_CPUID_EXIT 0x72ull
#define AMDVM_VCPU_EXPECTED_IOIO_EXIT 0x7bull
#define AMDVM_VCPU_EXIT_MSR 0x7cull
#define AMDVM_VCPU_EXIT_PHYSICAL_INTR 0x60ull

/* Stable architectural state order for the forthcoming QEMU vCPU ABI. */
#define AMDVM_X86_STATE_ABI_VERSION 2u
#define AMDVM_X86_INTERRUPTIBILITY_STI_SHADOW (1u << 0)
#define AMDVM_VCPU_EVENTINJ_VALID (1u << 31)
#define AMDVM_VCPU_EVENTINJ_TYPE_SHIFT 8u
#define AMDVM_VCPU_EVENTINJ_TYPE_MASK (7u << AMDVM_VCPU_EVENTINJ_TYPE_SHIFT)
#define AMDVM_VCPU_EVENTINJ_TYPE_EXTERNAL 0u
#define AMDVM_VCPU_EVENTINJ_TYPE_EXCEPTION (3u << AMDVM_VCPU_EVENTINJ_TYPE_SHIFT)
#define AMDVM_VCPU_EVENTINJ_ERROR_VALID (1u << 11)
#define AMDVM_VCPU_EVENTINJ_RESERVED_MASK 0x7ffff000u
#define AMDVM_VCPU_EXCEPTION_GP 13u
#define AMDVM_VCPU_EXCEPTION_PF 14u
#define AMDVM_VCPU_EXITINTINFO_VALID (1u << 31)
#define AMDVM_X86_GPR_COUNT 16u
#define AMDVM_X86_SEGMENT_COUNT 10u
#define AMDVM_X86_XSTATE_MAX_SIZE 4096u
#define AMDVM_X86_SEG_ES 0u
#define AMDVM_X86_SEG_CS 1u
#define AMDVM_X86_SEG_SS 2u
#define AMDVM_X86_SEG_DS 3u
#define AMDVM_X86_SEG_FS 4u
#define AMDVM_X86_SEG_GS 5u
#define AMDVM_X86_SEG_GDTR 6u
#define AMDVM_X86_SEG_LDTR 7u
#define AMDVM_X86_SEG_IDTR 8u
#define AMDVM_X86_SEG_TR 9u

/* GPR slots intentionally match QEMU target/i386/cpu.h R_* indices. */
typedef struct amdvm_x86_segment_state {
	uint16_t selector;
	uint16_t attributes;
	uint32_t limit;
	uint64_t base;
} amdvm_x86_segment_state_t;

typedef struct amdvm_x86_guest_state_v1 {
	uint32_t abi_version;
	uint32_t interruptibility;
	uint64_t gpr[AMDVM_X86_GPR_COUNT];
	uint64_t rip;
	uint64_t rflags;
	amdvm_x86_segment_state_t segments[AMDVM_X86_SEGMENT_COUNT];
	uint64_t cr[5]; /* CR0, reserved CR1, CR2, CR3, CR4 */
	uint64_t efer;
	uint64_t dr[8];
	uint64_t pat;
	uint64_t xcr0;
	uint64_t sysenter_cs;
	uint64_t sysenter_esp;
	uint64_t sysenter_eip;
	uint64_t star;
	uint64_t lstar;
	uint64_t cstar;
	uint64_t sfmask;
	uint64_t kernel_gs_base;
	uint32_t xstate_size;
	uint32_t xstate_reserved;
	uint8_t xstate[AMDVM_X86_XSTATE_MAX_SIZE];
} amdvm_x86_guest_state_v1_t;

#define AMDVM_HLT_PREFLIGHT_ABI_VERSION 1u
#define AMDVM_HLT_FPU_PREFLIGHT_ABI_VERSION 1u
#define AMDVM_HLT_RUN_ABI_VERSION 2u
#define AMDVM_HLT_RUN_REQUEST_ABI_VERSION 1u
#define AMDVM_HLT_EXPECTED_EXIT 0x78ull
#define AMDVM_HLT_RUN_CONFIRM_NO_KDP_WATCHPOINTS (1u << 0)
#define AMDVM_HLT_PREFLIGHT_ALLOCATIONS_VALID (1u << 0)
#define AMDVM_HLT_PREFLIGHT_NPT_READY          (1u << 1)
#define AMDVM_HLT_PREFLIGHT_HLT_PAYLOAD        (1u << 2)
#define AMDVM_HLT_PREFLIGHT_VMCB_PAGES_VALID   (1u << 3)
#define AMDVM_HLT_PREFLIGHT_SVM_UNTOUCHED       (1u << 4)
#define AMDVM_HLT_PREFLIGHT_VMRUN_NOT_EXECUTED  (1u << 5)
#define AMDVM_HLT_PREFLIGHT_DESTROYED           (1u << 6)

#define AMDVM_HLT_FPU_PREFLIGHT_LOCKED_CPU       (1u << 0)
#define AMDVM_HLT_FPU_PREFLIGHT_XSAVE_OK         (1u << 1)
#define AMDVM_HLT_FPU_PREFLIGHT_TS_CLEARED       (1u << 2)
#define AMDVM_HLT_FPU_PREFLIGHT_CPU_STABLE       (1u << 3)
#define AMDVM_HLT_FPU_PREFLIGHT_THREAD_STABLE    (1u << 4)
#define AMDVM_HLT_FPU_PREFLIGHT_INTERRUPTS_RESTORED (1u << 5)
#define AMDVM_HLT_FPU_PREFLIGHT_VMRUN_NOT_EXECUTED (1u << 6)
#define AMDVM_HLT_FPU_PREFLIGHT_CONTEXT_DESTROYED (1u << 7)

#define AMDVM_HLT_RUN_CONTEXT_INITIALIZED (1u << 0)
#define AMDVM_HLT_RUN_CALLED              (1u << 1)
#define AMDVM_HLT_RUN_LEASE_RETAINED      (1u << 2)
#define AMDVM_HLT_RUN_CONTEXT_DESTROYED   (1u << 3)

#define AMDVM_CPU_PRESENT          (1u << 0)
#define AMDVM_CPU_SVM              (1u << 1)
#define AMDVM_CPU_NPT              (1u << 2)
#define AMDVM_CPU_VMCR_SVMDIS      (1u << 3)
#define AMDVM_CPU_VMCR_LOCK        (1u << 4)
#define AMDVM_CPU_EFER_SVME        (1u << 5)
#define AMDVM_CPU_HSAVE_NONZERO    (1u << 6)
#define AMDVM_CPU_HSAVE_ALIGNED    (1u << 7)
#define AMDVM_CPU_NRIP_SAVE        (1u << 8)

typedef struct amdvm_probe_cpu_state {
	uint32_t cpu_index;
	uint32_t flags;
	uint64_t vm_cr;
	uint64_t efer;
	uint64_t hsave_pa;
} amdvm_probe_cpu_state_t;

typedef struct amdvm_probe_result {
	uint32_t abi_version;
	uint32_t logical_cpus_seen;
	uint32_t svm_cpuid_cpus;
	uint32_t npt_cpuid_cpus;
	uint32_t vm_cr_svmdis_cpus;
	uint32_t vm_cr_lock_cpus;
	uint32_t efer_svme_cpus;
	uint32_t hsave_nonzero_cpus;
	uint32_t hsave_valid_cpus;
	uint32_t cpu_index_overflow;
	uint32_t quarantined_memory_contexts;
	uint32_t quarantined_memory_ranges;
	uint64_t quarantined_guest_bytes;
	uint32_t nrip_save_cpus;
	uint32_t reserved[2];
	amdvm_probe_cpu_state_t cpus[AMDVM_PROBE_MAX_CPUS];
} amdvm_probe_result_t;

/* No physical addresses are returned to the user client. */
typedef struct amdvm_hlt_preflight_result {
	uint32_t abi_version;
	int32_t init_status;
	int32_t destroy_status;
	uint32_t flags;
	uint32_t reserved[4];
} amdvm_hlt_preflight_result_t;

/* Probes XNU's lazy #NM restore under a CPU-pinning lock; never enters SVM. */
typedef struct amdvm_hlt_fpu_preflight_result {
	uint32_t abi_version;
	uint32_t flags;
	int32_t init_status;
	int32_t first_xsave_status;
	int32_t second_xsave_status;
	int32_t destroy_status;
	uint32_t cpu_before;
	uint32_t cpu_after;
	uint32_t ts_before;
	uint32_t ts_after;
	uint32_t em_before;
	uint32_t em_after;
	uint64_t xcr0;
	uint32_t reserved[4];
} amdvm_hlt_fpu_preflight_result_t;

typedef struct amdvm_hlt_run_request {
	uint32_t abi_version;
	uint32_t flags;
} amdvm_hlt_run_request_t;

/* One synchronous HLT attempt; deliberately contains no physical addresses. */
typedef struct amdvm_hlt_run_result {
	uint32_t abi_version;
	uint32_t flags;
	int32_t context_status;
	int32_t owner_status;
	int32_t vmrun_status;
	int32_t destroy_status;
	uint64_t exit_code;
	uint64_t exit_info1;
	uint64_t exit_info2;
	uint64_t guest_rip;
	uint64_t guest_rflags;
	uint64_t next_rip;
} amdvm_hlt_run_result_t;

/*
 * Persistent-vCPU ABI: the code image is fixed and accepted only with an
 * explicit QEMU_RESET_VECTOR_V1 profile. CS.base + RIP is the linear address
 * 0xfffffff0; with reset paging disabled, NPT maps GPA page 0xfffff000 and the
 * first instruction byte is at page offset 0xff0.
 */
typedef struct amdvm_vcpu_create_request {
	uint32_t abi_version;
	uint32_t guest_profile;
	uint32_t code_size;
	uint8_t code[AMDVM_VCPU_GUEST_CODE_MAX];
	uint8_t reserved;
} amdvm_vcpu_create_request_t;

/* Complete immutable reset-vector page copied from QEMU's firmware FlatView. */
typedef struct amdvm_vcpu_set_reset_rom_request {
	uint32_t abi_version;
	uint32_t reserved;
	uint8_t page[AMDVM_VCPU_RESET_ROM_SIZE];
} amdvm_vcpu_set_reset_rom_request_t;

typedef struct amdvm_vcpu_lifecycle_result {
	uint32_t abi_version;
	int32_t context_status;
	uint32_t reserved[2];
} amdvm_vcpu_lifecycle_result_t;

/* Read-only reset-vector inspection. This selector never enters SVM. */
#define AMDVM_VCPU_DIAGNOSTIC_ROM_INSTALLED (1u << 0)
#define AMDVM_VCPU_DIAGNOSTIC_NPT_RX_VALID  (1u << 1)
#define AMDVM_VCPU_DIAGNOSTIC_RESET_STATE   (1u << 2)
typedef struct amdvm_vcpu_diagnostic_result {
	uint32_t abi_version;
	int32_t context_status;
	uint32_t guest_profile;
	uint32_t flags;
	uint64_t guest_physical_address;
	uint32_t rom_offset;
	uint32_t reserved;
	uint8_t entry_bytes[16];
	uint16_t cs_selector;
	uint16_t cs_attributes;
	uint32_t cs_limit;
	uint64_t cs_base;
	uint64_t rip;
	uint64_t rflags;
	uint64_t cr0;
	uint64_t efer;
} amdvm_vcpu_diagnostic_result_t;

/* Read-only VMCB controls needed to diagnose reset-vector VMEXIT behavior. */
#define AMDVM_VCPU_CONTROL_DIAG_CPUID_INTERCEPT (1u << 0)
#define AMDVM_VCPU_CONTROL_DIAG_UD_INTERCEPT    (1u << 1)
#define AMDVM_VCPU_CONTROL_DIAG_HLT_INTERCEPT   (1u << 2)
#define AMDVM_VCPU_CONTROL_DIAG_IOIO_INTERCEPT  (1u << 3)
#define AMDVM_VCPU_CONTROL_DIAG_INTR_INTERCEPT  (1u << 4)
#define AMDVM_VCPU_CONTROL_DIAG_NPT_ENABLED     (1u << 5)
#define AMDVM_VCPU_CONTROL_DIAG_NPT_ROOT_VALID  (1u << 6)
#define AMDVM_VCPU_CONTROL_DIAG_IOPM_VALID      (1u << 7)
#define AMDVM_VCPU_CONTROL_DIAG_MSRPM_VALID     (1u << 8)
#define AMDVM_VCPU_CONTROL_DIAG_V_INTR_MASKING  (1u << 9)
typedef struct amdvm_vcpu_control_diagnostic_result {
	uint32_t abi_version;
	int32_t context_status;
	uint32_t flags;
	uint16_t intercept_cr_read;
	uint16_t intercept_cr_write;
	uint16_t intercept_dr_read;
	uint16_t intercept_dr_write;
	uint32_t intercept_exceptions;
	uint32_t intercept_misc1;
	uint32_t intercept_misc2;
	uint32_t nested_paging_control;
	uint32_t asid;
	uint32_t tlb_control;
	uint32_t interrupt_control;
} amdvm_vcpu_control_diagnostic_result_t;

/* Monotonic QEMU kick generation; no user or physical addresses cross ABI. */
typedef struct amdvm_vcpu_kick_request {
	uint32_t abi_version;
	uint32_t reserved;
	uint64_t generation;
} amdvm_vcpu_kick_request_t;

/* User virtual address stays private to the calling task; no host PA crosses ABI. */
typedef struct amdvm_vcpu_memory_range {
	uint64_t guest_physical_address;
	uint64_t user_virtual_address;
	uint64_t size;
	uint32_t flags;
	uint32_t reserved;
} amdvm_vcpu_memory_range_t;

typedef struct amdvm_vcpu_memory_map_request {
	uint32_t abi_version;
	uint32_t range_count;
	uint32_t reserved[2];
	amdvm_vcpu_memory_range_t ranges[AMDVM_VCPU_MEMORY_MAX_RANGES];
} amdvm_vcpu_memory_map_request_t;

typedef struct amdvm_vcpu_memory_map_result {
	uint32_t abi_version;
	int32_t context_status;
	uint32_t range_count;
	uint32_t reserved;
	uint64_t guest_page_count;
	uint64_t npt_page_count;
} amdvm_vcpu_memory_map_result_t;

/* Guest MTRRs are emulated by choosing the corresponding nested PAT type. */
typedef struct amdvm_vcpu_memory_type_range {
	uint64_t guest_physical_address;
	uint64_t size;
	uint32_t memory_type;
	uint32_t reserved;
} amdvm_vcpu_memory_type_range_t;

typedef struct amdvm_vcpu_set_memory_types_request {
	uint32_t abi_version;
	uint32_t range_count;
	uint32_t reserved[2];
	amdvm_vcpu_memory_type_range_t ranges[
	    AMDVM_VCPU_MEMORY_TYPE_MAX_RANGES];
} amdvm_vcpu_set_memory_types_request_t;

typedef struct amdvm_vcpu_run_request {
	uint32_t abi_version;
	uint32_t flags;
	uint64_t gpr[15];
	uint64_t rip;
	uint64_t rflags;
	uint32_t event_injection;
	/* High VMCB EVENTINJ word, used only when ERROR_VALID is set. */
	uint32_t event_injection_error_code;
	uint64_t kick_generation;
	/* Zero means the legacy fixed-microguest request; v1 selects full state. */
	amdvm_x86_guest_state_v1_t state;
} amdvm_vcpu_run_request_t;

#define AMDVM_VCPU_ENTRY_TRANSLATION_VALID (1u << 0)
#define AMDVM_VCPU_ENTRY_BACKING_BYTES_VALID (1u << 1)
#define AMDVM_VCPU_ENTRY_CPUID_INTERCEPT (1u << 2)
#define AMDVM_VCPU_ENTRY_NPT_ENABLED (1u << 3)
/* Administrator-only evidence captured from the exact VMCB/NPT used by RUN. */
typedef struct amdvm_vcpu_entry_snapshot {
	uint32_t flags;
	uint32_t reserved;
	uint64_t guest_physical_address;
	uint64_t host_physical_address;
	uint64_t npt_leaf_entry;
	uint64_t npt_root;
	uint64_t cs_base;
	uint64_t rip;
	uint64_t rflags;
	uint64_t cr0;
	uint64_t cr3;
	uint64_t cr4;
	uint64_t efer;
	uint64_t event_injection;
	uint64_t exit_code_sentinel;
	uint64_t host_xcr0;
	uint64_t guest_xcr0;
	uint16_t cs_selector;
	uint16_t cs_attributes;
	uint32_t cs_limit;
	uint32_t nested_paging_control;
	uint32_t asid;
	uint32_t intercept_misc1;
	uint32_t intercept_misc2;
	uint32_t guest_xstate_size;
	uint8_t entry_bytes[16];
	uint64_t gpr[15];
} amdvm_vcpu_entry_snapshot_t;

typedef struct amdvm_vcpu_run_result {
	uint32_t abi_version;
	uint32_t flags;
	int32_t context_status;
	int32_t owner_status;
	int32_t vmrun_status;
	/* Hardware EXITINTINFO; valid means an event was interrupted during delivery. */
	uint32_t exit_int_info;
	uint64_t gpr[15];
	uint64_t rip;
	uint64_t rflags;
	uint64_t exit_code;
	uint64_t exit_info1;
	uint64_t exit_info2;
	amdvm_x86_guest_state_v1_t state;
	/* VMCB control-area NRIP at 0xc8; separate from EXITINFO2. */
	uint64_t next_rip;
	/* AMD DecodeAssists fields at VMCB offsets 0xd0/0xd1. */
	uint8_t instruction_length;
	uint8_t instruction_bytes[15];
	/* Snapshot of the entry image immediately before the production RUN path. */
	amdvm_vcpu_entry_snapshot_t entry;
} amdvm_vcpu_run_result_t;

#ifdef __cplusplus
static_assert(sizeof(amdvm_hlt_preflight_result_t) == 32,
	"HLT preflight result ABI size");
static_assert(sizeof(amdvm_hlt_fpu_preflight_result_t) == 72,
	"HLT FPU preflight result ABI size");
static_assert(sizeof(amdvm_hlt_run_request_t) == 8,
	"HLT run request ABI size");
static_assert(sizeof(amdvm_hlt_run_result_t) == 72,
	"HLT run result ABI size");
static_assert(sizeof(amdvm_vcpu_create_request_t) == 20,
	"vCPU create request ABI size");
static_assert(sizeof(amdvm_vcpu_set_reset_rom_request_t) == 4104,
	"vCPU reset ROM request ABI size");
static_assert(sizeof(amdvm_vcpu_lifecycle_result_t) == 16,
	"vCPU lifecycle result ABI size");
static_assert(sizeof(amdvm_vcpu_diagnostic_result_t) == 96,
	"vCPU diagnostic result ABI size");
static_assert(sizeof(amdvm_vcpu_control_diagnostic_result_t) == 48,
	"vCPU control diagnostic result ABI size");
static_assert(offsetof(amdvm_vcpu_diagnostic_result_t, entry_bytes) == 32,
	"vCPU diagnostic ROM bytes offset");
static_assert(sizeof(amdvm_vcpu_kick_request_t) == 16,
	"vCPU kick request ABI size");
static_assert(sizeof(amdvm_vcpu_memory_range_t) == 32,
	"vCPU memory range ABI size");
static_assert(sizeof(amdvm_vcpu_memory_map_request_t) == 2064,
	"vCPU memory map request ABI size");
static_assert(sizeof(amdvm_vcpu_memory_map_result_t) == 32,
	"vCPU memory map result ABI size");
static_assert(sizeof(amdvm_vcpu_memory_type_range_t) == 24,
	"vCPU memory type range ABI size");
static_assert(sizeof(amdvm_vcpu_set_memory_types_request_t) == 6160,
	"vCPU memory type request ABI size");
static_assert(sizeof(amdvm_x86_guest_state_v1_t) == 4608,
	"x86 guest state ABI size");
static_assert(offsetof(amdvm_x86_guest_state_v1_t, xstate) == 512,
	"x86 guest XSTATE offset");
static_assert(sizeof(amdvm_vcpu_run_request_t) == 4768,
	"vCPU run request ABI size");
static_assert(offsetof(amdvm_vcpu_run_request_t, event_injection) == 144,
	"vCPU event injection offset");
static_assert(offsetof(amdvm_vcpu_run_request_t,
	event_injection_error_code) == 148, "vCPU event error offset");
static_assert(offsetof(amdvm_vcpu_run_request_t, state) == 160,
	"vCPU run request state offset");
static_assert(sizeof(amdvm_vcpu_entry_snapshot_t) == 296,
	"vCPU pre-entry snapshot ABI size");
static_assert(offsetof(amdvm_vcpu_run_result_t, entry) == 4816,
	"vCPU pre-entry snapshot offset");
static_assert(sizeof(amdvm_vcpu_run_result_t) == 5112,
	"vCPU run result ABI size");
static_assert(offsetof(amdvm_vcpu_run_result_t, state) == 184,
	"vCPU run result state offset");
static_assert(offsetof(amdvm_vcpu_run_result_t, next_rip) == 4792,
	"vCPU run result NRIP offset");
static_assert(offsetof(amdvm_vcpu_run_result_t, instruction_length) == 4800,
	"vCPU run result instruction length offset");
static_assert(offsetof(amdvm_vcpu_run_result_t, instruction_bytes) == 4801,
	"vCPU run result instruction bytes offset");
static_assert(sizeof(amdvm_x86_segment_state_t) == 16,
	"x86 segment state ABI size");
static_assert(offsetof(amdvm_x86_guest_state_v1_t, gpr) == 8,
	"x86 state GPR offset");
static_assert(offsetof(amdvm_x86_guest_state_v1_t, segments) == 152,
	"x86 state segments offset");
static_assert(sizeof(amdvm_x86_guest_state_v1_t) == 4608,
	"x86 state ABI size");
static_assert(AMDVM_VCPU_RESET_VECTOR_LINEAR == 0xfffffff0ull,
	"QEMU reset vector linear address");
static_assert((AMDVM_VCPU_RESET_VECTOR_LINEAR & ~0xfffull) ==
	AMDVM_VCPU_RESET_ROM_GPA, "QEMU reset vector NPT page");
static_assert((AMDVM_VCPU_RESET_VECTOR_LINEAR & 0xfffull) ==
	AMDVM_VCPU_RESET_ROM_OFFSET, "QEMU reset vector ROM offset");
#else
_Static_assert(sizeof(amdvm_hlt_preflight_result_t) == 32,
	"HLT preflight result ABI size");
_Static_assert(sizeof(amdvm_hlt_fpu_preflight_result_t) == 72,
	"HLT FPU preflight result ABI size");
_Static_assert(sizeof(amdvm_hlt_run_request_t) == 8,
	"HLT run request ABI size");
_Static_assert(sizeof(amdvm_hlt_run_result_t) == 72,
	"HLT run result ABI size");
_Static_assert(sizeof(amdvm_vcpu_create_request_t) == 20,
	"vCPU create request ABI size");
_Static_assert(sizeof(amdvm_vcpu_set_reset_rom_request_t) == 4104,
	"vCPU reset ROM request ABI size");
_Static_assert(sizeof(amdvm_vcpu_lifecycle_result_t) == 16,
	"vCPU lifecycle result ABI size");
_Static_assert(sizeof(amdvm_vcpu_diagnostic_result_t) == 96,
	"vCPU diagnostic result ABI size");
_Static_assert(sizeof(amdvm_vcpu_control_diagnostic_result_t) == 48,
	"vCPU control diagnostic result ABI size");
_Static_assert(offsetof(amdvm_vcpu_diagnostic_result_t, entry_bytes) == 32,
	"vCPU diagnostic ROM bytes offset");
_Static_assert(sizeof(amdvm_vcpu_kick_request_t) == 16,
	"vCPU kick request ABI size");
_Static_assert(sizeof(amdvm_vcpu_memory_range_t) == 32,
	"vCPU memory range ABI size");
_Static_assert(sizeof(amdvm_vcpu_memory_map_request_t) == 2064,
	"vCPU memory map request ABI size");
_Static_assert(sizeof(amdvm_vcpu_memory_map_result_t) == 32,
	"vCPU memory map result ABI size");
_Static_assert(sizeof(amdvm_vcpu_memory_type_range_t) == 24,
	"vCPU memory type range ABI size");
_Static_assert(sizeof(amdvm_vcpu_set_memory_types_request_t) == 6160,
	"vCPU memory type request ABI size");
_Static_assert(sizeof(amdvm_x86_guest_state_v1_t) == 4608,
	"x86 guest state ABI size");
_Static_assert(offsetof(amdvm_x86_guest_state_v1_t, xstate) == 512,
	"x86 guest XSTATE offset");
_Static_assert(sizeof(amdvm_vcpu_run_request_t) == 4768,
	"vCPU run request ABI size");
_Static_assert(offsetof(amdvm_vcpu_run_request_t, event_injection) == 144,
	"vCPU event injection offset");
_Static_assert(offsetof(amdvm_vcpu_run_request_t,
	event_injection_error_code) == 148, "vCPU event error offset");
_Static_assert(offsetof(amdvm_vcpu_run_request_t, state) == 160,
	"vCPU run request state offset");
_Static_assert(sizeof(amdvm_vcpu_entry_snapshot_t) == 296,
	"vCPU pre-entry snapshot ABI size");
_Static_assert(offsetof(amdvm_vcpu_run_result_t, entry) == 4816,
	"vCPU pre-entry snapshot offset");
_Static_assert(sizeof(amdvm_vcpu_run_result_t) == 5112,
	"vCPU run result ABI size");
_Static_assert(offsetof(amdvm_vcpu_run_result_t, state) == 184,
	"vCPU run result state offset");
_Static_assert(offsetof(amdvm_vcpu_run_result_t, next_rip) == 4792,
	"vCPU run result NRIP offset");
_Static_assert(offsetof(amdvm_vcpu_run_result_t, instruction_length) == 4800,
	"vCPU run result instruction length offset");
_Static_assert(offsetof(amdvm_vcpu_run_result_t, instruction_bytes) == 4801,
	"vCPU run result instruction bytes offset");
_Static_assert(sizeof(amdvm_x86_segment_state_t) == 16,
	"x86 segment state ABI size");
_Static_assert(offsetof(amdvm_x86_guest_state_v1_t, gpr) == 8,
	"x86 state GPR offset");
_Static_assert(offsetof(amdvm_x86_guest_state_v1_t, segments) == 152,
	"x86 state segments offset");
_Static_assert(sizeof(amdvm_x86_guest_state_v1_t) == 4608,
	"x86 state ABI size");
_Static_assert(AMDVM_VCPU_RESET_VECTOR_LINEAR == 0xfffffff0ull,
	"QEMU reset vector linear address");
_Static_assert((AMDVM_VCPU_RESET_VECTOR_LINEAR & ~0xfffull) ==
	AMDVM_VCPU_RESET_ROM_GPA, "QEMU reset vector NPT page");
_Static_assert((AMDVM_VCPU_RESET_VECTOR_LINEAR & 0xfffull) ==
	AMDVM_VCPU_RESET_ROM_OFFSET, "QEMU reset vector ROM offset");
#endif

#endif
