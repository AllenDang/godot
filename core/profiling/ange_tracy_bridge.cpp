/**************************************************************************/
/*  ange_tracy_bridge.cpp                                                 */
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

// Installed into Godot core/profiling by tools/build_server.py. The only
// profiler implementation lives in the engine's existing TracyClient.cpp.
#include "core/profiling/ange_tracy_abi.h"
#include "core/profiling/profiling.h"

// Hosts whose extension side looks the entry point up in the main executable:
// macOS arm64 (dlsym RTLD_MAIN_ONLY) and Windows x86_64 (GetProcAddress on
// GetModuleHandle(nullptr)).
#if (defined(__APPLE__) && defined(__aarch64__)) || (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)))
#define ANGE_TRACY_HOST_PLATFORM 1
#endif

#if defined(GODOT_USE_TRACY) && defined(TRACY_ON_DEMAND) && defined(ANGE_TRACY_HOST_PLATFORM)
#include <common/TracyVersion.hpp>

#include <new>

static_assert(sizeof(tracy::ScopedZone) <= ANGE_TRACY_STORAGE_SIZE);
static_assert(alignof(tracy::ScopedZone) <= ANGE_TRACY_STORAGE_ALIGNMENT);

static uint32_t ange_tracy_connected() {
	return tracy::GetProfiler().IsConnected() ? 1 : 0;
}

static uint32_t ange_tracy_begin(AngeTracyZoneStorage *storage, uint32_t line,
		const char *file, uint64_t file_size, const char *function, uint64_t function_size,
		const char *name, uint64_t name_size) {
	if (!storage || reinterpret_cast<uintptr_t>(storage) % ANGE_TRACY_STORAGE_ALIGNMENT ||
			!file || !function || (!name && name_size) ||
			!ange_tracy_source_fits(file_size, function_size, name_size)) {
		return 0;
	}
	// Native dynamic-source constructor copies strings and owns the on-demand
	// connection generation. Do not replace it with cached plugin source pointers.
	new (storage->bytes) tracy::ScopedZone(line, file, file_size, function, function_size,
			name, name_size, uint32_t(0), -1, true);
	return 1;
}

static void ange_tracy_end(AngeTracyZoneStorage *storage) {
	std::launder(reinterpret_cast<tracy::ScopedZone *>(storage->bytes))->~ScopedZone();
}
#endif

#if defined(_WIN32)
// Puts the symbol in the executable's export table for GetProcAddress. The
// linker keeps this object alive via /INCLUDE (core/profiling/SCsub).
#define ANGE_TRACY_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define ANGE_TRACY_EXPORT __attribute__((visibility("default")))
#else
#define ANGE_TRACY_EXPORT
#endif

extern "C" ANGE_TRACY_EXPORT const AngeTracyHostApiV1 *ange_tracy_get_api_v1() {
#if defined(GODOT_USE_TRACY) && defined(TRACY_ON_DEMAND) && defined(ANGE_TRACY_HOST_PLATFORM)
	static const AngeTracyHostApiV1 api = {
		ANGE_TRACY_ABI_VERSION,
		sizeof(AngeTracyHostApiV1),
		ANGE_TRACY_STORAGE_SIZE,
		ANGE_TRACY_STORAGE_ALIGNMENT,
		(tracy::Version::Major << 16) | (tracy::Version::Minor << 8) | tracy::Version::Patch,
		ANGE_TRACY_ON_DEMAND,
		ange_tracy_connected,
		ange_tracy_begin,
		ange_tracy_end,
	};
	return &api;
#else
	return nullptr;
#endif
}
