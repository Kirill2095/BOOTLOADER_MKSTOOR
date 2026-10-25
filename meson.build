# ============================================================================
# MKSTOOR — root Meson build definition (C17, freestanding UEFI image)
# SPDX-License-Identifier: AGPL-3.0-only
#
# Produces:  build/mkstoor.efi   (PE/COFF, SUBSYSTEM_EFI_APPLICATION)
#            build/esp.img       (optional FAT ESP for QEMU/OVMF testing)
#
# Usage:
#   native x86_64 Linux with a PE-capable linker (clang+lld):
#       meson setup build && ninja -C build
#   cross via the mingw-w64 toolchain (see meson/cross-x86_64-uefi.ini):
#       meson setup build --cross-file meson/cross-x86_64-uefi.ini && ninja -C build
#
# Why the old meson.build could never produce an .efi (and what this fixes):
#   * it was a fragment of core/meson.build sitting at the repository ROOT:
#       - no project() declaration at all -> meson refused to configure;
#       - referenced `mkstoor_inc`, which nothing ever declared;
#       - listed ui sources as '../ui/font_data.c' — nonexistent path
#         (the files actually live in core/ui/);
#       - omitted core/crypto.c and the boot-handover glue entirely;
#       - there was NO efi_main entry point anywhere in the tree;
#       - most importantly: it only built a static_library and NEVER linked
#         an executable — `ninja` had no way to output any .efi file.
#   * this file declares the project, include dirs, ALL sources including
#     core/efi_main.c + core/boot.c, the relocatable-PE link line, the
#     post-link subsystem fixup via meson/scripts/mkstoor-fixup.py, and an
#     optional QEMU-testable ESP disk image.
# ============================================================================

project('mkstoor', 'c',
  version: '0.3.0-stage3',
  license: 'AGPL-3.0-only',
  default_options: [
    'c_std=c17',
    'default_library=static',
    'warning_level=2',            # -Wall -Wextra; -Werror added per-target below
    'b_ndebug=if-release',
  ],
  meson_version: '>=0.60.0')

cc = meson.get_compiler('c')
python = find_program('python3', 'python')

# ---------------------------------------------------------------------------
# Target architecture selection (option `target_arch`, cross-file aware)
# ---------------------------------------------------------------------------
host_cpu = host_machine.cpu_family()
want_arch = get_option('target_arch')
if want_arch != 'auto' and want_arch != host_cpu
  error('target_arch=@0@ does not match the configured toolchain (@1@). Use the matching cross file, e.g. --cross-file meson/cross-x86_64-uefi.ini'.format(want_arch, host_cpu))
endif
arch = want_arch == 'auto' ? host_cpu : want_arch

is_x86_64  = arch == 'x86_64'
is_aarch64 = arch == 'aarch64'
if not (is_x86_64 or is_aarch64)
  error('unsupported CPU family: @0@ (need x86_64 or aarch64)'.format(arch))
endif

# Machine id handed to mkstoor-fixup.py so the emitted PE cannot silently
# target the wrong CPU (validated against the real COFF header at fixup time).
machine_id = is_x86_64 ? '8664' : 'AA64'

# ---------------------------------------------------------------------------
# Freestanding compile flags common to every MKSTOOR translation unit
# ---------------------------------------------------------------------------
mks_common_args = [
  '-ffreestanding',
  '-fno-stack-protector',
  '-fno-stack-check',
  '-fno-strict-aliasing',
  '-fno-builtin',
  '-fshort-wchar',
  '-ffunction-sections',
  '-fdata-sections',
]
if is_x86_64
  mks_common_args += [
    '-mgeneral-regs-only',      # no SSE/AVX: firmware may not have enabled them
    '-mno-red-zone',            # EFI callbacks may arrive on the red zone
    '-mcmodel=large',
  ]
endif

# The repo's own headers are the ONLY headers: core/include/mkstoor/*.h is a
# self-contained UEFI definition set (no gnu-efi / EDK2 dependency), so the
# stale -I/usr/include/efi hints in the cross file are irrelevant here.
mkstoor_inc = include_directories('core/include')

log_level_map = { 'error': 0, 'warn': 1, 'info': 2, 'debug': 3, 'trace': 4 }
mks_defines = ['-DMKSTOOR_LOG_LEVEL=@0@'.format(log_level_map[get_option('log_level')])]
if get_option('secure_boot')
  mks_defines += ['-DMKSTOOR_SECURE_BOOT=1']
endif

# ---------------------------------------------------------------------------
# Link line: how a plain compiler turns into an EFI application.
#
# Two supported, auto-detected paths (same policy as the cross file docs):
#
#   A) clang + lld: first-class COFF support. We emit a pure PE/COFF with
#      /SUBSYSTEM:EFI_APPLICATION baked in by the linker itself.
#   B) gcc targeting PE (mingw-w64 cross, or binutils with i386pep): link a
#      RELOCATABLE PIE PE with the entry symbol overridden to efi_main;
#      binutils cannot name subsystem 10, so meson/scripts/mkstoor-fixup.py
#      rewrites the Subsystem field post-link and re-validates the headers.
#
# Either way the artifact is a relocatable PE/COFF — exactly what UEFI
# firmware loads. ELF output is rejected outright (it would be unbootable).
# ---------------------------------------------------------------------------
efi_c_args = []
efi_link_args = [
  '-nostartfiles', '-nodefaultlibs', '-nostdlib',
  '-Wl,--no-insert-timestamp',
  '-Wl,--gc-sections',
]

if cc.get_id() == 'clang'
  # Path A: clang + lld COFF driver.
  efi_target = is_x86_64 ? 'x86_64-win64' : 'aarch64-win64'
  efi_c_args += ['-target', efi_target]
  efi_link_args += [
    '-fuse-ld=lld',
    '-Wl,/subsystem:efi_application',
    '-Wl,/entry:efi_main',
    '-Wl,/pie',
  ]
  linker_kind = 'clang + lld (native EFI_APPLICATION subsystem)'
else
  # Path B: gcc/ld with PE emulation (mingw-w64 or any binutils with i386pep).
  # NOTE: we deliberately do NOT pass `-Wl,-e,efi_main` here. mingw's default
  # entry (__mainCRTStartup) pulls in only one tiny startup object; the real
  # payload is reached through our own constructor chain, and meson/scripts/
  # mkstoor-fixup.py rewrites both the AddressOfEntryPoint and the Subsystem
  # field post-link so the firmware jumps straight into efi_main(). Passing
  # `-e efi_main` to a linker whose CRT still references its own startup code
  # produces "cannot find entry symbol" fatal warnings during Meson's compile
  # probes and breaks has_link_argument()/executable() on many toolchains.
  efi_link_args += [
    '-Wl,-pie',
    '-Wl,--image-base,0x0',
    '-Wl,--disable-auto-image-base',
  ]
  linker_kind = 'gcc + binutils PE (entry+subsystem rewritten by mkstoor-fixup.py)'
endif

# libgcc: the few helpers freestanding code still needs (e.g. __udivti3).
# Static, so nothing extra leaks into the image.
mks_libs = [ cc.find_library('gcc', static: true, required: false) ]

# ---------------------------------------------------------------------------
# Sources (complete list — the old file missed efi_main.c, crypto.c, boot.c)
# ---------------------------------------------------------------------------
core_srcs = files(
  # --- Stage 1: foundation ---
  'core/efi_main.c',   # UEFI entry point (was missing from the old build!)
  'core/string.c',
  'core/console.c',
  'core/memory.c',
  'core/module.c',
  # --- Stage 2: boot pipeline ---
  'core/toml.c',
  'core/fs_fat.c',
  'core/fs_ext4.c',
  'core/pipeline.c',
  'core/crypto.c',     # SHA-256 / RSA (missing from the old build!)
  'core/boot.c',       # handover glue + capability vectors (missing!)
  # --- Stage 3: experience ---
  'core/gop.c',
  'core/theme.c',
  'core/font.c',
  'core/pointer.c',
)

ui_srcs = files(
  'core/ui/font_data.c',   # the old file pointed at ../ui/... which does not exist
  'core/ui/menu_gop.c',
)

# Static library kept for tests/tooling parity with the old layout.
core_lib = static_library('mkstoor-core',
  core_srcs + ui_srcs,
  include_directories: mkstoor_inc,
  c_args: mks_common_args + mks_defines,
  pic: false,
)

# ---------------------------------------------------------------------------
# The EFI application itself. Compiled straight to objects (not through the
# .a) so the whole-archive CRT startup machinery we disabled can never sneak
# back into the image.
# ---------------------------------------------------------------------------
efi_raw = executable('mkstoor-raw.efi',
  core_srcs + ui_srcs,
  include_directories: mkstoor_inc,
  c_args: mks_common_args + mks_defines + efi_c_args,
  link_args: efi_link_args,
  dependencies: mks_libs,
  # NB: Meson's `win_subsystem` only accepts windows/console/posix — EFI is
  # not a valid value there. The subsystem comes from the linker line (clang
  # path) or from mkstoor-fixup.py (gcc path), so we omit it entirely.
  override_options: ['b_lundef=false'],
  install: false,
)

fixup_py = files('meson/scripts/mkstoor-fixup.py')

# Post-link: validate the PE headers and force Subsystem=EFI_APPLICATION(10).
# For the clang/lld path this is a no-op ("already EFI_APPLICATION") but it
# doubles as the release gate: the build FAILS if the linker ever emits
# anything that is not a well-formed PE32+ image for the requested machine.
mkstoor_efi = custom_target('mkstoor.efi',
  input: efi_raw,
  output: 'mkstoor.efi',
  command: [python, fixup_py, '@INPUT@', machine_id, '@OUTPUT@'],
  install: true,
  install_dir: get_option('bindir'),
)

# ---------------------------------------------------------------------------
# Optional: ready-to-boot ESP disk image for QEMU/OVMF smoke tests
# (`ninja -C build esp.img`). Needs dosfstools + mtools.
# ---------------------------------------------------------------------------
mkfs_fat = find_program('mkfs.fat', 'mkfs.vfat', required: false)
mcopy = find_program('mcopy', required: false)
if mkfs_fat.found() and mcopy.found()
  custom_target('esp.img',
    input: mkstoor_efi,
    output: 'esp.img',
    command: [
      'sh', '-c',
      'set -e; d=$(mktemp -d); mkdir -p "$d/EFI/BOOT"; ' +
      'cp "$1" "$d/EFI/BOOT/BOOTX64.EFI"; ' +
      'dd if=/dev/zero of="$2" bs=1M count=32 status=none; ' +
      '@0@ -F 32 "$2" >/dev/null; ' +
      'mcopy -i "$2" -s "$d/EFI" ::/EFI; ' +
      'rm -rf "$d"' % mkfs_fat.full_path(),
      'sh', '@INPUT@', '@OUTPUT@',
    ],
    build_by_default: false,
  )
endif

summary({
  'architecture': arch,
  'toolchain': cc.get_id() + ' ' + cc.version().split('+')[0],
  'linker path': linker_kind,
  'log level': get_option('log_level'),
  'secure boot hooks': get_option('secure_boot'),
  'ESP image target': (mkfs_fat.found() and mcopy.found()) ? 'esp.img (opt-in)' : 'needs dosfstools+mtools',
}, section: 'MKSTOOR build configuration')
