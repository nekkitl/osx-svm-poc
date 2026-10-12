#ifndef AMDVM_VMRUN_ASM_H
#define AMDVM_VMRUN_ASM_H

/* Kept assembler-safe; amdvm_vmrun.h asserts this against the VMCB layout. */
#define AMDVM_VMRUN_VMCB_GUEST_RAX_OFFSET 0x5f8

/* amdvm_vmrun_once_asm private frame offsets from the post-allocation %rsp. */
#define AMDVM_VMRUN_FRAME_HOST_CR2        24
#define AMDVM_VMRUN_FRAME_GUEST_GPRS      32
#define AMDVM_VMRUN_FRAME_GUEST_VA        40
#define AMDVM_VMRUN_FRAME_GUEST_PA        48
#define AMDVM_VMRUN_FRAME_HOST_PA         56
#define AMDVM_VMRUN_FRAME_TEMP_RDX        16
#define AMDVM_VMRUN_FRAME_XSTATE_AREA      0
#define AMDVM_VMRUN_FRAME_XCR0             8
#define AMDVM_VMRUN_FRAME_GUEST_XSTATE    64
#define AMDVM_VMRUN_FRAME_SIZE            80

#endif
