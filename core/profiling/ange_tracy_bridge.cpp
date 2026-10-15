// Installed into Godot core/profiling by tools/build_server.py. The only
// profiler implementation lives in the engine's existing TracyClient.cpp.
#include "ange_tracy_abi.h"
#include "core/profiling/profiling.h"

// Hosts whose extension side looks the entry point up in the main executable:
// macOS arm64 (dlsym RTLD_MAIN_ONLY) and Windows x86_64 (GetProcAddress on
// GetModuleHandle(nullptr)).
#if (defined(__APPLE__) && defined(__aarch64__)) || (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)))
#define ANGE_TRACY_HOST_PLATFORM 1
#endif

#if defined(GODOT_USE_TRACY) && defined(TRACY_ON_DEMAND) && defined(ANGE_TRACY_HOST_PLATFORM)
#include <new>
#include <common/TracyVersion.hpp>

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

extern "C"
#if defined(_WIN32)
// Puts the symbol in the executable's export table for GetProcAddress. The
// linker keeps this object alive via /INCLUDE (core/profiling/SCsub).
__declspec(dllexport)
#elif defined(__APPLE__)
__attribute__((visibility("default")))
#endif
const AngeTracyHostApiV1 *ange_tracy_get_api_v1() {
#if defined(GODOT_USE_TRACY) && defined(TRACY_ON_DEMAND) && defined(ANGE_TRACY_HOST_PLATFORM)
	static const AngeTracyHostApiV1 api = {
		ANGE_TRACY_ABI_VERSION, sizeof(AngeTracyHostApiV1),
		ANGE_TRACY_STORAGE_SIZE, ANGE_TRACY_STORAGE_ALIGNMENT,
		(tracy::Version::Major << 16) | (tracy::Version::Minor << 8) | tracy::Version::Patch,
		ANGE_TRACY_ON_DEMAND, ange_tracy_connected, ange_tracy_begin, ange_tracy_end,
	};
	return &api;
#else
	return nullptr;
#endif
}
