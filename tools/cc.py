"""Host toolchain wrapper: compiles generated C with clang-cl."""
import glob
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor


def _find_clang():
    """clang-cl: SSBU_CLANG if set, else Visual Studio's own (the "C++ Clang tools" component),
    newest edition first, else a standalone LLVM install, else whatever is on PATH."""
    if os.environ.get("SSBU_CLANG"):
        return os.environ["SSBU_CLANG"]
    def vs_major(path):   # the folder is a year up to 2022 (2022 = 17), a major version since (18)
        name = path.split(os.sep + "Microsoft Visual Studio" + os.sep)[1].split(os.sep)[0]
        return {"2017": 15, "2019": 16, "2022": 17}.get(name, int(name) if name.isdigit() else 0)
    found = []
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        found += sorted(glob.glob(os.path.join(root, "Microsoft Visual Studio", "*", "*", "VC", "Tools", "Llvm", "x64", "bin", "clang-cl.exe")),
                        key=vs_major, reverse=True)
    for root in (os.environ.get("ProgramFiles", r"C:\Program Files"),):
        found += glob.glob(os.path.join(root, "LLVM", "bin", "clang-cl.exe"))
    return found[0] if found else "clang-cl"


CLANG = _find_clang()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime")
# Guest FP semantics need IEEE-exact arithmetic: no contraction, no fast-math.
CFLAGS = ["/nologo", "/arch:AVX2", "-mfma", "-mf16c", "-fno-strict-aliasing", "/clang:-ffp-contract=off",
          "-w", "/I", RUNTIME]

# Optional static codec libraries supplied explicitly by the developer.
# SSBU_OPUS_LIBS is a semicolon-separated list of Windows x64 .lib files.
OPUS_LIBS = [p for p in os.environ.get("SSBU_OPUS_LIBS", "").split(";") if p]
if not all(os.path.exists(p) for p in OPUS_LIBS):
    OPUS_LIBS = []
if OPUS_LIBS:
    CFLAGS.append("/DSSBU_HAVE_OPUS")
# libvpx for the video decode engine's VP9 (movies). SSBU_VPX_DIR overrides where (include/, lib/Win64/Release/).
VPX_DIR = os.environ.get("SSBU_VPX_DIR", "")
VPX_LIBS = [os.path.join(VPX_DIR, r"lib\Win64\Release\libvpx.lib")]
if not all(os.path.exists(p) for p in VPX_LIBS) or not os.path.exists(os.path.join(VPX_DIR, "include", "vpx", "vpx_decoder.h")):
    VPX_LIBS = []
if VPX_LIBS:
    CFLAGS += ["/DSSBU_HAVE_VPX", "/I", os.path.join(VPX_DIR, "include")]
# SDL3 for controllers (zlib licence): the runtime loads SDL3.dll at run time, so the build needs only the headers,
# and the link step copies the DLL beside the exe. SSBU_SDL3_DIR overrides where (include/, lib/x64/SDL3.dll).
SDL3_DIR = os.environ.get("SSBU_SDL3_DIR", os.path.join(ROOT, "deps", "SDL3"))
SDL3_DLL = os.path.join(SDL3_DIR, r"lib\x64\SDL3.dll")
if os.path.exists(os.path.join(SDL3_DIR, "include", "SDL3", "SDL_gamepad.h")) and os.path.exists(SDL3_DLL):
    CFLAGS += ["/DSSBU_HAVE_SDL3", "/I", os.path.join(SDL3_DIR, "include")]
else:
    SDL3_DLL = None
# (they were built against the DLL C runtime; this program uses the static one)
EXTRA_LIBS = OPUS_LIBS + VPX_LIBS + (["/NODEFAULTLIB:msvcrt.lib"] if OPUS_LIBS or VPX_LIBS else [])


def compile_one(src, obj, opt="/O2"):
    r = subprocess.run([CLANG, "/c", opt, *CFLAGS, src, "/Fo" + obj], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"compile failed: {src}\n{(r.stdout + r.stderr)[:4000]}")
    return obj


def compile_many(sources, jobs=16, opt="/O2"):
    """Compile each source next to itself; returns the object paths."""
    with ThreadPoolExecutor(jobs) as pool:
        return list(pool.map(lambda s: compile_one(s, os.path.splitext(s)[0] + ".obj", opt), sources))


def compile_each(jobs_list, jobs=16):
    """Compile (src, obj, opt) triples in parallel; returns [(src, error text)] for failures."""
    def one(job):
        src, obj, opt = job
        r = subprocess.run([CLANG, "/c", opt, *CFLAGS, src, "/Fo" + obj], capture_output=True, text=True)
        return (src, r.stdout + r.stderr) if r.returncode else None
    with ThreadPoolExecutor(jobs) as pool:
        return [f for f in pool.map(one, jobs_list) if f]


def link(objs, exe, options=()):
    """Link objects into an executable; `options` go to the linker."""
    rsp = exe + ".rsp"
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    lld = ["-fuse-ld=lld"] if os.path.exists(os.path.join(os.path.dirname(CLANG), "lld-link.exe")) else []
    r = subprocess.run([CLANG, "/nologo", *lld, "@" + rsp, "/Fe" + exe, "/link", "/HIGHENTROPYVA:NO", "/MAP:" + exe[:-4] + ".map", *options, "winmm.lib", "user32.lib", "gdi32.lib", "d3d11.lib", "dxgi.lib", "d3dcompiler.lib", "dxguid.lib", "ws2_32.lib", "ole32.lib", "oleaut32.lib", *EXTRA_LIBS],
                       capture_output=True, text=True)
    if r.returncode:
        lines = (r.stdout + r.stderr).splitlines()
        errors = [l for l in lines if "error" in l.lower()] or lines   # (the third-party libraries' CRT warnings drown them)
        raise RuntimeError("link failed\n" + "\n".join(errors)[:4000])
    return exe
