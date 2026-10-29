#include "tests/test_macros.h"
TEST_FORCE_LINK(test_importer_migration)

#include "modules/modules_enabled.gen.h"

#ifdef MODULE_WEBP_ENABLED

#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/resource_importer.h"
#include "core/object/class_db.h"
#include "editor/import/resource_importer_webp_texture.h"
#include "tests/core/io/webp_test_helpers.h"

namespace TestImporterMigration {

struct Access {
	static void restore(ResourceFormatImporter *p_previous) {
		ResourceFormatImporter::singleton = p_previous;
	}
};

struct Registry {
	ResourceFormatImporter *previous = ResourceFormatImporter::get_singleton();
	Ref<ResourceFormatImporter> format = memnew(ResourceFormatImporter);
	~Registry() {
		Access::restore(previous);
	}
};

class Importer : public ResourceImporter {
public:
	String name;
	float priority = 1.0;
	bool accepts = true;
	bool settings_valid = true;
	bool threaded = true;
	int order = 17;

	explicit Importer(const String &p_name) : name(p_name) {}
	String get_importer_name() const override { return name; }
	String get_visible_name() const override { return name; }
	void get_recognized_extensions(List<String> *p_extensions) const override {
		p_extensions->push_back("webp");
		p_extensions->push_back("png");
	}
	bool can_import(const String &p_path) const override { return accepts; }
	String get_save_extension() const override { return "ctex"; }
	String get_resource_type() const override { return name == "image" ? "Image" : "CompressedTexture2D"; }
	float get_priority() const override { return priority; }
	int get_import_order() const override { return order; }
	bool can_import_threaded() const override { return threaded; }
	void get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset = 0) const override {
		r_options->push_back(ImportOption(PropertyInfo(Variant::INT, "compress/mode"), 0));
		r_options->push_back(ImportOption(PropertyInfo(Variant::FLOAT, "compress/lossy_quality"), 0.7));
		r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, "mipmaps/generate"), false));
	}
	bool get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const override { return true; }
	Error import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files, Variant *r_metadata) override { return ERR_UNAVAILABLE; }
	bool are_import_settings_valid(const String &p_path, const Dictionary &p_meta) const override { return settings_valid; }
};

static Ref<Importer> add(Registry &p_registry, const String &p_name, float p_priority = 1.0) {
	Ref<Importer> importer = memnew(Importer(p_name));
	importer->priority = p_priority;
	p_registry.format->add_importer(importer);
	return importer;
}

static Ref<ResourceImporterWebPTexture> add_webp(Registry &p_registry) {
	if (!ClassDB::class_exists("ResourceImporterWebPTexture")) {
		GDREGISTER_CLASS(ResourceImporterWebPTexture);
	}
	Ref<ResourceImporterWebPTexture> importer = memnew(ResourceImporterWebPTexture);
	p_registry.format->add_importer(importer);
	return importer;
}

struct Sidecar {
	String path;
	Ref<ConfigFile> config = memnew(ConfigFile);
	ResourceUID::ID uid = 0x123456789;
	Sidecar(const String &p_path, const String &p_importer, bool p_explicit = false, const Dictionary &p_metadata = Dictionary()) : path(p_path) {
		config->set_value("remap", "importer", p_importer);
		config->set_value("remap", "type", p_importer == "image" ? "Image" : "Texture2D");
		config->set_value("remap", "path", p_path + ".ctex");
		config->set_value("remap", "uid", ResourceUID::get_singleton()->id_to_text(uid));
		config->set_value("remap", "metadata", p_metadata);
		config->set_value("remap", "importer_explicit", p_explicit);
		save();
	}
	void save() {
		REQUIRE(config->save(path + ".import") == OK);
	}
	~Sidecar() {
		DirAccess::remove_absolute(path + ".import");
	}
};

TEST_CASE("[F11][ResourceImporter] Saved Image Atlas and plugin choices survive global priorities") {
	Registry registry;
	add(registry, "texture");
	add_webp(registry);
	add(registry, "highest_plugin", 100.0);
	TestWebPSafety::Fixture fixture(TestWebPSafety::make_animation(2));
	for (const String &name : { String("image"), String("texture_atlas"), String("custom_importer") }) {
		Ref<Importer> selected = add(registry, name);
		selected->accepts = false; // Name lookup retains explicit choices even if discovery declines.
		Sidecar sidecar(fixture.path, name);
		CHECK(registry.format->get_importer_for_existing_file(fixture.path, name) == selected);
		CHECK(registry.format->are_import_settings_valid(fixture.path));
		CHECK(registry.format->get_resource_uid(fixture.path) == sidecar.uid);
		CHECK(registry.format->get_resource_type(fixture.path) == (name == "image" ? "Image" : "Texture2D"));
		int order = 0;
		bool threaded = false;
		String scheduled;
		REQUIRE(registry.format->get_import_order_threads_and_importer(fixture.path, order, threaded, scheduled) == OK);
		CHECK(scheduled == name);
		CHECK(order == selected->order);
		CHECK(threaded);
		selected->settings_valid = false;
		CHECK_FALSE(registry.format->are_import_settings_valid(fixture.path));
	}
}

TEST_CASE("[F11][ResourceImporter] Default and missing-importer discovery keep priority and registration ties") {
	Registry registry;
	Ref<Importer> texture = add(registry, "texture");
	Ref<ResourceImporterWebPTexture> webp = add_webp(registry);
	TestWebPSafety::Fixture fixture(TestWebPSafety::make_animation(2));
	CHECK(registry.format->get_importer_by_file(fixture.path) == webp);
	Ref<Importer> plugin = add(registry, "plugin", 3.0);
	CHECK(registry.format->get_importer_by_file(fixture.path) == plugin);
	CHECK(registry.format->get_importer_for_existing_file(fixture.path, "") == plugin);
	CHECK(registry.format->get_importer_for_existing_file(fixture.path, "removed", true) == plugin);
	Ref<Importer> tied = memnew(Importer("first_tie"));
	tied->priority = 3.0;
	registry.format->add_importer(tied, true);
	CHECK(registry.format->get_importer_by_file(fixture.path) == tied);
	CHECK(registry.format->get_importer_for_existing_file("image.PNG", "texture") == texture);
	CHECK(registry.format->get_importer_for_existing_file(fixture.path, "keep").is_null());
	CHECK(registry.format->get_importer_for_existing_file(fixture.path, "skip").is_null());
	Sidecar missing(fixture.path, "removed");
	CHECK_FALSE(registry.format->are_import_settings_valid(fixture.path));
}

TEST_CASE("[F11][ResourceImporter] Automatic WebP transitions use native peers and matching scheduling") {
	Registry registry;
	Ref<Importer> texture = add(registry, "texture");
	Ref<ResourceImporterWebPTexture> webp = add_webp(registry);
	add(registry, "highest_plugin", 100.0);
	TestWebPSafety::Fixture animated(TestWebPSafety::make_animation(2));
	TestWebPSafety::Fixture still(TestWebPSafety::make_static());
	for (int repeat = 0; repeat < 3; repeat++) {
		CHECK(registry.format->get_importer_for_existing_file(animated.path, "texture", false) == webp);
		CHECK(registry.format->get_importer_for_existing_file(still.path, "webp_texture", false) == texture);
	}
	Sidecar sidecar(animated.path, "texture");
	CHECK_FALSE(registry.format->are_import_settings_valid(animated.path));
	int order = -1;
	bool threaded = true;
	String scheduled;
	REQUIRE(registry.format->get_import_order_threads_and_importer(animated.path, order, threaded, scheduled) == OK);
	CHECK(scheduled == "webp_texture");
	CHECK(order == webp->get_import_order());
	CHECK_FALSE(threaded);
	CHECK(registry.format->get_import_order(animated.path) == webp->get_import_order());
	sidecar.config->set_value("remap", "importer", "webp_texture");
	sidecar.path = still.path;
	sidecar.save();
	REQUIRE(registry.format->get_import_order_threads_and_importer(still.path, order, threaded, scheduled) == OK);
	CHECK(scheduled == "texture");
	CHECK(threaded);
	DirAccess::remove_absolute(animated.path + ".import");
}

TEST_CASE("[F11][ResourceImporter] Explicit native selections and keep skip remain stable") {
	Registry registry;
	Ref<Importer> texture = add(registry, "texture");
	Ref<ResourceImporterWebPTexture> webp = add_webp(registry);
	TestWebPSafety::Fixture animated(TestWebPSafety::make_animation(2));
	TestWebPSafety::Fixture still(TestWebPSafety::make_static());
	CHECK(registry.format->get_importer_for_existing_file(animated.path, "texture", true) == texture);
	CHECK(registry.format->get_importer_for_existing_file(still.path, "webp_texture", true) == webp);
	Sidecar pinned(animated.path, "texture", true);
	CHECK(registry.format->are_import_settings_valid(animated.path));
	int order = 0;
	bool threaded = false;
	String scheduled;
	REQUIRE(registry.format->get_import_order_threads_and_importer(animated.path, order, threaded, scheduled) == OK);
	CHECK(scheduled == "texture");
	CHECK(threaded);
	for (const String &name : { String("keep"), String("skip") }) {
		pinned.config->set_value("remap", "importer", name);
		pinned.save();
		CHECK(registry.format->are_import_settings_valid(animated.path));
		CHECK(registry.format->get_import_order_threads_and_importer(animated.path, order, threaded, scheduled) == ERR_INVALID_PARAMETER);
	}
}

TEST_CASE("[F11][ResourceImporter] Legacy sidecars retain native choices without provenance") {
	Registry registry;
	Ref<Importer> texture = add(registry, "texture");
	Ref<ResourceImporterWebPTexture> webp = add_webp(registry);
	for (bool animated : { false, true }) {
		TestWebPSafety::Fixture fixture(animated ? TestWebPSafety::make_animation(2) : TestWebPSafety::make_static());
		Dictionary metadata;
		metadata["animated_webp"] = animated;
		metadata["format_version"] = webp->get_format_version();
		const String name = animated ? "texture" : "webp_texture";
		Sidecar legacy(fixture.path, name, false, metadata);
		legacy.config->erase_section_key("remap", "importer_explicit");
		legacy.save();
		CHECK(registry.format->are_import_settings_valid(fixture.path));
		int order = 0;
		bool threaded = false;
		String scheduled;
		REQUIRE(registry.format->get_import_order_threads_and_importer(fixture.path, order, threaded, scheduled) == OK);
		CHECK(scheduled == name);
		CHECK(threaded == animated);
	}
}

TEST_CASE("[F11][ResourceImporter] In-memory Texture options remain dormant across migration") {
	Registry registry;
	add(registry, "texture");
	Ref<ResourceImporterWebPTexture> webp = add_webp(registry);
	TestWebPSafety::Fixture fixture(TestWebPSafety::make_animation(2));
	Sidecar sidecar(fixture.path, "texture");
	sidecar.config->set_value("params", "compress/mode", 2);
	sidecar.save();
	HashMap<StringName, Variant> values;
	values["compress/mode"] = 2;
	values["compress/lossy_quality"] = 0.23; // Supplied by a custom import, absent on disk.
	values["mipmaps/generate"] = true; // Supplied by project defaults, absent on disk.
	values["unknown/option"] = 123;
	List<ResourceImporter::ImportOption> options;
	webp->get_import_options_for_import(fixture.path, values, &options);
	REQUIRE(options.size() == 3);
	HashSet<StringName> declared;
	for (const ResourceImporter::ImportOption &option : options) {
		CHECK_FALSE(declared.has(option.option.name));
		declared.insert(option.option.name);
		CHECK(option.option.usage == PROPERTY_USAGE_STORAGE);
		sidecar.config->set_value("params", option.option.name, values[option.option.name]);
	}
	sidecar.config->set_value("remap", "importer", "webp_texture");
	sidecar.save();
	List<ResourceImporter::ImportOption> dormant;
	webp->get_import_options(fixture.path, &dormant);
	REQUIRE(dormant.size() == 3);
	for (const ResourceImporter::ImportOption &option : dormant) {
		CHECK(option.default_value == values[option.option.name]);
	}
}

TEST_CASE("[F11][ResourceImporter] Native transitions require the exact extension and registered peers") {
	Registry registry;
	Ref<Importer> texture = add(registry, "texture");
	Ref<Importer> animated = add(registry, "webp_texture", 2.0);
	CHECK(registry.format->get_importer_for_existing_file("picture.WeBp", "texture", false) == animated);
	CHECK(registry.format->get_importer_for_existing_file("picture.png", "texture", false) == texture);
	CHECK(registry.format->get_importer_for_existing_file("picture.notwebp", "texture", false) == texture);
	registry.format->remove_importer(animated);
	CHECK(registry.format->get_importer_for_existing_file("picture.webp", "texture", false) == texture);
	registry.format->add_importer(animated);
	registry.format->remove_importer(texture);
	animated->accepts = false;
	CHECK(registry.format->get_importer_for_existing_file("picture.webp", "webp_texture", false) == animated);
}

TEST_CASE("[F11][ResourceImporter] Dormant Texture options survive repeated animated sidecars") {
	Registry registry;
	add(registry, "texture");
	Ref<ResourceImporterWebPTexture> webp = add_webp(registry);
	TestWebPSafety::Fixture fixture(TestWebPSafety::make_animation(2));
	Sidecar sidecar(fixture.path, "texture");
	sidecar.config->set_value("params", "compress/mode", 2);
	sidecar.config->set_value("params", "compress/lossy_quality", 0.23);
	sidecar.config->set_value("params", "mipmaps/generate", true);
	sidecar.config->set_value("params", "unknown/option", 123);
	sidecar.save();
	for (int repeat = 0; repeat < 3; repeat++) {
		List<ResourceImporter::ImportOption> options;
		webp->get_import_options(fixture.path, &options);
		REQUIRE(options.size() == 3);
		sidecar.config->erase_section("params");
		HashMap<StringName, Variant> values;
		for (const ResourceImporter::ImportOption &option : options) {
			CHECK(option.option.usage == PROPERTY_USAGE_STORAGE);
			CHECK_FALSE(webp->get_option_visibility(fixture.path, option.option.name, values));
			values[option.option.name] = option.default_value;
			sidecar.config->set_value("params", option.option.name, option.default_value);
		}
		CHECK(int(values["compress/mode"]) == 2);
		CHECK(double(values["compress/lossy_quality"]) == doctest::Approx(0.23));
		CHECK(bool(values["mipmaps/generate"]));
		sidecar.config->set_value("remap", "importer", "webp_texture");
		sidecar.save();
		CHECK(registry.format->get_resource_uid(fixture.path) == sidecar.uid);
	}
	TestWebPSafety::Fixture fresh(TestWebPSafety::make_animation(2));
	List<ResourceImporter::ImportOption> options;
	webp->get_import_options(fresh.path, &options);
	CHECK(options.is_empty());
}

TEST_CASE("[SceneTree][F11][ResourceImporter] Actual WebP import retains bytes metadata and resource type") {
	Registry registry;
	add(registry, "texture");
	Ref<ResourceImporterWebPTexture> webp = add_webp(registry);
	for (bool animated : { false, true }) {
		TestWebPSafety::Fixture fixture(animated ? TestWebPSafety::make_animation(2) : TestWebPSafety::make_static());
		const String save_path = fixture.path + ".imported";
		HashMap<StringName, Variant> options;
		List<String> variants;
		List<String> generated;
		Variant metadata;
		REQUIRE(webp->import(123, fixture.path, save_path, options, &variants, &generated, &metadata) == OK);
		CHECK(FileAccess::get_file_as_bytes(fixture.path) == FileAccess::get_file_as_bytes(save_path + ".webp"));
		Dictionary meta = metadata;
		CHECK(bool(meta["animated_webp"]) == animated);
		CHECK(webp->are_import_settings_valid(fixture.path, meta));
		Sidecar sidecar(fixture.path, "webp_texture", true, meta);
		CHECK(registry.format->are_import_settings_valid(fixture.path));
		CHECK(registry.format->get_resource_uid(fixture.path) == sidecar.uid);
		CHECK(registry.format->get_resource_type(fixture.path) == "Texture2D");
		CHECK(webp->get_resource_type() == "Texture2D");
		CHECK(variants.is_empty());
		CHECK(generated.is_empty());
		DirAccess::remove_absolute(save_path + ".webp");
	}
}

} // namespace TestImporterMigration

#endif // MODULE_WEBP_ENABLED
