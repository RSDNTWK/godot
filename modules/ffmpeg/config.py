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


def get_doc_classes():
    return [
        "VideoStreamMedia",
        "AudioStreamMedia",
    ]


def get_doc_path():
    return "doc_classes"
