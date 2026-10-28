# Copyright The Phorepoc Authors
# SPDX-License-Identifier: MIT

"""build_mesa_symbian -- Mesa (mesa.lock, pinned) compiled for ARM Symbian (GPU plan G2, OURS, 2026-10-09).

Runs INSIDE the toolchain mesa image (ghcr.io/phorepoc/mesa), after a meson host configure + build
of the generators (build.sh does all three steps). For every TU of the static libraries in LIBS it takes the host
compile command from <build>/mesa-build/host/compile_commands.json, keeps the include dirs and the per-target -D macros,
keeps only the global configure macros that hold on Symbian (GLOBAL_KEEP; Linux facts like futex/dlfcn/memfd are
dropped), and recompiles it with the P.I.P.S. client line of an ARM-only DLL:
  openc_cflags.py --arm (symcc's ARM flags + prefix header + stdapis includes + build_mmp's cdriver transform)
  with -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp (soft-float ABI = links with the rest of the ROM;
  armv7 for ldrex/strex atomics and VFP),
  plus symbian/mesa_symbian.h force-included (the OS facts Mesa's detect_os.h cannot find).
C++ TUs (the GLSL compiler) use the arm-none-eabi libstdc++ headers (header-only std::vector) ahead of stdapis.

Output: <build>/mesa-build/arm/<lib>.a (+ objects under <build>/mesa-build/arm/obj/). Logs per TU on failure: <build>/mesa-build/arm/err/.

    python3 build_mesa_symbian.py [-j N] [--only <lib>[,<lib>]] [--keep-going]
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

FLAGS_VERSION = 2   # bump when base_flags / c_and_cxx / GLOBAL_* / a PATCH of a header change (full rebuild)

HERE = os.path.dirname(os.path.abspath(__file__))
# where things are (build.sh sets them; the defaults are the Phorepoc checkouts next to this one)
MESA = os.path.abspath(os.environ.get("MESA_SRC") or os.path.join(HERE, "mesa"))        # the pinned Mesa (mesa.lock)
CORE = os.path.abspath(os.environ.get("PHOREPOC_CORE") or os.path.join(HERE, "..", "core"))
PDK = os.path.abspath(os.environ.get("PHOREPOC_PDK") or os.path.join(HERE, "..", "pdk"))
BUILD = os.path.abspath(os.environ.get("PHOREPOC_BUILD") or os.path.join(CORE, "build"))
WORK = os.path.join(BUILD, "mesa-build")       # core's registry links phgl with mesa-build/arm/*.a
HOST = os.path.join(WORK, "host")
OUT = os.path.join(WORK, "arm")
SHIM = os.path.join(HERE, "symbian")
SEAM = os.path.join(CORE, "os", "src", "symgl")   # symmali.h + symgl_native.h: core's side of the seam

# the static libraries phgl.dll is made of: softpipe (G2) and lima, the Mali-400 driver (G4) + the panfrost tiling
# helpers it shares
LIBS = ["libegl_symbian", "libgles2_entry", "libgles1_entry", "libmesa_util_c11", "libmesa_util", "libblake3", "libcompiler", "libnir", "libglcpp", "libglsl", "libglsl_util",
        "libglapi", "libmesa", "libgallium", "libgalliumvl_stub", "libsoftpipe", "libws_null", "liblima", "libpanfrost_shared"]

# TUs left out (Linux/x86-only, or features a GLES-only software stack never reaches)
DROP = {
    "libmesa_util": {"u_debug_stack_android.cpp", "os_socket.c", "u_debug_memory.c",
                     "disk_cache.c", "disk_cache_os.c", "mesa_cache_db.c", "mesa_cache_db_multipart.c",
                     "fossilize_db.c", "perf/u_trace.c", "perf/u_perfetto.cc",
                     "texcompress_astc.cpp", "texcompress_astc_luts.cpp", "texcompress_astc_luts_wrap.cpp",
                     "anon_file.c", "os_file_notify.c", "u_dl.c", "u_sync_provider.c"},
    "libgallium": {"rtasm_execmem.c"},
    "libblake3": {"blake3_sse2_x86-64_unix.S", "blake3_sse41_x86-64_unix.S", "blake3_avx2_x86-64_unix.S",
                  "blake3_avx512_x86-64_unix.S"},
}

# Mesa files replaced by Symbian versions (symbian/src; the why is in each file)
REPLACE = {
    "libmesa_util": {"memstream.c": "memstream_symbian.c", "cnd_monotonic.c": "cnd_monotonic_symbian.c",
                     "u_process.c": "u_process_symbian.c"},
}
# small source edits (lib -> file -> [(old, new)]): the edited copy goes to <build>/mesa-build/arm/patched/ and is compiled
# in place of the original (with the original's directory still first for "sibling.h" includes)
PATCH = {
    "libmesa_util": {
        # the OS sysinfo header blocks (two) end in #error for an unknown OS; Symbian needs none of them
        "os_misc.c": [("#else\n#error unexpected platform in os_sysinfo.c\n", "#else\n/* Symbian (OURS): none */\n")],
        # <thread> (gthreads) is not needed for the two thread_locals (per-process globals on Symbian, mesa_symbian.h)
        "u_qsort.cpp": [("#include <thread>\n", "")],
        # Mesa's default log = stderr (nowhere in a Symbian GUI process) -> syslog = RDebug (symbian/include/syslog.h)
        "log.c": [("#else\n      mesa_log_control |= MESA_LOG_CONTROL_FILE;\n#endif",
                   "#elif DETECT_OS_SYMBIAN\n      mesa_log_control |= MESA_LOG_CONTROL_SYSLOG;\n"
                   "#else\n      mesa_log_control |= MESA_LOG_CONTROL_FILE;\n#endif")],
    },
    "libsoftpipe": {
        # softpipe makes a texture tile cache per (shader stage, sampler view) up front: 6 x 128 x 256 KB = 192 MB,
        # zeroed. Linux calloc maps lazy zero pages for that; Symbian's heap commits + zeroes every byte (the context
        # failed in a 64 MB heap). The 16 tile entries (256 KB) of a cache are allocated on its first lookup instead.
        "sp_tex_tile_cache.h": [("   struct softpipe_tex_cached_tile entries[NUM_TEX_TILE_ENTRIES];",
                                 "   struct softpipe_tex_cached_tile *entries;   /* OURS (Symbian): on first use */")],
        "sp_tex_tile_cache.c": [
            ("struct softpipe_tex_tile_cache *\nsp_create_tex_tile_cache(",
             "/* OURS: what last_tile points at before the entries exist (never matches a valid address) */\n"
             "static struct softpipe_tex_cached_tile sp_tex_tile_none = { .addr = { .bits = { .invalid = 1 } } };\n\n"
             "static bool\nsp_tex_tile_cache_alloc_entries(struct softpipe_tex_tile_cache *tc)\n{\n"
             "   if (!tc->entries) {\n"
             "      tc->entries = MALLOC(NUM_TEX_TILE_ENTRIES * sizeof(*tc->entries));\n"
             "      if (!tc->entries)\n         return false;\n"
             "      for (unsigned p = 0; p < NUM_TEX_TILE_ENTRIES; p++)\n"
             "         tc->entries[p].addr.bits.invalid = 1;\n"
             "   }\n   return true;\n}\n\n"
             "struct softpipe_tex_tile_cache *\nsp_create_tex_tile_cache("),
            ("      for (pos = 0; pos < ARRAY_SIZE(tc->entries); pos++) {\n"
             "         tc->entries[pos].addr.bits.invalid = 1;\n      }\n"
             "      tc->last_tile = &tc->entries[0]; /* any tile */",
             "      (void)pos;\n      tc->last_tile = &sp_tex_tile_none;"),
            ("      FREE( tc );", "      FREE( tc->entries );\n      FREE( tc );"),
            ("ARRAY_SIZE(tc->entries)", "(tc->entries ? NUM_TEX_TILE_ENTRIES : 0)"),
            ("   tile = tc->entries + tex_cache_pos( addr );",
             "   if (!sp_tex_tile_cache_alloc_entries(tc))\n      return &sp_tex_tile_none;\n"
             "   tile = tc->entries + tex_cache_pos( addr );"),
        ],
    },
    "libmesa": {
        # which of st_api_create_context's two "no memory" exits (G2 bring-up)
        "st_manager.c": [("#include \"st_debug.h\"\n", "#include \"st_debug.h\"\n#include \"util/log.h\"\n"),
                         ("   if (!pipe) {\n      *error = ST_CONTEXT_ERROR_NO_MEMORY;",
                          "   if (!pipe) {\n      mesa_loge(\"st_api_create_context: screen->context_create failed\");\n"
                          "      *error = ST_CONTEXT_ERROR_NO_MEMORY;"),
                         ("   if (!st) {\n      *error = ST_CONTEXT_ERROR_NO_MEMORY;",
                          "   if (!st) {\n      mesa_loge(\"st_api_create_context: st_create_context failed\");\n"
                          "      *error = ST_CONTEXT_ERROR_NO_MEMORY;")],
        "st_context.c": [("   if (!ctx)\n      return NULL;\n   memset(ctx, 0, sizeof(*ctx));",
                          "   if (!ctx) {\n      mesa_loge(\"st_create_context: gl_context alloc (%u bytes) failed\",\n"
                          "                (unsigned)sizeof(struct gl_context));\n      return NULL;\n   }\n"
                          "   memset(ctx, 0, sizeof(*ctx));"),
                         ("                                 options)) {\n      align_free(ctx);",
                          "                                 options)) {\n"
                          "      mesa_loge(\"st_create_context: _mesa_initialize_context failed\");\n      align_free(ctx);"),
                         ("   st = st_create_context_priv(ctx, pipe, options);\n   if (!st) {",
                          "   st = st_create_context_priv(ctx, pipe, options);\n   if (!st) {\n"
                          "      mesa_loge(\"st_create_context: st_create_context_priv failed\");")],
        "context.c": [("      shared = _mesa_alloc_shared_state(ctx, options);\n      if (!shared)\n         return GL_FALSE;",
                       "      shared = _mesa_alloc_shared_state(ctx, options);\n      if (!shared) {\n"
                       "         mesa_loge(\"_mesa_initialize_context: _mesa_alloc_shared_state failed\");\n"
                       "         return GL_FALSE;\n      }"),
                      ("   if (!init_attrib_groups( ctx ))\n      goto fail;",
                       "   if (!init_attrib_groups( ctx )) {\n"
                       "      mesa_loge(\"_mesa_initialize_context: init_attrib_groups failed\");\n      goto fail;\n   }")],
    },
    # lima's few OS calls that are not drmIoctl: imported-buffer size (lseek on a dma-buf), BO CPU mappings (mmap of
    # the DRM fd), closing sync files -- all routed to the shim (symbian/src/drm_symbian.c, symbian/include/xf86drm.h)
    "liblima": {
        "lima_draw.c": [('#include "lima_job.h"\n', '#include "lima_job.h"\n#include "util/log.h"\n'),
                        ('      struct pipe_vertex_buffer *pvb = vb->vb + pve->vertex_buffer_index;\n      struct lima_resource *res = lima_resource(pvb->buffer.resource);\n\n      lima_job_add_bo(job, LIMA_PIPE_GP, res->bo, LIMA_SUBMIT_BO_READ);\n',
                         '      struct pipe_vertex_buffer *pvb = vb->vb + pve->vertex_buffer_index;\n      struct lima_resource *res = lima_resource(pvb->buffer.resource);\n\n      if (!res) {\n         /* OURS (Symbian): a vertex element whose buffer slot holds no resource (Dungeon Hunter 2, GLES1: user crash at\n          * lima_draw.c, release build has the asserts above compiled out). Point the attribute at the (always valid)\n          * vertex shader BO with stride 0 instead of dereferencing NULL. */\n         static unsigned lima_null_vb_warned;\n         if (!lima_null_vb_warned++)\n            mesa_loge("lima: draw with an empty vertex buffer slot %u (count %u, mask %x): attribute %d dropped",\n                      pve->vertex_buffer_index, vb->count, vb->enabled_mask, i);\n         attribute[n++] = ctx->vs->bo->va;\n         attribute[n++] = (lima_pipe_format_to_attrib_type(pve->src_format) << 2) |\n            (util_format_get_nr_components(pve->src_format) - 1);\n         continue;\n      }\n\n      lima_job_add_bo(job, LIMA_PIPE_GP, res->bo, LIMA_SUBMIT_BO_READ);\n')],
        "lima_bo.c": [("      size = lseek(h, 0, SEEK_END);", "      size = sym_drm_fd_size(h);"),
                      ("      lseek(h, 0, SEEK_SET);\n", ""),
                      ("      bo->map = os_mmap(0, bo->size, PROT_READ | PROT_WRITE,\n"
                       "                        MAP_SHARED, bo->screen->fd, bo->offset);",
                       "      bo->map = sym_drm_mmap(bo->screen->fd, bo->offset, bo->size);"),
                      ("      os_munmap(bo->map, bo->size);", "      sym_drm_munmap(bo->map, bo->size);")],
        "lima_fence.c": [("      close(fence->fd);", "      sym_drm_close_fd(fence->fd);")],
        "lima_job.c": [("      close(ctx->in_sync_fd);", "      sym_drm_close_fd(ctx->in_sync_fd);")],
        # no shader disk cache on Symbian (util's disk_cache.c is dropped; its header stubs the calls) and no ELF
        # build-id note to key it by
        "lima_disk_cache.c": [("   const struct build_id_note *note =\n"
                               "      build_id_find_nhdr_for_addr(lima_disk_cache_init);\n"
                               "   assert(note && build_id_length(note) == BUILD_ID_EXPECTED_HASH_LENGTH); /* sha1 */\n\n"
                               "   const uint8_t *id_sha1 = build_id_data(note);\n"
                               "   assert(id_sha1);\n\n"
                               "   char timestamp[BLAKE3_HEX_LEN];\n"
                               "   _mesa_sha1_format(timestamp, id_sha1);\n\n"
                               "   screen->disk_cache = disk_cache_create(screen->base.get_name(&screen->base), timestamp, 0);",
                               "   screen->disk_cache = NULL;   /* OURS (Symbian): no disk cache */")],
        # PATH_MAX lives in <sys/syslimits.h> on P.I.P.S. (<limits.h> does not pull it in)
        "lima_util.c": [("#include <time.h>\n", "#include <time.h>\n#include <sys/syslimits.h>\n")],
        # no driconf (xmlconfig/expat) on Symbian
        "lima_screen.c": [("   driParseConfigFiles(config->options, config->options_info,\n"
                           "                       &(driConfigFileParseParams) { .driverName = \"lima\" });\n",
                           "   (void)config;   /* OURS (Symbian): no driconf */\n")],
    },
}

# libraries whose PATCH touches a header: compiled from a mirror of this source directory (make_mirror)
MIRROR = {"libsoftpipe": "src/gallium/drivers/softpipe"}

# OURS sources added to a library (compiled with that library's include dirs + macros)
EXTRA = {
    "libmesa_util": ["posix_extra.c", "cxx_extra.cpp"],
}

# libraries meson does not build in our configure (EGL is off there: its DRI driver needs libdrm + a loader) --
# compiled from an explicit source list with the include dirs + macros of the `borrow` libraries
_EGL_MAIN = ["eglapi.c", "eglarray.c", "eglconfigdebug.c", "eglconfig.c", "eglcontext.c", "eglcurrent.c",
             "egldevice.c", "egldisplay.c", "eglglobals.c", "eglimage.c", "egllog.c", "eglsurface.c", "eglsync.c"]
PSEUDO = {
    # Mesa's EGL core + OUR Symbian driver/frontend/winsys (symbian/src; egl_symbian.c says why HAIKU)
    "libegl_symbian": dict(
        borrow=["libsoftpipe", "libmesa", "liblima"],
        incs=[os.path.join(MESA, "src/egl"), os.path.join(MESA, "src/egl/main"),
              os.path.join(MESA, "src/gallium/drivers"), os.path.join(MESA, "src/gallium/winsys"),
              os.path.join(SHIM, "src")],
        defs=["-D_EGL_NATIVE_PLATFORM=_EGL_PLATFORM_HAIKU", "-D_EGL_BUILT_IN_DRIVER_HAIKU", "-DGALLIUM_SOFTPIPE",
              "-DGALLIUM_LIMA"],
        srcs=[os.path.join(MESA, "src/egl/main", f) for f in _EGL_MAIN]
             + [os.path.join(MESA, "src/util/streaming-load-memcpy.c")]   # was a link stub (not in libmesa_util's list): readpixels from WC memory (st_pbo_compute); a plain memcpy on ARM32
             + [os.path.join(SHIM, "src", f) for f in ("egl_symbian.c", "sym_frontend.c", "sym_winsys.c",
                                                        "drm_symbian.c")]),
}

# the public GLES entry points (glClear ...): Mesa builds them into libGLESv2.so / libGLESv1_CM.so, which our
# configure (no EGL/GLX frontend) leaves out -- their dispatch headers are generated here (gen_gldispatch_mapi.py)
GEN = os.path.join(WORK, "gen")
PSEUDO["libgles2_entry"] = dict(borrow=["libglapi"], incs=[GEN], defs=[],
                                srcs=[os.path.join(MESA, "src/mesa/glapi/es2api/libgles2_public.c")])
PSEUDO["libgles1_entry"] = dict(borrow=["libglapi"], incs=[GEN], defs=[],
                                srcs=[os.path.join(MESA, "src/mesa/glapi/es1api/libgles1_public.c")])


def generate() -> None:
    os.makedirs(GEN, exist_ok=True)
    d = os.path.join(MESA, "src/mesa/glapi")
    for api, out in (("glesv2", "es2_glapi_mapi_tmp.h"), ("glesv1", "es1_glapi_mapi_tmp.raw.h")):
        path = os.path.join(GEN, out)
        if not os.path.isfile(path):
            with open(path, "w", newline="\n") as f:
                subprocess.run([sys.executable, "new/gen_gldispatch_mapi.py", api, "glapi/registry/gl.xml"],
                               cwd=d, stdout=f, check=True)
    # Mesa ships libGLESv1_CM.so and libGLESv2.so as two libraries; ours is ONE DLL (phgl.dll) with both entry
    # sets, so the GLES1 public entries drop every name GLES2 already defines (the same glapi dispatch stub)
    es1 = os.path.join(GEN, "es1_glapi_mapi_tmp.h")
    if not os.path.isfile(es1) or os.path.getmtime(es1) < os.path.getmtime(os.path.join(GEN, "es1_glapi_mapi_tmp.raw.h")):
        import re
        es2_names = set(re.findall(r"GLAPIENTRY\s+(gl\w+)\s*\(", open(os.path.join(GEN, "es2_glapi_mapi_tmp.h")).read()))
        out, skip = [], False
        for line in open(os.path.join(GEN, "es1_glapi_mapi_tmp.raw.h")):
            m = re.match(r"GLAPI .*GLAPIENTRY\s+(gl\w+)\s*\(", line)
            if m and m.group(1) in es2_names:
                if line.rstrip().endswith(";"):
                    continue            # the prototype line
                skip = True             # the stub body up to its closing brace
                continue
            if skip:
                if line.startswith("}"):
                    skip = False
                continue
            out.append(line)
        with open(es1, "w", newline="\n") as f:
            f.writelines(out)
        for o in os.listdir(os.path.join(OUT, "obj", "libgles1_entry")) if os.path.isdir(os.path.join(OUT, "obj", "libgles1_entry")) else []:
            os.remove(os.path.join(OUT, "obj", "libgles1_entry", o))


# global configure macros that are true on Symbian too (everything else meson found is a Linux/glibc fact)
GLOBAL_KEEP = {
    "NDEBUG", "__STDC_CONSTANT_MACROS", "__STDC_FORMAT_MACROS", "__STDC_LIMIT_MACROS", "PACKAGE_VERSION",
    "PACKAGE_BUGREPORT", "HAVE_OPENGL", "HAVE_OPENGL_ES_1", "HAVE_OPENGL_ES_2", "HAVE_SOFTPIPE", "HAVE_SWRAST",
    "VIDEO_CODEC_VC1DEC", "VIDEO_CODEC_H264DEC", "VIDEO_CODEC_H264ENC", "VIDEO_CODEC_H265DEC", "VIDEO_CODEC_H265ENC",
    "VIDEO_CODEC_AV1DEC", "VIDEO_CODEC_AV1ENC", "VIDEO_CODEC_VP9DEC", "VIDEO_CODEC_MPEG12DEC", "VIDEO_CODEC_JPEGDEC",
    "USE_LIBGLVND", "HAVE_GFX_COMPUTE", "GLAPI_EXPORT_PROTO_ENTRY_POINTS", "MESA_DEBUG", "DRAW_LLVM_AVAILABLE",
    "AMD_LLVM_AVAILABLE", "GALLIVM_USE_ORCJIT", "THREAD_SANITIZER", "USE_GCC_ATOMIC_BUILTINS", "HAVE_PTHREAD",
    "HAVE_STRUCT_TIMESPEC", "HAVE_STRTOF", "HAVE_STRTOK_R", "HAVE_SYSCONF",
    "XXH_FORCE_ALIGN_CHECK", "XXH_FORCE_MEMORY_ACCESS",
}
GLOBAL_DROP = {
    "_FILE_OFFSET_BITS", "HAVE_LIMA", "MESA_SYSTEM_HAS_KMS_DRM", "ALLOW_KCMP", "HAVE_UINT128", "HAVE_REALLOCARRAY",
    "HAVE_FMEMOPEN", "_GNU_SOURCE", "USE_SSE41", "HAVE___BUILTIN_IA32_CLFLUSHOPT", "MAJOR_IN_SYSMACROS", "HAS_SCHED_H",
    "HAS_SCHED_GETAFFINITY", "HAVE_LINUX_FUTEX_H", "HAVE_ENDIAN_H", "HAVE_DLFCN_H", "HAVE_SYS_SHM_H", "HAVE_CET_H",
    "HAVE_POLL_H", "HAVE_SYS_INOTIFY_H", "HAVE_LINUX_UDMABUF_H", "HAVE_MKOSTEMP", "HAVE_MEMFD_CREATE",
    "HAVE_RANDOM_R", "HAVE_FLOCK", "HAVE_GETRANDOM", "HAVE_POSIX_FALLOCATE", "HAVE_SECURE_GETENV", "HAVE_GNU_QSORT_R",
    "HAVE_PROGRAM_INVOCATION_NAME", "HAVE_ISSIGNALING", "HAVE_POSIX_MEMALIGN", "HAVE_DIRENT_D_TYPE", "HAVE_STRTOD_L",
    "HAVE_DLADDR", "HAVE_DL_ITERATE_PHDR", "HAVE_PTHREAD_SETAFFINITY", "HAVE_LIBDRM", "HAVE_OPENMP",
    "HAVE_RENDERDOC_INTEGRATION", "USE_DRICONF", "SYSCONFDIR", "DATADIR",
}

def _cxx_incs() -> list[str]:
    """the arm-none-eabi libstdc++ header dirs for the armv7-a softfp multilib, as the compiler itself reports them
    (Ubuntu's packaged toolchain and ARM's own put them in different places, under different GCC versions)"""
    r = subprocess.run(["arm-none-eabi-g++", "-march=armv7-a", "-mthumb", "-mfloat-abi=softfp", "-mfpu=vfpv3-d16",
                        "-x", "c++", "-E", "-v", "-"], input="", capture_output=True, text=True)
    lines = r.stderr.splitlines()
    try:
        a = lines.index("#include <...> search starts here:") + 1
        b = lines.index("End of search list.")
    except ValueError:
        return []
    return [os.path.normpath(l.strip()) for l in lines[a:b] if "c++" in l]


CXX_INC = _cxx_incs()


def base_flags() -> list[str]:
    out = subprocess.run([sys.executable, os.path.join(PDK, "tools", "openc_cflags.py"), "--arm",
                          "--std=gnu11"], check=True, capture_output=True, text=True).stdout.split("\n")
    flags = []
    for f in out:
        if not f:
            continue
        f = {"-march=armv5te": "-march=armv7-a", "-mfloat-abi=soft": "-mfloat-abi=softfp", "-O2": "-O2"}.get(f, f)
        flags.append(f)
    # -mword-relocations: armv7 would load addresses with movw/movt (R_ARM_MOVW_ABS_NC/MOVT_ABS), which an E32Image
    # relocation table (32-bit words only) cannot express -- GCC documents SymbianOS as the case for this switch
    return flags + ["-mfpu=vfpv3-d16", "-mword-relocations", "-fno-math-errno", "-fno-trapping-math", "-fno-common",
                    "-Wno-unknown-pragmas", "-Wno-attributes", "-Werror=implicit-function-declaration",
                    "-Werror=incompatible-pointer-types", "-Werror=int-conversion", "-Werror=return-type",
                    "-include", os.path.join(SHIM, "mesa_symbian.h"),
                    # GCC's own freestanding headers (stdalign.h, stdatomic.h ...) where stdapis has none
                    "-idirafter", subprocess.run(["arm-none-eabi-gcc", "-print-file-name=include"], check=True,
                                                 capture_output=True, text=True).stdout.strip()]


def c_and_cxx(flags: list[str]) -> tuple[list[str], list[str]]:
    c = list(flags)
    cxx = []
    skip = False
    for f in flags:
        if skip:
            skip = False
            continue
        if f == "-x":
            skip = True
            continue
        if f.startswith("-std="):
            continue
        if f in ("-DSYMCC_C_DRIVER", "-Dwchar_t=unsigned short", "-D__wchar_t_defined"):
            continue
        cxx.append(f)
    # -flifetime-dse=1 (as Mesa's meson.build): the GLSL IR's operator new stores node_linalloc into the object
    # before its constructor runs; GCC's default (2) deletes those stores as dead -> NULL linear context, crash
    # in linear_zalloc_child on the first shader (seen on the phone 2026-10-09)
    cxx = ["-x", "c++", "-std=gnu++17", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics",
           "-flifetime-dse=1"] + \
          [x for i in CXX_INC for x in ("-isystem", i)] + cxx
    return c, cxx


def tu_flags(entry: dict) -> tuple[list[str], str]:
    args = shlex.split(entry["command"])
    d = entry["directory"]
    keep = []
    for a in args:
        if a.startswith("-I"):
            p = a[2:]
            if p.startswith("/usr/"):
                continue
            keep.append("-I" + os.path.normpath(os.path.join(d, p)))
        elif a.startswith("-D"):
            name = a[2:].split("=", 1)[0]
            if name in GLOBAL_DROP:
                continue
            keep.append(a)
    src = os.path.normpath(os.path.join(d, entry["file"]))
    return keep, src


def make_mirror(lib: str, srcdir: str) -> str:
    """A library whose PATCH edits a HEADER: its whole source directory is copied (edits applied) and every TU of the
    library compiles from the copy -- a quoted #include searches the including file's own directory first, so a
    patched header next to only the patched .c would leave the other TUs on the old one. Unchanged files are not
    rewritten (mtimes drive the recompile check)."""
    dst = os.path.join(OUT, "patched", lib)
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(srcdir):
        p = os.path.join(srcdir, name)
        if not os.path.isfile(p):
            continue
        text = open(p, "rb").read().decode("latin-1").replace("\r\n", "\n")
        for old, new in PATCH.get(lib, {}).get(name, []):
            assert text.count(old) >= 1, (p, old)
            text = text.replace(old, new)
        q = os.path.join(dst, name)
        if not os.path.isfile(q) or open(q, "rb").read().decode("latin-1") != text:
            with open(q, "w", encoding="latin-1", newline="\n") as f:
                f.write(text)
    return dst


def lib_of(entry: dict) -> str | None:
    o = entry["output"]
    i = o.find(".a.p/")
    if i < 0:
        return None
    return os.path.basename(o[:i])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only", default="")
    ap.add_argument("--keep-going", action="store_true")
    a = ap.parse_args()
    libs = a.only.split(",") if a.only else LIBS
    generate()
    cc = json.load(open(os.path.join(HOST, "compile_commands.json")))
    cflags, cxxflags = c_and_cxx(base_flags())
    os.makedirs(os.path.join(OUT, "err"), exist_ok=True)
    for f in os.listdir(os.path.join(OUT, "err")):
        os.remove(os.path.join(OUT, "err", f))
    jobs = []
    lib_flags = {}
    mirrors = {lib: (os.path.join(MESA, rel), make_mirror(lib, os.path.join(MESA, rel)))
               for lib, rel in MIRROR.items() if lib in libs}
    for e in cc:
        lib = lib_of(e)
        if lib not in libs:
            continue
        rel = os.path.relpath(os.path.normpath(os.path.join(e["directory"], e["file"])), MESA)
        if any(rel.endswith("/" + x) or rel == x or os.path.basename(rel) == x for x in DROP.get(lib, ())):
            continue
        extra, src = tu_flags(e)
        obj = os.path.join(OUT, "obj", lib, os.path.basename(e["output"]))
        edits = PATCH.get(lib, {}).get(os.path.basename(src))
        if lib in mirrors and os.path.dirname(src) == mirrors[lib][0]:
            src = os.path.join(mirrors[lib][1], os.path.basename(src))   # (edits already applied in the mirror)
            edits = None
        if edits:
            text = open(src, encoding="utf-8").read()
            for old, new in edits:
                assert text.count(old) >= 1, (src, old)
                text = text.replace(old, new)
            patched = os.path.join(OUT, "patched", lib, os.path.basename(src))
            os.makedirs(os.path.dirname(patched), exist_ok=True)
            if not os.path.isfile(patched) or open(patched, encoding="utf-8").read() != text:
                with open(patched, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            extra = ["-iquote", os.path.dirname(src)] + extra
            src = patched
        rep = REPLACE.get(lib, {}).get(os.path.basename(src))
        if rep:
            # the replacement still finds the original's sibling headers ("foo.h")
            extra = ["-iquote", os.path.dirname(src)] + extra
            src = os.path.join(SHIM, "src", rep)
        jobs.append((lib, src, obj, extra))
        lib_flags.setdefault(lib, (extra, os.path.dirname(src)))
    for lib, p in PSEUDO.items():
        if lib not in libs:
            continue
        incs, defs = [], []
        for b in p["borrow"]:
            for e in cc:
                if lib_of(e) == b:
                    for f in tu_flags(e)[0]:
                        (incs if f.startswith("-I") else defs).append(f)
                    break
        flags = [f"-I{i}" for i in p["incs"]] + list(dict.fromkeys(incs)) + list(dict.fromkeys(defs)) + p["defs"]
        for src in p["srcs"]:
            jobs.append((lib, src, os.path.join(OUT, "obj", lib, os.path.basename(src) + ".o"), flags))
    for lib, files in EXTRA.items():
        if lib in libs and lib in lib_flags:
            extra, d = lib_flags[lib]
            for f in files:
                jobs.append((lib, os.path.join(SHIM, "src", f), os.path.join(OUT, "obj", lib, "ours_" + f + ".o"),
                             extra))
    print(f"{len(jobs)} TUs in {len(libs)} libraries", flush=True)
    # a shim HEADER changed, or the flags (FLAGS_VERSION, bumped by hand when a flag/define/patch that affects every
    # TU changes) -> everything recompiles; a shim .c/.cpp is a TU of its own (its mtime). Mesa's own headers are not
    # tracked.
    stamp_file = os.path.join(OUT, "flags.stamp")
    if not os.path.isfile(stamp_file) or open(stamp_file).read().strip() != str(FLAGS_VERSION):
        with open(stamp_file, "w", newline="\n") as f:
            f.write(str(FLAGS_VERSION) + "\n")
    stamp = max([os.path.getmtime(stamp_file), os.path.getmtime(os.path.join(SHIM, "mesa_symbian.h"))] +
                [os.path.getmtime(os.path.join(d, f)) for d, _, fs in os.walk(os.path.join(SHIM, "include")) for f in fs])

    def run(job):
        lib, src, obj, extra = job
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        if os.path.isfile(obj) and os.path.getmtime(obj) > max(os.path.getmtime(src), stamp):
            return lib, obj, True
        cxx = src.endswith((".cpp", ".cc"))
        if src.endswith(".S"):
            return lib, obj, None
        # Mesa's include dirs (and our overrides) BEFORE the Symbian system includes: epoc32/include carries the
        # 2010 KHR/EGL/GLES headers, older than the ones Mesa is written against
        cmd = (["arm-none-eabi-gcc", f"-I{os.path.join(SHIM, 'include')}", f"-I{SEAM}"] + extra + (cxxflags if cxx else cflags)
               + ["-c", src, "-o", obj])
        r = subprocess.run(cmd, capture_output=True, text=True)
        err = os.path.join(OUT, "err", lib + "__" + os.path.basename(src) + ".txt")
        if r.returncode != 0:
            with open(err, "w", newline="\n") as f:
                f.write(" ".join(shlex.quote(c) for c in cmd) + "\n\n" + r.stderr)
            return lib, obj, False
        if os.path.exists(err):
            os.remove(err)
        return lib, obj, True

    ok = {lib: [] for lib in libs}
    bad = []
    with ThreadPoolExecutor(a.j) as ex:
        for lib, obj, good in ex.map(run, jobs):
            if good:
                ok[lib].append(obj)
            elif good is False:
                bad.append((lib, obj))
    for lib in libs:
        ar = os.path.join(OUT, lib + ".a")
        if os.path.exists(ar):
            os.remove(ar)
        if ok[lib] and (not bad or a.keep_going):
            subprocess.run(["arm-none-eabi-ar", "rcs", ar] + ok[lib], check=True)
        print(f"  {lib}: {len(ok[lib])} ok, {sum(1 for l, _ in bad if l == lib)} failed")
    print(f"{len(bad)} failed (logs: <build>/mesa-build/arm/err/)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
