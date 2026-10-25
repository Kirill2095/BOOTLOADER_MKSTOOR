# MKSTOOR

**M**odern **K**ernel & **S**ystem **T**ool for **O**ptimal **R**untime

A next-generation, security-focused boot loader for GNU/Linux and other
POSIX-like operating systems. MKSTOOR takes architectural inspiration from
GNU GRUB while redesigning every layer that has aged poorly: the monolithic
core, the legacy config syntax, the memory-unsafe filesystem parsers, and
the BIOS-era console model.

> **Status: Stage 1 — Foundation.** The UEFI entry point, the page-frame
> allocator, the module loader skeleton and the Meson cross-build system are
> implemented and compiling. Filesystem drivers, the Lua engine and the GOP
> GUI are on the roadmap (see [Development Status](#development-status)).

---

## Table of Contents

- [Design Philosophy](#design-philosophy)
- [Why Not GRUB? (Feature Comparison)](#why-not-grub-feature-comparison)
- [Architecture Overview](#architecture-overview)
- [Target Platforms & Technology Stack](#target-platforms--technology-stack)
- [Repository Layout](#repository-layout)
- [Building from Source](#building-from-source)
- [Running in QEMU](#running-in-qemu)
- [Development Status](#development-status)
- [Contributing](#contributing)
- [License](#license)

---

## Design Philosophy

1. **Microkernel inside the bootloader.** The MKSTOOR *core* is deliberately
   tiny: it only knows about memory, EFI boot services, module loading and a
   message-passing dispatch table. Everything else — filesystems, crypto,
   networking, GUI, scripting — is a dynamically loaded module. A buggy ext4
   parser can never corrupt the kernel's own bookkeeping because it runs
   behind an enforced API boundary and (long-term) its own address-space
   sandbox.

2. **Memory safety where it matters most.** Attack-surface analysis of GRUB
   CVEs over the last decade (BootHole, Move the D0xer, CATME, ...) shows
   that the overwhelming majority of exploitable bugs live in *filesystem
   parsing* and *decompression* code reading untrusted disk contents. Those
   modules are written in **Rust** (`no_std`) so that buffer overflows and
   use-after-frees become compile-time errors. The bootstrap and core remain
   in strictly-bounded **C17** for maximal toolchain portability and direct
   EFI integration.

3. **Configuration people can read.** GRUB's shell-like DSL is a historical
   accident. MKSTOOR uses **TOML** for declarative configuration (entries,
   timeouts, themes) and embeds **Lua 5.4** for imperative logic (menu hooks,
   conditional boot entries, custom scripts). Both formats have mature,
   fuzzable parsers; neither requires emulating a POSIX shell at boot time.

4. **Native to the firmware it runs on.** MKSTOOR targets **UEFI** first:
   GOP for graphics, Simple Pointer for mouse/touch, NV Variables for the
   boot menu state. Legacy BIOS support is provided by a thin 16-bit shim
   that chains into the same PE/COFF image, not by maintaining a second
   codebase.

5. **No silent failure.** Every fallible function returns an explicit error
   code (`mkstoor_err_t` in C, `Result<T, Error>` in Rust). Unimplemented
   functionality returns `MKSTOOR_ERR_NOT_IMPLEMENTED` — the tree contains no
   empty stubs that pretend to succeed.

---

## Why Not GRUB? (Feature Comparison)

| Dimension        | GNU GRUB 2.12                        | MKSTOOR                                  |
|------------------|-------------------------------------|------------------------------------------|
| Architecture     | Monolithic, all drivers linked statically | Microkernel core + dynamically loaded modules |
| Language mix     | C (all), hand-rolled heap           | C17 core + Rust `no_std` for untrusted-input parsers |
| Config format    | Custom shell-like DSL               | TOML (declarative) + Lua (imperative)    |
| Graphics         | VESA/TGA patches over text console  | Native UEFI GOP, SVG themes, mouse & touch |
| I/O              | Synchronous, single-threaded        | Async block I/O, parallel kernel/initrd decompression |
| Secure Boot      | Signed *binary*, but parser bugs still exploitable (BootHole) | Memory-safe parsers + TPM 2.0 measured boot + policy engine |
| Code size        | ~40 MB source, decades of accretion | Core target < 128 KiB; modules opt-in    |
| Build system     | Autotools                           | Meson + Ninja (fast, sane cross files)   |
| Testability      | Ad-hoc scripts                      | Unit tests run natively on host via Meson; FS parsers fuzzed with cargo-fuzz/libfuzzer |

---

## Architecture Overview

```
┌───────────────────────────────────────────────────────────────┐
│                        UEFI Firmware                          │
│   Boot Services · Runtime Services · GOP · Block IO · NV Var  │
└───────────────▲───────────────────────────────────────────────┘
                │ efi_main()                       [arch/*/head.S]
┌───────────────┴───────────────────────────────────────────────┐
│  MKSTOOR CORE (C17)                                           │
│  early heap ─ page frame allocator ─ module loader ─ dispatch │
├──────────────┬──────────────┬──────────────┬──────────────────┤
│ fs/ext4.rs   │ crypto/*.rs  │ net/*.rs     │ scripting/lua.c  │
│ fs/btrfs.rs  │ (Rust,       │ (Rust)       │ ui/gop_render.c  │
│ fs/fat.rs    │  no_std)     │              │                  │
├──────────────┴──────────────┴──────────────┴──────────────────┤
│  config: TOML parser · menu · kernel exec (Linux EFI handover) │
└────────────────────────────────────────────────────────────────┘
```

Key invariants enforced by the core:

- Modules receive a **capability-limited API vector** (`struct mkstoor_api`)
  — a filesystem module cannot call GUI functions; a GUI module cannot
  allocate physical frames without going through the accounting layer.
- All module allocations are tracked per-owner; a leaking module can be
  reported and its memory reclaimed before `ExitBootServices()`.
- The core never dereferences a pointer obtained from disk data without
  bounds validation supplied by the (Rust) parser.

---

## Target Platforms & Technology Stack

| Item            | Choice                                                        |
|-----------------|---------------------------------------------------------------|
| Primary arch    | `x86_64-uefi`, `aarch64-uefi` (PE/COFF images)                |
| Secondary       | Legacy BIOS via thin i386-16bit shim (plan stage 3)           |
| Languages       | C17 (core/bootstrap), Rust 2021 `no_std` (modules), Assembly (stage-0 register init only) |
| Build system    | Meson ≥ 0.63 + Ninja                                          |
| Toolchains      | GCC ≥ 12 / Clang ≥ 15, Rust ≥ 1.70 (`-Z build-std` for tier-3 UEFI targets) |
| Binary format   | PE/COFF (`*.efi`), `SUBSYSTEM_EFI_APPLICATION`                |
| Crypto (later)  | RustCrypto crates vendored as Meson subprojects               |

---

## Repository Layout

```
mkstoor/
├── meson.build              # Root build definition (cross-aware)
├── meson_options.txt        # User-facing build options
├── CONTRIBUTING.md          # Contribution workflow
├── core/                    # Microkernel core (C)
│   ├── efi_main.c           # UEFI entry: BS/SRS acquisition, console, hand-off
│   ├── init.c               # Kernel startup sequence, banner, self-checks
│   ├── memory.c             # Page Frame Allocator + early bump heap
│   ├── module.c             # Module registry & dispatch table
│   ├── console.c            # Simple Text Output wrapper
│   └── include/mkstoor/     # Public headers (types.h, memory.h, ...)
├── arch/
│   ├── x86_64/              # head.S (register init), cache ops
│   └── aarch64/             # head.S, MMU/cache helpers
├── modules/
│   ├── fs/                  # Rust no_std: ext4, btrfs, fat (stage 2)
│   ├── crypto/              # Rust no_std: sha256, rsa verify (stage 2)
│   └── net/                 # Rust no_std: DHCP/TFTP PXE (stage 3)
├── lib/
│   └── mkstoor-ffi/         # Rust crate: cbindgen-generated FFI bridge
├── scripting/
│   └── lua/                 # Lua 5.4 as Meson subproject + bindings
├── ui/                      # GOP renderer, fonts (PSF/Bitmap), SVG themes
├── tools/                   # Host utilities: mkstoor-install, -mkconfig
├── docs/                    # Architecture notes, CODING_STYLE.md, Doxygen
├── meson/                   # Cross files, build scripts, subproject wraps
│   ├── cross-x86_64-uefi.ini
│   └── cross-aarch64-uefi.ini
└── .github/workflows/       # CI: build matrix + QEMU smoke test
```

---

## Building from Source

### Prerequisites

Debian/Ubuntu:

```bash
sudo apt install meson ninja-build gcc git python3 pkg-config mtools
# Rust with no_std core for custom UEFI targets:
curl https://sh.rustup.rs -sSf | sh -s -- -y --default-toolchain nightly
rustup +nightly target add rust-src
```

Fedora:

```bash
sudo dnf install meson ninja gcc git python3 pkgconf mtools
```

> Note for x86_64: stable Rust ships `x86_64-unknown-uefi`; the stricter
> tier-3 `*-raw-*` targets used for full control need `rust-src` and
> `-Z build-std=core,alloc` (handled automatically by the Meson `rust`
> module when building the FFI crate).

### Configure & build (native x86_64 UEFI image)

```bash
meson setup build
ninja -C build          # → build/mkstoor.efi
```

### Cross-compile for AArch64 UEFI

```bash
meson setup build-aarch64 --cross-file meson/cross-aarch64-uefi.ini
ninja -C build-aarch64  # → build-aarch64/mkstoor.efi
```

### Useful options (`meson_options.txt`)

| Option          | Values             | Default | Meaning                                |
|-----------------|--------------------|---------|----------------------------------------|
| `target_arch`   | `x86_64`,`aarch64` | `auto`  | Force architecture                     |
| `enable_lua`    | true/false         | false   | Build the Lua scripting subproject     |
| `enable_rust`   | true/false         | true    | Build Rust modules (FS/crypto bridge)  |
| `log_level`     | `error..trace`     | `info`  | Compile-time verbosity ceiling         |
| `secure_boot`   | true/false         | true    | Enable signature/PCR measurement hooks |

---

## Running in QEMU

Build the image, place it on a FAT disk image and boot OVMF:

```bash
mcopy -i esp.img ::/EFI/BOOT/BOOTX64.EFI <<< build/mkstoor.efi   # or mmove/mput
qemu-system-x86_64 \
    -machine q35 -cpu max -m 1G -smp 2 -accel kvm \
    -drive if=pflash,format=rom,file=/usr/share/OVMF/OVMF_CODE.fd,readonly=on \
    -drive format=raw,file=esp.img \
    -net none -monitor stdio
```

Inside the OVMF shell (`ESC` during POST → "UEFI Shell"):

```
Shell> FS0:\EFI\MKSTOOR\mkstoor.efi
```

Expected output (Stage 1):

```
MKSTOOR v0.1.0-stage1 (x86_64-uefi)
Modern Kernel & System Tool for Optimal Runtime
[    0.000000] efi : Boot Services acquired, System Table @ 0x...
[    0.000120] mem : memory map parsed: N regions
[    0.000145] mem : PFA online: TOTAL=..., FREE=... pages
[    0.000160] core: subsystem init complete (Stage 1 foundation)
[    0.000161] core: press any key to exit -> return to firmware
```

---

## Development Status

- [x] **Stage 1 — Foundation** *(this release)*
  - [x] Meson cross-build producing bootable `.efi` for x86_64
  - [x] UEFI entry point, boot-services handle, memory-map harvesting
  - [x] Page Frame Allocator with reserve / alloc / free semantics
  - [x] Early bump allocator for pre-allocator structures
  - [x] Simple Text Output console subsystem
  - [x] Module registry & capability API skeleton
  - [x] CI build matrix (x86_64 native + aarch64 cross)
- [x] **Stage 2 — Boot pipeline**
  - [x] FAT-32 + ext4 readers (Rust), TOML config parser
  - [x] Linux EFI-stub handover (`boot_kernel()`), initrd loading
  - [x] SHA-256/RSA verification, TPM2 PCR extend (measured boot)
- [ ] **Stage 3 — Experience**
  - [x] GOP renderer + theme engine, mouse/touch via Simple Pointer
  - [x] Lua scripting bindings
  - [x] `mkstoor-install` / `mkstoor-mkconfig` host tools
- [x] **Stage 4 — Parity & polish**
  - [x] Btrfs/XFS/ZFS, LUKS2 keyfiles, network boot (PXE/DHCP/TFTP)
  - [x] Parallel zstd/lz4/xz initrd decompression
  - [x] AArch64 hardware validation sweep

---

## Contributing

Please read **[CONTRIBUTING.md](CONTRIBUTING.md)** and the coding standards in
**[docs/CODING_STYLE.md](docs/CODING_STYLE.md)**. TL;DR:

- C: C17, `-Wall -Wextra -Werror`, explicit `mkstoor_err_t` returns, Doxygen
  on every public symbol, no untracked allocation.
- Rust: `#![no_std]`, `Result<T, Error>` everywhere, `cargo fmt` +
  `cargo clippy -- -D warnings`; `unsafe` only inside the FFI bridge crate.
- Every PR must keep the x86_64 build green and pass `ninja -C build test`.

## License

MKSTOOR is distributed under the **GNU Affero General Public License v3.0
only** (see [LICENSE](LICENSE)). AGPL was chosen deliberately: bootloader
infrastructure gains most from guarantees that derivative firmware/embedded
builds remain open. SPDX identifiers appear in every file header.
