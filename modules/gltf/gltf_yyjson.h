/**************************************************************************/
/*  gltf_yyjson.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "gltf_defines.h"

#include "core/os/memory.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

#include <yyjson.h>

class GLTFDocument;
class GLTFState;

// RAII arena-pooled yyjson document reader (zero per-node mallocs).
class GLTFYyjsonDoc {
	void *pool_mem = nullptr;
	yyjson_alc alc = {};
	yyjson_doc *doc = nullptr;

	GLTFYyjsonDoc(const GLTFYyjsonDoc &) = delete;
	GLTFYyjsonDoc &operator=(const GLTFYyjsonDoc &) = delete;

public:
	GLTFYyjsonDoc() = default;
	~GLTFYyjsonDoc() { clear(); }

	void clear() {
		if (doc) {
			yyjson_doc_free(doc);
			doc = nullptr;
		}
		if (pool_mem) {
			Memory::free_static(pool_mem);
			pool_mem = nullptr;
		}
	}

	Error parse(const char *p_utf8, size_t p_len, String *r_err_msg = nullptr, int *r_err_line = nullptr);
	bool is_valid() const { return doc != nullptr; }
	yyjson_val *root() const { return doc ? yyjson_doc_get_root(doc) : nullptr; }
};

Variant gltf_yyjson_val_to_variant(yyjson_val *p_val);
Variant gltf_yyjson_parse_to_variant(const char *p_utf8, size_t p_len, Error *r_err = nullptr, String *r_err_msg = nullptr);

// Fast direct yyjson -> GLTFState population (bypasses Variant Dictionary construction
// for bufferViews, accessors, nodes, scenes, meshes, materials, skins, cameras, lights, animations).
Error gltf_yyjson_populate_state(
		const GLTFYyjsonDoc &p_jdoc,
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		const String &p_search_path,
		bool &r_used_direct_pipeline);
