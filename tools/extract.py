#!/usr/bin/env python3
"""Extract matching base ExeFS and plaintext RomFS from a user-supplied XC2 NSP."""
import argparse
from pathlib import Path

from nca import copy_out, nsp_ncas, pfs0_entries

ROOT = Path(__file__).resolve().parent.parent


def write_romfs(section, destination):
    """Publish a complete image; never overwrite existing data or hide a short read."""
    destination = Path(destination)
    if section.data_size <= 0:
        raise ValueError("RomFS section is empty")
    if destination.exists():
        raise FileExistsError(destination)
    partial = destination.with_name(destination.name + ".partial")
    done = 0
    with partial.open("xb") as output:
        while done < section.data_size:
            size = min(32 << 20, section.data_size - done)
            data = section.read(done, size)
            if len(data) != size:
                raise OSError(f"Short RomFS read at {done}: expected {size}, got {len(data)}")
            output.write(data)
            done += size
            print(f"\rRomFS: {done * 100 // section.data_size}%", end="", flush=True)
    partial.rename(destination)
    print()


def extract(package, keys, root):
    root = Path(root).resolve()
    exefs = root / "exefs_base"
    image = root / "romfs" / "romfs.bin"
    config = root / "dump" / "modules" / "romfs.cfg"
    for path in (exefs, image, image.with_name("romfs.bin.partial"), config):
        if path.exists():
            raise FileExistsError(f"Refusing to overwrite {path}")
    ncas = nsp_ncas(str(package), str(keys))
    try:
        programs = [nca for _, nca in ncas if nca.content_type == 0]
        if len(programs) != 1:
            raise ValueError(f"Expected one Program NCA, found {len(programs)}")
        program = programs[0]
        if program.title_id & 0xFFF == 0x800:
            raise ValueError("Use the base 1.0.0 NSP, not an update package")
        section = program.section(1)
        entries = list(pfs0_entries(section))
        names = [name for name, _, _ in entries]
        if any(name in ("", ".", "..") or "/" in name or "\\" in name for name in names):
            raise ValueError("ExeFS contains an unsafe filename")
        if len(set(names)) != len(names) or not {"main", "sdk", "subsdk0"}.issubset(names):
            raise ValueError("ExeFS is missing XC2 modules or contains duplicate filenames")
        exefs.mkdir(parents=True)
        for name, offset, size in entries:
            destination = exefs / name
            copy_out(section, offset, size, str(destination))
            if destination.stat().st_size != size:
                raise OSError(f"Short ExeFS read: {name}")
        image.parent.mkdir(parents=True, exist_ok=True)
        write_romfs(program.section(0), image)
        config.parent.mkdir(parents=True, exist_ok=True)
        config.write_text("romfs/romfs.bin\n0\n", encoding="utf-8")
        print(f"Ready: {root}. Run br.ps1 from this project.")
    finally:
        for _, nca in ncas:
            nca.f.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path, help="Your XC2 base 1.0.0 NSP")
    parser.add_argument("--keys", type=Path, required=True, help="Directory containing your prod.keys")
    parser.add_argument("--out", type=Path, default=ROOT, help="Project data root (default: this checkout)")
    args = parser.parse_args()
    extract(args.package, args.keys, args.out)


if __name__ == "__main__":
    main()
