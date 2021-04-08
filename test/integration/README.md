# Integration tests

These tests validate our CMake-built packages and make sure reasonable
interactions with the Halide-generated libraries and targets work. They run on
GitHub Actions, rather than the buildbots, to test building, installing, and
using Halide in **simple** cases from a clean build environment. In particular,
this folder **should not** be added to the main Halide build
via `add_subdirectory`.

The assumption is that we are building Halide with the latest CMake version, but
that our users might be on our oldest supported version. GitHub Actions makes it
easy to use two different versions on two different VMs.

There are scenarios here for JIT compilation, AOT compilation, and AOT _cross_
compilation from Linux to Windows (using the mingw-w64 toolchain with the
statically linked runtime). This test in particular cannot be easily run on the
buildbots because it requires three VMs: an Ubuntu build machine for Halide, an
Ubuntu developer machine which installs the DEB packages, and a Windows end-user
machine that runs the cross-compiled pipeline and makes sure it works.

To run these yourself, you will need to install MinGW and wine (both 64-bit) as
shown in the `packaging.yml` workflow file.
