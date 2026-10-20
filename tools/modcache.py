"""What game version trees share: NRO load addresses, compiled module build dirs, and the
check that every module's imports resolve. Used by tools/recomp.py.

A tree is the port's own folder or a game version's folder laid out like it (dump/, build/,
save/: tools/install_version.py). Versions share much of their code: sdk and subsdk0 are the
same from 11.0.0 to 13.0.5, and fighter modules (NROs) can be the same between nearby updates.
A module is known by its build id, and its generated C and objects depend only on its bytes,
its load address and the tools, so a module another tree already built is reused as it is.

NRO load addresses (registry): <port>/module_bases.txt, "<build id> <name> <base> <end>",
shared by every tree, so a module keeps one address in every version. It starts as every
tree's dump/nro/bases.txt (the per-tree record, which stays the authority for that tree). A
new module goes past every module of every tree, so that no version's module is in another's
way later; once the space for NROs is used up, it takes any free slots of its own tree.
Modules must not share a 16 MB dispatch slot (runtime/host.c SLOT_SHIFT), and NROs live
between subsdk/sdk's slots and the heap: [0x7200000000, 0x7400000000), 512 slots.

Module build dirs: each holds a stamp, module.txt: "<build id> <base> <name> <tools
fingerprint>", written once its C is generated and compiled. A tree that lacks a module's dir
takes the files of another tree's dir for the same build id and base: hard links when the
volume allows (instant, no space), else copies, with their times, so objects stay up to date.
recomp.py replaces a file rather than writing into it (write_if_changed, compile_dir), so a
tree never changes another's files through a link. With the same tools fingerprint (the
generator's sources and the runtime's native-function set) the C is not generated again.
"""
import hashlib
import os
import re
import shutil

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
REGISTRY = os.path.join(ROOT, "module_bases.txt")
NRO_BASE, NRO_LIMIT, SLOT = 0x7200000000, 0x7400000000, 0x1000000
FIXED = ("main", "subsdk0", "sdk")
# the sources whose output is the generated C (a change in any of them generates it again)
GENERATOR = ("recomp.py", "lift.py", "lift_simd.py", "a64.py", "funcs.py", "eh.py", "nso.py")


def trees():
    """The port's own folder, then every folder in it laid out like it (with a dump/)."""
    out = [ROOT]
    for n in sorted(os.listdir(ROOT)):
        p = os.path.join(ROOT, n)
        if n not in ("dump", "build") and os.path.isdir(os.path.join(p, "dump")):
            out.append(p)
    return out


def same_path(a, b):
    return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def replace_file(path, text):
    """Write a file as a new one (never through a link another tree shares)."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


# ---- NRO load addresses -------------------------------------------------------------
def read_bases(path):
    """[name, base, end, build id] rows of a tree's dump/nro/bases.txt."""
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [l.split() for l in f if len(l.split()) == 4]


def registry():
    """build id -> (name, base, end) of every NRO any tree has placed."""
    reg = {}
    if os.path.exists(REGISTRY):
        with open(REGISTRY) as f:
            for line in f:
                p = line.split()
                if len(p) == 4 and not line.startswith("#"):
                    reg[p[0]] = (p[1], int(p[2], 16), int(p[3], 16))
    added = False
    for t in trees():
        for name, base, end, bid in read_bases(os.path.join(t, "dump", "nro", "bases.txt")):
            if bid not in reg:
                reg[bid] = (name, int(base, 16), int(end, 16))
                added = True
    if added or not os.path.exists(REGISTRY):
        write_registry(reg)
    return reg


def write_registry(reg):
    rows = sorted(reg.items(), key=lambda kv: (kv[1][1], kv[0]))
    replace_file(REGISTRY, "# NRO load addresses by build id, shared by every game version tree (tools/modcache.py):\n"
                 "# <build id> <name> <base> <end>\n"
                 + "".join(f"{bid} {n} {b:x} {e:x}\n" for bid, (n, b, e) in rows))


def slots(base, end):
    return set(range(base // SLOT, (end - 1) // SLOT + 1))


def place(build_id, name, size, rows):
    """(name, base, reused) for a new NRO in a tree whose NROs are `rows` (bases.txt rows):
    the address it has in the other trees if it is free here, else a new one."""
    reg = registry()
    used = set()
    for r in rows:
        used |= slots(int(r[1], 16), int(r[2], 16))
    known = reg.get(build_id)
    if known and not slots(known[1], known[1] + size) & used:
        return known[0], known[1], True
    top = max([NRO_BASE] + [e for _n, _b, e in reg.values()] + [int(r[2], 16) for r in rows])
    base = (top + SLOT - 1) // SLOT * SLOT
    if base + size > NRO_LIMIT:   # the shared space is used up: any slots this tree has free
        base = next((b for b in range(NRO_BASE, NRO_LIMIT - size + 1, SLOT) if not slots(b, b + size) & used), None)
        if base is None:
            raise SystemExit(f"no room left for {name} ({size:,} bytes) among this tree's modules")
    if not known:
        reg[build_id] = (name, base, base + size)
        write_registry(reg)
    return name, base, False


# ---- module build dirs ---------------------------------------------------------------
def fingerprint(extra):
    """The tools' fingerprint: the generator's sources and `extra` (the runtime's set of
    native functions and wraps, which change what is generated)."""
    h = hashlib.sha256()
    for n in GENERATOR:
        with open(os.path.join(TOOLS, n), "rb") as f:
            h.update(n.encode() + b"\0" + f.read().replace(b"\r\n", b"\n") + b"\0")
    h.update(extra.encode())
    return h.hexdigest()[:16]


TAIL = re.compile(r'const RtModule rt_module_(\w+) = \{\s*"\w+", 0x([0-9A-F]+)ull,.*?"([0-9A-F]+)",\s*\};', re.S)


def build_info(d):
    """(build id, base, name, tools fingerprint or None) of a module build dir, or None:
    from its stamp, or for a dir made before stamps, from the end of its g_module.c."""
    try:
        with open(os.path.join(d, "module.txt")) as f:
            p = f.read().split()
        return p[0], int(p[1], 16), p[2], p[3]
    except (OSError, IndexError, ValueError):
        pass
    try:
        with open(os.path.join(d, "g_module.c"), "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 4096))
            m = TAIL.search(f.read().decode("utf-8", "replace"))
    except OSError:
        return None
    return (m.group(3), int(m.group(2), 16), m.group(1), None) if m else None


def write_stamp(d, build_id, base, name, fp):
    replace_file(os.path.join(d, "module.txt"), f"{build_id} {base:x} {name} {fp}\n")


def clear_stamp(d):
    try:
        os.remove(os.path.join(d, "module.txt"))
    except OSError:
        pass


def module_dirs(tree):
    b = os.path.join(tree, "build")
    out = [os.path.join(b, n) for n in FIXED]
    nro = os.path.join(b, "nro")
    if os.path.isdir(nro):
        out += [os.path.join(nro, n) for n in sorted(os.listdir(nro))]
    return [d for d in out if os.path.isdir(d)]


_built = None


def find_built(build_id, base, skip, fp):
    """Another tree's build dir of the module with this build id at this base, or None: one
    generated with the same tools first, else the most recently generated."""
    global _built
    if _built is None:
        _built = {}
        for t in trees():
            for d in module_dirs(t):
                info = build_info(d)
                manifest = os.path.join(d, "manifest.tsv")
                if info and os.path.exists(manifest) and os.path.exists(os.path.join(d, "g_module.obj")):
                    _built.setdefault((info[0], info[1]), []).append((info[3] == fp, os.path.getmtime(manifest), d))
    found = [c for c in _built.get((build_id, base), []) if not same_path(c[2], skip)]
    return max(found)[2] if found else None


def adopt(src, dst):
    """Fill module build dir `dst` with `src`'s files (hard links, else copies). Returns how
    many were linked and copied."""
    os.makedirs(dst, exist_ok=True)
    for n in os.listdir(dst):   # whatever another module left here
        if n.endswith((".c", ".obj", ".tsv", ".txt", ".tmp")):
            os.remove(os.path.join(dst, n))
    linked = copied = 0
    for n in os.listdir(src):
        if not n.endswith((".c", ".obj", ".tsv", ".txt")):
            continue
        s, d = os.path.join(src, n), os.path.join(dst, n)
        try:
            os.link(s, d)
            linked += 1
        except OSError:
            shutil.copy2(s, d)
            copied += 1
    return linked, copied


# ---- imports ---------------------------------------------------------------------------
def unresolved_imports(mods, native):
    """name -> (strong, weak) symbols the module refers to (by relocation) that no module
    exports and the runtime does not implement (`native`): what would stay unbound when the
    game runs (runtime/host.c resolve). mods: name -> nso.Module."""
    exported = set()
    for m in mods.values():
        exported |= {s[0] for s in m.exports}
    out = {}
    for name, m in mods.items():
        refs = {r[2] for r in m.relocs if r[1] != 1027 and r[2]}
        strong, weak = set(), set()
        for i in refs:
            sname, _v, _s, info, shndx = m.symbols[i]
            if not sname or shndx or sname in exported or sname in native:
                continue
            (weak if info >> 4 == 2 else strong).add(sname)
        out[name] = (sorted(strong), sorted(weak))
    return out


def report_imports(mods, native, path):
    """Print and write (to `path`) every module's unresolved imports; returns how many strong ones."""
    res = unresolved_imports(mods, native)
    lines = ["# symbols a module refers to that no module exports and the runtime does not implement (HLE):",
             "# <module> strong|weak <symbol>   (strong ones trap when called; weak ones read as 0)"]
    total_s = total_w = 0
    for name in mods:
        strong, weak = res[name]
        total_s += len(strong)
        total_w += len(weak)
        lines += [f"{name} strong {s}" for s in strong] + [f"{name} weak {s}" for s in weak]
        if strong:
            more = f" (+{len(strong) - 3} more)" if len(strong) > 3 else ""
            print(f"  {name}: {len(strong)} unresolved import(s): {', '.join(strong[:3])}{more}")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    replace_file(path, "\n".join(lines) + "\n")
    print(f"imports: {total_s} strong and {total_w} weak symbols unresolved across {len(mods)} modules (listed in {path})")
    return total_s
