#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# recipes/prepare.sh -- run by the build before the components (pdk docs/layering.md), on the host: Mesa for ARM
# (build.sh in the mesa toolchain image) -> $PHOREPOC_BUILD/mesa-build/arm/*.a, which phgl links.
set -eu
: "${PHOREPOC_BUILD:?}" "${PHOREPOC_PDK:?}" "${PHOREPOC_CORE:?}" "${PHOREPOC_ARCH:?}"
[ "$PHOREPOC_ARCH" = arm ] || { echo "mesa-symbian: nothing to build for $PHOREPOC_ARCH (the lima backend is ARM only)"; exit 0; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(dirname "$HERE")"   # (the folder of all the checkouts)
docker run --rm -v "$ROOT:$ROOT" -v "$PHOREPOC_BUILD:$PHOREPOC_BUILD" -w "$HERE" \
	-e PHOREPOC_BUILD -e PHOREPOC_CORE -e PHOREPOC_PDK \
	ghcr.io/phorepoc/mesa bash build.sh
