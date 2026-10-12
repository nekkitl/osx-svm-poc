# Third-party notes

This export contains the AMDVM provider and its small validation tools. It does not bundle QEMU, XNU, VirtualBox, a Kernel Debug Kit, a macOS SDK, firmware, or a guest ISO.

The work was informed by [AMD's Architecture Programmer's Manual](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2), public XNU interfaces/source, and observed VirtualBox behavior. No VirtualBox or XNU source files are included. Obtain external components from their upstream projects and follow their licenses.

Apple [marks kernel extensions as deprecated](https://developer.apple.com/support/kernel-extensions) and recommends user-space system extensions where available. This project needs privileged CPU instructions and is experimental kernel research; that does not make its interfaces supported by Apple. Loading requirements vary by macOS version and machine policy. See [Apple's current kernel-extension guidance](https://support.apple.com/guide/deployment/system-extensions-in-macos-depa5fb8376f/web) before attempting to install a build.

The project-authored material in this export is licensed under GNU GPL version 2 or (at your option) any later version; see [`LICENSE`](LICENSE). This grant does not replace the licenses of separately obtained external components. The export does not bundle QEMU, XNU, VirtualBox, a Kernel Debug Kit, firmware, or a guest ISO.
