def can_build(env, platform):
    return platform in ("ios", "macos", "android")


def configure(env):
    pass


def is_enabled():
    # Profiling builds only. Use module_perfci_enabled=yes to enable it.
    return False
