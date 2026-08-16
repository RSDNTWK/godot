def can_build(env, platform):
    return True


def get_opts(platform):
    from SCons.Variables import BoolVariable

    return [
        BoolVariable("mp3_extra_formats", "Build mp3 module with MP1/MP2 decoding support", False),
    ]


def configure(env):
    env.module_add_dependencies("mp3", ["ffmpeg"])


def get_doc_classes():
    return [
        "AudioStreamMP3",
        "ResourceImporterMP3",
    ]


def get_doc_path():
    return "doc_classes"
