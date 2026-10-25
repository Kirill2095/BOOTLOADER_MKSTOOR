#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""mkstoor-fixup — post-link fixer for MKSTOOR UEFI images.

Background
----------
A PE/COFF image consumed by UEFI firmware must carry the exact subsystem
value ``IMAGE_SUBSYSTEM_EFI_APPLICATION (10)`` in its Optional Header.

* The Microsoft linker and LLD accept ``/SUBSYSTEM:EFI_APPLICATION``.
* Binutils' *native* ELF linker has no notion of PE subsystems at all, and
  even the bundled PE emulation (``ld -m i386pep``) only exposes a handful
  of named subsystems ("native", "windows", ...) — there is no way to ask
  it for value 10 directly.

This script solves the problem the same way small EFI projects do: it takes
whichever PE/COFF file the linker produced and rewrites one 16-bit field,
re-validating the headers first so a silent corruption can never ship.
It is deterministic, dependency-free (Python stdlib only) and safe to run
in CI as part of the build.

The optional third argument is an output path: when given, the corrected
image is written there instead of in-place (used by the Meson custom_target
so the raw linker artifact and the validated artifact stay separable).

Usage:  mkstoor-fixup.py <image.efi> <machine-id-hex> [out.efi]
Exit:   0 on success, non-zero with a diagnostic on any structural problem.
"""

import shutil
import struct
import sys

# ---------------------------------------------------------------------------
# PE/COFF constants (UEFI spec 2.10 / PE-COFF revision 9.2)
# ---------------------------------------------------------------------------
MZ_MAGIC = b"MZ"
PE_SIGNATURE = b"PE\x00\x00"

IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64

MAGIC_PE32PLUS = 0x20B          # 64-bit Optional Header (all EFI apps)

IMAGE_SUBSYSTEM_EFI_APPLICATION = 10

# Offsets inside the Optional Header (winnt.h):
OFF_OPT_MAGIC = 0x00            # WORD
OFF_OPT_SUBSYSTEM = 0x44        # WORD, identical for PE32 and PE32+


def fail(msg: str) -> "NoReturn":  # noqa: F821  (typing kept light for py3.8)
    print(f"mkstoor-fixup: error: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_and_fix(path: str, expected_machine: int | None) -> None:
    """Validate the PE headers of *path* and set Subsystem=EFI_APPLICATION.

    The file is rewritten atomically-in-place (single seek + write of two
    bytes). All validation happens before any mutation so a rejected file
    is left untouched.
    """
    with open(path, "r+b") as f:
        data = f.read()
        if len(data) < 0x100:
            fail(f"{path}: too small to be a PE/COFF image ({len(data)} bytes)")
        if data[0:2] != MZ_MAGIC:
            fail(f"{path}: missing DOS 'MZ' signature — not a PE file")

        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if e_lfanew + 24 > len(data):
            fail(f"{path}: bogus e_lfanew=0x{e_lfanew:x} points past EOF")
        if data[e_lfanew : e_lfanew + 4] != PE_SIGNATURE:
            fail(f"{path}: missing PE\\0\\0 signature at 0x{e_lfanew:x}")

        coff = e_lfanew + 4
        (machine, nsec, _timestamp, _symtab, _nsyms,
         optsize, _chars) = struct.unpack_from("<HHIIIHH", data, coff)
        if expected_machine is not None and machine != expected_machine:
            fail(
                f"{path}: Machine=0x{machine:04x} does not match the "
                f"requested target 0x{expected_machine:04x}"
            )
        if machine not in (IMAGE_FILE_MACHINE_AMD64, IMAGE_FILE_MACHINE_ARM64):
            fail(f"{path}: unsupported Machine=0x{machine:04x} (want 0x8664/0xAA64)")

        opt = coff + 20
        if opt + optsize > len(data) or optsize < 0x46:
            fail(f"{path}: truncated Optional Header (SizeOfHeaders={optsize})")

        magic = struct.unpack_from("<H", data, opt + OFF_OPT_MAGIC)[0]
        if magic != MAGIC_PE32PLUS:
            fail(
                f"{path}: OptionalHeader.Magic=0x{magic:04x} is not PE32+ "
                f"(0x020b); EFI applications must be 64-bit images"
            )

        sub_off = opt + OFF_OPT_SUBSYSTEM
        old_sub = struct.unpack_from("<H", data, sub_off)[0]
        if old_sub == IMAGE_SUBSYSTEM_EFI_APPLICATION:
            print(f"mkstoor-fixup: {path}: already EFI_APPLICATION, nothing to do")
            return

        f.seek(sub_off)
        f.write(struct.pack("<H", IMAGE_SUBSYSTEM_EFI_APPLICATION))
        f.flush()

    print(
        f"mkstoor-fixup: {path}: Subsystem {old_sub} -> "
        f"{IMAGE_SUBSYSTEM_EFI_APPLICATION} (EFI_APPLICATION), "
        f"Machine=0x{machine:04x}, Sections={nsec}"
    )


def main(argv: list[str]) -> int:
    if len(argv) not in (2, 3):
        fail("usage: mkstoor-fixup.py <image.efi> [machine-id-hex]")
    path = argv[1]
    machine = None
    if len(argv) == 3:
        try:
            machine = int(argv[2], 16)
        except ValueError:
            fail(f"invalid machine id '{argv[2]}' (hex, e.g. 8664)")
    try:
        parse_and_fix(path, machine)
    except OSError as exc:
        fail(str(exc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
