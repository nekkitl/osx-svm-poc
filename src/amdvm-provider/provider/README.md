# Provider internals

These files are the kernel-side building blocks used by `AMDVMProbe.kext`. They are experimental XNU code and depend on the macOS SDK/KPI selected at build time.

## Components

- `amdvm_svm_owner.*` — per-CPU SVM ownership checks and lease acquire/release for `EFER.SVME` and `VM_HSAVE_PA`.
- `amdvm_physmem.*` — wired kernel memory and pinned user ranges, retaining descriptors for their full lifetime and validating physical segments.
- `amdvm_npt.*` — four-level AMD Nested Page Table construction, permissions, and tracked page-table allocation.
- `amdvm_vmcb.*` — VMCB fields, guest state conversion/validation, intercept maps, and exit-state transfer.
- `amdvm_xstate.*` — XSAVE/XRSTOR state validation and transfer helpers.
- `amdvm_vmrun.*` and `amdvm_vmrun_x86_64.S` — checked C entry boundary and x86_64 assembly world switch. The regular build keeps arbitrary entry gated; an opt-in build exposes a fixed experimental path.
- `amdvm_hlt_context.*` — bounded single-page HLT context used by the one-shot hardware smoke.
- `amdvm_quarantine.*` — retain backing allocations when cleanup cannot be proven safe.
- `amdvm_host_mtrr.*` — host memory-type inspection and the limited compatibility profile.
- `amdvm_vcpu_lifecycle.h`, `amdvm_kick_logic.h` — state/lifecycle helpers used by the evolving vCPU ABI.

## Boundary

The fixed HLT test maps a tightly limited guest page and expects `VMEXIT_HLT` (`0x78`). It validates one transition on a specific machine/build. It does not demonstrate that arbitrary guest instructions, firmware, an operating system, interrupt delivery, or concurrent teardown are safe. The larger vCPU ABI in the provider is an experimental interface; this export does not include its QEMU caller.

For the overview and evidence limits, see [`../../../docs/en/README.md`](../../../docs/en/README.md) and [`../../../docs/ru/README.md`](../../../docs/ru/README.md). Build instructions are in those documents' sibling `build-and-run.md` files.
