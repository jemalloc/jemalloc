"Utilities for replicating autoconf behavior"

load("@aspect_bazel_lib//lib:expand_template.bzl", "expand_template")

def define_macro_if(macro, condition):
    """Controls conditional macro definition based on a build condition.

    Args:
        macro: String macro name to define
        condition: Build condition that should enable the macro

    Returns:
        A select dictionary mapping conditions to substitution dictionaries
    """

    return select({
        condition: define_macro(macro),
        "//conditions:default": undefine_macro(macro),
    })

def define_macro_if_with(macro, condition, value):
    """Conditionally defines a macro with a specific value based on a build condition.

    Args:
        macro: String macro name to define
        condition: Build condition that should enable the macro
        value: Value to assign to the macro when defined

    Returns:
        A select dictionary mapping conditions to substitution dictionaries
    """

    return select({
        condition: define_macro_with(macro, value),
        "//conditions:default": undefine_macro(macro),
    })

def define_macro(macro):
    """Unconditionally defines a macro.

    Args:
        macro: String macro name to define

    Returns:
        A substitution dictionary for the macro
    """

    return {"#undef {}".format(macro): "#define {}".format(macro)}

def define_macro_if_any(macro, conditions):
    """Defines a macro if any of the specified conditions are met.

    Args:
        macro: String macro name to define
        conditions: List of build conditions that should individually enable the macro

    Returns:
        A select dictionary mapping conditions to substitution dictionaries
    """

    substitutions = {}
    defined = define_macro(macro)
    for condition in conditions:
        substitutions[condition] = defined

    substitutions["//conditions:default"] = undefine_macro(macro)

    return select(substitutions)

def define_macro_with(macro, value):
    """Unconditionally defines a macro with a specific value.

    Args:
        macro: String macro name to define
        value: Value to assign to the macro

    Returns:
        A substitution dictionary for the macro with value
    """

    return {"#undef {}".format(macro): "#define {} {}".format(macro, value)}

def undefine_macro(macro):
    """Explicitly undefines a macro with a commented annotation.

    Args:
        macro: String macro name to undefine

    Returns:
        A substitution dictionary that replaces the undef with a comment
    """

    return {"#undef {}".format(macro): "/* #UNDEF {} */".format(macro)}

def configure_header(name, substitutions, **kwargs):
    """Generates a header file with autoconf-style macro substitutions.

    Args:
        name: Target name
        substitutions: Dictionary of substitutions to apply
        **kwargs: Additional arguments passed to expand_template

    Returns:
        The processed header file with all substitutions applied
    """

    return expand_template(
        name = name,
        # The expand_template substitutions are applied serially and overlapping macro names can interfere with each other.
        # Explicitly marking that a macro has been processed such that the template string no longer matches prevents this.
        substitutions = substitutions | {"#UNDEF": "#undef"},
        **kwargs
    )
