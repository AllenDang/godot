/**************************************************************************/
/*  file_access_mmap.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "file_access_mmap.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/file_access_pack.h"

#if defined(UNIX_ENABLED) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(WINDOWS_ENABLED)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

void FileAccessMMapView::close() {
	if (is_mapped) {
#if defined(UNIX_ENABLED) || defined(__APPLE__)
		if (data_ptr && data_size > 0) {
			munmap(const_cast<uint8_t *>(data_ptr), data_size);
		}
#elif defined(WINDOWS_ENABLED)
		if (data_ptr) {
			UnmapViewOfFile(data_ptr);
		}
		if (win_map_handle) {
			CloseHandle(static_cast<HANDLE>(win_map_handle));
			win_map_handle = nullptr;
		}
		if (win_file_handle && win_file_handle != INVALID_HANDLE_VALUE) {
			CloseHandle(static_cast<HANDLE>(win_file_handle));
			win_file_handle = nullptr;
		}
#endif
		is_mapped = false;
	}
	fallback_buffer.clear();
	data_ptr = nullptr;
	data_size = 0;
}

Error FileAccessMMapView::open(const String &p_path) {
	close();

	// Resolve res:// / user:// to OS path when not shadowed by a PackedData (.pck) archive.
	bool in_pack = PackedData::get_singleton() && !PackedData::get_singleton()->is_disabled() &&
			PackedData::get_singleton()->has_path(p_path);

	if (!in_pack) {
		String os_path = p_path;
		if (p_path.begins_with("res://") || p_path.begins_with("user://")) {
			if (ProjectSettings::get_singleton()) {
				os_path = ProjectSettings::get_singleton()->globalize_path(p_path);
			}
		}

#if defined(UNIX_ENABLED) || defined(__APPLE__)
		if (os_path.is_absolute_path()) {
			const CharString utf8 = os_path.utf8();
			const int fd = ::open(utf8.get_data(), O_RDONLY | O_CLOEXEC);
			if (fd >= 0) {
				struct stat st;
				if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
					const size_t sz = static_cast<size_t>(st.st_size);
					void *mapped = ::mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
					::close(fd);
					if (mapped != MAP_FAILED) {
#if defined(POSIX_MADV_SEQUENTIAL)
						posix_madvise(mapped, sz, POSIX_MADV_SEQUENTIAL);
#elif defined(MADV_SEQUENTIAL)
						madvise(mapped, sz, MADV_SEQUENTIAL);
#endif
						data_ptr = static_cast<const uint8_t *>(mapped);
						data_size = sz;
						is_mapped = true;
						return OK;
					}
				} else {
					const bool empty_reg = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0);
					::close(fd);
					if (empty_reg) {
						static const uint8_t kEmptyByte = 0;
						data_ptr = &kEmptyByte;
						data_size = 0;
						return OK;
					}
				}
			}
		}
#elif defined(WINDOWS_ENABLED)
		if (os_path.is_absolute_path()) {
			const Char16String utf16 = os_path.utf16();
			HANDLE fh = CreateFileW(reinterpret_cast<LPCWSTR>(utf16.get_data()), GENERIC_READ,
					FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (fh != INVALID_HANDLE_VALUE) {
				LARGE_INTEGER li_size;
				if (GetFileSizeEx(fh, &li_size) && li_size.QuadPart > 0) {
					HANDLE mh = CreateFileMappingW(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
					if (mh != nullptr) {
						const void *view = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
						if (view != nullptr) {
							win_file_handle = fh;
							win_map_handle = mh;
							data_ptr = static_cast<const uint8_t *>(view);
							data_size = static_cast<size_t>(li_size.QuadPart);
							is_mapped = true;
							return OK;
						}
						CloseHandle(mh);
					}
				} else if (GetFileSizeEx(fh, &li_size) && li_size.QuadPart == 0) {
					CloseHandle(fh);
					static const uint8_t kEmptyByte = 0;
					data_ptr = &kEmptyByte;
					data_size = 0;
					return OK;
				}
				CloseHandle(fh);
			}
		}
#endif
	}

	Error err = OK;
	Ref<FileAccess> fa = FileAccess::open(p_path, FileAccess::READ, &err);
	if (fa.is_null()) {
		return err != OK ? err : ERR_FILE_CANT_OPEN;
	}
	const uint64_t len = fa->get_length();
	if (len == 0) {
		static const uint8_t kEmptyByte = 0;
		data_ptr = &kEmptyByte;
		data_size = 0;
		return OK;
	}
	if (fallback_buffer.resize(static_cast<int64_t>(len)) != OK) {
		return ERR_OUT_OF_MEMORY;
	}
	const uint64_t read_len = fa->get_buffer(fallback_buffer.ptrw(), len);
	if (read_len != len) {
		fallback_buffer.clear();
		return ERR_FILE_CORRUPT;
	}
	data_ptr = fallback_buffer.ptr();
	data_size = static_cast<size_t>(len);
	return OK;
}
