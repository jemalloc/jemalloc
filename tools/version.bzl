"Version string utilities."

def parse_version(version):
    """Parses raw version string into a structured version object.

    Returns:
        struct: Contains version components:
            - full: Full version
            - gid: Version git id
            - major: Major version number
            - minor: Minor version number
            - patch: Patch version number
            - bugfix: Bugfix version
            - nrev: Revision number
    """
    prefix, gid = version.split("-bcr", 2)
    major, minor, patch = prefix.split(".", 3)

    return struct(
        full = prefix,
        gid = "0",
        major = major,
        minor = minor,
        patch = patch,
        bugfix = "0",
        nrev = "0",
    )
