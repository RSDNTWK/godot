def can_build(env, platform):
    dependencies = ["ogg"]
    if env["module_ffmpeg_enabled"]:
        dependencies.append("ffmpeg")
    env.module_add_dependencies("vorbis", dependencies)
    return True


def configure(env):
    pass


def get_doc_classes():
    return [
        "AudioStreamOggVorbis",
        "AudioStreamPlaybackOggVorbis",
        "ResourceImporterOggVorbis",
    ]


def get_doc_path():
    return "doc_classes"
