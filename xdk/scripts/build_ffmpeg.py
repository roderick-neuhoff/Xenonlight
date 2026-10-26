"""Builds XBMC-360's Xbox 360 FFmpeg (1.2.12 with VMX128 H.264 code) with the
Xbox 360 SDK compiler, using the source lists and settings from its own
Visual Studio projects. Produces avcodec.lib, avutil.lib and pthreads.lib.

    python xdk/scripts/build_ffmpeg.py
Environment: XEDK (default: the extracted SDK under C:\\AI\\xbox\\dev kits).
"""
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))          # xdk/
FFMPEG = os.path.join(ROOT, "deps", "XBMC-360", "libraries", "ffmpeg")
VCPROJ = os.path.join(FFMPEG, "vcproj")
OUT = os.path.join(ROOT, "build", "ffmpeg")
XEDK = os.environ.get("XEDK", r"C:\AI\xbox\dev kits\XBOX360 SDK 21256.3\extracted\XDK")
# An installed SDK may set XEDK to its "kit" folder, which has no Jul12 compiler.
if not os.path.isdir(os.path.join(XEDK, "TechPreview", "Jul12Compiler")):
    XEDK = os.path.join(os.path.dirname(XEDK), "extracted", "XDK")
JUL12 = os.path.join(XEDK, "TechPreview", "Jul12Compiler")

ENV = dict(os.environ)
ENV["PATH"] = os.pathsep.join([os.path.join(JUL12, "bin", "win32"), os.path.join(XEDK, "bin", "win32"), ENV["PATH"]])
ENV["INCLUDE"] = os.pathsep.join([os.path.join(JUL12, "include", "xbox"), os.path.join(XEDK, "include", "xbox")])
ENV["LIB"] = os.pathsep.join([os.path.join(JUL12, "lib", "xbox"), os.path.join(XEDK, "lib", "xbox")])

# Windows' CreateProcess ignores the child's PATH, so call the tools by path.
CL = os.path.join(JUL12, "bin", "win32", "cl.exe")
LIBEXE = os.path.join(JUL12, "bin", "win32", "lib.exe")

# /Ou: instruction prescheduling for Xenon's in-order cores. /Oc: no
# divide-by-zero trap checks around integer divisions.
COMMON = ["/nologo", "/c", "/Ox", "/Ou", "/Oc", "/Oi", "/GF", "/Gy", "/MT", "/W3", "/wd4305", "/TC",
          "/D", "WIN32", "/D", "NDEBUG", "/D", "_WINDOWS", "/D", "_XBOX"]

# Our VMX functions (xdk/ffmpeg_xbox), built into avcodec.lib.
XBOX_DSP = os.path.join(ROOT, "ffmpeg_xbox")
EXTRA_SOURCES = {"avcodec": [os.path.join(XBOX_DSP, "h264_vmx.c")]}

# Hooks at the end of FFmpeg's DSP init functions (after the per-arch ones)
# that install the VMX functions. (file, exact text, replacement)
HOOK_FIXUPS = []
for _name, _ctx, _args, _call in (
        ("h264qpel", "H264QpelContext *c, int bit_depth", "c, bit_depth", "    if (ARCH_X86)\n        ff_h264qpel_init_x86(c, bit_depth);\n"),
        ("h264chroma", "H264ChromaContext *c, int bit_depth", "c, bit_depth", "    if (ARCH_X86)\n        ff_h264chroma_init_x86(c, bit_depth);\n"),
        ("videodsp", "VideoDSPContext *ctx, int bpc", "ctx, bpc", "    if (ARCH_X86)\n        ff_videodsp_init_x86(ctx, bpc);\n")):
    _decl = f"void ff_{_name}_init_x86({_ctx});\n"
    HOOK_FIXUPS.append((f"libavcodec/{_name}.h", _decl, _decl + f"void ff_{_name}_init_xbox({_ctx});\n"))
    HOOK_FIXUPS.append((f"libavcodec/{_name}.c", _call,
                        _call + f"#ifdef _XBOX\n    ff_{_name}_init_xbox({_args});\n#endif\n"))


HOOK_FIXUPS += [
    ("libavcodec/h264dsp.h",
     "void ff_h264dsp_init_x86(H264DSPContext *c, const int bit_depth,\n                         const int chroma_format_idc);\n",
     "void ff_h264dsp_init_x86(H264DSPContext *c, const int bit_depth,\n                         const int chroma_format_idc);\n"
     "void ff_h264dsp_init_xbox(H264DSPContext *c, const int bit_depth, const int chroma_format_idc);\n"),
    ("libavcodec/h264dsp.c",
     "    if (ARCH_X86) ff_h264dsp_init_x86(c, bit_depth, chroma_format_idc);\n",
     "    if (ARCH_X86) ff_h264dsp_init_x86(c, bit_depth, chroma_format_idc);\n"
     "#ifdef _XBOX\n    ff_h264dsp_init_xbox(c, bit_depth, chroma_format_idc);\n#endif\n"),
]


# Slice workers 0-4 go on hardware threads 1-5 as before; a sixth worker wraps
# around to thread 0 instead of asking for a thread 6 that doesn't exist.
HOOK_FIXUPS += [
    ("libavcodec/xb_thread.c", "#define hw_thread(x) (x+1)\n", "#define hw_thread(x) (((x) + 1) % 6)\n"),
]


def apply_fixups(fixups):
    """Applies exact text replacements to FFmpeg's sources; already-applied ones are skipped."""
    for rel, old, new in fixups:
        path = os.path.join(FFMPEG, rel)
        text = open(path, encoding="utf-8", errors="surrogateescape", newline="").read()
        crlf = "\r\n" in text
        if crlf:
            old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
        if new in text:
            continue
        if old not in text:
            raise SystemExit(f"fixup no longer matches: {rel}")
        with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(text.replace(old, new, 1))
# The FFmpeg libraries need HAVE_AV_CONFIG_H (pulls in libavutil/internal.h);
# XBMC-360's "Release" configuration sets it, its "Release_LTCG" one doesn't.
FFMPEG_DEFINES = ["HAVE_AV_CONFIG_H"]

LIBS = {
    # name: (vcxproj, extra include dirs relative to the project dir, forced includes, extra defines)
    "avutil": ("libavutil/libavutil.vcxproj", [r"..\external\include", r"..\.."], ["config.h"], FFMPEG_DEFINES),
    "avcodec": ("libavcodec/libavcodec.vcxproj", [r"..\..", r"..\external\zlib", r"..\external\include"],
                [r"libavutil/intmath.h", "config.h"], FFMPEG_DEFINES),
    "pthreads": ("pthreads/pthreads.vcxproj", [r"../external/pthreads/"], [], ["HAVE_CONFIG_H", "PTW32_STATIC_LIB", "_LIB"]),
}


def sources_of(vcxproj):
    text = open(vcxproj, encoding="utf-8", errors="replace").read()
    return [s for s in re.findall(r'<ClCompile Include="([^"]+)"', text) if s.lower().endswith(".c")]


def compile_one(args):
    src, obj, flags = args
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src):
        return src, 0, ""
    r = subprocess.run([CL] + flags + [src, "/Fo" + obj], env=ENV, capture_output=True, text=True,
                       errors="replace", cwd=os.path.dirname(obj))
    return src, r.returncode, r.stdout + r.stderr


def build(name):
    proj, incs, forced, defines = LIBS[name]
    proj_path = os.path.join(VCPROJ, proj)
    proj_dir = os.path.dirname(proj_path)
    obj_dir = os.path.join(OUT, name)
    os.makedirs(obj_dir, exist_ok=True)

    flags = list(COMMON)
    for inc in incs:
        flags += ["/I", os.path.normpath(os.path.join(proj_dir, inc))]
    for f in forced:
        flags += ["/FI" + f]
    for d in defines:
        flags += ["/D", d]

    if name == "avcodec":
        flags += ["/I", XBOX_DSP]

    # Objects are only rebuilt when their source is newer, so start over when
    # the flags change.
    stamp = os.path.join(obj_dir, "flags.txt")
    if not os.path.exists(stamp) or open(stamp).read() != " ".join(flags):
        for f in os.listdir(obj_dir):
            if f.endswith(".obj"):
                os.remove(os.path.join(obj_dir, f))
        with open(stamp, "w") as f:
            f.write(" ".join(flags))

    jobs, objs = [], []
    sources = [os.path.normpath(os.path.join(proj_dir, s)) for s in sources_of(proj_path)]
    for src in sources + EXTRA_SOURCES.get(name, []):
        # Object names must be unique across sub-directories (e.g. two dsputil.c).
        rel = os.path.relpath(src, FFMPEG if src.startswith(FFMPEG) else ROOT).replace("\\", "_").replace("/", "_")
        obj = os.path.join(obj_dir, os.path.splitext(rel)[0] + ".obj")
        jobs.append((src, obj, flags))
        objs.append(obj)

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for src, rc, out in pool.map(compile_one, jobs):
            if rc != 0:
                failed += 1
                print(f"FAILED {os.path.relpath(src, FFMPEG)}")
                print("\n".join(l for l in out.splitlines() if "error" in l.lower())[:3000])
    print(f"{name}: {len(jobs)} sources, {failed} failed")
    if failed:
        return False

    lib = os.path.join(OUT, name + ".lib")
    rsp = os.path.join(OUT, name + ".rsp")
    with open(rsp, "w") as f:
        f.write("\n".join(f'"{o}"' for o in objs))
    r = subprocess.run([LIBEXE, "/nologo", "/OUT:" + lib, "@" + rsp], env=ENV, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout + r.stderr)
        return False
    print(f"  -> {lib} ({os.path.getsize(lib) // 1024} KB)")
    return True


if __name__ == "__main__":
    names = sys.argv[1:] or ["pthreads", "avutil", "avcodec"]
    apply_fixups(HOOK_FIXUPS)
    ok = all([build(n) for n in names])
    sys.exit(0 if ok else 1)
