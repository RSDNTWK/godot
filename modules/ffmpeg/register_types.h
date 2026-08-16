#ifndef FFMPEG_REGISTER_TYPES_H
#define FFMPEG_REGISTER_TYPES_H

#include "modules/register_module_types.h"

void initialize_ffmpeg_module(ModuleInitializationLevel p_level);
void uninitialize_ffmpeg_module(ModuleInitializationLevel p_level);

#endif
