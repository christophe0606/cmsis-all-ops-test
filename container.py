import struct
import hashlib
import argparse
from pathlib import Path

ALIGN = 16
MAGIC = 0xBEEFDEAD

def _get_network(filename: Path) -> bytes:
    if filename.suffix.lower() == ".pte":
        with open(filename, "rb") as f:
            return f.read()
    else:
        raise ValueError(f"Unknown network file type: {filename}")


def _get_network_length(filename: Path) -> int:
    if filename.suffix.lower() == ".pte":
        return filename.stat().st_size
    else:
        raise ValueError(f"Unknown network file type: {filename}")


class Container:
    HEADER_SIZE = 16 # magic + image size + object count + maximum object size
    NETWORK_DESC_SIZE = 8 # 4 bytes for object size + 4 bytes for object offset

    def __init__(self, base_addr: int, output: Path):
        if base_addr % 4 != 0:
            raise ValueError("base_addr must be 4-byte aligned")
        self.base_addr = base_addr

        self.output = output
        self.file = open(output, "w+b")
        self._offset = 0
        self._total_size = 0
        self._max_object_size = 0
        # Padding to add before writing a network
        self._network_padding = []
        self._network_offsets = []
        self._network_sizes = []

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.file.close()
        # Return False (or None) so any exception is propagated.
        return False

    def write_uint32(self, value: int):
        if value < 0 or value > 0xFFFFFFFF:
            raise ValueError(f"Value does not fit in uint32_t: {value}")
        self.file.write(struct.pack("<I", value))
        self._offset += 4

    def write_padding(self, nb_bytes: int):
        self.file.write(bytes([0]*nb_bytes))
        self._offset += nb_bytes

    @staticmethod
    def align_up(value: int, alignment: int) -> int:
        return (value + alignment - 1) & ~(alignment - 1)

    def compute_layout(self, filenames: list[Path]):
        # Objects are written after the table of network descriptions
        offset = self.base_addr + Container.HEADER_SIZE + len(filenames)*Container.NETWORK_DESC_SIZE
        for file in filenames:
            aligned_offset = Container.align_up(offset, ALIGN)
            pad = aligned_offset - offset
            offset += pad
            self._network_padding.append(bytes([0]*pad))
            network_size = _get_network_length(file)
            self._network_sizes.append(network_size)
            self._max_object_size = max(self._max_object_size, network_size)
            self._network_offsets.append(offset - self.base_addr)
            offset += network_size
        self._total_size = offset - self.base_addr

    def write_header(self):
        self.write_uint32(MAGIC)
        self.write_uint32(self._total_size)
        self.write_uint32(len(self._network_sizes))
        self.write_uint32(self._max_object_size)

    def write_network_tables(self):
        for network_size, object_offset in zip(self._network_sizes, self._network_offsets):
            self.write_uint32(network_size)
            self.write_uint32(object_offset)

    def write_networks(self, filenames: list[Path]):
        for file, padding, object_offset in zip(filenames, self._network_padding, self._network_offsets):
            self.write_padding(len(padding))
            expected_offset = object_offset
            if self._offset != expected_offset:
                raise RuntimeError(
                    f"Internal layout error for {file}: offset {self._offset} != {expected_offset}"
                )
            network = _get_network(file)
            self.file.write(network)
            self._offset += len(network)

    def compute_md5(self):
        self.file.flush()
        self.file.seek(0)
        digest = hashlib.file_digest(self.file, "md5").hexdigest()
        return digest

    @property
    def offset(self):
        return self._offset


def generate_container(base_addr: int, filenames: list[Path], output: Path):
    with Container(base_addr, output) as container:
        container.compute_layout(filenames)
        container.write_header()
        container.write_network_tables()
        container.write_networks(filenames)

        md5 = container.compute_md5()
        print(f"md5 hash = {md5}")
