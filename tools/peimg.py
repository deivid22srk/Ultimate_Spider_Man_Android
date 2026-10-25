"""Shared PE loading for the recompiler tools (optionally rebased)."""
import pefile


def load(path, new_base=None):
    pe = pefile.PE(path)
    if new_base is not None and new_base != pe.OPTIONAL_HEADER.ImageBase:
        pe.relocate_image(new_base)
        pe.OPTIONAL_HEADER.ImageBase = new_base
    base = pe.OPTIONAL_HEADER.ImageBase
    img = bytearray(pe.get_memory_mapped_image(ImageBase=base))
    return pe, base, img


def exports(pe, base):
    out = {}
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for e in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            name = e.name.decode() if e.name else "#%d" % e.ordinal
            out[name] = base + e.address
    return out
