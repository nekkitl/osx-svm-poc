	.text
	#include "amdvm_vmrun_asm.h"
	.p2align 4, 0x90
	.globl _amdvm_vmrun_once_asm
_amdvm_vmrun_once_asm:
	/* Preserve the XNU C ABI callee-saved registers and entry flags. */
	pushq	%rbp
	movq	%rsp, %rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	pushfq
	cli
	/*
	 * Do not enter the partially-switched region while single-step TF is set.
	 * This check runs before CLGI or any host-state transition. If TF is set,
	 * return through the ordinary XNU debug path with original flags intact.
	 */
	testq	$(1 << 8), (%rsp)
	jnz	.Ldebug_active_early
	subq	$AMDVM_VMRUN_FRAME_SIZE, %rsp

	/* Save all pointers in the named private frame before loading guest GPRs. */
	movq	%rdi, AMDVM_VMRUN_FRAME_HOST_PA(%rsp)
	movq	%rsi, AMDVM_VMRUN_FRAME_GUEST_PA(%rsp)
	movq	%rdx, AMDVM_VMRUN_FRAME_GUEST_VA(%rsp)
	movq	%rcx, AMDVM_VMRUN_FRAME_GUEST_GPRS(%rsp)
	movq	%r8, AMDVM_VMRUN_FRAME_XSTATE_AREA(%rsp)
	movq	%r9, AMDVM_VMRUN_FRAME_GUEST_XSTATE(%rsp)

	/* Recheck XNU's lazy-FPU state with migration already disabled. */
	movq	%cr0, %rax
	testq	$(1 << 3 | 1 << 2), %rax
	jnz	.Lfpu_not_ready
	movq	%cr2, %rax
	movq	%rax, AMDVM_VMRUN_FRAME_HOST_CR2(%rsp)

	/* Keep host interrupts gated while setting up the world switch. */
	clgi

	/* VMSAVE snapshots the VMLOAD-switched host MSR/segment state. */
	movq	AMDVM_VMRUN_FRAME_HOST_PA(%rsp), %rax
	vmsave	%rax

	/* Save exactly the current thread's enabled user XSTATE on this CPU. */
	xorl	%ecx, %ecx
	xgetbv
	shlq	$32, %rdx
	orq	%rdx, %rax
	movq	%rax, AMDVM_VMRUN_FRAME_XCR0(%rsp)
	movq	AMDVM_VMRUN_FRAME_XSTATE_AREA(%rsp), %r8
	movq	AMDVM_VMRUN_FRAME_XCR0(%rsp), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	xsave64	(%r8)

	/* Start the guest with its own saved SIMD/FPU state, never host state. */
	movq	AMDVM_VMRUN_FRAME_GUEST_XSTATE(%rsp), %r8
	movq	AMDVM_VMRUN_FRAME_XCR0(%rsp), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	xrstor64	(%r8)

	/* Transfer RAX through the VMCB; VMRUN uses RAX for the VMCB PA. */
	movq	AMDVM_VMRUN_FRAME_GUEST_GPRS(%rsp), %rax
	movq	0(%rax), %rcx
	movq	AMDVM_VMRUN_FRAME_GUEST_VA(%rsp), %rax
	movq	%rcx, AMDVM_VMRUN_VMCB_GUEST_RAX_OFFSET(%rax)

	/* Load non-RAX GPRs; VMCB supplies guest RSP. */
	movq	AMDVM_VMRUN_FRAME_GUEST_GPRS(%rsp), %rax
	movq	8(%rax), %rbx
	movq	16(%rax), %rcx
	movq	24(%rax), %rdx
	movq	32(%rax), %rsi
	movq	40(%rax), %rdi
	movq	48(%rax), %rbp
	movq	56(%rax), %r8
	movq	64(%rax), %r9
	movq	72(%rax), %r10
	movq	80(%rax), %r11
	movq	88(%rax), %r12
	movq	96(%rax), %r13
	movq	104(%rax), %r14
	movq	112(%rax), %r15

	/*
	 * Match VBox's interrupt-window protocol: GIF remains clear, while IF=1
	 * lets a pending host IRQ force a VMEXIT instead of being stranded.
	 */
	sti

	/* Load guest state only after host state is snapshotted and GIF is clear. */
	movq	AMDVM_VMRUN_FRAME_GUEST_PA(%rsp), %rax
	vmload	%rax
	vmrun	%rax
	vmsave	%rax
	cli

	/* Restore host FS/GS/TR/LDTR/MSR state before doing further host work. */
	movq	AMDVM_VMRUN_FRAME_HOST_PA(%rsp), %rax
	vmload	%rax

	/* Read guest RAX from the VMCB and store all returned guest GPRs. */
	movq	%rdx, AMDVM_VMRUN_FRAME_TEMP_RDX(%rsp)
	movq	AMDVM_VMRUN_FRAME_GUEST_VA(%rsp), %rax
	movq	AMDVM_VMRUN_VMCB_GUEST_RAX_OFFSET(%rax), %rdx
	movq	AMDVM_VMRUN_FRAME_GUEST_GPRS(%rsp), %rax
	movq	%rdx, 0(%rax)
	movq	AMDVM_VMRUN_FRAME_TEMP_RDX(%rsp), %rdx
	movq	%rbx, 8(%rax)
	movq	%rcx, 16(%rax)
	movq	%rdx, 24(%rax)
	movq	%rsi, 32(%rax)
	movq	%rdi, 40(%rax)
	movq	%rbp, 48(%rax)
	movq	%r8, 56(%rax)
	movq	%r9, 64(%rax)
	movq	%r10, 72(%rax)
	movq	%r11, 80(%rax)
	movq	%r12, 88(%rax)
	movq	%r13, 96(%rax)
	movq	%r14, 104(%rax)
	movq	%r15, 112(%rax)

	/* Preserve the guest XSTATE before restoring any host SIMD state. */
	movq	AMDVM_VMRUN_FRAME_GUEST_XSTATE(%rsp), %r8
	movq	AMDVM_VMRUN_FRAME_XCR0(%rsp), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	xsave64	(%r8)

	/* Flush IBPB-covered predictor state; this CPU does not advertise IBPB_RET. */
	movl	$0x49, %ecx
	movl	$1, %eax
	xorl	%edx, %edx
	wrmsr

	/* Guest GPRs are already captured; scrub them before host kernel code. */
	xorl	%eax, %eax
	xorl	%ebx, %ebx
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	xorl	%esi, %esi
	xorl	%edi, %edi
	xorl	%ebp, %ebp
	xorl	%r8d, %r8d
	xorl	%r9d, %r9d
	xorl	%r10d, %r10d
	xorl	%r11d, %r11d
	xorl	%r12d, %r12d
	xorl	%r13d, %r13d
	xorl	%r14d, %r14d
	xorl	%r15d, %r15d

	/* Refill 32 RSB slots with safe return sites before host control flow. */
	movl	$16, %r10d
.Lfill_rsb:
	call	.Lrsb_slot1
	int3
.Lrsb_slot1:
	call	.Lrsb_slot2
	int3
.Lrsb_slot2:
	addq	$16, %rsp
	decl	%r10d
	jnz	.Lfill_rsb
	lfence

	/* Host CR2 is not restored by VMRUN; restore it explicitly. */
	movq	AMDVM_VMRUN_FRAME_HOST_CR2(%rsp), %rax
	movq	%rax, %cr2
	/* VMRUN leaves host DR7 disabled; normalize host DR6 without reading it. */
	movl	$0xffff0ff0, %eax
	movq	%rax, %dr6

	/* Restore host XSTATE before allowing any interrupt to resume. */
	movq	AMDVM_VMRUN_FRAME_XSTATE_AREA(%rsp), %r8
	movq	AMDVM_VMRUN_FRAME_XCR0(%rsp), %rax
	movq	%rax, %rdx
	shrq	$32, %rdx
	xrstor64	(%r8)
	stgi

	/* Restore entry flags and callee-saved registers; return to XNU. */
	xorl	%eax, %eax
	jmp	.Lreturn
.Lfpu_not_ready:
	movl	$-6, %eax
.Lreturn:
	addq	$AMDVM_VMRUN_FRAME_SIZE, %rsp
	popfq
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	retq

.Ldebug_active_early:
	movl	$-2, %eax
	popfq
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	retq
