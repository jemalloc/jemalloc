"Transition build and platform settings"

load("@rules_cc//cc:defs.bzl", "CcInfo")

PLATFORM = select({
    "@platforms//os:freebsd": "freebsd",
    "@platforms//os:linux": "linux",
    "@platforms//os:windows": "windows",
    "//settings/platform:darwin": "darwin",
    "//conditions:default": "unknown",
})

def _platform_constraints_transition_impl(settings, attr):
    # Assume typical defaults for target platforms
    default_settings = {
        "//settings/flags:enable_zone_allocator": {"darwin": "yes", "default": "no"},
        "//settings/platform:glibc_overrides_support": {"linux": "yes", "default": "no"},
        "//settings/platform:lg_page": {"darwin": "14", "default": "12"},
        "//settings/platform:memalign_support": {"linux": "yes", "default": "no"},
        "//settings/platform:malloc_size_support": {"darwin": "yes", "default": "no"},
        "//settings/platform:usable_size_const": {"linux": "", "default": "const"},
        "//settings/platform:valloc_support": {"darwin": "yes", "linux": "yes", "default": "no"},
    }

    result = {}
    for label, platform_defaults in default_settings.items():
        configured_value = settings[label]

        # If unset, use platform-specific default or universal fallback
        result[label] = (platform_defaults.get(attr.platform, platform_defaults["default"]) if configured_value == "__auto__" else configured_value)

    # Apply explicitly passed settings
    boolean_settings = ["//settings:enable_jet", "//settings/flags:enable_fill"]
    for setting in boolean_settings:
        result[setting] = attr.settings[Label(setting)] == "True" if Label(setting) in attr.settings else settings[setting]

    string_settings = ["//settings:with_test", "//settings/flags:jemalloc_prefix"]
    for setting in string_settings:
        result[setting] = attr.settings[Label(setting)] if Label(setting) in attr.settings else settings[setting]

    # Equivalent of JEMALLOC_CPREFIX=`echo ${JEMALLOC_PREFIX} | tr "a-z" "A-Z"`
    result["//settings/flags:jemalloc_cprefix"] = result["//settings/flags:jemalloc_prefix"].upper()

    return result

_platform_constraints_transition = transition(
    implementation = _platform_constraints_transition_impl,
    inputs = [
        "//settings:enable_jet",
        "//settings:with_test",
        "//settings/flags:jemalloc_prefix",
        "//settings/flags:jemalloc_cprefix",
        "//settings/flags:enable_fill",
        "//settings/flags:enable_zone_allocator",
        "//settings/platform:glibc_overrides_support",
        "//settings/platform:lg_page",
        "//settings/platform:malloc_size_support",
        "//settings/platform:memalign_support",
        "//settings/platform:usable_size_const",
        "//settings/platform:valloc_support",
    ],
    outputs = [
        "//settings:enable_jet",
        "//settings:with_test",
        "//settings/flags:jemalloc_prefix",
        "//settings/flags:jemalloc_cprefix",
        "//settings/flags:enable_fill",
        "//settings/flags:enable_zone_allocator",
        "//settings/platform:glibc_overrides_support",
        "//settings/platform:lg_page",
        "//settings/platform:malloc_size_support",
        "//settings/platform:memalign_support",
        "//settings/platform:usable_size_const",
        "//settings/platform:valloc_support",
    ],
)

def _transition_default_constraints_impl(ctx):
    target = ctx.attr.src[0]
    providers = [DefaultInfo, CcInfo, InstrumentedFilesInfo, OutputGroupInfo]

    return [target[provider] for provider in providers if provider in target]

transition_default_constraints = rule(
    doc = "Configure detects target platform constraints. The target platform should declare these, but " +
          "intelligent defaults can be set for the majority of cases.",
    implementation = _transition_default_constraints_impl,
    attrs = {
        "src": attr.label(cfg = _platform_constraints_transition),
        "platform": attr.string(
            doc = "Hack to inject platform constraints into the transition to apply unset defaults",
            mandatory = True,
            values = [
                "freebsd",
                "linux",
                "darwin",
                "windows",
                "unknown",
            ],
        ),
        "settings": attr.label_keyed_string_dict(
            doc = "Settings to explicitly transition",
            default = {},
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
