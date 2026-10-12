# Build and run

This is a developer workflow. It does not install a kext for you or change system security settings.

## Requirements

- An x86_64 AMD Hackintosh with AMD SVM enabled and Nested Paging (NPT) available. A CPUID SVM bit alone does not prove firmware/macOS permits `VMRUN`.
- x86_64 macOS. The recorded hardware check used macOS Tahoe / Darwin 25 and a Ryzen 7 5700X.
- Xcode Command Line Tools, `xcrun`, the macOS SDK, and clang.
- For the KPI import audit, matching XNU/KDK source exports containing `all-kpi.exp`. Matching versions matter for internal KPIs.
- Administrator access to call the IOUserClient. Loading a third-party kext may require a signature, owner approval, and reboot depending on the macOS version and machine policy.

## 1. Build the client and kext bundle

From the repository root:

```sh
cd src/amdvm-provider
make
```

Expected outputs:

- `build/amdvm-probe` — user-space client.
- `build/AMDVMProbe.kext/` — compiled bundle.

The `make` target only compiles. It does not sign, install, or load the bundle, change boot settings, or execute `VMRUN`.

If the SDK/KPI changed, check compilation and imports first. For example, when a matching `all-kpi.exp` is available:

```sh
make hlt-run-kpi-audit XNU_KPI_EXPORTS=/path/to/all-kpi.exp
```

A local audit does not replace checking that the bundle binds to the running kernel and is approved by macOS.

## 2. Source-level checks

The Makefile provides independent checks for C/C++ helpers, some with AddressSanitizer and UndefinedBehaviorSanitizer. Examples:

```sh
make vmcb-state-roundtrip-test
make npt-isolation-test
make npt-capacity-test
make quarantine-registry-test
make vcpu-lifecycle-test
```

These targets do not load a kext or execute privileged instructions. Each checks only the software logic named by that target. Building them requires Xcode clang and the SDK.

## 3. Load the bundle on a test system

Installation/loading depends on the macOS release, signing, and policy of the specific computer. Review Apple's policy for that setup, sign your own build, and inspect dependencies, identity, and KPI imports. macOS may require explicit approval for third-party system software and a reboot.

This PoC does not recommend disabling SIP/AMFI or changing boot policy. Do not reuse system UUIDs, hashes, or commands from old reports; they refer to different builds. Before a hardware experiment, confirm that the intended bundle is loaded and that the client ABI matches the kext ABI. A mismatch should stop the operation.

## 4. Progress from read-only checks to hardware entry

The commands below are relative to the repository root. The client looks up `AMDVMProbeService`. If the kext is not loaded or did not register its service, the operation fails. Service requests require administrator privilege.

```sh
sudo ./src/amdvm-provider/build/amdvm-probe
```

With no arguments, the client makes a read-only CPUID/MSR query across online CPUs. A successful query confirms a capability/state snapshot; it does not enable SVM or prove `VMRUN` works.

The next two calls prepare context without entering hardware virtualization:

```sh
sudo ./src/amdvm-provider/build/amdvm-probe --hlt-preflight
sudo ./src/amdvm-provider/build/amdvm-probe --hlt-fpu-preflight
```

They report allocation/NPT and XSTATE/FPU preflight. Both paths should explicitly report `vmrun_executed=no`. They check prerequisites and do not start a guest.

### Experimental HLT `VMRUN`

The normal build is fail-closed. A separate opt-in target compiles the fixed one-shot HLT path:

```sh
cd src/amdvm-provider
make hlt-run-kpi-audit XNU_KPI_EXPORTS=/path/to/all-kpi.exp
```

Only after a separate review and approved loading of this exact candidate, the client invocation is:

```sh
sudo ./src/amdvm-provider/build/hlt-run/amdvm-probe --hlt-run --confirm-no-kdp-watchpoints
```

This command executes a real `VMRUN`. It is limited to a fixed one-shot HLT guest and remains experimental. Confirm that a kernel debugger and KDP hardware watchpoints are inactive; the confirmation flag is an operator assertion, and the client cannot detect these conditions itself. Keep a recovery path available. Do not use this selector with an arbitrary payload, OS, or QEMU.

The historical hardware result was `vmrun_status=0`, `exit_code=0x78` (`VMEXIT_HLT`), successful destroy, and return to the shell. It applies to a specific build loaded in October 2026; a new build is not automatically equivalent.

## What counts as success

For a source-level target, success means only that the named helper passed that check. A hardware smoke also requires a matching ABI, a fresh query showing SVM/NPT, passing FPU preflight, exit code `0x78`, successful lease/context release, and return to the shell. Record the loaded bundle UUID/hash and exact OS/SDK versions with the result.

These checks do not establish that a full guest OS can boot, that SMP works, that every kernel fault is recoverable, or that the code is production-ready. A complete guest and QEMU integration are outside this package.
