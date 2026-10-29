/**************************************************************************/
/*  resource_importer_webp_texture.cpp                                    */
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

#include "resource_importer_webp_texture.h"

#include "core/io/config_file.h"
#include "core/io/dir_access.h"

#include "modules/webp/texture_loader_webp.h"

String ResourceImporterWebPTexture::get_importer_name() const {
	return "webp_texture";
}

String ResourceImporterWebPTexture::get_visible_name() const {
	return "Animated WebP Texture2D";
}

void ResourceImporterWebPTexture::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("webp");
}

bool ResourceImporterWebPTexture::can_import(const String &p_path) const {
	uint32_t frame_count = 0;
	if (ResourceFormatWebP::get_webp_frame_count(p_path, frame_count) != OK) {
		return false;
	}

	return frame_count > 1;
}

String ResourceImporterWebPTexture::get_save_extension() const {
	return "webp";
}

String ResourceImporterWebPTexture::get_resource_type() const {
	return "Texture2D";
}

float ResourceImporterWebPTexture::get_priority() const {
	return 2.0;
}

int ResourceImporterWebPTexture::get_format_version() const {
	return 3;
}

String ResourceImporterWebPTexture::get_import_settings_string() const {
	// Invalidate cached import settings when the animation's stored format changes.
	return itos(get_format_version());
}

void ResourceImporterWebPTexture::get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset) const {
	// Keep the saved Texture options dormant while the source is animated, so
	// switching back to a static texture does not reconstruct them from defaults.
	Ref<ConfigFile> config;
	config.instantiate();
	if (config->load(p_path + ".import") != OK || !config->has_section("params")) {
		return;
	}
	Ref<ResourceImporter> texture = ResourceFormatImporter::get_singleton()->get_importer_by_name("texture");
	if (texture.is_null()) {
		return;
	}
	List<ImportOption> texture_options;
	texture->get_import_options(p_path, &texture_options);
	for (const ImportOption &option : texture_options) {
		if (config->has_section_key("params", option.option.name)) {
			ImportOption dormant = option;
			dormant.option.usage = PROPERTY_USAGE_STORAGE;
			dormant.default_value = config->get_value("params", option.option.name);
			r_options->push_back(dormant);
		}
	}
}

bool ResourceImporterWebPTexture::get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const {
	return false;
}

Error ResourceImporterWebPTexture::import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files, Variant *r_metadata) {
	bool animated = false;
	const Error err = ResourceFormatWebP::validate_texture_source(p_source_file, animated);
	ERR_FAIL_COND_V_MSG(err != OK, err, vformat("Failed to import WebP texture from '%s'.", p_source_file));

	if (r_metadata) {
		Dictionary meta;
		meta["animated_webp"] = animated;
		meta["format_version"] = get_format_version();
		*r_metadata = meta;
	}

	// The runtime WebP loader creates the same AnimatedTexture directly. Retain the
	// original compressed animation instead of serializing every decoded RGBA frame.
	return DirAccess::copy_absolute(p_source_file, p_save_path + ".webp");
}

bool ResourceImporterWebPTexture::are_import_settings_valid(const String &p_path, const Dictionary &p_meta) const {
	if (!p_meta.has("animated_webp") || int(p_meta.get("format_version", 0)) != get_format_version()) {
		return false;
	}

	bool is_animated = false;
	if (ResourceFormatWebP::is_animated_webp(p_path, is_animated) != OK) {
		return false;
	}

	return is_animated == bool(p_meta["animated_webp"]);
}
