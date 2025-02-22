def _settings_transition_impl(settings, attr):
    return {
        "@jemalloc//settings/flags:jemalloc_prefix": "je_",
        "@jemalloc//settings/flags:jemalloc_cprefix": "JE_",
    }

_settings_transition = transition(
    implementation = _settings_transition_impl,
    inputs = [
        "@jemalloc//settings/flags:jemalloc_prefix",
        "@jemalloc//settings/flags:jemalloc_cprefix",
    ],
    outputs = [
        "@jemalloc//settings/flags:jemalloc_prefix",
        "@jemalloc//settings/flags:jemalloc_cprefix",
        #        "//settings/platform:lg_page",
    ],
)

def _transition_settings_impl(ctx):
    return [ctx.attr.src[0][DefaultInfo]]

#    target = ctx.attr.src
#    possible_providers = [
#        DefaultInfo,
#        CcInfo,
#        InstrumentedFilesInfo,
#        OutputGroupInfo,
#    ]
#
#    print(target[0])
#
#    return [target[0][provider] for provider in possible_providers if provider in target[0]]

transition_settings = rule(
    implementation = _transition_settings_impl,
    attrs = {
        "target": attr.label(cfg = _settings_transition),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

platform_transition_binary = rule(
    implementation = _platform_transition_binary_impl,
    attrs = {
        "basename": attr.string(),
        "binary": attr.label(allow_files = True, cfg = binary_cfg),
        "target_platform": attr.label(
            doc = "The target platform to transition the binary.",
            mandatory = True,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
    executable = True,
    doc = "Transitions the binary to use the provided platform. Will forward RunEnvironmentInfo",
)
