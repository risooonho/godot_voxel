#include "voxel_stream_vxb.h"
#include "../util/utility.h"
#include "file_utils.h"
#include <core/os/dir_access.h>
#include <core/os/file_access.h>

namespace {
const Vector3i DEFAULT_BLOCK_SIZE(16, 16, 16);
const uint8_t FORMAT_VERSION = 1;
const char *FORMAT_META_MAGIC = "VXBM";
const char *FORMAT_BLOCK_MAGIC = "VXB_";
const char *META_FILE_NAME = "meta.vxbm";
const char *BLOCK_FILE_EXTENSION = ".vxb";
} // namespace

VoxelStreamVXB::VoxelStreamVXB() {
	// Defaults
	_meta.block_size = DEFAULT_BLOCK_SIZE;
	_meta.lod_count = 1;
	_meta.version = FORMAT_VERSION;
}

// TODO Have configurable block size

void VoxelStreamVXB::emerge_block(Ref<VoxelBuffer> out_buffer, Vector3i origin_in_voxels, int lod) {

	ERR_FAIL_COND(out_buffer.is_null());

	if (_directory_path.empty()) {
		emerge_block_fallback(out_buffer, origin_in_voxels, lod);
		return;
	}

	if (!_meta.loaded) {
		if (load_meta() != OK) {
			return;
		}
	}

	CRASH_COND(!_meta.loaded);
	ERR_FAIL_COND(lod >= _meta.lod_count);
	ERR_FAIL_COND(_meta.block_size != out_buffer->get_size());

	Vector3i block_pos = get_block_position(origin_in_voxels) >> lod;
	String file_path = get_block_file_path(block_pos, lod);

	FileAccess *f = nullptr;
	{
		Error err;
		f = open_file(file_path, FileAccess::READ, err);
		// Had to add ERR_FILE_CANT_OPEN because that's what Godot actually returns when the file doesn't exist...
		if (f == nullptr && (err == ERR_FILE_NOT_FOUND || err == ERR_FILE_CANT_OPEN)) {
			emerge_block_fallback(out_buffer, origin_in_voxels, lod);
			return;
		}
	}

	ERR_FAIL_COND(f == nullptr);

	{
		{
			uint8_t version;
			Error err = check_magic_and_version(f, FORMAT_VERSION, FORMAT_BLOCK_MAGIC, version);
			ERR_FAIL_COND(err != OK);
		}

		uint32_t size_to_read = f->get_32();
		ERR_FAIL_COND(!_block_serializer.decompress_and_deserialize(f, size_to_read, **out_buffer));
	}

	f->close();
	memdelete(f);
}

void VoxelStreamVXB::immerge_block(Ref<VoxelBuffer> buffer, Vector3i origin_in_voxels, int lod) {

	ERR_FAIL_COND(_directory_path.empty());
	ERR_FAIL_COND(buffer.is_null());

	if (!_meta.saved) {
		Error err = save_meta();
		ERR_FAIL_COND(err != OK);
	}

	Vector3i block_pos = get_block_position(origin_in_voxels) >> lod;
	String file_path = get_block_file_path(block_pos, lod);

	//print_line(String("Saving VXB {0}").format(varray(block_pos.to_vec3())));

	{
		Error err = check_directory_created(file_path.get_base_dir());
		ERR_FAIL_COND(err != OK);
	}

	{
		FileAccess *f = nullptr;
		{
			Error err;
			// Create file if not exists, always truncate
			f = open_file(file_path, FileAccess::WRITE, err);
		}
		ERR_FAIL_COND(f == nullptr);

		f->store_buffer((uint8_t *)FORMAT_BLOCK_MAGIC, 4);
		f->store_8(FORMAT_VERSION);

		const std::vector<uint8_t> &data = _block_serializer.serialize_and_compress(**buffer);
		f->store_32(data.size());
		f->store_buffer(data.data(), data.size());

		f->close();
		memdelete(f);
	}
}

String VoxelStreamVXB::get_directory() const {
	return _directory_path;
}

void VoxelStreamVXB::set_directory(String dirpath) {
	if (_directory_path != dirpath) {
		_directory_path = dirpath;
		_meta.loaded = false;
	}
}

Error VoxelStreamVXB::save_meta() {

	CRASH_COND(_directory_path.empty());

	// Make sure the directory exists
	{
		Error err = check_directory_created(_directory_path);
		if (err != OK) {
			ERR_PRINT("Could not save meta");
			return err;
		}
	}

	String meta_path = _directory_path.plus_file(META_FILE_NAME);

	{
		Error err;
		FileAccess *f = open_file(meta_path, FileAccess::WRITE, err);
		ERR_FAIL_COND_V(f == nullptr, err);

		f->store_buffer((uint8_t *)FORMAT_META_MAGIC, 4);
		f->store_8(FORMAT_VERSION);

		f->store_8(_meta.lod_count);
		store_vec3u32(f, _meta.block_size);

		memdelete(f);
	}

	_meta.loaded = true;
	_meta.saved = true;
	return OK;
}

Error VoxelStreamVXB::load_meta() {
	CRASH_COND(_directory_path.empty());

	String meta_path = _directory_path.plus_file(META_FILE_NAME);

	Meta meta;
	{
		Error err;
		FileAccessRef f = open_file(meta_path, FileAccess::READ, err);
		// Had to add ERR_FILE_CANT_OPEN because that's what Godot actually returns when the file doesn't exist...
		if (!_meta.saved && (err == ERR_FILE_NOT_FOUND || err == ERR_FILE_CANT_OPEN)) {
			// This is a new terrain, save the meta we have and consider it current
			Error save_err = save_meta();
			ERR_FAIL_COND_V(save_err != OK, save_err);
			return OK;
		}
		ERR_FAIL_COND_V(!f, err);

		err = check_magic_and_version(f.f, FORMAT_VERSION, FORMAT_META_MAGIC, meta.version);
		if (err != OK) {
			return err;
		}

		meta.lod_count = f->get_8();
		meta.block_size = get_vec3u32(f);
	}

	meta.loaded = true;
	_meta = meta;
	return OK;
}

String VoxelStreamVXB::get_block_file_path(const Vector3i &block_pos, unsigned int lod) const {
	// TODO This is probably extremely inefficient, also given the nature of Godot strings

	// Save under a folder, because there could be other kinds of data to store in this terrain
	String path = "blocks/lod";
	path += String::num_uint64(lod);
	path += '/';
	for (unsigned int i = 0; i < 3; ++i) {
		if (block_pos[i] >= 0) {
			path += '+';
		}
		path += String::num_int64(block_pos[i]);
	}
	path += BLOCK_FILE_EXTENSION;
	return _directory_path.plus_file(path);
}

Vector3i VoxelStreamVXB::get_block_position(const Vector3i &origin_in_voxels) const {
	return origin_in_voxels.udiv(_meta.block_size);
}

void VoxelStreamVXB::_bind_methods() {

	ClassDB::bind_method(D_METHOD("set_directory", "directory"), &VoxelStreamVXB::set_directory);
	ClassDB::bind_method(D_METHOD("get_directory"), &VoxelStreamVXB::get_directory);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "directory"), "set_directory", "get_directory");
}