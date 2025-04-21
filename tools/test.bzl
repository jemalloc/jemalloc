"Various test related utility functions"

def join_conf(conf1, conf2):
    """Joins two configuration strings with a ',' delimiter.

    Args:
        conf1: First configuration string or None
        conf2: Second configuration string or None

    Returns:
        A properly joined configuration string without dangling commas.
    """

    if conf1 and conf2:
        return "%s,%s" % (conf1, conf2)
    elif not conf1:
        return conf2
    elif not conf2:
        return conf1
    else:
        return ""

def test_name(data, suffix = None):
    """Generates a test name based on the source file name.

    Args:
        data: Dict with test data containing 'src' and optional 'name' key
        suffix: String suffix to remove from the source file name

    Returns:
        A string representing the test name.
    """
    name = data.get("name", data["src"].lower().removesuffix(".c").removesuffix(".cpp").replace("/", "_"))
    if suffix:
        return "%s_%s" % (name, suffix)
    else:
        return name
