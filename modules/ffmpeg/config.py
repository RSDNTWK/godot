def can_build(env, platform):
    # FFmpeg is configured per target platform by the module build script.
    return not env["arch"].startswith("rv")


def get_opts(platform):
    from SCons.Variables import PathVariable

    return [
        PathVariable(
            "ffmpeg_dir",
            "Root of the prebuilt LGPL FFmpeg target directory",
            "",
            PathVariable.PathAccept,
        )
    ]


def configure(env):
    env.module_add_dependencies("ffmpeg", ["ogg"])
    if env["platform"] in ["windows", "linuxbsd", "android"] and env["threads"]:
        env.module_add_dependencies("ffmpeg", ["media_stream"])
        env.module_add_dependencies("ffmpeg", ["youtube"], optional=True)


def get_doc_classes():
    return [
        "VideoStreamMedia",
        "AudioStreamMedia",
        "AudioStreamPCMDecoder",
        "MediaStreamPlayer",
    ]


def get_doc_path():
    return "doc_classes"
