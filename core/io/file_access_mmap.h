/**************************************************************************/
/*  file_access_mmap.h                                                    */
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
