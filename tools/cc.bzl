"Project specific configuration of rules_cc"

load("@bazel_skylib//lib:selects.bzl", "selects")

_COPTS_GCC_COMPATIBLE = [
    "-Wall",
    "-Wextra",
    "-Wsign-compare",
    "-Wundef",
    "-Wno-format-zero-length",
    "-Wpointer-arith",
    "-Wno-missing-braces",
    "-Wno-missing-field-initializers",
    "-Wimplicit-fallthrough",
    "-pipe",
]

_COPTS_GCC = _COPTS_GCC_COMPATIBLE + ["-Wno-missing-attributes"]

_COPTS_MSVC_COMPATIBLE = [
    "/Zi",
    "/MT",
    "/W3",
    "/FS",
]

# Platform and compiler COPTS consistent with configure/make
COPTS = ["-D_REENTRANT"] + select({
    "@rules_cc//cc/compiler:clang": _COPTS_GCC_COMPATIBLE + ["-Wshorten-64-to-32"],
    "@rules_cc//cc/compiler:gcc": _COPTS_GCC,
    "@rules_cc//cc/compiler:mingw-gcc": _COPTS_GCC,
    "@rules_cc//cc/compiler:msvc-cl": _COPTS_MSVC_COMPATIBLE,
    "@rules_cc//cc/compiler:clang-cl": _COPTS_MSVC_COMPATIBLE,
    "//conditions:default": [],
}) + selects.with_or({
    ("@platforms//os:windows", "//settings/platform:darwin"): [],
    "//settings/compiler:gcc_compatible": ["-fvisibility=hidden"],
    "//conditions:default": [],
}) + selects.with_or({
    ("@platforms//os:android", "@platforms//os:linux"): [
        "-DPIC",
        "-D_GNU_SOURCE",
    ],
    "@platforms//os:freebsd": [
        "-DPIC",
        "-D_BSD_SOURCE",
    ],
    "@platforms//os:macos": ["-DPIC"],
    "//conditions:default": [],
}) + select({
    "//settings/flags:prof_gcc": ["-fno-omit-frame-pointer"],
    "//conditions:default": [],
})
