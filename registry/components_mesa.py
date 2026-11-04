# Copyright The Phorepoc Authors
# SPDX-License-Identifier: MIT
"""Mesa in core's GL backend: phgl.dll links the archives recipes/prepare.sh builds -- ARM (softpipe + the lima Mali
driver) or x32 (softpipe + the svga driver of a VM with 3D). Without this layer phgl builds alone and its EGL / GLES exports stay unimplemented."""

COMPONENTS = {}

_COMMON = ("libegl_symbian", "libgles2_entry", "libgles1_entry", "libmesa", "libglapi", "libsoftpipe")
_REST = ("libgallium", "libgalliumvl_stub", "libglsl", "libglcpp", "libglsl_util", "libnir", "libcompiler",
         "libmesa_util", "libmesa_util_c11", "libblake3", "libws_null")

EXTEND = {
    "phgl": dict(
        arm_archives=[f".mesa-build/arm/{a}.a" for a in _COMMON + ("liblima", "libpanfrost_shared") + _REST],
        x32_archives=[f".mesa-build/x32/{a}.a" for a in _COMMON + ("libsvga", "libsvgadrm") + _REST],
        # (-mno-popcnt: the 64-bit population count is a libgcc function)
        x32_sys_archives=["libgcc.a"]),
}
