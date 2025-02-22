"Reproduce the public_symbols.txt generation from configure.ac"

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")

def _public_symbols_impl(ctx):
    output = ctx.actions.declare_file(ctx.attr.out)
    prefix = ctx.attr._prefix[BuildSettingInfo].value

    # List of key:value pairs for custom mangling
    mangling_map = {}
    if ctx.attr.mangling_map:
        # Parse "orig:mangled,orig2:mangled2" format
        pairs = ctx.attr.mangling_map.split(",")
        for pair in pairs:
            orig, mangled = pair.split(":")
            mangling_map[orig] = mangled

    # Generate mappings, with mangling_map taking precedence
    content = ""
    for sym in ctx.attr.symbols:
        if sym in mangling_map:
            content += "{sym}:{mangling_map[sym]}\n".format(sym = sym, mangling_map = mangling_map)
        else:
            content += "{sym}:{prefix}{sym}\n".format(sym = sym, prefix = prefix, mangling_map = mangling_map)

    ctx.actions.write(
        output = output,
        content = content,
    )

    return DefaultInfo(files = depset([output]))

public_symbols = rule(
    implementation = _public_symbols_impl,
    attrs = {
        "symbols": attr.string_list(
            doc = "List of all public symbols",
            allow_empty = False,
            mandatory = True,
        ),
        "mangling_map": attr.string(
            doc = "Untested feature to support the equivalent of --with-mangling=<key_1>:<value_1>,<key_2>:<value_2>,...",
        ),
        "out": attr.string(
            doc = "Name of the generated file",
            mandatory = True,
        ),
        "_prefix": attr.label(default = "//settings/flags:jemalloc_prefix"),
    },
)
