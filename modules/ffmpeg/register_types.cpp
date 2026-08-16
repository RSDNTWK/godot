#include "register_types.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "video_stream_media.h"

static Ref<ResourceFormatLoaderMedia> resource_loader_media;

void initialize_ffmpeg_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        resource_loader_media.instantiate();
        ResourceLoader::add_resource_format_loader(resource_loader_media, true);
        GDREGISTER_CLASS(VideoStreamMedia);
        GDREGISTER_CLASS(AudioStreamMedia);
    }
}

void uninitialize_ffmpeg_module(ModuleInitializationLevel p_level) {
    if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        ResourceLoader::remove_resource_format_loader(resource_loader_media);
        resource_loader_media.unref();
    }
}
