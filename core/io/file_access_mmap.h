/**************************************************************************/
/*  file_access_mmap.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"

// Zero-copy read-only file mapping view for local OS files (POSIX mmap /
// Win32 CreateFileMappingW), with automatic fallback to FileAccess buffer
// reading for packed (.pck) or virtual paths.
class FileAccessMMapView {
	const uint8_t *data_ptr = nullptr;
	size_t data_size = 0;
	bool is_mapped = false;
#if defined(WINDOWS_ENABLED)
	void *win_file_handle = nullptr;
	void *win_map_handle = nullptr;
#endif
	Vector<uint8_t> fallback_buffer;

	FileAccessMMapView(const FileAccessMMapView &) = delete;
	FileAccessMMapView &operator=(const FileAccessMMapView &) = delete;

public:
	FileAccessMMapView() = default;
	~FileAccessMMapView() { close(); }

	Error open(const String &p_path);
	void close();

	bool is_valid() const { return data_ptr != nullptr; }
	bool is_zero_copy_mapped() const { return is_mapped; }
	const uint8_t *ptr() const { return data_ptr; }
	size_t size() const { return data_size; }
};
