# Copyright The Phorepoc Authors
# SPDX-License-Identifier: MIT
"""Mesa in core's GL backend: phgl.dll links the archives recipes/prepare.sh builds (ARM). Without this layer phgl
builds alone and its EGL / GLES exports stay unimplemented."""

COMPONENTS = {}

EXTEND = {
    "phgl": dict(arm_archives=[f".mesa-build/arm/{a}.a" for a in (
        "libegl_symbian", "libgles2_entry", "libgles1_entry", "libmesa", "libglapi", "libsoftpipe",
        "liblima", "libpanfrost_shared", "libgallium", "libgalliumvl_stub",
        "libglsl", "libglcpp", "libglsl_util", "libnir", "libcompiler", "libmesa_util",
        "libmesa_util_c11", "libblake3", "libws_null")]),
}
