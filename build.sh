#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: MIT

# build.sh -- Mesa for ARM Symbian, start to end. Runs inside the toolchain's mesa image (ghcr.io/phorepoc/mesa),
# with the Phorepoc core and pdk checkouts and a build directory where `ph source export` has made the include tree:
#
#   PHOREPOC_CORE=../core PHOREPOC_PDK=../pdk PHOREPOC_BUILD=../core/build ./build.sh [-j N]
#
# 1. fetch the Mesa release pinned in mesa.lock into ./mesa (MESA_SRC = use another checkout of that commit);
# 2. meson host configure + build: only for Mesa's generators (NIR, glapi and format tables, GLSL parsers, lima's
#    algebraic passes) and compile_commands.json;
# 3. build_mesa_symbian.py: every needed source file recompiled for ARM Symbian -> $PHOREPOC_BUILD/mesa-build/arm/*.a,
#    which core's registry links into phgl.dll;
# 4. gen_gl_fronts.py: the libEGL / libGLESv2 / libGLESv1_CM fronts and phgl.def, into core's os/src/phgl/gen/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export PHOREPOC_CORE="${PHOREPOC_CORE:-$HERE/../core}"
export PHOREPOC_PDK="${PHOREPOC_PDK:-$HERE/../pdk}"
export PHOREPOC_BUILD="${PHOREPOC_BUILD:-$PHOREPOC_CORE/build}"
export MESA_SRC="${MESA_SRC:-$HERE/mesa}"
COMMIT="$(awk '$1 == "commit" {print $2}' "$HERE/mesa.lock")"
URL="$(awk '$1 == "url" {print $2}' "$HERE/mesa.lock")"

if [ ! -d "$MESA_SRC/.git" ]; then
  git init -q "$MESA_SRC"
  git -C "$MESA_SRC" remote add origin "$URL"
fi
if [ "$(git -C "$MESA_SRC" rev-parse -q --verify HEAD 2>/dev/null)" != "$COMMIT" ]; then
  git -C "$MESA_SRC" fetch -q --depth 1 origin "$COMMIT"
  git -C "$MESA_SRC" checkout -q --detach "$COMMIT"
fi

HOST="$PHOREPOC_BUILD/mesa-build/host"
if [ ! -f "$HOST/compile_commands.json" ]; then
  meson setup "$HOST" "$MESA_SRC" -Dbuildtype=release -Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
    -Dgallium-drivers=softpipe,lima -Dvulkan-drivers= -Dllvm=disabled -Dgles1=enabled -Dgles2=enabled -Dopengl=true \
    -Dshader-cache=disabled -Dxmlconfig=disabled -Dzlib=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled \
    -Dlibunwind=disabled -Dbuild-tests=false -Dlmsensors=disabled -Ddisplay-info=disabled -Dgallium-va=disabled \
    -Dvideo-codecs= -Dtools=
fi
# every static library (not just the default targets: with no frontend configured, meson leaves libmesa, libglsl ...
# out of "all"), so all the generated sources and headers the Symbian build compiles against exist
ninja -C "$HOST" $(ninja -C "$HOST" -t targets all | awk -F: '$1 ~ /\.a$/ {print $1}')

python3 "$HERE/build_mesa_symbian.py" "$@"
python3 "$HERE/gen_gl_fronts.py"
