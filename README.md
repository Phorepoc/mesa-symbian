# mesa-symbian

This is [Mesa](https://mesa3d.org) on Symbian. It gives [Phorepoc](https://github.com/Phorepoc) OpenGL ES 1.1 and 2.0
with EGL, so 3D games and apps run. On the PinePhone it draws with the Mali-400 GPU through Mesa's lima driver, and on
any other board it falls back to softpipe, Mesa's software renderer.

We don't keep a copy of Mesa here. The build downloads the release named in `mesa.lock` (26.2.4 at the moment) and
compiles it for Symbian. This repository holds the parts Symbian needs on top:

- `symbian/src/` has the EGL driver, the glue between Mesa and Symbian's windows, and a small emulation of the Linux
  GPU interface that lima expects to talk to.
- `symbian/include/` has the POSIX headers Mesa assumes are there, like `pthread.h` and `xf86drm.h`.
- `build_mesa_symbian.py` compiles each Mesa source file Symbian needs, and `gen_gl_fronts.py` generates the
  `libEGL`, `libGLESv2` and `libGLESv1_CM` libraries apps link against.

The rest of the GL stack lives in [core](https://github.com/Phorepoc/core). That's `phgl.dll`, which these libraries are
linked into, and the code that connects GL surfaces to the screen. The Mali driver itself is in
[adaptation-a64](https://github.com/Phorepoc/adaptation-a64).

## Building

Check out core and the [pdk](https://github.com/Phorepoc/pdk) next to this repository and run `ph source export -a`
once. Then build in the toolchain's Mesa image:

```sh
docker run --rm -v "$PWD/..:/src" -w /src/mesa-symbian ghcr.io/phorepoc/mesa bash build.sh
```

The libraries end up in `core/build/mesa-build/arm/`, ready for the PinePhone build to pick up.

## License

Our code is MIT, like Mesa. Three files are adapted from Mesa's Haiku support and keep their authors' copyright, which
`NOTICE` lists. Mesa itself keeps its own licenses.

Contributions are welcome under the same terms. Add your name to `AUTHORS` in your first pull request.
