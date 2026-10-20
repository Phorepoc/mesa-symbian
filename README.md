# mesa-symbian

[Mesa](https://mesa3d.org) ported to Symbian for [Phorepoc](https://github.com/Phorepoc): OpenGL ES 1.1 / 2.0 and EGL,
rendered by Mesa's softpipe on any board, or by the Mali-400 (Mesa's lima driver) on the Allwinner A64.

Mesa is not copied here. This repository holds what Symbian needs on top of it and the recipe that builds it:

| path | what |
|---|---|
| `symbian/src/` | the Symbian EGL driver, gallium winsys and frontend; the DRM uAPI emulation lima talks to; POSIX and C runtime gaps |
| `symbian/include/` | the headers Mesa expects from a POSIX system (`pthread.h`, `xf86drm.h` ...) |
| `symbian/mesa_symbian.h` | force-included into every Mesa file: the OS facts Mesa's `detect_os.h` cannot find |
| `build_mesa_symbian.py` | recompiles each needed Mesa source file for ARM Symbian, from a host build's `compile_commands.json` |
| `gen_gl_fronts.py` | the `libEGL` / `libGLESv2` / `libGLESv1_CM` fronts over `phgl.dll`, at the 2010 ordinals |
| `mesa.lock` | the Mesa release this builds (26.2.4) |
| `build.sh` | all of it, start to end |

The other half is in [core](https://github.com/Phorepoc/core): `phgl.dll`, which links these archives, and the
`symgl` window-surface code with the GPU channel seam (`symmali.h`, `symgl_native.h`). A board with a GPU brings its own
channel ([adaptation-a64](https://github.com/Phorepoc/adaptation-a64): the Mali-400).

## Building

Inside the toolchain's mesa image, with core and the [pdk](https://github.com/Phorepoc/pdk) checked out next to this
repository and the include tree exported (`ph source export -a`):

```sh
docker run --rm -v "$PWD/..:/src" -w /src/mesa-symbian ghcr.io/phorepoc/mesa ./build.sh
```

Output: `core/build/mesa-build/arm/*.a`, and the generated fronts in `core/os/src/phgl/gen/`.

## Licence

MIT (see `LICENSE`), copyright The Phorepoc Authors (`AUTHORS`). Three files are adapted from Mesa's Haiku support and
keep their authors' copyright; `NOTICE` names them. Mesa itself keeps its own licences.

Contributions are welcome under the same terms: add yourself to `AUTHORS`.
