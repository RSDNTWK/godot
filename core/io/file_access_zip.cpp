/**************************************************************************/
/*  file_access_zip.cpp                                                   */
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

#ifdef MINIZIP_ENABLED

#include "file_access_zip.h"

#include "core/io/file_access.h"
#include "core/io/zip_io.h"

extern "C" {

struct ZipData {
	Ref<FileAccess> f;
};

static void *godot_open(voidpf opaque, const char *p_fname, int mode) {
	if (mode & ZLIB_FILEFUNC_MODE_WRITE) {
		return nullptr;
	}

	Ref<FileAccess> f = FileAccess::open(String::utf8(p_fname), FileAccess::READ);
	ERR_FAIL_COND_V(f.is_null(), nullptr);

	ZipData *zd = memnew(ZipData);
	zd->f = f;
	return zd;
}

static uLong godot_read(voidpf opaque, voidpf stream, void *buf, uLong size) {
	ZipData *zd = (ZipData *)stream;
	const uint64_t read = zd->f->get_buffer((uint8_t *)buf, size);
	return read <= size ? read : 0;
}

static uLong godot_write(voidpf opaque, voidpf stream, const void *buf, uLong size) {
	return 0;
}

static long godot_tell(voidpf opaque, voidpf stream) {
	ZipData *zd = (ZipData *)stream;
	return zd->f->get_position();
}

static long godot_seek(voidpf opaque, voidpf stream, uLong offset, int origin) {
	ZipData *zd = (ZipData *)stream;

	uint64_t pos = offset;
	switch (origin) {
		case ZLIB_FILEFUNC_SEEK_CUR:
			pos = zd->f->get_position() + offset;
			break;
		case ZLIB_FILEFUNC_SEEK_END:
			pos = zd->f->get_length() + offset;
			break;
		default:
			break;
	}

	zd->f->seek(pos);
	return 0;
}

static int godot_close(voidpf opaque, voidpf stream) {
	ZipData *zd = (ZipData *)stream;
	memdelete(zd);
	return 0;
}

static int godot_testerror(voidpf opaque, voidpf stream) {
	ZipData *zd = (ZipData *)stream;
	return zd->f->get_error() != OK ? 1 : 0;
}

static voidpf godot_alloc(voidpf opaque, uInt items, uInt size) {
	return memalloc((size_t)items * size);
}

static void godot_free(voidpf opaque, voidpf address) {
	memfree(address);
}
} // extern "C"

void ZipArchive::close_handle(unzFile p_file) const {
	ERR_FAIL_NULL_MSG(p_file, "Cannot close a file if none is open.");
	unzCloseCurrentFile(p_file);
	unzClose(p_file);
}

unzFile ZipArchive::get_file_handle(const String &p_file) const {
	ERR_FAIL_COND_V_MSG(!file_exists(p_file), nullptr, vformat("File '%s' doesn't exist.", p_file));
	File file = files[p_file];

	zlib_filefunc_def io;
	memset(&io, 0, sizeof(io));

	io.opaque = nullptr;
	io.zopen_file = godot_open;
	io.zread_file = godot_read;
	io.zwrite_file = godot_write;

	io.ztell_file = godot_tell;
	io.zseek_file = godot_seek;
	io.zclose_file = godot_close;
	io.zerror_file = godot_testerror;

	io.alloc_mem = godot_alloc;
	io.free_mem = godot_free;

	unzFile pkg = unzOpen2(packages[file.package].filename.utf8().get_data(), &io);
	ERR_FAIL_NULL_V_MSG(pkg, nullptr, vformat("Cannot open file '%s'.", packages[file.package].filename));
	int unz_err = unzGoToFilePos(pkg, &file.file_pos);
	if (unz_err != UNZ_OK || unzOpenCurrentFile(pkg) != UNZ_OK) {
		unzClose(pkg);
		ERR_FAIL_V(nullptr);
	}

	return pkg;
}

bool ZipArchive::try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key) {
	// load with offset feature only supported for PCK files
	ERR_FAIL_COND_V_MSG(p_offset != 0, false, "Invalid PCK data. Note that loading files with a non-zero offset isn't supported with ZIP archives.");

	if (p_path.get_extension().nocasecmp_to("zip") != 0 && p_path.get_extension().nocasecmp_to("pcz") != 0) {
		return false;
	}

	zlib_filefunc_def io;
	memset(&io, 0, sizeof(io));

	io.opaque = nullptr;
	io.zopen_file = godot_open;
	io.zread_file = godot_read;
	io.zwrite_file = godot_write;

	io.ztell_file = godot_tell;
	io.zseek_file = godot_seek;
	io.zclose_file = godot_close;
	io.zerror_file = godot_testerror;

	unzFile zfile = unzOpen2(p_path.utf8().get_data(), &io);
	ERR_FAIL_NULL_V(zfile, false);

	unz_global_info64 gi;
	int err = unzGetGlobalInfo64(zfile, &gi);
	if (err != UNZ_OK) {
		unzClose(zfile);
		ERR_FAIL_V(false);
	}

	struct PendingFile {
		String name;
		File file;
	};
	Vector<PendingFile> pending_files;
	for (uint64_t i = 0; i < gi.number_entry; i++) {
		unz_file_info64 file_info;
		PendingFile pending;
		err = godot_unzip_get_current_file_info(zfile, file_info, pending.name);
		if (err == UNZ_OK) {
			err = unzGetFilePos(zfile, &pending.file.file_pos);
		}
		if (err != UNZ_OK) {
			unzClose(zfile);
			ERR_FAIL_V(false);
		}
		pending.name = String("res://") + pending.name;
		pending_files.push_back(pending);
		if ((i + 1) < gi.number_entry && unzGoToNextFile(zfile) != UNZ_OK) {
			unzClose(zfile);
			ERR_FAIL_V(false);
		}
	}
	unzClose(zfile);

	Package pkg;
	pkg.filename = p_path;
	packages.push_back(pkg);
	const int pkg_num = packages.size() - 1;
	for (PendingFile &pending : pending_files) {
		pending.file.package = pkg_num;
		files[pending.name] = pending.file;
		uint8_t md5[16] = {};
		PackedData::get_singleton()->add_path(p_path, pending.name, 1, 0, 0, md5, this, p_replace_files, false);
	}

	return true;
}

bool ZipArchive::file_exists(const String &p_name) const {
	return files.has(p_name);
}

Ref<FileAccess> ZipArchive::get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_decryption_key, const Vector<PackedData::PackedFile> *p_delta_snapshot) {
	return memnew(FileAccessZip(p_path, *p_file));
}

ZipArchive *ZipArchive::get_singleton() {
	if (instance == nullptr) {
		instance = memnew(ZipArchive);
	}

	return instance;
}

ZipArchive::ZipArchive() {
	instance = this;
}

ZipArchive::~ZipArchive() {
	packages.clear();
}

Error FileAccessZip::open_internal(const String &p_path, int p_mode_flags) {
	_close();

	ERR_FAIL_COND_V(p_mode_flags & FileAccess::WRITE, FAILED);
	ZipArchive *arch = ZipArchive::get_singleton();
	ERR_FAIL_NULL_V(arch, FAILED);
	zfile = arch->get_file_handle(p_path);
	ERR_FAIL_NULL_V(zfile, FAILED);

	int err = unzGetCurrentFileInfo64(zfile, &file_info, nullptr, 0, nullptr, 0, nullptr, 0);
	if (err != UNZ_OK) {
		_close();
		return FAILED;
	}
	last_error = OK;

	return OK;
}

void FileAccessZip::_close() {
	if (!zfile) {
		return;
	}

	ZipArchive *arch = ZipArchive::get_singleton();
	ERR_FAIL_NULL(arch);
	arch->close_handle(zfile);
	zfile = nullptr;
	at_eof = false;
	last_error = OK;
}

bool FileAccessZip::is_open() const {
	return zfile != nullptr;
}

void FileAccessZip::seek(uint64_t p_position) {
	ERR_FAIL_NULL(zfile);

	const int err = unzSeekCurrentFile(zfile, p_position);
	last_error = err >= 0 ? OK : ERR_FILE_CANT_READ;
	if (last_error == OK) {
		at_eof = false;
	}
}

void FileAccessZip::seek_end(int64_t p_position) {
	ERR_FAIL_NULL(zfile);
	seek(get_length() + p_position);
}

uint64_t FileAccessZip::get_position() const {
	ERR_FAIL_NULL_V(zfile, 0);
	return unztell64(zfile);
}

uint64_t FileAccessZip::get_length() const {
	ERR_FAIL_NULL_V(zfile, 0);
	return file_info.uncompressed_size;
}

bool FileAccessZip::eof_reached() const {
	ERR_FAIL_NULL_V(zfile, true);

	return at_eof;
}

uint64_t FileAccessZip::get_buffer(uint8_t *p_dst, uint64_t p_length) const {
	ERR_FAIL_COND_V(!p_dst && p_length > 0, 0);
	ERR_FAIL_NULL_V(zfile, 0);
	if (last_error != OK || p_length == 0 || at_eof) {
		return 0;
	}

	uint64_t total = 0;
	while (total < p_length) {
		const unsigned int requested = MIN(p_length - total, uint64_t(INT32_MAX));
		const uint64_t before = unztell64(zfile);
		const int read = unzReadCurrentFile(zfile, p_dst + total, requested);
		if (read < 0) {
			const uint64_t after = unztell64(zfile);
			if (after >= before && after - before <= requested) {
				total += after - before;
			}
			last_error = ERR_FILE_CANT_READ;
			break;
		}
		total += read;
		if (static_cast<unsigned int>(read) < requested) {
			if (unztell64(zfile) < file_info.uncompressed_size) {
				last_error = ERR_FILE_CANT_READ;
			} else {
				at_eof = true;
			}
			break;
		}
	}
	return total;
}

Error FileAccessZip::get_error() const {
	if (!zfile) {
		return ERR_UNCONFIGURED;
	}
	if (last_error != OK) {
		return last_error;
	}
	if (eof_reached()) {
		return ERR_FILE_EOF;
	}

	return OK;
}

void FileAccessZip::flush() {
	ERR_FAIL();
}

bool FileAccessZip::store_buffer(const uint8_t *p_src, uint64_t p_length) {
	ERR_FAIL_V(false);
}

bool FileAccessZip::file_exists(const String &p_name) {
	return false;
}

void FileAccessZip::close() {
	_close();
}

FileAccessZip::FileAccessZip(const String &p_path, const PackedData::PackedFile &p_file) {
	open_internal(p_path, FileAccess::READ);
}

FileAccessZip::~FileAccessZip() {
	_close();
}

#endif // MINIZIP_ENABLED
