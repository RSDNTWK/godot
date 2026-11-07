def can_build(env, platform):
    return platform in ["windows", "linuxbsd", "android"] and env["threads"]


def configure(env):
    env.module_add_dependencies("media_stream", ["mbedtls"])
