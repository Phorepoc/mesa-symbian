#!/usr/bin/env bash
# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0

# recipes/prepare.sh -- run by the build before the components (pdk docs/layering.md), on the host: Mesa for ARM or x32
# (build.sh in the mesa toolchain image) -> $PHOREPOC_BUILD/mesa-build/$PHOREPOC_ARCH/*.a (arm: softpipe + the lima Mali driver; x32: softpipe), which phgl links.
set -eu
: "${PHOREPOC_BUILD:?}" "${PHOREPOC_PDK:?}" "${PHOREPOC_CORE:?}" "${PHOREPOC_ARCH:?}"
case "$PHOREPOC_ARCH" in arm|x32) ;; *) echo "mesa-symbian: nothing to build for $PHOREPOC_ARCH"; exit 0;; esac
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(dirname "$HERE")"   # (the folder of all the checkouts)
docker run --rm -v "$ROOT:$ROOT" -v "$PHOREPOC_BUILD:$PHOREPOC_BUILD" -w "$HERE" \
	-e PHOREPOC_BUILD -e PHOREPOC_CORE -e PHOREPOC_PDK -e PHOREPOC_ARCH \
	ghcr.io/phorepoc/mesa bash build.sh
