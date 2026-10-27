def can_build(env, platform):
    # Declare dependencies before SConstruct checks them. Registering a disabled
    # FFmpeg dependency in configure() can leave module ordering waiting forever.
    dependencies = ["opus"]
    if env["module_ffmpeg_enabled"]:
        dependencies.append("ffmpeg")
    else:
        dependencies += ["ogg", "vorbis"]
    env.module_add_dependencies("webm", dependencies)
    if env["arch"].startswith("rv"):
        return False
    if platform == "iphone":
        return False
    # Can work in theory but our libvpx/SCsub is too broken to compile NEON .s
    # files properly on Linux arm32. Could be fixed by someone motivated.
    if platform in ["x11", "server"] and env["arch"] in ["arm", "arm32"]:
        return False
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "VideoStreamWebm",
    ]


def get_doc_path():
    return "doc_classes"
