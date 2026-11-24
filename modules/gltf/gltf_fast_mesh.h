/**************************************************************************/
/*  gltf_fast_mesh.h                                                      */
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

#include "gltf_defines.h"

#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

class GLTFDocument;
class GLTFMesh;
class GLTFState;

struct GLTFFastMorphSpec {
	int32_t position = -1;
	int32_t normal = -1;
	int32_t tangent = -1;
};

struct GLTFFastPrimSpec {
	int32_t mode = 4; // Default TRIANGLES (4).
	int32_t indices = -1;
	int32_t material = -1;
	int32_t position = -1;
	int32_t normal = -1;
	int32_t tangent = -1;
	int32_t color_0 = -1;
	int32_t texcoord[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	int32_t joints_0 = -1;
	int32_t joints_1 = -1;
	int32_t weights_0 = -1;
	int32_t weights_1 = -1;
	bool dense_verts_verified = false;
	LocalVector<GLTFFastMorphSpec> targets;
};

// Direct zero-copy / single-pass ArrayMesh builder for standard GLB / glTF 2.0 primitives.
// Supports KHR_mesh_quantization, EXT/KHR_meshopt_compression, sparse accessors,
// all 7 glTF primitive topologies, 4/8 bone weights, morph targets, and LoadThreadPool chunking.
Error gltf_build_fast_array_mesh(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		const String &p_unique_mesh_name,
		const String &p_original_mesh_name,
		const Vector<String> &p_target_names,
		const Vector<float> &p_weights,
		const Variant &p_mesh_extras,
		bool p_discard_materials,
		bool p_force_generate_tangents,
		bool p_force_disable_compression,
		const LocalVector<GLTFFastPrimSpec> &p_prims,
		Ref<GLTFMesh> &r_out_gltf_mesh);
