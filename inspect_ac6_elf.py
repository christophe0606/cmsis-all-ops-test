#!/usr/bin/env python3

import argparse
import struct
from pathlib import Path


PT_LOAD = 1
SHF_ALLOC = 0x2
STT_FUNC = 2


def cstr(blob: bytes, offset: int) -> str:
    end = blob.find(b"\0", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("utf-8", errors="replace")


def read_elf(path: Path):
    data = path.read_bytes()
    if len(data) < 0x34 or data[:4] != b"\x7fELF" or data[4] != 1:
        raise RuntimeError(f"{path} is not an ELF32 file")
    endian = "<" if data[5] == 1 else ">"

    ehdr_fmt = endian + "HHIIIIIHHHHHH"
    phdr_fmt = endian + "IIIIIIII"
    shdr_fmt = endian + "IIIIIIIIII"
    sym_fmt = endian + "IIIBBH"

    eh = struct.unpack_from(ehdr_fmt, data, 0x10)
    e_phoff, e_shoff = eh[4], eh[5]
    e_phentsize, e_phnum = eh[8], eh[9]
    e_shentsize, e_shnum, e_shstrndx = eh[10], eh[11], eh[12]

    phdrs = []
    for index in range(e_phnum):
        ph = struct.unpack_from(phdr_fmt, data, e_phoff + index * e_phentsize)
        phdrs.append(
            {
                "index": index,
                "type": ph[0],
                "offset": ph[1],
                "vaddr": ph[2],
                "paddr": ph[3],
                "filesz": ph[4],
                "memsz": ph[5],
                "flags": ph[6],
                "align": ph[7],
            }
        )

    sections = []
    for index in range(e_shnum):
        sh = struct.unpack_from(shdr_fmt, data, e_shoff + index * e_shentsize)
        sections.append(
            {
                "index": index,
                "name_off": sh[0],
                "type": sh[1],
                "flags": sh[2],
                "addr": sh[3],
                "offset": sh[4],
                "size": sh[5],
                "link": sh[6],
                "entsize": sh[9],
            }
        )

    shstr = sections[e_shstrndx]
    shstr_data = data[shstr["offset"] : shstr["offset"] + shstr["size"]]
    for section in sections:
        section["name"] = cstr(shstr_data, section["name_off"])

    symbols = []
    for section in sections:
        if section["type"] not in (2, 11) or not section["entsize"]:
            continue
        strtab = sections[section["link"]]
        strtab_data = data[strtab["offset"] : strtab["offset"] + strtab["size"]]
        for offset in range(section["offset"], section["offset"] + section["size"], section["entsize"]):
            sym = struct.unpack_from(sym_fmt, data, offset)
            name = cstr(strtab_data, sym[0])
            if name:
                shndx = sym[5]
                symbols.append(
                    {
                        "name": name,
                        "value": sym[1],
                        "size": sym[2],
                        "info": sym[3],
                        "type": sym[3] & 0xF,
                        "section": sections[shndx]["name"] if shndx < len(sections) else f"SHN_{shndx}",
                    }
                )

    return data, phdrs, sections, symbols


def file_offset_for_addr(sections, phdrs, addr: int) -> int | None:
    for section in sections:
        if section["addr"] <= addr < section["addr"] + section["size"]:
            return section["offset"] + (addr - section["addr"])
    for phdr in phdrs:
        if phdr["type"] != PT_LOAD:
            continue
        if phdr["vaddr"] <= addr < phdr["vaddr"] + phdr["filesz"]:
            return phdr["offset"] + (addr - phdr["vaddr"])
        if phdr["paddr"] <= addr < phdr["paddr"] + phdr["filesz"]:
            return phdr["offset"] + (addr - phdr["paddr"])
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("--symbol", action="append", default=[])
    parser.add_argument("--contains", action="append", default=[])
    parser.add_argument("--dump-symbol", action="append", default=[])
    parser.add_argument("--dump-addr", nargs=2, action="append", default=[], metavar=("ADDR", "SIZE"))
    parser.add_argument("--scan-word-range", nargs=2, metavar=("START", "END"))
    args = parser.parse_args()

    data, phdrs, sections, symbols = read_elf(args.elf)

    print(f"ELF: {args.elf}")
    print("Program headers:")
    for ph in phdrs:
        type_name = "PT_LOAD" if ph["type"] == PT_LOAD else str(ph["type"])
        print(
            f"  #{ph['index']:2d} {type_name:7s} off=0x{ph['offset']:08x} "
            f"vaddr=0x{ph['vaddr']:08x} paddr=0x{ph['paddr']:08x} "
            f"filesz=0x{ph['filesz']:x} memsz=0x{ph['memsz']:x} flags=0x{ph['flags']:x}"
        )

    print("Allocated sections:")
    for section in sections:
        if not (section["flags"] & SHF_ALLOC):
            continue
        print(
            f"  [{section['index']:2d}] {section['name']:<32s} "
            f"addr=0x{section['addr']:08x} off=0x{section['offset']:08x} "
            f"size=0x{section['size']:x} flags=0x{section['flags']:x}"
        )

    needles = [*args.symbol, *args.contains]
    if needles:
        print("Symbols:")
        for symbol in symbols:
            if any(symbol["name"] == needle or needle in symbol["name"] for needle in needles):
                print(
                    f"  {symbol['name']:<48s} value=0x{symbol['value']:08x} "
                    f"size=0x{symbol['size']:x} section={symbol['section']}"
                )

    for name in args.dump_symbol:
        matches = [symbol for symbol in symbols if symbol["name"] == name]
        if not matches:
            print(f"Dump {name}: not found")
            continue
        symbol = matches[0]
        symbol_addr = symbol["value"] & ~1 if symbol["type"] == STT_FUNC else symbol["value"]
        next_addrs = sorted(
            (other["value"] & ~1 if other["type"] == STT_FUNC else other["value"])
            for other in symbols
            if other["section"] == symbol["section"]
            and (other["value"] & ~1 if other["type"] == STT_FUNC else other["value"]) > symbol_addr
        )
        size = symbol["size"] or ((next_addrs[0] - symbol_addr) if next_addrs else 64)
        size = min(size, 128)
        offset = file_offset_for_addr(sections, phdrs, symbol_addr)
        if offset is None:
            print(f"Dump {name}: no file offset")
            continue
        blob = data[offset : offset + size]
        print(f"Dump {name} @ 0x{symbol['value']:08x} (data 0x{symbol_addr:08x}), {len(blob)} bytes:")
        for index in range(0, len(blob), 16):
            chunk = blob[index : index + 16]
            words = [
                struct.unpack_from("<I", chunk, word_offset)[0]
                for word_offset in range(0, len(chunk) - (len(chunk) % 4), 4)
            ]
            print(
                f"  +0x{index:02x}: {chunk.hex(' '):<47s} "
                + " ".join(f"0x{word:08x}" for word in words)
            )

    for addr_text, size_text in args.dump_addr:
        addr = int(addr_text, 0)
        size = int(size_text, 0)
        offset = file_offset_for_addr(sections, phdrs, addr)
        if offset is None:
            print(f"Dump addr 0x{addr:08x}: no file offset")
            continue
        blob = data[offset : offset + size]
        print(f"Dump addr 0x{addr:08x}, {len(blob)} bytes:")
        for index in range(0, len(blob), 16):
            chunk = blob[index : index + 16]
            words = [
                struct.unpack_from("<I", chunk, word_offset)[0]
                for word_offset in range(0, len(chunk) - (len(chunk) % 4), 4)
            ]
            text = "".join(chr(byte) if 32 <= byte <= 126 else "." for byte in chunk)
            print(
                f"  +0x{index:02x}: {chunk.hex(' '):<47s} "
                + " ".join(f"0x{word:08x}" for word in words)
                + f"  {text}"
            )

    if args.scan_word_range:
        start = int(args.scan_word_range[0], 0)
        end = int(args.scan_word_range[1], 0)
        print(f"Words in [0x{start:08x}, 0x{end:08x}):")
        for section in sections:
            if not (section["flags"] & SHF_ALLOC) or section["type"] == 8:
                continue
            blob = data[section["offset"] : section["offset"] + section["size"]]
            for index in range(0, len(blob) - 3, 4):
                word = struct.unpack_from("<I", blob, index)[0]
                if start <= word < end:
                    print(
                        f"  {section['name']:<32s} addr=0x{section['addr'] + index:08x} "
                        f"off=0x{section['offset'] + index:08x} word=0x{word:08x}"
                    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
