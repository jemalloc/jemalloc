# Bazel build of jemalloc

This is a minimal fork of [jemalloc](https://github.com/jemalloc/jemalloc) to maintain a set of overlay files for 
building jemalloc with [Bazel](https://bazel.build/). 

The upstream jemalloc build performs a number of feature detection steps in the configure script. All of which need to be
statically declared via platform and compiler constraints. Some of these have been made configurable under
//settings, but the list is not exhaustive. An attempt has been made to default to logical values assuming the most
common configurations. But these assumptions cannot satisfy all cases in the wild and may break on some systems. 
Contributors are encouraged to open MRs to add more configuration options to //settings/platform.

## Usage

See the [examples/BUILD.bazel](examples/BUILD.bazel) file for examples of how to consume `@jemalloc`.

## Notes

- **//settings/flags:jemalloc_prefix (--with-jemalloc-prefix):** In contrast to the make build, this defaults to having
 no prefix. The original reasoning was that jemalloc was likely to replace malloc on linux but not MacOS or Windows.
 In practice in Bazel, this forcibly bifurcates the API based on target platform making the cc_library more difficult
 to consume by cross-platform downstream targets. Anyone wanting to eject jemalloc as an .so will likely want to
 reintroduce this behavior.
- **//settings/platform:lg_vaddr:** Defaults to the hard-coded cross compilation value of 57 from the configure script.
 If targeting a 32-bit platform, this should be explicitly set.
- **glibc:** Assumes a modern version. If jemalloc is compiled to be the default allocator on linux, it assumes glibc
 and some publicly exposed symbols can be wrapped: __malloc_hook, __realloc_hook, __free_hook, __memalign_hook.
 However, they are deprecated since glibc 2.34. The jemalloc source likely needs to be updated to reflect this.

Platform configurations that have defaults that are applied via transition.
- //settings/flags:enable_zone_allocator
- //settings/platform:glibc_overrides_support
- //settings/platform:lg_page
- //settings/platform:memalign_support
- //settings/platform:malloc_size_support
- //settings/platform:usable_size_const
- //settings/platform:valloc_support

## Validated Platforms

- Ubuntu 20.04 & 22.04
- Debian 10
- MacOS 15 (M3)

## Known Issues

- **Windows:** An effort to was made to translate upstream MSVC configuration, but it has not been validated and will 
    likely fail.
- **FreeBSD:** An effort was made to translate existing autoconf feature detection to Bazel config settings, but no 
    attempt has been made to build on FreeBSD.
- **Centos 7:** Fails due to the lack of defining `JEMALLOC_MADV_FREE`. Though, the original build does not set it when 
    `MADV_FREE` is present. Supporting both and being faithful to the configure script would require an additional 
    platform setting.
- C++ compilation might be broken for LLVM around detecting libstdc++.

## Contributing

Before contributing new versions or updates to the Bazel Central Registry, these contributions should be made as Merge 
Requests here:
  - Fork the repository targeting the existing BCR release branch (e.g `bcr-5.3.0`) for the jemalloc version or create 
    a new one 
  - Verify the that all tests pass on the supported platforms e.g. `bazel test //...`
  - Open a Merge Request
