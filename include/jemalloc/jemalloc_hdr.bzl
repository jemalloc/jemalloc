"""jemalloc_hdr"""

def _jemalloc_hdr_impl(ctx):
    out = ctx.outputs.out

    args = ctx.actions.args()
    args.add(out)
    args.add_all(ctx.files.srcs)

    ctx.actions.run(
        mnemonic = "JemallocHdrGen",
        inputs = ctx.files.srcs,
        outputs = [out],
        executable = ctx.executable._generator,
        arguments = [args],
    )

    return [DefaultInfo(files = depset([out]))]

jemalloc_hdr = rule(
    doc = "A rule to generate `jemalloc.h` headers.",
    implementation = _jemalloc_hdr_impl,
    attrs = {
        "out": attr.output(
            doc = "The output header",
            mandatory = True,
        ),
        "srcs": attr.label_list(
            doc = "The source files to include",
            allow_files = True,
            mandatory = True,
        ),
        "_generator": attr.label(
            cfg = "exec",
            executable = True,
            default = Label("//include/jemalloc:jemalloc_hdr_gen"),
        ),
    },
)
