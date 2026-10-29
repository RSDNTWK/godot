/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "register_types.h"

#include "video_stream_theora.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"

#include "modules/modules_enabled.gen.h"

#ifdef TOOLS_ENABLED
#include "editor/movie_writer_ogv.h"
#ifdef MODULE_FFMPEG_ENABLED
#include "modules/ffmpeg/movie_writer_ogv_ffmpeg.h"
#endif
#endif

static Ref<ResourceFormatLoaderTheora> resource_loader_theora;
#ifdef TOOLS_ENABLED
static MovieWriter *writer_ogv = nullptr;
#endif

void initialize_theora_module(ModuleInitializationLevel p_level) {
	switch (p_level) {
		case MODULE_INITIALIZATION_LEVEL_SERVERS: {
#ifdef TOOLS_ENABLED
#ifdef MODULE_FFMPEG_ENABLED
			if constexpr (GD_IS_CLASS_ENABLED(MovieWriterOGVFFmpeg)) {
				if (MovieWriterOGVFFmpeg::is_available()) {
					writer_ogv = memnew(MovieWriterOGVFFmpeg);
				}
			}
#endif
			if (!writer_ogv) {
				if constexpr (GD_IS_CLASS_ENABLED(MovieWriterOGV)) {
					writer_ogv = memnew(MovieWriterOGV);
				}
			}
			if (writer_ogv) {
				MovieWriter::add_writer(writer_ogv);
			}
#endif
		} break;
		case MODULE_INITIALIZATION_LEVEL_SCENE: {
			resource_loader_theora.instantiate();
#ifndef MODULE_FFMPEG_ENABLED
			ResourceLoader::add_resource_format_loader(resource_loader_theora, true);
#endif
			GDREGISTER_CLASS(VideoStreamTheora);
		} break;
		default:
			break;
	}
}
void uninitialize_theora_module(ModuleInitializationLevel p_level) {
	switch (p_level) {
		case MODULE_INITIALIZATION_LEVEL_SCENE: {
#ifndef MODULE_FFMPEG_ENABLED
			ResourceLoader::remove_resource_format_loader(resource_loader_theora);
#endif
			resource_loader_theora.unref();
		} break;
		case MODULE_INITIALIZATION_LEVEL_SERVERS: {
#ifdef TOOLS_ENABLED
			if (writer_ogv) {
				memdelete(writer_ogv);
				writer_ogv = nullptr;
			}
#endif
		} break;
		default:
			break;
	}
}
