/**************************************************************************/
/*  gltf_buffer_view.cpp                                                  */
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

#include "gltf_buffer_view.h"
#include "gltf_buffer_view.compat.inc"

#include "../gltf_state.h"

#include "core/object/class_db.h"

#include <meshoptimizer.h>

void GLTFBufferView::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_buffer_view_data", "state"), &GLTFBufferView::load_buffer_view_data);

	ClassDB::bind_static_method("GLTFBufferView", D_METHOD("from_dictionary", "dictionary"), &GLTFBufferView::from_dictionary);
	ClassDB::bind_method(D_METHOD("to_dictionary"), &GLTFBufferView::to_dictionary);

	ClassDB::bind_method(D_METHOD("get_buffer"), &GLTFBufferView::get_buffer);
	ClassDB::bind_method(D_METHOD("set_buffer", "buffer"), &GLTFBufferView::set_buffer);
	ClassDB::bind_method(D_METHOD("get_byte_offset"), &GLTFBufferView::get_byte_offset);
	ClassDB::bind_method(D_METHOD("set_byte_offset", "byte_offset"), &GLTFBufferView::set_byte_offset);
	ClassDB::bind_method(D_METHOD("get_byte_length"), &GLTFBufferView::get_byte_length);
	ClassDB::bind_method(D_METHOD("set_byte_length", "byte_length"), &GLTFBufferView::set_byte_length);
	ClassDB::bind_method(D_METHOD("get_byte_stride"), &GLTFBufferView::get_byte_stride);
	ClassDB::bind_method(D_METHOD("set_byte_stride", "byte_stride"), &GLTFBufferView::set_byte_stride);
	ClassDB::bind_method(D_METHOD("get_indices"), &GLTFBufferView::get_indices);
	ClassDB::bind_method(D_METHOD("set_indices", "indices"), &GLTFBufferView::set_indices);
	ClassDB::bind_method(D_METHOD("get_vertex_attributes"), &GLTFBufferView::get_vertex_attributes);
	ClassDB::bind_method(D_METHOD("set_vertex_attributes", "is_attributes"), &GLTFBufferView::set_vertex_attributes);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "buffer"), "set_buffer", "get_buffer"); // GLTFBufferIndex
	ADD_PROPERTY(PropertyInfo(Variant::INT, "byte_offset"), "set_byte_offset", "get_byte_offset"); // int
	ADD_PROPERTY(PropertyInfo(Variant::INT, "byte_length"), "set_byte_length", "get_byte_length"); // int
	ADD_PROPERTY(PropertyInfo(Variant::INT, "byte_stride"), "set_byte_stride", "get_byte_stride"); // int
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "indices"), "set_indices", "get_indices"); // bool
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "vertex_attributes"), "set_vertex_attributes", "get_vertex_attributes"); // bool
}

GLTFBufferIndex GLTFBufferView::get_buffer() const {
	return buffer;
}

void GLTFBufferView::set_buffer(GLTFBufferIndex p_buffer) {
	buffer = p_buffer;
}

int64_t GLTFBufferView::get_byte_offset() const {
	return byte_offset;
}

void GLTFBufferView::set_byte_offset(int64_t p_byte_offset) {
	byte_offset = p_byte_offset;
}

int64_t GLTFBufferView::get_byte_length() const {
	return byte_length;
}

void GLTFBufferView::set_byte_length(int64_t p_byte_length) {
	byte_length = p_byte_length;
}

int64_t GLTFBufferView::get_byte_stride() const {
	return byte_stride;
}

void GLTFBufferView::set_byte_stride(int64_t p_byte_stride) {
	byte_stride = p_byte_stride;
}

bool GLTFBufferView::get_indices() const {
	return indices;
}

void GLTFBufferView::set_indices(bool p_indices) {
	indices = p_indices;
}

bool GLTFBufferView::get_vertex_attributes() const {
	return vertex_attributes;
}

void GLTFBufferView::set_vertex_attributes(bool p_attributes) {
	vertex_attributes = p_attributes;
}

void GLTFBufferView::configure_meshopt(GLTFBufferIndex p_buffer, int64_t p_byte_offset, int64_t p_byte_length, int64_t p_byte_stride, int64_t p_count, MeshoptMode p_mode, MeshoptFilter p_filter) {
	meshopt_buffer = p_buffer;
	meshopt_byte_offset = p_byte_offset;
	meshopt_byte_length = p_byte_length;
	meshopt_byte_stride = p_byte_stride;
	meshopt_count = p_count;
	meshopt_mode = p_mode;
	meshopt_filter = p_filter;
}

Error GLTFBufferView::decode_meshopt(const GLTFState *p_gltf_state) {
	if (meshopt_mode == MESHOPT_MODE_NONE) {
		return OK;
	}
	if (!decoded_meshopt_data.is_empty()) {
		return OK;
	}
	ERR_FAIL_NULL_V(p_gltf_state, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(meshopt_count <= 0 || meshopt_byte_stride <= 0 || meshopt_byte_length <= 0 || meshopt_byte_offset < 0, ERR_PARSE_ERROR);

	const uint8_t *buf_ptr = nullptr;
	int64_t buf_len = 0;
	ERR_FAIL_COND_V(!p_gltf_state->get_raw_buffer_span(meshopt_buffer, buf_ptr, buf_len), ERR_PARSE_ERROR);
	ERR_FAIL_COND_V(meshopt_byte_offset > buf_len || meshopt_byte_length > buf_len - meshopt_byte_offset, ERR_FILE_CORRUPT);

	const int64_t dst_size = meshopt_count * meshopt_byte_stride;
	ERR_FAIL_COND_V(dst_size < byte_length || dst_size <= 0, ERR_FILE_CORRUPT);

	Vector<uint8_t> decoded;
	ERR_FAIL_COND_V(decoded.resize(dst_size) != OK, ERR_OUT_OF_MEMORY);

	const unsigned char *src = reinterpret_cast<const unsigned char *>(buf_ptr + meshopt_byte_offset);
	const size_t src_size = static_cast<size_t>(meshopt_byte_length);
	uint8_t *dst = decoded.ptrw();

	int rc = -1;
	switch (meshopt_mode) {
		case MESHOPT_MODE_ATTRIBUTES: {
			ERR_FAIL_COND_V(meshopt_byte_stride % 4 != 0 || meshopt_byte_stride > 256, ERR_PARSE_ERROR);
			rc = meshopt_decodeVertexBuffer(dst, static_cast<size_t>(meshopt_count), static_cast<size_t>(meshopt_byte_stride), src, src_size);
			break;
		}
		case MESHOPT_MODE_TRIANGLES: {
			ERR_FAIL_COND_V((meshopt_byte_stride != 2 && meshopt_byte_stride != 4) || (meshopt_count % 3 != 0), ERR_PARSE_ERROR);
			rc = meshopt_decodeIndexBuffer(dst, static_cast<size_t>(meshopt_count), static_cast<size_t>(meshopt_byte_stride), src, src_size);
			break;
		}
		case MESHOPT_MODE_INDICES: {
			ERR_FAIL_COND_V(meshopt_byte_stride != 2 && meshopt_byte_stride != 4, ERR_PARSE_ERROR);
			rc = meshopt_decodeIndexSequence(dst, static_cast<size_t>(meshopt_count), static_cast<size_t>(meshopt_byte_stride), src, src_size);
			break;
		}
		default:
			return ERR_PARSE_ERROR;
	}
	ERR_FAIL_COND_V_MSG(rc != 0, ERR_FILE_CORRUPT, vformat("glTF meshopt decompression failed with code %d.", rc));

	switch (meshopt_filter) {
		case MESHOPT_FILTER_NONE:
			break;
		case MESHOPT_FILTER_OCTAHEDRAL:
			ERR_FAIL_COND_V(meshopt_byte_stride != 4 && meshopt_byte_stride != 8, ERR_PARSE_ERROR);
			meshopt_decodeFilterOct(dst, static_cast<size_t>(meshopt_count), static_cast<size_t>(meshopt_byte_stride));
			break;
		case MESHOPT_FILTER_QUATERNION:
			ERR_FAIL_COND_V(meshopt_byte_stride != 8, ERR_PARSE_ERROR);
			meshopt_decodeFilterQuat(dst, static_cast<size_t>(meshopt_count), static_cast<size_t>(meshopt_byte_stride));
			break;
		case MESHOPT_FILTER_EXPONENTIAL:
			ERR_FAIL_COND_V(meshopt_byte_stride % 4 != 0, ERR_PARSE_ERROR);
			meshopt_decodeFilterExp(dst, static_cast<size_t>(meshopt_count), static_cast<size_t>(meshopt_byte_stride));
			break;
		case MESHOPT_FILTER_COLOR:
			ERR_FAIL_COND_V(meshopt_byte_stride != 4 && meshopt_byte_stride != 8, ERR_PARSE_ERROR);
			meshopt_decodeFilterColor(dst, static_cast<size_t>(meshopt_count), static_cast<size_t>(meshopt_byte_stride));
			break;
	}

	decoded_meshopt_data = decoded;
	return OK;
}

bool GLTFBufferView::get_view_span(const GLTFState *p_gltf_state, const uint8_t *&r_ptr, int64_t &r_length) const {
	r_ptr = nullptr;
	r_length = 0;
	ERR_FAIL_NULL_V(p_gltf_state, false);

	if (meshopt_mode != MESHOPT_MODE_NONE) {
		if (decoded_meshopt_data.is_empty()) {
			Error err = const_cast<GLTFBufferView *>(this)->decode_meshopt(p_gltf_state);
			ERR_FAIL_COND_V(err != OK, false);
		}
		const int64_t avail = decoded_meshopt_data.size();
		const int64_t len = (byte_length > 0 && byte_length <= avail) ? byte_length : avail;
		r_ptr = decoded_meshopt_data.ptr();
		r_length = len;
		return true;
	}

	const uint8_t *buf_ptr = nullptr;
	int64_t buf_len = 0;
	if (!p_gltf_state->get_raw_buffer_span(buffer, buf_ptr, buf_len)) {
		return false;
	}
	ERR_FAIL_COND_V(byte_offset < 0 || byte_length < 0 || byte_offset > buf_len || byte_length > buf_len - byte_offset, false);
	r_ptr = buf_ptr + byte_offset;
	r_length = byte_length;
	return true;
}

Vector<uint8_t> GLTFBufferView::load_buffer_view_data(const Ref<GLTFState> p_gltf_state) const {
	ERR_FAIL_COND_V(p_gltf_state.is_null(), Vector<uint8_t>());
	if (meshopt_mode != MESHOPT_MODE_NONE) {
		if (decoded_meshopt_data.is_empty()) {
			Error err = const_cast<GLTFBufferView *>(this)->decode_meshopt(p_gltf_state.ptr());
			ERR_FAIL_COND_V(err != OK, Vector<uint8_t>());
		}
		if (byte_length > 0 && byte_length < decoded_meshopt_data.size()) {
			return decoded_meshopt_data.slice(0, byte_length);
		}
		return decoded_meshopt_data;
	}

	const uint8_t *span_ptr = nullptr;
	int64_t span_len = 0;
	if (get_view_span(p_gltf_state.ptr(), span_ptr, span_len)) {
		Vector<uint8_t> out;
		out.resize(span_len);
		if (span_len > 0) {
			memcpy(out.ptrw(), span_ptr, static_cast<size_t>(span_len));
		}
		return out;
	}

	const Vector<PackedByteArray> &buffers = p_gltf_state->get_buffers();
	ERR_FAIL_INDEX_V(buffer, buffers.size(), Vector<uint8_t>());
	const PackedByteArray &buffer_data = buffers[buffer];
	const int64_t byte_end = byte_offset + byte_length;
	return buffer_data.slice(byte_offset, byte_end);
}

GLTFBufferViewIndex GLTFBufferView::write_new_buffer_view_into_state(const Ref<GLTFState> &p_gltf_state, const PackedByteArray &p_input_data, const int64_t p_alignment, const ArrayBufferTarget p_target, const int64_t p_byte_stride, const GLTFBufferIndex p_buffer_index, const bool p_deduplicate) {
	ERR_FAIL_COND_V_MSG(p_buffer_index < 0, -1, "Buffer index must be greater than or equal to zero.");
	const bool target_is_indices = p_target == ArrayBufferTarget::TARGET_ELEMENT_ARRAY_BUFFER;
	const bool target_is_vertex_attributes = p_target == ArrayBufferTarget::TARGET_ARRAY_BUFFER;
	if (target_is_vertex_attributes) {
		ERR_FAIL_COND_V_MSG(p_byte_stride < 4 || p_byte_stride % 4 != 0, -1, "glTF export: Vertex attributes using TARGET_ARRAY_BUFFER must have a byte stride that is a multiple of 4 as required by section 3.6.2.4 of the glTF specification.");
	}
	Vector<Ref<GLTFBufferView>> state_buffer_views = p_gltf_state->get_buffer_views();
	const int buffer_view_index = state_buffer_views.size();
	if (p_deduplicate) {
		for (int i = 0; i < buffer_view_index; i++) {
			const Ref<GLTFBufferView> &existing_buffer_view = state_buffer_views[i];
			if (existing_buffer_view->get_byte_offset() % p_alignment == 0 &&
					existing_buffer_view->get_byte_length() == p_input_data.size() &&
					existing_buffer_view->get_byte_stride() == p_byte_stride &&
					existing_buffer_view->get_indices() == target_is_indices &&
					existing_buffer_view->get_vertex_attributes() == target_is_vertex_attributes) {
				if (existing_buffer_view->load_buffer_view_data(p_gltf_state) == p_input_data) {
					return i;
				}
			}
		}
	}
	Vector<PackedByteArray> state_buffers = p_gltf_state->get_buffers();
	if (state_buffers.size() <= p_buffer_index) {
		state_buffers.resize(p_buffer_index + 1);
	}
	PackedByteArray state_buffer = state_buffers[p_buffer_index];
	const int64_t input_data_size = p_input_data.size();
	int64_t byte_offset = state_buffer.size();
	int64_t padding_bytes = 0;
	if (byte_offset % p_alignment != 0) {
		padding_bytes = p_alignment - (byte_offset % p_alignment);
		byte_offset += padding_bytes;
	}
	state_buffer.resize(byte_offset + input_data_size);
	if (padding_bytes > 0) {
		memset(state_buffer.ptrw() + (byte_offset - padding_bytes), 0, padding_bytes);
	}
	uint8_t *buffer_ptr = state_buffer.ptrw();
	memcpy(buffer_ptr + byte_offset, p_input_data.ptr(), input_data_size);
	state_buffers.set(p_buffer_index, state_buffer);
	p_gltf_state->set_buffers(state_buffers);
	Ref<GLTFBufferView> buffer_view;
	buffer_view.instantiate();
	buffer_view->set_buffer(p_buffer_index);
	buffer_view->set_byte_offset(byte_offset);
	buffer_view->set_byte_length(input_data_size);
	buffer_view->set_byte_stride(p_byte_stride);
	buffer_view->set_indices(target_is_indices);
	buffer_view->set_vertex_attributes(target_is_vertex_attributes);
	state_buffer_views.append(buffer_view);
	p_gltf_state->set_buffer_views(state_buffer_views);
	return buffer_view_index;
}

Ref<GLTFBufferView> GLTFBufferView::from_dictionary(const Dictionary &p_dict) {
	Ref<GLTFBufferView> buffer_view;
	buffer_view.instantiate();
	if (p_dict.has("buffer")) {
		buffer_view->set_buffer(p_dict["buffer"]);
	}
	if (p_dict.has("byteLength")) {
		buffer_view->set_byte_length(p_dict["byteLength"]);
	}
	if (p_dict.has("byteOffset")) {
		buffer_view->set_byte_offset(p_dict["byteOffset"]);
	}
	if (p_dict.has("byteStride")) {
		buffer_view->byte_stride = p_dict["byteStride"];
		if (buffer_view->byte_stride < 4 || buffer_view->byte_stride > 252 || buffer_view->byte_stride % 4 != 0) {
			ERR_PRINT("glTF import: Invalid byte stride " + itos(buffer_view->byte_stride) + " for buffer view. If defined, byte stride must be a multiple of 4 and between 4 and 252.");
		}
	}
	if (p_dict.has("target")) {
		const int target = p_dict["target"];
		buffer_view->indices = target == ArrayBufferTarget::TARGET_ELEMENT_ARRAY_BUFFER;
		buffer_view->vertex_attributes = target == ArrayBufferTarget::TARGET_ARRAY_BUFFER;
	}
	if (p_dict.has("extensions")) {
		const Dictionary exts = p_dict["extensions"];
		Dictionary mext;
		if (exts.has("KHR_meshopt_compression")) {
			mext = exts["KHR_meshopt_compression"];
		} else if (exts.has("EXT_meshopt_compression")) {
			mext = exts["EXT_meshopt_compression"];
		}
		if (!mext.is_empty() && mext.has("buffer") && mext.has("byteLength") && mext.has("byteStride") && mext.has("count") && mext.has("mode")) {
			const String mode_str = mext["mode"];
			MeshoptMode mode = MESHOPT_MODE_NONE;
			if (mode_str == "ATTRIBUTES") {
				mode = MESHOPT_MODE_ATTRIBUTES;
			} else if (mode_str == "TRIANGLES") {
				mode = MESHOPT_MODE_TRIANGLES;
			} else if (mode_str == "INDICES") {
				mode = MESHOPT_MODE_INDICES;
			}
			MeshoptFilter filter = MESHOPT_FILTER_NONE;
			if (mext.has("filter")) {
				const String fstr = mext["filter"];
				if (fstr == "OCTAHEDRAL") {
					filter = MESHOPT_FILTER_OCTAHEDRAL;
				} else if (fstr == "QUATERNION") {
					filter = MESHOPT_FILTER_QUATERNION;
				} else if (fstr == "EXPONENTIAL") {
					filter = MESHOPT_FILTER_EXPONENTIAL;
				} else if (fstr == "COLOR") {
					filter = MESHOPT_FILTER_COLOR;
				}
			}
			if (mode != MESHOPT_MODE_NONE) {
				buffer_view->configure_meshopt(
						mext["buffer"],
						mext.get("byteOffset", 0),
						mext["byteLength"],
						mext["byteStride"],
						mext["count"],
						mode,
						filter);
			}
		}
	}
	return buffer_view;
}

Dictionary GLTFBufferView::to_dictionary() const {
	Dictionary dict;
	ERR_FAIL_COND_V_MSG(buffer == -1, dict, "Buffer index must be set to a valid buffer before converting to Dictionary.");
	dict["buffer"] = buffer;
	dict["byteLength"] = byte_length;
	if (byte_offset != 0) {
		dict["byteOffset"] = byte_offset;
	}
	if (byte_stride != -1) {
		dict["byteStride"] = byte_stride;
	}
	if (indices) {
		dict["target"] = ArrayBufferTarget::TARGET_ELEMENT_ARRAY_BUFFER;
	} else if (vertex_attributes) {
		dict["target"] = ArrayBufferTarget::TARGET_ARRAY_BUFFER;
	}
	return dict;
}
