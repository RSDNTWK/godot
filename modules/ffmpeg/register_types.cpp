#include "register_types.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "video_stream_media.h"
#include "modules/modules_enabled.gen.h"
#ifdef MODULE_MEDIA_STREAM_ENABLED
#include "ffmpeg_async_playback.h"
#endif
#if defined(MODULE_MEDIA_STREAM_ENABLED) || defined(WEB_ENABLED)
#include "audio_stream_pcm_decoder.h"
#include "media_stream_player.h"
#endif

static Ref<ResourceFormatLoaderMedia> resource_loader_media;

void initialize_ffmpeg_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        resource_loader_media.instantiate();
        ResourceLoader::add_resource_format_loader(resource_loader_media, true);
        GDREGISTER_CLASS(VideoStreamMedia);
        GDREGISTER_CLASS(AudioStreamMedia);
#ifdef MODULE_MEDIA_STREAM_ENABLED
        register_ffmpeg_async_playback_types();
#endif
#if defined(MODULE_MEDIA_STREAM_ENABLED) || defined(WEB_ENABLED)
        GDREGISTER_CLASS(AudioStreamPCMDecoder);
        GDREGISTER_CLASS(MediaStreamPlayer);
#endif
    }
}

void uninitialize_ffmpeg_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        ResourceLoader::remove_resource_format_loader(resource_loader_media);
        resource_loader_media.unref();
    }
}
