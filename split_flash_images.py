#!/usr/bin/env python3

import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


DEFAULT_FLASH_BIN = "flash.bin"
DEFAULT_FLASH_BASE = 0xC0000000
DEFAULT_FLASH_SIZE = 0x04000000
FLASH_SECTION_NAMES = (".flash", "ER_EXTFLASH", ".flash_he", ".flash_hp")
TOOLCHAIN_ROOT_RE = re.compile(r'REGISTERED_TOOLCHAIN_ROOT\s+"([^"]+)"')
PT_NULL = 0
PT_LOAD = 1


def objcopy_names() -> tuple[str, ...]:
    if os.name == "nt":
        return ("llvm-objcopy.exe", "llvm-objcopy")
    return ("llvm-objcopy", "llvm-objcopy.exe")


def repo_path(path: str) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate
    return Path(__file__).resolve().parent / candidate


def resolve_from_base(base: Path, path: str) -> Path:
    candidate = Path(path).expanduser()
    if candidate.is_absolute():
        return candidate
    return (base / candidate).resolve()


def parse_simple_cbuild_run_outputs(cbuild_run: Path) -> list[dict[str, str]]:
    outputs: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    in_output = False

    for raw_line in cbuild_run.read_text(encoding="utf-8").splitlines():
        stripped = raw_line.strip()
        if stripped == "output:":
            in_output = True
            continue

        if in_output and raw_line.startswith("  ") and not raw_line.startswith("    ") and stripped.endswith(":"):
            if current:
                outputs.append(current)
                current = None
            break

        if not in_output:
            continue

        line = stripped
        if line.startswith("- "):
            if current:
                outputs.append(current)
            current = {}
            line = line[2:].strip()

        if not line or ":" not in line or current is None:
            continue

        key, value = line.split(":", 1)
        current[key.strip()] = value.strip().strip("\"'")

    if current:
        outputs.append(current)
    return outputs


def infer_paths_from_cbuild_run(cbuild_run: Path) -> tuple[Path, Path | None]:
    outputs = parse_simple_cbuild_run_outputs(cbuild_run)
    base = cbuild_run.parent

    elf: Path | None = None
    flash_bin: Path | None = None

    for output in outputs:
        file = output.get("file")
        if not file:
            continue

        output_type = output.get("type", "").lower()
        load = output.get("load", "").lower()
        path = resolve_from_base(base, file)

        is_elf_image = (
            output_type in {"elf", "axf"}
            or Path(file).suffix.lower() in {".elf", ".axf"}
        )
        if is_elf_image:
            if elf is None and load in {"image", "image+symbols"}:
                elf = path
        elif output_type == "bin" and flash_bin is None and load in {"image", "image+symbols"}:
            flash_bin = path

    if elf is None:
        raise ValueError(f"No internal ELF output found in {cbuild_run}")


    return elf, flash_bin


def find_default_cbuild_run() -> Path | None:
    repo = Path(__file__).resolve().parent
    candidates = sorted(
        (repo / "out").glob("*.cbuild-run.yml"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    return candidates[0] if candidates else None


def objcopy_candidates_from_dir(path: Path) -> list[Path]:
    return [path / name for name in objcopy_names()] + [path / "bin" / name for name in objcopy_names()]


def generated_toolchain_candidates() -> list[Path]:
    repo = Path(__file__).resolve().parent
    candidates: list[Path] = []

    for toolchain in (repo / "tmp").glob("*/toolchain.cmake"):
        try:
            text = toolchain.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        match = TOOLCHAIN_ROOT_RE.search(text)
        if match:
            candidates.extend(objcopy_candidates_from_dir(Path(match.group(1))))

    for commands_path in (repo / "out").glob("all_ops/*/Debug/compile_commands.json"):
        try:
            commands = json.loads(commands_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue

        for entry in commands:
            command = entry.get("command", "")
            match = re.search(r"([A-Za-z]:[/\\][^\" ]*[/\\]bin[/\\]clang(?:\+\+)?\.exe)", command)
            if match:
                candidates.extend(objcopy_candidates_from_dir(Path(match.group(1)).parent))
                break

    return candidates


def vcpkg_artifact_candidates() -> list[Path]:
    candidates: list[Path] = []
    roots: list[Path] = []

    for env_name in ("VCPKG_ROOT", "USERPROFILE", "HOME"):
        value = os.environ.get(env_name)
        if not value:
            continue
        root = Path(value).expanduser()
        roots.append(root / ".vcpkg" / "artifacts")
        roots.append(root / "artifacts")

    seen: set[Path] = set()
    for root in roots:
        if not root.exists() or root in seen:
            continue
        seen.add(root)

        for artifact in root.glob("*/compilers.arm.llvm.embedded/*/artifact.json"):
            candidates.extend(objcopy_candidates_from_dir(artifact.parent))

    return candidates


def find_objcopy(explicit: str | None) -> str:
    if explicit:
        return explicit

    for candidate in [*generated_toolchain_candidates(), *vcpkg_artifact_candidates()]:
        if candidate.is_file():
            return str(candidate)

    for tool in objcopy_names():
        found = shutil.which(tool)
        if found:
            return found

    raise FileNotFoundError(
        "llvm-objcopy not found. Pass --objcopy with the path to llvm-objcopy."
    )


def run_objcopy(objcopy: str, args: list[str], error_message: str | None = None) -> None:
    completed = subprocess.run([objcopy, *args], check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise RuntimeError(error_message or detail or f"llvm-objcopy failed with exit code {completed.returncode}")


def flash_section_list() -> str:
    return ", ".join(FLASH_SECTION_NAMES)


def write_flash_bin_from_reference_elf(objcopy: str, reference_elf: Path, flash_bin: Path) -> str:
    # --dump-section writes the raw external-flash section payload to flash_bin; it does not create an ELF.
    errors: list[str] = []
    with tempfile.TemporaryDirectory(prefix="split_flash_dump_", dir=flash_bin.parent) as tmp_dir:
        tmp_flash_bin = Path(tmp_dir) / flash_bin.name
        for section_name in FLASH_SECTION_NAMES:
            if tmp_flash_bin.exists():
                tmp_flash_bin.unlink()
            try:
                run_objcopy(
                    objcopy,
                    [f"--dump-section={section_name}={tmp_flash_bin}", str(reference_elf)],
                )
                os.replace(tmp_flash_bin, flash_bin)
                return section_name
            except RuntimeError as error:
                errors.append(str(error))

    raise RuntimeError(
        f"{reference_elf} does not contain a supported external-flash section "
        f"({flash_section_list()}). Last objcopy error: {errors[-1] if errors else 'none'}"
    )


def parse_int(value: str) -> int:
    return int(value, 0)


def ranges_overlap(start: int, size: int, region_start: int, region_size: int) -> bool:
    if size == 0:
        return False
    end = start + size
    region_end = region_start + region_size
    return start < region_end and region_start < end


def patch_external_flash_load_segments(elf: Path, flash_base: int, flash_size: int) -> int:
    """Turn external-flash PT_LOAD entries into PT_NULL entries.

    llvm-objcopy --remove-section removes the .flash section header, but it can
    leave an orphan PT_LOAD program header. pyOCD programs PT_LOAD entries, so
    the internal ELF must remove those headers too.
    """
    data = bytearray(elf.read_bytes())
    if len(data) < 0x34 or data[:4] != b"\x7fELF":
        raise RuntimeError(f"{elf} is not an ELF file.")

    elf_class = data[4]
    endian_id = data[5]
    if endian_id == 1:
        endian = "<"
    elif endian_id == 2:
        endian = ">"
    else:
        raise RuntimeError(f"{elf} has an unsupported ELF data encoding.")

    if elf_class == 1:
        header_fmt = endian + "HHIIIIIHHHHHH"
        ph_fmt = endian + "IIIIIIII"
        ph_type_index = 0
        ph_offset_index = 1
        ph_vaddr_index = 2
        ph_paddr_index = 3
        ph_filesz_index = 4
        ph_memsz_index = 5
        e_phoff_index = 4
        e_phentsize_index = 8
        e_phnum_index = 9
        header_offset = 0x10
    elif elf_class == 2:
        header_fmt = endian + "HHIQQQIHHHHHH"
        ph_fmt = endian + "IIQQQQQQ"
        ph_type_index = 0
        ph_offset_index = 2
        ph_vaddr_index = 3
        ph_paddr_index = 4
        ph_filesz_index = 5
        ph_memsz_index = 6
        e_phoff_index = 4
        e_phentsize_index = 8
        e_phnum_index = 9
        header_offset = 0x10
    else:
        raise RuntimeError(f"{elf} has an unsupported ELF class.")

    header_size = struct.calcsize(header_fmt)
    if len(data) < header_offset + header_size:
        raise RuntimeError(f"{elf} has a truncated ELF header.")

    header = struct.unpack_from(header_fmt, data, header_offset)
    phoff = header[e_phoff_index]
    phentsize = header[e_phentsize_index]
    phnum = header[e_phnum_index]
    expected_phentsize = struct.calcsize(ph_fmt)
    if phentsize < expected_phentsize:
        raise RuntimeError(f"{elf} has unsupported program header entries.")

    patched = 0
    for index in range(phnum):
        entry_offset = phoff + index * phentsize
        if entry_offset + expected_phentsize > len(data):
            raise RuntimeError(f"{elf} has a truncated program header table.")

        program_header = list(struct.unpack_from(ph_fmt, data, entry_offset))
        if program_header[ph_type_index] != PT_LOAD:
            continue

        filesz = program_header[ph_filesz_index]
        memsz = program_header[ph_memsz_index]
        vaddr = program_header[ph_vaddr_index]
        paddr = program_header[ph_paddr_index]
        if not (
            ranges_overlap(vaddr, memsz, flash_base, flash_size)
            or ranges_overlap(paddr, memsz, flash_base, flash_size)
            or ranges_overlap(paddr, filesz, flash_base, flash_size)
        ):
            continue

        for field in range(len(program_header)):
            program_header[field] = 0
        program_header[ph_type_index] = PT_NULL
        struct.pack_into(ph_fmt, data, entry_offset, *program_header)
        patched += 1

    if patched:
        elf.write_bytes(data)

    return patched


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Split all_ops ELF/AXF output into an internal-memory ELF/AXF and one OSPI "
            "flash binary containing only the external-flash section."
        )
    )
    parser.add_argument(
        "--cbuild-run",
        default=None,
        help="CMSIS *.cbuild-run.yml to infer input/output paths from. Default: newest file in out/.",
    )
    parser.add_argument("--elf", default=None, help="Input ELF/AXF. Default: inferred from --cbuild-run.")
    parser.add_argument(
        "--flash-bin",
        default=None,
        help=f"Output binary containing only the external-flash section. Default: {DEFAULT_FLASH_BIN}",
    )
    parser.add_argument("--objcopy", default=None, help="Path to llvm-objcopy.")
    parser.add_argument(
        "--flash-base",
        default=DEFAULT_FLASH_BASE,
        type=parse_int,
        help=f"External flash base address. Default: 0x{DEFAULT_FLASH_BASE:08X}.",
    )
    parser.add_argument(
        "--flash-size",
        default=DEFAULT_FLASH_SIZE,
        type=parse_int,
        help=f"External flash size. Default: 0x{DEFAULT_FLASH_SIZE:X}.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    try:
        inferred_elf: Path | None = None
        inferred_flash_bin: Path | None = None

        cbuild_run_arg = args.cbuild_run
        if not cbuild_run_arg and not args.elf:
            default_cbuild_run = find_default_cbuild_run()
            cbuild_run_arg = str(default_cbuild_run) if default_cbuild_run else None

        if cbuild_run_arg:
            cbuild_run = repo_path(cbuild_run_arg)
            if not cbuild_run.is_file():
                print(f"cbuild-run file not found: {cbuild_run}", file=sys.stderr)
                return 1
            inferred_elf, inferred_flash_bin = infer_paths_from_cbuild_run(cbuild_run)

        if not args.elf and inferred_elf is None:
            print("No input ELF specified and no cbuild-run file found.", file=sys.stderr)
            return 1

        elf = repo_path(args.elf) if args.elf else inferred_elf

        flash_bin = (
            repo_path(args.flash_bin)
            if args.flash_bin
            else inferred_flash_bin or repo_path(DEFAULT_FLASH_BIN)
        )
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1

    if not elf.is_file():
        print(f"Input ELF not found: {elf}", file=sys.stderr)
        return 1

    reference_elf = elf

    try:
        objcopy = find_objcopy(args.objcopy)
        reference_elf.parent.mkdir(parents=True, exist_ok=True)
        flash_bin.parent.mkdir(parents=True, exist_ok=True)

        try:
            flash_section = write_flash_bin_from_reference_elf(objcopy, reference_elf, flash_bin)
        except RuntimeError:
            if flash_bin.is_file() and flash_bin.stat().st_size > 0:
                patched = patch_external_flash_load_segments(reference_elf, args.flash_base, args.flash_size)
                if patched:
                    print(f"Removed {patched} stale external-flash LOAD segment(s) from: {reference_elf}")
                print(f"{reference_elf} is already internal-only; keeping existing OSPI flash image: {flash_bin}")
                return 0
            raise RuntimeError(
                f"{reference_elf} does not contain a supported external-flash section "
                f"({flash_section_list()}) and no existing flash image was found. "
                "Rebuild before running split_flash_images.py again."
            )

        with tempfile.TemporaryDirectory(prefix="split_flash_", dir=reference_elf.parent) as tmp_dir:
            tmp_elf = Path(tmp_dir) / reference_elf.name

            run_objcopy(
                objcopy,
                [*(f"--remove-section={section_name}" for section_name in FLASH_SECTION_NAMES), str(reference_elf), str(tmp_elf)],
            )
            patched = patch_external_flash_load_segments(tmp_elf, args.flash_base, args.flash_size)
            if patched:
                print(f"Removed {patched} external-flash LOAD segment(s) from: {tmp_elf}")

            os.replace(tmp_elf, reference_elf)

    except (FileNotFoundError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1

    print(f"Rewrote internal-only ELF: {reference_elf}")
    print(f"Wrote OSPI flash image from {flash_section}: {flash_bin}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
