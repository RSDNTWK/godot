def can_build(env, platform):
    # ToI uses its iframe on Web. The native resolver needs a worker thread.
    return platform in ["windows", "linuxbsd", "android"] and env["threads"]


def configure(env):
    env.module_add_dependencies("youtube", ["mbedtls", "media_stream"])


def get_doc_classes():
    return ["YouTubeResolver"]


def get_doc_path():
    return "doc_classes"
