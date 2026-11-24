/**************************************************************************/
/*  gltf_fast_mesh.cpp                                                    */
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

#include "gltf_fast_mesh.h"

#include "gltf_document.h"
#include "gltf_state.h"
#include "structures/gltf_accessor.h"
#include "structures/gltf_buffer_view.h"
#include "structures/gltf_mesh.h"

#include "core/math/aabb.h"
#include "core/math/math_funcs.h"
#include "core/os/load_thread_pool.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"

#include <thirdparty/misc/mikktspace.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include <cmath>
#include <cstring>

namespace {

_FORCE_INLINE_ uint16_t pack_unorm16(float p_val) {
	return static_cast<uint16_t>(CLAMP(p_val * 65535.0f, 0.0f, 65535.0f));
}

_FORCE_INLINE_ void pack_oct_direction(uint8_t *r_dst, const Vector2 &p_oct, bool p_is_tangent) {
	uint16_t pair[2] = { pack_unorm16(p_oct.x), pack_unorm16(p_oct.y) };
	if (p_is_tangent && pair[0] == 0 && pair[1] == 65535) {
		pair[0] = 65535;
	}
	std::memcpy(r_dst, pair, 4);
}

// Exact inline match of RenderingServer's _get_axis_angle (rendering_server.cpp:320).
_FORCE_INLINE_ void compute_tbn_axis_angle(const Vector3 &p_normal, const Vector4 &p_tangent, float &r_angle, Vector3 &r_axis) {
	const Vector3 normal = p_normal.normalized();
	const Vector3 tangent = Vector3(p_tangent.x, p_tangent.y, p_tangent.z).normalized();
	const float d = p_tangent.w;
	const Vector3 binormal = normal.cross(tangent).normalized();
	real_t angle = 0;

	Basis tbn;
	tbn.rows[0] = tangent;
	tbn.rows[1] = binormal;
	tbn.rows[2] = normal;
	tbn.get_axis_angle(r_axis, angle);
	r_angle = float(angle);

	if (d < 0.0f) {
		r_angle = CLAMP((1.0f - r_angle / float(Math::PI)) * 0.5f, 0.0f, 0.49999f);
	} else {
		r_angle = CLAMP((r_angle / float(Math::PI)) * 0.5f + 0.5f, 0.500008f, 1.0f);
	}
}

int accessor_num_components(GLTFAccessor::GLTFAccessorType p_type) {
	switch (p_type) {
		case GLTFAccessor::TYPE_SCALAR:
			return 1;
		case GLTFAccessor::TYPE_VEC2:
			return 2;
		case GLTFAccessor::TYPE_VEC3:
			return 3;
		case GLTFAccessor::TYPE_VEC4:
		case GLTFAccessor::TYPE_MAT2:
			return 4;
		case GLTFAccessor::TYPE_MAT3:
			return 9;
		case GLTFAccessor::TYPE_MAT4:
			return 16;
		default:
			return 0;
	}
}

int component_byte_size(GLTFAccessor::GLTFComponentType p_type) {
	switch (p_type) {
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_BYTE:
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_BYTE:
			return 1;
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_SHORT:
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_SHORT:
		case GLTFAccessor::COMPONENT_TYPE_HALF_FLOAT:
			return 2;
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_INT:
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_INT:
		case GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT:
			return 4;
		case GLTFAccessor::COMPONENT_TYPE_DOUBLE_FLOAT:
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_LONG:
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_LONG:
			return 8;
		default:
			return 0;
	}
}

_FORCE_INLINE_ float read_component_as_float(const uint8_t *p_ptr, GLTFAccessor::GLTFComponentType p_comp_type, bool p_normalized) {
	switch (p_comp_type) {
		case GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT: {
			float v;
			std::memcpy(&v, p_ptr, sizeof(float));
			return v;
		}
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_BYTE: {
			const int8_t v = static_cast<int8_t>(*p_ptr);
			return p_normalized ? MAX(float(v) / 127.0f, -1.0f) : float(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_BYTE: {
			const uint8_t v = *p_ptr;
			return p_normalized ? (float(v) / 255.0f) : float(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_SHORT: {
			int16_t v;
			std::memcpy(&v, p_ptr, sizeof(int16_t));
			return p_normalized ? MAX(float(v) / 32767.0f, -1.0f) : float(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_SHORT: {
			uint16_t v;
			std::memcpy(&v, p_ptr, sizeof(uint16_t));
			return p_normalized ? (float(v) / 65535.0f) : float(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_INT: {
			uint32_t v;
			std::memcpy(&v, p_ptr, sizeof(uint32_t));
			return p_normalized ? float(double(v) / 4294967295.0) : float(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_INT: {
			int32_t v;
			std::memcpy(&v, p_ptr, sizeof(int32_t));
			return p_normalized ? MAX(float(double(v) / 2147483647.0), -1.0f) : float(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_HALF_FLOAT: {
			uint16_t v;
			std::memcpy(&v, p_ptr, sizeof(uint16_t));
			return Math::half_to_float(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_DOUBLE_FLOAT: {
			double v;
			std::memcpy(&v, p_ptr, sizeof(double));
			return float(v);
		}
		default:
			return 0.0f;
	}
}

_FORCE_INLINE_ uint32_t read_component_as_u32(const uint8_t *p_ptr, GLTFAccessor::GLTFComponentType p_comp_type) {
	switch (p_comp_type) {
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_BYTE:
			return uint32_t(*p_ptr);
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_BYTE:
			return uint32_t(MAX(int32_t(static_cast<int8_t>(*p_ptr)), 0));
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_SHORT: {
			uint16_t v;
			std::memcpy(&v, p_ptr, sizeof(uint16_t));
			return uint32_t(v);
		}
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_SHORT: {
			int16_t v;
			std::memcpy(&v, p_ptr, sizeof(int16_t));
			return uint32_t(MAX(int32_t(v), 0));
		}
		case GLTFAccessor::COMPONENT_TYPE_UNSIGNED_INT: {
			uint32_t v;
			std::memcpy(&v, p_ptr, sizeof(uint32_t));
			return v;
		}
		case GLTFAccessor::COMPONENT_TYPE_SIGNED_INT: {
			int32_t v;
			std::memcpy(&v, p_ptr, sizeof(int32_t));
			return uint32_t(MAX(v, 0));
		}
		case GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT: {
			float v;
			std::memcpy(&v, p_ptr, sizeof(float));
			return v > 0.0f ? uint32_t(v) : 0u;
		}
		default:
			return 0u;
	}
}

struct FastAccessorView {
	const uint8_t *data = nullptr;
	int64_t stride = 0;
	int64_t count = 0;
	int comps = 0;
	int comp_size = 0;
	GLTFAccessor::GLTFComponentType comp_type = GLTFAccessor::COMPONENT_TYPE_NONE;
	bool normalized = false;
	bool is_zero = false;
	bool is_materialized_f32 = false;
	bool is_materialized_u32 = false;
	LocalVector<float> materialized_f32;
	LocalVector<uint32_t> materialized_u32;

	bool init(const GLTFState *p_state, int32_t p_accessor_idx, bool p_as_uint = false) {
		if (p_accessor_idx < 0) {
			return false;
		}
		const Vector<Ref<GLTFAccessor>> &accessors = p_state->get_accessors();
		ERR_FAIL_INDEX_V(p_accessor_idx, accessors.size(), false);
		const Ref<GLTFAccessor> &acc = accessors[p_accessor_idx];
		ERR_FAIL_COND_V(acc.is_null(), false);

		count = acc->get_count();
		ERR_FAIL_COND_V(count < 0, false);
		comp_type = acc->get_component_type();
		normalized = acc->get_normalized();
		comps = accessor_num_components(acc->get_accessor_type());
		comp_size = component_byte_size(comp_type);
		ERR_FAIL_COND_V(comps <= 0 || comp_size <= 0, false);

		const int64_t elem_bytes = int64_t(comps) * int64_t(comp_size);
		const Vector<Ref<GLTFBufferView>> &bvs = p_state->get_buffer_views();
		const GLTFBufferViewIndex bvi = acc->get_buffer_view();

		const uint8_t *base_ptr = nullptr;
		int64_t base_stride = elem_bytes;
		if (bvi >= 0) {
			ERR_FAIL_INDEX_V(bvi, bvs.size(), false);
			const Ref<GLTFBufferView> &bv = bvs[bvi];
			ERR_FAIL_COND_V(bv.is_null(), false);
			const uint8_t *view_ptr = nullptr;
			int64_t view_len = 0;
			ERR_FAIL_COND_V(!bv->get_view_span(p_state, view_ptr, view_len), false);
			base_stride = bv->get_byte_stride() > 0 ? bv->get_byte_stride() : elem_bytes;
			ERR_FAIL_COND_V(base_stride < elem_bytes, false);
			const int64_t byte_off = acc->get_byte_offset();
			ERR_FAIL_COND_V(byte_off < 0, false);
			if (count > 0) {
				const int64_t needed = byte_off + (count - 1) * base_stride + elem_bytes;
				ERR_FAIL_COND_V(needed > view_len, false);
			}
			base_ptr = view_ptr + byte_off;
		} else {
			is_zero = true;
		}

		const int64_t sparse_cnt = acc->get_sparse_count();
		if (sparse_cnt <= 0) {
			data = base_ptr;
			stride = base_stride;
			return true;
		}

		// Sparse accessor: materialize into contiguous f32 or u32 buffer and apply sparse overrides.
		const GLTFBufferViewIndex idx_bvi = acc->get_sparse_indices_buffer_view();
		const GLTFBufferViewIndex val_bvi = acc->get_sparse_values_buffer_view();
		ERR_FAIL_INDEX_V(idx_bvi, bvs.size(), false);
		ERR_FAIL_INDEX_V(val_bvi, bvs.size(), false);
		const GLTFAccessor::GLTFComponentType idx_comp_type = acc->get_sparse_indices_component_type();
		const int idx_comp_size = component_byte_size(idx_comp_type);
		ERR_FAIL_COND_V(idx_comp_size != 1 && idx_comp_size != 2 && idx_comp_size != 4, false);

		const uint8_t *idx_view_ptr = nullptr;
		int64_t idx_view_len = 0;
		ERR_FAIL_COND_V(!bvs[idx_bvi]->get_view_span(p_state, idx_view_ptr, idx_view_len), false);
		const int64_t idx_off = acc->get_sparse_indices_byte_offset();
		const int64_t idx_stride = bvs[idx_bvi]->get_byte_stride() > 0 ? bvs[idx_bvi]->get_byte_stride() : idx_comp_size;
		ERR_FAIL_COND_V(idx_off < 0 || idx_off + (sparse_cnt - 1) * idx_stride + idx_comp_size > idx_view_len, false);

		const uint8_t *val_view_ptr = nullptr;
		int64_t val_view_len = 0;
		ERR_FAIL_COND_V(!bvs[val_bvi]->get_view_span(p_state, val_view_ptr, val_view_len), false);
		const int64_t val_off = acc->get_sparse_values_byte_offset();
		const int64_t val_stride = bvs[val_bvi]->get_byte_stride() > 0 ? bvs[val_bvi]->get_byte_stride() : elem_bytes;
		ERR_FAIL_COND_V(val_off < 0 || val_off + (sparse_cnt - 1) * val_stride + elem_bytes > val_view_len, false);

		is_zero = false;
		if (p_as_uint) {
			is_materialized_u32 = true;
			materialized_u32.resize(uint32_t(count * comps));
			if (base_ptr) {
				for (int64_t i = 0; i < count; i++) {
					const uint8_t *src = base_ptr + i * base_stride;
					for (int c = 0; c < comps; c++) {
						materialized_u32[uint32_t(i * comps + c)] = read_component_as_u32(src + c * comp_size, comp_type);
					}
				}
			} else {
				std::memset(materialized_u32.ptr(), 0, materialized_u32.size() * sizeof(uint32_t));
			}
			for (int64_t s = 0; s < sparse_cnt; s++) {
				const uint32_t target_idx = read_component_as_u32(idx_view_ptr + idx_off + s * idx_stride, idx_comp_type);
				ERR_FAIL_COND_V(int64_t(target_idx) >= count, false);
				const uint8_t *vsrc = val_view_ptr + val_off + s * val_stride;
				for (int c = 0; c < comps; c++) {
					materialized_u32[target_idx * uint32_t(comps) + uint32_t(c)] = read_component_as_u32(vsrc + c * comp_size, comp_type);
				}
			}
		} else {
			is_materialized_f32 = true;
			materialized_f32.resize(uint32_t(count * comps));
			if (base_ptr && comp_type == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT && base_stride == elem_bytes) {
				std::memcpy(materialized_f32.ptr(), base_ptr, size_t(count * elem_bytes));
			} else if (base_ptr) {
				for (int64_t i = 0; i < count; i++) {
					const uint8_t *src = base_ptr + i * base_stride;
					for (int c = 0; c < comps; c++) {
						materialized_f32[uint32_t(i * comps + c)] = read_component_as_float(src + c * comp_size, comp_type, normalized);
					}
				}
			} else {
				std::memset(materialized_f32.ptr(), 0, materialized_f32.size() * sizeof(float));
			}
			for (int64_t s = 0; s < sparse_cnt; s++) {
				const uint32_t target_idx = read_component_as_u32(idx_view_ptr + idx_off + s * idx_stride, idx_comp_type);
				ERR_FAIL_COND_V(int64_t(target_idx) >= count, false);
				const uint8_t *vsrc = val_view_ptr + val_off + s * val_stride;
				for (int c = 0; c < comps; c++) {
					materialized_f32[target_idx * uint32_t(comps) + uint32_t(c)] = read_component_as_float(vsrc + c * comp_size, comp_type, normalized);
				}
			}
		}
		return true;
	}

	_FORCE_INLINE_ bool is_tight_f32(int p_expected_comps) const {
		return !is_zero && !is_materialized_f32 && data != nullptr &&
				comp_type == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT &&
				comps == p_expected_comps &&
				stride == int64_t(p_expected_comps) * 4;
	}

	_FORCE_INLINE_ uint32_t read_index(int64_t p_idx) const {
		if (is_materialized_u32) {
			return materialized_u32[uint32_t(p_idx)];
		}
		if (is_zero || !data) {
			return 0u;
		}
		return read_component_as_u32(data + p_idx * stride, comp_type);
	}

	_FORCE_INLINE_ Vector2 read_vec2(int64_t p_idx) const {
		if (is_materialized_f32) {
			const float *p = &materialized_f32[uint32_t(p_idx * comps)];
			return Vector2(p[0], comps >= 2 ? p[1] : 0.0f);
		}
		if (is_zero || !data) {
			return Vector2();
		}
		const uint8_t *p = data + p_idx * stride;
		if (comp_type == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT && comps >= 2) {
			float xy[2];
			std::memcpy(xy, p, 8);
			return Vector2(xy[0], xy[1]);
		}
		return Vector2(
				read_component_as_float(p, comp_type, normalized),
				comps >= 2 ? read_component_as_float(p + comp_size, comp_type, normalized) : 0.0f);
	}

	_FORCE_INLINE_ Vector3 read_vec3(int64_t p_idx) const {
		if (is_materialized_f32) {
			const float *p = &materialized_f32[uint32_t(p_idx * comps)];
			return Vector3(p[0], comps >= 2 ? p[1] : 0.0f, comps >= 3 ? p[2] : 0.0f);
		}
		if (is_zero || !data) {
			return Vector3();
		}
		const uint8_t *p = data + p_idx * stride;
		if (comp_type == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT && comps >= 3) {
			float xyz[3];
			std::memcpy(xyz, p, 12);
			return Vector3(xyz[0], xyz[1], xyz[2]);
		}
		return Vector3(
				read_component_as_float(p, comp_type, normalized),
				comps >= 2 ? read_component_as_float(p + comp_size, comp_type, normalized) : 0.0f,
				comps >= 3 ? read_component_as_float(p + 2 * comp_size, comp_type, normalized) : 0.0f);
	}

	_FORCE_INLINE_ Vector4 read_vec4(int64_t p_idx, float p_default_w = 1.0f) const {
		if (is_materialized_f32) {
			const float *p = &materialized_f32[uint32_t(p_idx * comps)];
			return Vector4(
					p[0],
					comps >= 2 ? p[1] : 0.0f,
					comps >= 3 ? p[2] : 0.0f,
					comps >= 4 ? p[3] : p_default_w);
		}
		if (is_zero || !data) {
			return Vector4(0.0f, 0.0f, 0.0f, p_default_w);
		}
		const uint8_t *p = data + p_idx * stride;
		if (comp_type == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT && comps >= 4) {
			float xyzw[4];
			std::memcpy(xyzw, p, 16);
			return Vector4(xyzw[0], xyzw[1], xyzw[2], xyzw[3]);
		}
		return Vector4(
				read_component_as_float(p, comp_type, normalized),
				comps >= 2 ? read_component_as_float(p + comp_size, comp_type, normalized) : 0.0f,
				comps >= 3 ? read_component_as_float(p + 2 * comp_size, comp_type, normalized) : 0.0f,
				comps >= 4 ? read_component_as_float(p + 3 * comp_size, comp_type, normalized) : p_default_w);
	}

	_FORCE_INLINE_ void read_joints4(int64_t p_idx, uint16_t r_out[4]) const {
		if (is_materialized_u32) {
			const uint32_t *p = &materialized_u32[uint32_t(p_idx * comps)];
			for (int c = 0; c < 4; c++) {
				r_out[c] = c < comps ? uint16_t(p[c]) : 0u;
			}
			return;
		}
		if (is_zero || !data) {
			r_out[0] = r_out[1] = r_out[2] = r_out[3] = 0;
			return;
		}
		const uint8_t *p = data + p_idx * stride;
		if (comp_type == GLTFAccessor::COMPONENT_TYPE_UNSIGNED_BYTE && comps >= 4) {
			r_out[0] = p[0];
			r_out[1] = p[1];
			r_out[2] = p[2];
			r_out[3] = p[3];
			return;
		}
		if (comp_type == GLTFAccessor::COMPONENT_TYPE_UNSIGNED_SHORT && comps >= 4) {
			std::memcpy(r_out, p, 8);
			return;
		}
		for (int c = 0; c < 4; c++) {
			r_out[c] = c < comps ? uint16_t(read_component_as_u32(p + c * comp_size, comp_type)) : 0u;
		}
	}
};

#if defined(__aarch64__) || defined(_M_ARM64)
// NEON 4-wide octahedral normal + tangent encoder for contiguous float32 streams.
int encode_directions_neon(uint8_t *r_dst, const float *p_normals, const float *p_tangents, int p_vertex_count) {
	const int n = p_vertex_count & ~3;
	if (n <= 0) {
		return 0;
	}
	const float32x4_t zero = vdupq_n_f32(0.0f);
	const float32x4_t one = vdupq_n_f32(1.0f);
	const float32x4_t half = vdupq_n_f32(0.5f);

	auto oct_encode = [&](float32x4_t x, float32x4_t y, float32x4_t z) {
		const float32x4_t scale = vaddq_f32(vaddq_f32(vabsq_f32(x), vabsq_f32(y)), vabsq_f32(z));
		const float32x4_t safe_scale = vbslq_f32(vcgtq_f32(scale, zero), scale, one);
		const float32x4_t px = vdivq_f32(x, safe_scale);
		const float32x4_t py = vdivq_f32(y, safe_scale);
		const uint32x4_t above = vcgeq_f32(z, zero);
		const float32x4_t fold_x = vmulq_f32(vsubq_f32(one, vabsq_f32(py)), vbslq_f32(vcgeq_f32(px, zero), one, vnegq_f32(one)));
		const float32x4_t fold_y = vmulq_f32(vsubq_f32(one, vabsq_f32(px)), vbslq_f32(vcgeq_f32(py, zero), one, vnegq_f32(one)));
		const float32x4_t ux = vaddq_f32(vmulq_f32(vbslq_f32(above, px, fold_x), half), half);
		const float32x4_t uy = vaddq_f32(vmulq_f32(vbslq_f32(above, py, fold_y), half), half);
		return float32x4x2_t{ ux, uy };
	};

	auto pack_u16 = [&](float32x4_t f) {
		const float32x4_t clamped = vminq_f32(vmaxq_f32(f, zero), one);
		return vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(clamped, 65535.0f)));
	};

	for (int i = 0; i < n; i += 4) {
		const float32x4x3_t nrm = vld3q_f32(p_normals + 3 * i);
		const float32x4x4_t tan = vld4q_f32(p_tangents + 4 * i);
		const float32x4x2_t a = oct_encode(nrm.val[0], nrm.val[1], nrm.val[2]);
		const float32x4x2_t b = oct_encode(tan.val[0], tan.val[1], tan.val[2]);
		const float32x4_t sy = vaddq_f32(vmulq_f32(vmaxq_f32(b.val[1], vdupq_n_f32(1.0f / 32767.0f)), half), half);
		uint16x4_t tx = pack_u16(b.val[0]);
		const uint16x4_t ty = pack_u16(vbslq_f32(vcgeq_f32(tan.val[3], zero), sy, vsubq_f32(one, sy)));
		tx = vorr_u16(tx, vand_u16(vceq_u16(tx, vdup_n_u16(0)), vceq_u16(ty, vdup_n_u16(65535))));
		const uint16x4x4_t out = { pack_u16(a.val[0]), pack_u16(a.val[1]), tx, ty };
		vst4_u16(reinterpret_cast<uint16_t *>(r_dst) + 4 * i, out);
	}
	return n;
}

// NEON 4-wide compressed TBN axis-angle + AABB-relative uint16x4 vertex encoder.
// Matches Basis::get_axis_angle() + octahedron_encode() to < 1e-6 rad (< 0.01 LSB of uint16),
// falling back to exact scalar compute_tbn_axis_angle on rare singular TBN frames (theta == 0 or PI).
void pack_compressed_tbn_range_neon(
		uint8_t *r_vw,
		uint32_t p_vtx_offset,
		uint32_t p_nrm_offset,
		const float *p_positions,
		const float *p_normals,
		const float *p_tangents,
		const Vector3 &p_aabb_pos,
		const Vector3 &p_inv_aabb_size,
		uint32_t p_begin,
		uint32_t p_end) {
	const float32x4_t zero = vdupq_n_f32(0.0f);
	const float32x4_t one = vdupq_n_f32(1.0f);
	const float32x4_t neg_one = vdupq_n_f32(-1.0f);
	const float32x4_t half = vdupq_n_f32(0.5f);
	const float32x4_t eps = vdupq_n_f32(CMP_EPSILON);
	const float32x4_t pi_v = vdupq_n_f32(float(Math::PI));
	const float32x4_t inv_pi_v = vdupq_n_f32(1.0f / float(Math::PI));
	const float32x4_t aabb_px = vdupq_n_f32(p_aabb_pos.x);
	const float32x4_t aabb_py = vdupq_n_f32(p_aabb_pos.y);
	const float32x4_t aabb_pz = vdupq_n_f32(p_aabb_pos.z);
	const float32x4_t inv_sx = vdupq_n_f32(p_inv_aabb_size.x);
	const float32x4_t inv_sy = vdupq_n_f32(p_inv_aabb_size.y);
	const float32x4_t inv_sz = vdupq_n_f32(p_inv_aabb_size.z);

	auto pack_u16 = [&](float32x4_t f) {
		const float32x4_t clamped = vminq_f32(vmaxq_f32(f, zero), one);
		return vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(clamped, 65535.0f)));
	};

	uint32_t i = p_begin;
	for (; i + 4 <= p_end; i += 4) {
		const float32x4x3_t pos = vld3q_f32(p_positions + 3 * size_t(i));
		const float32x4x3_t n_raw = vld3q_f32(p_normals + 3 * size_t(i));

		// Normalize normal.
		const float32x4_t nl2 = vfmaq_f32(vfmaq_f32(vmulq_f32(n_raw.val[0], n_raw.val[0]), n_raw.val[1], n_raw.val[1]), n_raw.val[2], n_raw.val[2]);
		const float32x4_t n_inv = vbslq_f32(vcgtq_f32(nl2, zero), vdivq_f32(one, vsqrtq_f32(vbslq_f32(vcgtq_f32(nl2, zero), nl2, one))), zero);
		const float32x4_t nx = vmulq_f32(n_raw.val[0], n_inv);
		const float32x4_t ny = vmulq_f32(n_raw.val[1], n_inv);
		const float32x4_t nz = vmulq_f32(n_raw.val[2], n_inv);

		float32x4_t tx, ty, tz, tw;
		if (p_tangents) {
			const float32x4x4_t t_raw = vld4q_f32(p_tangents + 4 * size_t(i));
			const float32x4_t tl2 = vfmaq_f32(vfmaq_f32(vmulq_f32(t_raw.val[0], t_raw.val[0]), t_raw.val[1], t_raw.val[1]), t_raw.val[2], t_raw.val[2]);
			const float32x4_t t_inv = vbslq_f32(vcgtq_f32(tl2, zero), vdivq_f32(one, vsqrtq_f32(vbslq_f32(vcgtq_f32(tl2, zero), tl2, one))), zero);
			tx = vmulq_f32(t_raw.val[0], t_inv);
			ty = vmulq_f32(t_raw.val[1], t_inv);
			tz = vmulq_f32(t_raw.val[2], t_inv);
			tw = t_raw.val[3];
		} else {
			// Vector3(n.z, -n.x, n.y).cross(n) = (-nx*nz - ny*ny, ny*nx - nz*nz, nz*ny + nx*nx)
			const float32x4_t cx = vnegq_f32(vfmaq_f32(vmulq_f32(nx, nz), ny, ny));
			const float32x4_t cy = vfmsq_f32(vmulq_f32(ny, nx), nz, nz);
			const float32x4_t cz = vfmaq_f32(vmulq_f32(nz, ny), nx, nx);
			const float32x4_t tl2 = vfmaq_f32(vfmaq_f32(vmulq_f32(cx, cx), cy, cy), cz, cz);
			const float32x4_t t_inv = vbslq_f32(vcgtq_f32(tl2, zero), vdivq_f32(one, vsqrtq_f32(vbslq_f32(vcgtq_f32(tl2, zero), tl2, one))), zero);
			tx = vmulq_f32(cx, t_inv);
			ty = vmulq_f32(cy, t_inv);
			tz = vmulq_f32(cz, t_inv);
			tw = one;
		}

		// binormal = cross(n, t).normalized()
		const float32x4_t bcx = vfmsq_f32(vmulq_f32(ny, tz), nz, ty);
		const float32x4_t bcy = vfmsq_f32(vmulq_f32(nz, tx), nx, tz);
		const float32x4_t bcz = vfmsq_f32(vmulq_f32(nx, ty), ny, tx);
		const float32x4_t bl2 = vfmaq_f32(vfmaq_f32(vmulq_f32(bcx, bcx), bcy, bcy), bcz, bcz);
		const float32x4_t b_inv = vbslq_f32(vcgtq_f32(bl2, zero), vdivq_f32(one, vsqrtq_f32(vbslq_f32(vcgtq_f32(bl2, zero), bl2, one))), zero);
		const float32x4_t bx = vmulq_f32(bcx, b_inv);
		const float32x4_t by = vmulq_f32(bcy, b_inv);
		const float32x4_t bz = vmulq_f32(bcz, b_inv);

		// Off-diagonal skew of TBN rows = [t; b; n].
		const float32x4_t dx = vsubq_f32(ny, bz); // rows[2][1] - rows[1][2]
		const float32x4_t dy = vsubq_f32(tz, nx); // rows[0][2] - rows[2][0]
		const float32x4_t dz = vsubq_f32(bx, ty); // rows[1][0] - rows[0][1]
		const float32x4_t r01_r10 = vabsq_f32(vsubq_f32(ty, bx));
		const float32x4_t r02_r20 = vabsq_f32(vsubq_f32(tz, nx));
		const float32x4_t r12_r21 = vabsq_f32(vsubq_f32(bz, ny));
		const uint32x4_t sing = vandq_u32(vandq_u32(vcltq_f32(r01_r10, eps), vcltq_f32(r02_r20, eps)), vcltq_f32(r12_r21, eps));

		if (unlikely(vmaxvq_u32(sing) != 0)) {
			for (uint32_t k = 0; k < 4; k++) {
				const uint32_t vi = i + k;
				const Vector3 pv(p_positions[3 * size_t(vi) + 0], p_positions[3 * size_t(vi) + 1], p_positions[3 * size_t(vi) + 2]);
				const Vector3 nv(p_normals[3 * size_t(vi) + 0], p_normals[3 * size_t(vi) + 1], p_normals[3 * size_t(vi) + 2]);
				Vector4 tv;
				if (p_tangents) {
					tv = Vector4(p_tangents[4 * size_t(vi) + 0], p_tangents[4 * size_t(vi) + 1], p_tangents[4 * size_t(vi) + 2], p_tangents[4 * size_t(vi) + 3]);
				} else {
					const Vector3 c = Vector3(nv.z, -nv.x, nv.y).cross(nv.normalized()).normalized();
					tv = Vector4(c.x, c.y, c.z, 1.0f);
				}
				float angle = 0.0f;
				Vector3 axis;
				compute_tbn_axis_angle(nv, tv, angle, axis);
				pack_oct_direction(r_vw + p_nrm_offset + vi * 4, axis.octahedron_encode(), false);
				const Vector3 rel = (pv - p_aabb_pos) * p_inv_aabb_size;
				const uint16_t xyza[4] = { pack_unorm16(rel.x), pack_unorm16(rel.y), pack_unorm16(rel.z), pack_unorm16(angle) };
				std::memcpy(r_vw + p_vtx_offset + vi * 8, xyza, 8);
			}
			continue;
		}

		const float32x4_t s2 = vfmaq_f32(vfmaq_f32(vmulq_f32(dx, dx), dy, dy), dz, dz);
		const float32x4_t inv_s = vbslq_f32(vcgtq_f32(s2, vdupq_n_f32(CMP_EPSILON * CMP_EPSILON)), vdivq_f32(one, vsqrtq_f32(s2)), one);
		const float32x4_t ax = vmulq_f32(dx, inv_s);
		const float32x4_t ay = vmulq_f32(dy, inv_s);
		const float32x4_t az = vmulq_f32(dz, inv_s);

		// Degree-7 minimax acos(|c|), max error < 2e-7 rad across [-1, 1].
		const float32x4_t cos_v = vminq_f32(vmaxq_f32(vmulq_f32(vsubq_f32(vaddq_f32(vaddq_f32(tx, by), nz), one), half), neg_one), one);
		const float32x4_t u = vabsq_f32(cos_v);
		float32x4_t poly = vdupq_n_f32(-0.0012624911f);
		poly = vfmaq_f32(vdupq_n_f32(0.0066700901f), poly, u);
		poly = vfmaq_f32(vdupq_n_f32(-0.0170881256f), poly, u);
		poly = vfmaq_f32(vdupq_n_f32(0.0308918810f), poly, u);
		poly = vfmaq_f32(vdupq_n_f32(-0.0501743046f), poly, u);
		poly = vfmaq_f32(vdupq_n_f32(0.0889789874f), poly, u);
		poly = vfmaq_f32(vdupq_n_f32(-0.2145988016f), poly, u);
		poly = vfmaq_f32(vdupq_n_f32(1.5707963050f), poly, u);
		const float32x4_t acos_u = vmulq_f32(vsqrtq_f32(vmaxq_f32(vsubq_f32(one, u), zero)), poly);
		const float32x4_t angle = vbslq_f32(vcltq_f32(cos_v, zero), vsubq_f32(pi_v, acos_u), acos_u);
		const float32x4_t norm_ang = vmulq_f32(angle, inv_pi_v);

		const float32x4_t ang_neg = vminq_f32(vmaxq_f32(vmulq_f32(vsubq_f32(one, norm_ang), half), zero), vdupq_n_f32(0.49999f));
		const float32x4_t ang_pos = vminq_f32(vmaxq_f32(vaddq_f32(vmulq_f32(norm_ang, half), half), vdupq_n_f32(0.500008f)), one);
		const float32x4_t r_ang = vbslq_f32(vcltq_f32(tw, zero), ang_neg, ang_pos);

		// Octahedral encode axis -> uint16x2.
		const float32x4_t scale = vaddq_f32(vaddq_f32(vabsq_f32(ax), vabsq_f32(ay)), vabsq_f32(az));
		const float32x4_t safe_scale = vbslq_f32(vcgtq_f32(scale, zero), scale, one);
		const float32x4_t px = vdivq_f32(ax, safe_scale);
		const float32x4_t py = vdivq_f32(ay, safe_scale);
		const uint32x4_t above = vcgeq_f32(az, zero);
		const float32x4_t fold_x = vmulq_f32(vsubq_f32(one, vabsq_f32(py)), vbslq_f32(vcgeq_f32(px, zero), one, neg_one));
		const float32x4_t fold_y = vmulq_f32(vsubq_f32(one, vabsq_f32(px)), vbslq_f32(vcgeq_f32(py, zero), one, neg_one));
		const float32x4_t oct_x = vaddq_f32(vmulq_f32(vbslq_f32(above, px, fold_x), half), half);
		const float32x4_t oct_y = vaddq_f32(vmulq_f32(vbslq_f32(above, py, fold_y), half), half);
		const uint16x4x2_t oct_u16 = { pack_u16(oct_x), pack_u16(oct_y) };
		vst2_u16(reinterpret_cast<uint16_t *>(r_vw + p_nrm_offset + i * 4), oct_u16);

		// Relative position + r_angle -> uint16x4.
		const float32x4_t rx = vmulq_f32(vsubq_f32(pos.val[0], aabb_px), inv_sx);
		const float32x4_t ry = vmulq_f32(vsubq_f32(pos.val[1], aabb_py), inv_sy);
		const float32x4_t rz = vmulq_f32(vsubq_f32(pos.val[2], aabb_pz), inv_sz);
		const uint16x4x4_t xyza_u16 = { pack_u16(rx), pack_u16(ry), pack_u16(rz), pack_u16(r_ang) };
		vst4_u16(reinterpret_cast<uint16_t *>(r_vw + p_vtx_offset + i * 8), xyza_u16);
	}

	for (; i < p_end; i++) {
		const Vector3 pv(p_positions[3 * size_t(i) + 0], p_positions[3 * size_t(i) + 1], p_positions[3 * size_t(i) + 2]);
		const Vector3 nv(p_normals[3 * size_t(i) + 0], p_normals[3 * size_t(i) + 1], p_normals[3 * size_t(i) + 2]);
		Vector4 tv;
		if (p_tangents) {
			tv = Vector4(p_tangents[4 * size_t(i) + 0], p_tangents[4 * size_t(i) + 1], p_tangents[4 * size_t(i) + 2], p_tangents[4 * size_t(i) + 3]);
		} else {
			const Vector3 c = Vector3(nv.z, -nv.x, nv.y).cross(nv.normalized()).normalized();
			tv = Vector4(c.x, c.y, c.z, 1.0f);
		}
		float angle = 0.0f;
		Vector3 axis;
		compute_tbn_axis_angle(nv, tv, angle, axis);
		pack_oct_direction(r_vw + p_nrm_offset + i * 4, axis.octahedron_encode(), false);
		const Vector3 rel = (pv - p_aabb_pos) * p_inv_aabb_size;
		const uint16_t xyza[4] = { pack_unorm16(rel.x), pack_unorm16(rel.y), pack_unorm16(rel.z), pack_unorm16(angle) };
		std::memcpy(r_vw + p_vtx_offset + i * 8, xyza, 8);
	}
}
#endif

struct MikktFastContext {
	const Vector3 *positions = nullptr;
	const Vector3 *normals = nullptr;
	const Vector2 *uvs = nullptr;
	const uint32_t *indices = nullptr;
	int face_count = 0;
	int vertex_count = 0;
	Vector4 *out_tangents = nullptr;
};

int mikkt_get_num_faces(const SMikkTSpaceContext *p_ctx) {
	return static_cast<const MikktFastContext *>(p_ctx->m_pUserData)->face_count;
}

int mikkt_get_num_verts_of_face(const SMikkTSpaceContext *, const int) {
	return 3;
}

void mikkt_get_position(const SMikkTSpaceContext *p_ctx, float r_pos[], const int p_face, const int p_vert) {
	const MikktFastContext *d = static_cast<const MikktFastContext *>(p_ctx->m_pUserData);
	const uint32_t idx = d->indices[p_face * 3 + p_vert];
	const Vector3 &v = d->positions[idx];
	r_pos[0] = v.x;
	r_pos[1] = v.y;
	r_pos[2] = v.z;
}

void mikkt_get_normal(const SMikkTSpaceContext *p_ctx, float r_norm[], const int p_face, const int p_vert) {
	const MikktFastContext *d = static_cast<const MikktFastContext *>(p_ctx->m_pUserData);
	const uint32_t idx = d->indices[p_face * 3 + p_vert];
	const Vector3 &n = d->normals[idx];
	r_norm[0] = n.x;
	r_norm[1] = n.y;
	r_norm[2] = n.z;
}

void mikkt_get_texcoord(const SMikkTSpaceContext *p_ctx, float r_uv[], const int p_face, const int p_vert) {
	const MikktFastContext *d = static_cast<const MikktFastContext *>(p_ctx->m_pUserData);
	const uint32_t idx = d->indices[p_face * 3 + p_vert];
	const Vector2 &uv = d->uvs[idx];
	r_uv[0] = uv.x;
	r_uv[1] = uv.y;
}

void mikkt_set_tspace_basic(const SMikkTSpaceContext *p_ctx, const float p_tan[], const float p_sign, const int p_face, const int p_vert) {
	MikktFastContext *d = static_cast<MikktFastContext *>(p_ctx->m_pUserData);
	const uint32_t idx = d->indices[p_face * 3 + p_vert];
	if (idx < uint32_t(d->vertex_count)) {
		// Match SurfaceTool::mikktSetTSpaceDefault + SurfaceTool::commit_to_arrays sign convention:
		// SurfaceTool negates fvBiTangent, so binormal_sign = -p_sign.
		d->out_tangents[idx] = Vector4(p_tan[0], p_tan[1], p_tan[2], -p_sign);
	}
}

} // namespace

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
		Ref<GLTFMesh> &r_out_gltf_mesh) {
	ERR_FAIL_COND_V(p_state.is_null(), ERR_INVALID_PARAMETER);

	Ref<GLTFMesh> gltf_mesh;
	gltf_mesh.instantiate();
	if (!p_original_mesh_name.is_empty()) {
		gltf_mesh->set_original_name(p_original_mesh_name);
	}
	gltf_mesh->set_name(p_unique_mesh_name);
	if (p_mesh_extras.get_type() == Variant::DICTIONARY) {
		Dictionary ext_dict = p_mesh_extras;
		if (!ext_dict.is_empty()) {
			gltf_mesh->set_meta("extras", ext_dict);
		}
	}

	Ref<ArrayMesh> array_mesh;
	array_mesh.instantiate();
	array_mesh->set_name(p_unique_mesh_name);
	if (gltf_mesh->has_meta("extras")) {
		array_mesh->set_meta("extras", gltf_mesh->get_meta("extras"));
	}

	// Determine total blend shape count across primitives.
	uint32_t mesh_target_count = 0;
	for (uint32_t pi = 0; pi < p_prims.size(); pi++) {
		if (p_prims[pi].targets.size() > mesh_target_count) {
			mesh_target_count = p_prims[pi].targets.size();
		}
	}
	if (mesh_target_count > 0) {
		array_mesh->set_blend_shape_mode(Mesh::BLEND_SHAPE_MODE_NORMALIZED);
		for (uint32_t k = 0; k < mesh_target_count; k++) {
			String bs_name;
			if (int(k) < p_target_names.size() && !p_target_names[int(k)].is_empty()) {
				bs_name = p_target_names[int(k)];
			} else {
				bs_name = String("morph_") + itos(k);
			}
			array_mesh->add_blend_shape(StringName(bs_name));
		}
	}

	TypedArray<Material> instance_materials;
	const Vector<Ref<Material>> &state_materials = p_state->get_materials();

	for (uint32_t prim_i = 0; prim_i < p_prims.size(); prim_i++) {
		const GLTFFastPrimSpec &prim = p_prims[prim_i];
		ERR_FAIL_COND_V(prim.mode < 0 || prim.mode >= 7, ERR_FILE_CORRUPT);
		ERR_FAIL_COND_V(prim.position < 0, ERR_PARSE_ERROR);

		FastAccessorView pos_acc;
		ERR_FAIL_COND_V(!pos_acc.init(p_state.ptr(), prim.position, false), ERR_PARSE_ERROR);
		const int32_t orig_vertex_num = int32_t(pos_acc.count);
		ERR_FAIL_COND_V(orig_vertex_num <= 0, ERR_INVALID_DECLARATION);

		const bool has_color = prim.color_0 >= 0;

		// Resolve material & UV coordinate channel mapping BEFORE attribute packing.
		Ref<Material> mat;
		String mat_name;
		int primary_uv_slot = 0;
		int secondary_uv_slot = 1;
		if (!p_discard_materials) {
			if (prim.material >= 0) {
				ERR_FAIL_INDEX_V(prim.material, state_materials.size(), ERR_FILE_CORRUPT);
				mat = state_materials[prim.material];
				ERR_FAIL_COND_V(mat.is_null(), ERR_FILE_CORRUPT);
				if (mat->has_meta("_gltf_primary_texture_coord")) {
					primary_uv_slot = int(mat->get_meta("_gltf_primary_texture_coord"));
					if (primary_uv_slot != 0 && !mat->has_meta("_gltf_secondary_texture_coord")) {
						secondary_uv_slot = 0;
					}
				}
				if (mat->has_meta("_gltf_secondary_texture_coord")) {
					secondary_uv_slot = int(mat->get_meta("_gltf_secondary_texture_coord"));
				}
				Ref<BaseMaterial3D> base_mat = mat;
				if (has_color && base_mat.is_valid()) {
					base_mat->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
				}
			} else {
				Ref<StandardMaterial3D> default_mat;
				default_mat.instantiate();
				if (has_color) {
					default_mat->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
				}
				mat = default_mat;
			}
			instance_materials.append(mat);
			mat_name = mat->get_name();
		}

		// Decode & expand topology indices (supporting all 7 glTF modes).
		Mesh::PrimitiveType godot_prim = Mesh::PRIMITIVE_TRIANGLES;
		LocalVector<uint32_t> expanded_indices;
		bool has_index_buffer = false;
		bool defer_direct_tri_indices = false;
		int32_t direct_index_count = 0;

		auto read_raw_index = [&](const FastAccessorView *p_idx_acc, int64_t k) -> uint32_t {
			return p_idx_acc ? p_idx_acc->read_index(k) : uint32_t(k);
		};

		FastAccessorView idx_acc;
		const FastAccessorView *idx_acc_ptr = nullptr;
		int64_t raw_topo_count = orig_vertex_num;
		if (prim.indices >= 0) {
			ERR_FAIL_COND_V(!idx_acc.init(p_state.ptr(), prim.indices, true), ERR_PARSE_ERROR);
			idx_acc_ptr = &idx_acc;
			raw_topo_count = idx_acc.count;
		}

		const bool tight_tri_u16 = (prim.mode == 4 && idx_acc_ptr && !idx_acc.is_materialized_u32 && !idx_acc.is_zero && idx_acc.data &&
				idx_acc.comp_type == GLTFAccessor::COMPONENT_TYPE_UNSIGNED_SHORT && idx_acc.stride == 2);
		const bool tight_tri_u32 = (prim.mode == 4 && idx_acc_ptr && !idx_acc.is_materialized_u32 && !idx_acc.is_zero && idx_acc.data &&
				idx_acc.comp_type == GLTFAccessor::COMPONENT_TYPE_UNSIGNED_INT && idx_acc.stride == 4);

		switch (prim.mode) {
			case 0: { // POINTS
				godot_prim = Mesh::PRIMITIVE_POINTS;
				if (idx_acc_ptr) {
					has_index_buffer = raw_topo_count > 0;
					expanded_indices.resize(uint32_t(raw_topo_count));
					for (int64_t k = 0; k < raw_topo_count; k++) {
						expanded_indices[uint32_t(k)] = idx_acc.read_index(k);
					}
				}
			} break;
			case 1: { // LINES
				godot_prim = Mesh::PRIMITIVE_LINES;
				ERR_FAIL_COND_V(raw_topo_count < 2 || (raw_topo_count % 2) != 0, ERR_PARSE_ERROR);
				if (idx_acc_ptr) {
					has_index_buffer = true;
					expanded_indices.resize(uint32_t(raw_topo_count));
					for (int64_t k = 0; k < raw_topo_count; k++) {
						expanded_indices[uint32_t(k)] = idx_acc.read_index(k);
					}
				}
			} break;
			case 2: { // LINE_LOOP -> expand to PRIMITIVE_LINES (2 * N indices)
				godot_prim = Mesh::PRIMITIVE_LINES;
				ERR_FAIL_COND_V(raw_topo_count < 2, ERR_PARSE_ERROR);
				has_index_buffer = true;
				expanded_indices.resize(uint32_t(raw_topo_count * 2));
				for (int64_t k = 0; k < raw_topo_count; k++) {
					const uint32_t a = read_raw_index(idx_acc_ptr, k);
					const uint32_t b = read_raw_index(idx_acc_ptr, (k + 1) % raw_topo_count);
					expanded_indices[uint32_t(k * 2 + 0)] = a;
					expanded_indices[uint32_t(k * 2 + 1)] = b;
				}
			} break;
			case 3: { // LINE_STRIP
				godot_prim = Mesh::PRIMITIVE_LINE_STRIP;
				ERR_FAIL_COND_V(raw_topo_count < 2, ERR_PARSE_ERROR);
				if (idx_acc_ptr) {
					has_index_buffer = true;
					expanded_indices.resize(uint32_t(raw_topo_count));
					for (int64_t k = 0; k < raw_topo_count; k++) {
						expanded_indices[uint32_t(k)] = idx_acc.read_index(k);
					}
				}
			} break;
			case 4: { // TRIANGLES (CCW -> CW: swap i1 & i2)
				godot_prim = Mesh::PRIMITIVE_TRIANGLES;
				ERR_FAIL_COND_V(raw_topo_count < 3 || (raw_topo_count % 3) != 0, ERR_PARSE_ERROR);
				has_index_buffer = true;
				if ((tight_tri_u16 || tight_tri_u32) && !p_force_generate_tangents) {
					defer_direct_tri_indices = true;
					direct_index_count = int32_t(raw_topo_count);
				} else {
					expanded_indices.resize(uint32_t(raw_topo_count));
					uint32_t *iw = expanded_indices.ptr();
					if (idx_acc_ptr) {
						if (tight_tri_u16) {
							const uint16_t *src16 = reinterpret_cast<const uint16_t *>(idx_acc.data);
							for (int64_t k = 0; k < raw_topo_count; k += 3) {
								iw[k + 0] = src16[k + 0];
								iw[k + 1] = src16[k + 2];
								iw[k + 2] = src16[k + 1];
							}
						} else if (tight_tri_u32) {
							const uint32_t *src32 = reinterpret_cast<const uint32_t *>(idx_acc.data);
							for (int64_t k = 0; k < raw_topo_count; k += 3) {
								iw[k + 0] = src32[k + 0];
								iw[k + 1] = src32[k + 2];
								iw[k + 2] = src32[k + 1];
							}
						} else {
							for (int64_t k = 0; k < raw_topo_count; k += 3) {
								iw[k + 0] = idx_acc.read_index(k + 0);
								iw[k + 1] = idx_acc.read_index(k + 2);
								iw[k + 2] = idx_acc.read_index(k + 1);
							}
						}
					} else {
						for (int64_t k = 0; k < raw_topo_count; k += 3) {
							iw[k + 0] = uint32_t(k + 0);
							iw[k + 1] = uint32_t(k + 2);
							iw[k + 2] = uint32_t(k + 1);
						}
					}
				}
			} break;
			case 5: { // TRIANGLE_STRIP -> expand to PRIMITIVE_TRIANGLES (CW)
				godot_prim = Mesh::PRIMITIVE_TRIANGLES;
				ERR_FAIL_COND_V(raw_topo_count < 3, ERR_PARSE_ERROR);
				has_index_buffer = true;
				const int64_t tri_cnt = raw_topo_count - 2;
				expanded_indices.resize(uint32_t(tri_cnt * 3));
				uint32_t *iw = expanded_indices.ptr();
				for (int64_t t = 0; t < tri_cnt; t++) {
					const uint32_t i0 = read_raw_index(idx_acc_ptr, t);
					const uint32_t i1 = read_raw_index(idx_acc_ptr, t + 1);
					const uint32_t i2 = read_raw_index(idx_acc_ptr, t + 2);
					if ((t & 1) == 0) {
						iw[t * 3 + 0] = i0;
						iw[t * 3 + 1] = i2;
						iw[t * 3 + 2] = i1;
					} else {
						iw[t * 3 + 0] = i0;
						iw[t * 3 + 1] = i1;
						iw[t * 3 + 2] = i2;
					}
				}
			} break;
			case 6: { // TRIANGLE_FAN -> expand to PRIMITIVE_TRIANGLES (CW)
				godot_prim = Mesh::PRIMITIVE_TRIANGLES;
				ERR_FAIL_COND_V(raw_topo_count < 3, ERR_PARSE_ERROR);
				has_index_buffer = true;
				const int64_t tri_cnt = raw_topo_count - 2;
				expanded_indices.resize(uint32_t(tri_cnt * 3));
				uint32_t *iw = expanded_indices.ptr();
				const uint32_t center = read_raw_index(idx_acc_ptr, 0);
				for (int64_t t = 0; t < tri_cnt; t++) {
					const uint32_t i1 = read_raw_index(idx_acc_ptr, t + 1);
					const uint32_t i2 = read_raw_index(idx_acc_ptr, t + 2);
					iw[t * 3 + 0] = center;
					iw[t * 3 + 1] = i2;
					iw[t * 3 + 2] = i1;
				}
			} break;
		}

		// Determine active vertex subset via 64-bit bitset (zero-remap when all vertices are used).
		bool identity_mapping = true;
		int32_t vertex_num = orig_vertex_num;
		LocalVector<int32_t> source_vertices;

		if (has_index_buffer && idx_acc_ptr != nullptr && !(prim.dense_verts_verified && defer_direct_tri_indices)) {
			const uint32_t word_count = uint32_t((orig_vertex_num + 63) >> 6);
			LocalVector<uint64_t> used_bits;
			used_bits.resize(word_count);
			std::memset(used_bits.ptr(), 0, word_count * sizeof(uint64_t));

			int32_t unique_used = 0;
			uint64_t *bw = used_bits.ptr();
			if (defer_direct_tri_indices) {
				if (tight_tri_u16) {
					const uint16_t *src16 = reinterpret_cast<const uint16_t *>(idx_acc.data);
					for (int32_t k = 0; k < direct_index_count; k++) {
						const uint32_t vi = src16[k];
						ERR_FAIL_COND_V(vi >= uint32_t(orig_vertex_num), ERR_INVALID_DATA);
						const uint64_t mask = 1ULL << (vi & 63u);
						uint64_t &word = bw[vi >> 6];
						if ((word & mask) == 0) {
							word |= mask;
							unique_used++;
						}
					}
				} else {
					const uint32_t *src32 = reinterpret_cast<const uint32_t *>(idx_acc.data);
					for (int32_t k = 0; k < direct_index_count; k++) {
						const uint32_t vi = src32[k];
						ERR_FAIL_COND_V(vi >= uint32_t(orig_vertex_num), ERR_INVALID_DATA);
						const uint64_t mask = 1ULL << (vi & 63u);
						uint64_t &word = bw[vi >> 6];
						if ((word & mask) == 0) {
							word |= mask;
							unique_used++;
						}
					}
				}
			} else {
				const uint32_t idx_cnt = expanded_indices.size();
				const uint32_t *ir = expanded_indices.ptr();
				for (uint32_t k = 0; k < idx_cnt; k++) {
					const uint32_t vi = ir[k];
					ERR_FAIL_COND_V(vi >= uint32_t(orig_vertex_num), ERR_INVALID_DATA);
					const uint64_t mask = 1ULL << (vi & 63u);
					uint64_t &word = bw[vi >> 6];
					if ((word & mask) == 0) {
						word |= mask;
						unique_used++;
					}
				}
			}
			ERR_FAIL_COND_V(unique_used <= 0, ERR_INVALID_DECLARATION);

			if (unique_used < orig_vertex_num) {
				identity_mapping = false;
				vertex_num = unique_used;
				source_vertices.resize(uint32_t(vertex_num));
				LocalVector<uint32_t> old_to_new;
				old_to_new.resize(uint32_t(orig_vertex_num));
				int32_t out_v = 0;
				for (int32_t v = 0; v < orig_vertex_num; v++) {
					if (bw[uint32_t(v) >> 6] & (1ULL << (uint32_t(v) & 63u))) {
						old_to_new[uint32_t(v)] = uint32_t(out_v);
						source_vertices[uint32_t(out_v)] = v;
						out_v++;
					}
				}
				if (defer_direct_tri_indices) {
					defer_direct_tri_indices = false;
					expanded_indices.resize(uint32_t(direct_index_count));
					uint32_t *iw = expanded_indices.ptr();
					if (tight_tri_u16) {
						const uint16_t *src16 = reinterpret_cast<const uint16_t *>(idx_acc.data);
						for (int32_t k = 0; k < direct_index_count; k += 3) {
							iw[k + 0] = old_to_new[src16[k + 0]];
							iw[k + 1] = old_to_new[src16[k + 2]];
							iw[k + 2] = old_to_new[src16[k + 1]];
						}
					} else {
						const uint32_t *src32 = reinterpret_cast<const uint32_t *>(idx_acc.data);
						for (int32_t k = 0; k < direct_index_count; k += 3) {
							iw[k + 0] = old_to_new[src32[k + 0]];
							iw[k + 1] = old_to_new[src32[k + 2]];
							iw[k + 2] = old_to_new[src32[k + 1]];
						}
					}
				} else {
					const uint32_t idx_cnt = expanded_indices.size();
					uint32_t *iw = expanded_indices.ptr();
					for (uint32_t k = 0; k < idx_cnt; k++) {
						iw[k] = old_to_new[iw[k]];
					}
				}
			}
		}

		// Initialize optional attribute accessors.
		FastAccessorView norm_acc, tan_acc, col_acc, uv0_acc, uv1_acc;
		FastAccessorView j0_acc, j1_acc, w0_acc, w1_acc;
		const bool has_normal = prim.normal >= 0 && norm_acc.init(p_state.ptr(), prim.normal, false) && norm_acc.count >= orig_vertex_num;
		const bool has_src_tangent = prim.tangent >= 0 && tan_acc.init(p_state.ptr(), prim.tangent, false) && tan_acc.count >= orig_vertex_num;
		const bool valid_color = has_color && col_acc.init(p_state.ptr(), prim.color_0, false) && col_acc.count >= orig_vertex_num;

		const int32_t uv0_acc_idx = (primary_uv_slot >= 0 && primary_uv_slot < 8) ? prim.texcoord[primary_uv_slot] : -1;
		const int32_t uv1_acc_idx = (secondary_uv_slot >= 0 && secondary_uv_slot < 8 && secondary_uv_slot != primary_uv_slot) ? prim.texcoord[secondary_uv_slot] : -1;
		const bool has_uv0 = uv0_acc_idx >= 0 && uv0_acc.init(p_state.ptr(), uv0_acc_idx, false) && uv0_acc.count >= orig_vertex_num;
		const bool has_uv1 = uv1_acc_idx >= 0 && uv1_acc.init(p_state.ptr(), uv1_acc_idx, false) && uv1_acc.count >= orig_vertex_num;

		FastAccessorView extra_uv_acc[6];
		bool has_extra_uv[6] = { false, false, false, false, false, false };
		for (int t = 0; t < 6; t++) {
			const int32_t acc_i = prim.texcoord[2 + t];
			if (acc_i >= 0 && extra_uv_acc[t].init(p_state.ptr(), acc_i, false) && extra_uv_acc[t].count >= orig_vertex_num) {
				has_extra_uv[t] = true;
			}
		}

		const bool has_joints_0 = prim.joints_0 >= 0 && j0_acc.init(p_state.ptr(), prim.joints_0, true) && j0_acc.count >= orig_vertex_num;
		const bool has_joints_1 = has_joints_0 && prim.joints_1 >= 0 && j1_acc.init(p_state.ptr(), prim.joints_1, true) && j1_acc.count >= orig_vertex_num;
		const bool has_weights_0 = prim.weights_0 >= 0 && w0_acc.init(p_state.ptr(), prim.weights_0, false) && w0_acc.count >= orig_vertex_num;
		const bool has_weights_1 = has_weights_0 && prim.weights_1 >= 0 && w1_acc.init(p_state.ptr(), prim.weights_1, false) && w1_acc.count >= orig_vertex_num;
		const bool use_8_bones = has_joints_0 && has_joints_1 && has_weights_0 && has_weights_1;

		// Zero-copy position span when tight float32 & identity mapping; materialize only otherwise.
		LocalVector<Vector3> positions_owned;
		const Vector3 *pos_ptr = nullptr;
		if (identity_mapping && pos_acc.is_tight_f32(3) && sizeof(Vector3) == 12) {
			pos_ptr = reinterpret_cast<const Vector3 *>(pos_acc.data);
		} else {
			positions_owned.resize(uint32_t(vertex_num));
			for (int32_t i = 0; i < vertex_num; i++) {
				const int32_t src_v = identity_mapping ? i : source_vertices[uint32_t(i)];
				positions_owned[uint32_t(i)] = pos_acc.read_vec3(src_v);
			}
			pos_ptr = positions_owned.ptr();
		}

		Vector3 aabb_min = pos_ptr[0];
		Vector3 aabb_max = pos_ptr[0];
		int32_t aabb_i = 1;
#if defined(__aarch64__) || defined(_M_ARM64)
		if (sizeof(Vector3) == 12 && vertex_num >= 16) {
			const float *pf = reinterpret_cast<const float *>(pos_ptr);
			float32x4_t min_x = vdupq_n_f32(aabb_min.x), max_x = min_x;
			float32x4_t min_y = vdupq_n_f32(aabb_min.y), max_y = min_y;
			float32x4_t min_z = vdupq_n_f32(aabb_min.z), max_z = min_z;
			aabb_i = 0;
			for (; aabb_i + 4 <= vertex_num; aabb_i += 4) {
				const float32x4x3_t v = vld3q_f32(pf + 3 * size_t(aabb_i));
				min_x = vminq_f32(min_x, v.val[0]);
				max_x = vmaxq_f32(max_x, v.val[0]);
				min_y = vminq_f32(min_y, v.val[1]);
				max_y = vmaxq_f32(max_y, v.val[1]);
				min_z = vminq_f32(min_z, v.val[2]);
				max_z = vmaxq_f32(max_z, v.val[2]);
			}
			aabb_min = Vector3(vminvq_f32(min_x), vminvq_f32(min_y), vminvq_f32(min_z));
			aabb_max = Vector3(vmaxvq_f32(max_x), vmaxvq_f32(max_y), vmaxvq_f32(max_z));
		}
#endif
		for (; aabb_i < vertex_num; aabb_i++) {
			const Vector3 &p = pos_ptr[uint32_t(aabb_i)];
			aabb_min = aabb_min.min(p);
			aabb_max = aabb_max.max(p);
		}
		const Vector3 small_vec3(CMP_EPSILON, CMP_EPSILON, CMP_EPSILON);
		AABB surface_aabb(aabb_min, (aabb_max - aabb_min).max(small_vec3));
		const bool is_mesh_2d = Math::is_zero_approx(aabb_min.z) && Math::is_zero_approx(aabb_max.z);

		// Optional MikkTSpace tangent generation when requested by import flags.
		const bool want_mikkt_tangents = p_force_generate_tangents && godot_prim == Mesh::PRIMITIVE_TRIANGLES && !has_src_tangent && has_normal && has_uv0 && has_index_buffer;
		LocalVector<Vector3> cached_normals;
		LocalVector<Vector2> cached_uv0;
		LocalVector<Vector4> generated_tangents;
		const Vector3 *norm_ptr_direct = (identity_mapping && has_normal && norm_acc.is_tight_f32(3) && sizeof(Vector3) == 12)
				? reinterpret_cast<const Vector3 *>(norm_acc.data)
				: nullptr;
		if (want_mikkt_tangents || (mesh_target_count > 0 && has_normal && !norm_ptr_direct)) {
			cached_normals.resize(uint32_t(vertex_num));
			if (norm_ptr_direct) {
				std::memcpy(cached_normals.ptr(), norm_ptr_direct, size_t(vertex_num) * 12);
			} else {
				for (int32_t i = 0; i < vertex_num; i++) {
					const int32_t src_v = identity_mapping ? i : source_vertices[uint32_t(i)];
					cached_normals[uint32_t(i)] = norm_acc.read_vec3(src_v);
				}
			}
		}
		if (want_mikkt_tangents) {
			cached_uv0.resize(uint32_t(vertex_num));
			for (int32_t i = 0; i < vertex_num; i++) {
				const int32_t src_v = identity_mapping ? i : source_vertices[uint32_t(i)];
				cached_uv0[uint32_t(i)] = uv0_acc.read_vec2(src_v);
			}
			generated_tangents.resize(uint32_t(vertex_num));
			for (int32_t i = 0; i < vertex_num; i++) {
				generated_tangents[uint32_t(i)] = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
			}
			MikktFastContext mctx;
			mctx.positions = pos_ptr;
			mctx.normals = cached_normals.ptr();
			mctx.uvs = cached_uv0.ptr();
			mctx.indices = expanded_indices.ptr();
			mctx.face_count = int(expanded_indices.size() / 3);
			mctx.vertex_count = vertex_num;
			mctx.out_tangents = generated_tangents.ptr();

			SMikkTSpaceInterface mkif = {};
			mkif.m_getNumFaces = mikkt_get_num_faces;
			mkif.m_getNumVerticesOfFace = mikkt_get_num_verts_of_face;
			mkif.m_getPosition = mikkt_get_position;
			mkif.m_getNormal = mikkt_get_normal;
			mkif.m_getTexCoord = mikkt_get_texcoord;
			mkif.m_setTSpaceBasic = mikkt_set_tspace_basic;

			SMikkTSpaceContext msc = {};
			msc.m_pInterface = &mkif;
			msc.m_pUserData = &mctx;
			genTangSpaceDefault(&msc);
		}

		const bool has_effective_tangent = has_src_tangent || !generated_tangents.is_empty();

		// Determine whether compressed static 3D attributes can be used.
		bool use_compressed = !p_force_disable_compression && !is_mesh_2d && has_normal &&
				mesh_target_count == 0 && !has_joints_0 && !has_weights_0;
		if (use_compressed && has_effective_tangent) {
			for (int32_t i = 0; i < vertex_num; i++) {
				const int32_t src_v = identity_mapping ? i : source_vertices[uint32_t(i)];
				const Vector3 n = norm_ptr_direct ? norm_ptr_direct[i] : (cached_normals.is_empty() ? norm_acc.read_vec3(src_v) : cached_normals[uint32_t(i)]);
				const Vector4 t = !generated_tangents.is_empty() ? generated_tangents[uint32_t(i)] : tan_acc.read_vec4(src_v, 1.0f);
				if (std::abs(Vector3(t.x, t.y, t.z).dot(n)) > 0.0001f) {
					use_compressed = false;
					break;
				}
			}
		}

		// Assemble exact Godot ArrayFormat bitmask.
		uint64_t format = RSE::ARRAY_FLAG_FORMAT_CURRENT_VERSION | RSE::ARRAY_FORMAT_VERTEX;
		if (has_normal) {
			format |= RSE::ARRAY_FORMAT_NORMAL | RSE::ARRAY_FORMAT_TANGENT;
		} else if (has_effective_tangent) {
			format |= RSE::ARRAY_FORMAT_TANGENT;
		}
		if (valid_color) {
			format |= RSE::ARRAY_FORMAT_COLOR;
		}
		if (has_uv0) {
			format |= RSE::ARRAY_FORMAT_TEX_UV;
		}
		if (has_uv1) {
			format |= RSE::ARRAY_FORMAT_TEX_UV2;
		}
		int custom_channels[3] = { 0, 0, 0 };
		for (int ci = 0; ci < 3; ci++) {
			if (has_extra_uv[ci * 2 + 1]) {
				custom_channels[ci] = 4;
			} else if (has_extra_uv[ci * 2]) {
				custom_channels[ci] = 2;
			} else {
				break;
			}
			format |= (1ULL << (RSE::ARRAY_CUSTOM0 + ci));
			const uint64_t cfmt = (custom_channels[ci] == 2) ? RSE::ARRAY_CUSTOM_RG_FLOAT : RSE::ARRAY_CUSTOM_RGBA_FLOAT;
			format |= cfmt << (RSE::ARRAY_FORMAT_CUSTOM_BASE + ci * RSE::ARRAY_FORMAT_CUSTOM_BITS);
		}
		if (has_joints_0) {
			format |= RSE::ARRAY_FORMAT_BONES;
		}
		if (has_weights_0) {
			format |= RSE::ARRAY_FORMAT_WEIGHTS;
		}
		if (use_8_bones) {
			format |= RSE::ARRAY_FLAG_USE_8_BONE_WEIGHTS;
		}
		const int32_t index_count = has_index_buffer ? (defer_direct_tri_indices ? direct_index_count : int32_t(expanded_indices.size())) : 0;
		if (index_count > 0) {
			format |= RSE::ARRAY_FORMAT_INDEX;
		}
		if (use_compressed) {
			format |= RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
		}

		uint32_t offsets[RSE::ARRAY_MAX];
		uint32_t vertex_elem_size = 0;
		uint32_t normal_elem_size = 0;
		uint32_t attrib_elem_size = 0;
		uint32_t skin_elem_size = 0;
		RenderingServer::get_singleton()->mesh_surface_make_offsets_from_format(
				format, vertex_num, index_count, offsets,
				vertex_elem_size, normal_elem_size, attrib_elem_size, skin_elem_size);

		const int32_t vertex_array_size = int32_t(vertex_elem_size + normal_elem_size) * vertex_num;
		const int32_t attrib_array_size = int32_t(attrib_elem_size) * vertex_num;
		const int32_t skin_array_size = int32_t(skin_elem_size) * vertex_num;
		const int32_t index_array_size = (index_count > 0) ? int32_t(offsets[RSE::ARRAY_INDEX]) * index_count : 0;

		Vector<uint8_t> vertex_array;
		vertex_array.resize(vertex_array_size);
		Vector<uint8_t> attrib_array;
		attrib_array.resize(attrib_array_size);
		Vector<uint8_t> skin_array;
		skin_array.resize(skin_array_size);
		Vector<uint8_t> index_array;
		index_array.resize(index_array_size);

		uint8_t *vw = vertex_array.ptrw();
		uint8_t *aw = attrib_array.ptrw();
		uint8_t *sw = skin_array.ptrw();

		// Precompute UV scale if using compressed UVs (ARM NEON 4-wide vector reduction on tight float32 UVs).
		Vector4 uv_scale(0.0f, 0.0f, 0.0f, 0.0f);
		if (use_compressed && (has_uv0 || has_uv1)) {
			Vector2 min0(0.0f, 0.0f), max0(0.0f, 0.0f);
			Vector2 min1(0.0f, 0.0f), max1(0.0f, 0.0f);
#if defined(__aarch64__) || defined(_M_ARM64)
			auto reduce_uv_minmax_neon = [](const float *p_uv, int32_t p_cnt, Vector2 &r_min, Vector2 &r_max) {
				int32_t k = 0;
				if (p_cnt >= 4) {
					float32x4_t vmin0 = vld1q_f32(p_uv);
					float32x4_t vmax0 = vmin0;
					float32x4_t vmin1 = vld1q_f32(p_uv + 4);
					float32x4_t vmax1 = vmin1;
					k = 4;
					for (; k + 4 <= p_cnt; k += 4) {
						const float32x4_t a = vld1q_f32(p_uv + 2 * size_t(k));
						const float32x4_t b = vld1q_f32(p_uv + 2 * size_t(k) + 4);
						vmin0 = vminq_f32(vmin0, a);
						vmax0 = vmaxq_f32(vmax0, a);
						vmin1 = vminq_f32(vmin1, b);
						vmax1 = vmaxq_f32(vmax1, b);
					}
					const float32x4_t vmin = vminq_f32(vmin0, vmin1);
					const float32x4_t vmax = vmaxq_f32(vmax0, vmax1);
					float mn[4], mx[4];
					vst1q_f32(mn, vmin);
					vst1q_f32(mx, vmax);
					r_min = Vector2(MIN(mn[0], mn[2]), MIN(mn[1], mn[3])).min(Vector2(0.0f, 0.0f));
					r_max = Vector2(MAX(mx[0], mx[2]), MAX(mx[1], mx[3])).max(Vector2(0.0f, 0.0f));
				}
				for (; k < p_cnt; k++) {
					const Vector2 uv(p_uv[2 * size_t(k)], p_uv[2 * size_t(k) + 1]);
					r_min = r_min.min(uv);
					r_max = r_max.max(uv);
				}
			};
			if (identity_mapping && (!has_uv0 || uv0_acc.is_tight_f32(2)) && (!has_uv1 || uv1_acc.is_tight_f32(2))) {
				if (has_uv0) {
					reduce_uv_minmax_neon(reinterpret_cast<const float *>(uv0_acc.data), vertex_num, min0, max0);
				}
				if (has_uv1) {
					reduce_uv_minmax_neon(reinterpret_cast<const float *>(uv1_acc.data), vertex_num, min1, max1);
				}
			} else
#endif
			{
				for (int32_t i = 0; i < vertex_num; i++) {
					const int32_t src_v = identity_mapping ? i : source_vertices[uint32_t(i)];
					if (has_uv0) {
						const Vector2 uv = uv0_acc.read_vec2(src_v);
						min0 = min0.min(uv);
						max0 = max0.max(uv);
					}
					if (has_uv1) {
						const Vector2 uv2 = uv1_acc.read_vec2(src_v);
						min1 = min1.min(uv2);
						max1 = max1.max(uv2);
					}
				}
			}
			max0 = max0.abs().max(min0.abs());
			max1 = max1.abs().max(min1.abs());
			if (!(min0.x >= 0.0f && min1.x >= 0.0f && max0.x <= 1.0f && max1.x <= 1.0f &&
						min0.y >= 0.0f && min1.y >= 0.0f && max0.y <= 1.0f && max1.y <= 1.0f)) {
				uv_scale = Vector4(max0.x * 2.0f, max0.y * 2.0f, max1.x * 2.0f, max1.y * 2.0f);
			}
		}
		const bool has_nonzero_uv_scale = !uv_scale.is_zero_approx();

		const bool fast_memcpy_uv0 = (attrib_elem_size == 8 && !valid_color && !has_uv1 && custom_channels[0] == 0 &&
				has_uv0 && !use_compressed && identity_mapping && uv0_acc.is_tight_f32(2));
		if (fast_memcpy_uv0) {
			std::memcpy(aw, uv0_acc.data, size_t(vertex_num) * 8);
		}

#if defined(__aarch64__) || defined(_M_ARM64)
		const bool fast_neon_uv0_u16 = (use_compressed && attrib_elem_size == 4 && offsets[RSE::ARRAY_TEX_UV] == 0 &&
				!valid_color && !has_uv1 && custom_channels[0] == 0 && has_uv0 &&
				identity_mapping && cached_uv0.is_empty() && uv0_acc.is_tight_f32(2));
#endif

		auto pack_attrib_range = [&](uint32_t p_begin, uint32_t p_end) {
			if (attrib_elem_size == 0 || fast_memcpy_uv0) {
				return;
			}
			uint32_t i = p_begin;
#if defined(__aarch64__) || defined(_M_ARM64)
			if (fast_neon_uv0_u16) {
				const float *src_uv = reinterpret_cast<const float *>(uv0_acc.data);
				const float32x4_t zero = vdupq_n_f32(0.0f);
				const float32x4_t one = vdupq_n_f32(1.0f);
				if (has_nonzero_uv_scale) {
					const float inv_x = uv_scale.x > 0.0f ? (1.0f / uv_scale.x) : 0.0f;
					const float inv_y = uv_scale.y > 0.0f ? (1.0f / uv_scale.y) : 0.0f;
					const float sc_arr[4] = { inv_x, inv_y, inv_x, inv_y };
					const float32x4_t sc_v = vld1q_f32(sc_arr);
					const float32x4_t half = vdupq_n_f32(0.5f);
					for (; i + 4 <= p_end; i += 4) {
						const float32x4_t u0 = vminq_f32(vmaxq_f32(vfmaq_f32(half, vld1q_f32(src_uv + 2 * size_t(i)), sc_v), zero), one);
						const float32x4_t u1 = vminq_f32(vmaxq_f32(vfmaq_f32(half, vld1q_f32(src_uv + 2 * size_t(i) + 4), sc_v), zero), one);
						const uint16x4_t q0 = vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(u0, 65535.0f)));
						const uint16x4_t q1 = vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(u1, 65535.0f)));
						vst1q_u16(reinterpret_cast<uint16_t *>(aw + size_t(i) * 4), vcombine_u16(q0, q1));
					}
				} else {
					for (; i + 4 <= p_end; i += 4) {
						const float32x4_t u0 = vminq_f32(vmaxq_f32(vld1q_f32(src_uv + 2 * size_t(i)), zero), one);
						const float32x4_t u1 = vminq_f32(vmaxq_f32(vld1q_f32(src_uv + 2 * size_t(i) + 4), zero), one);
						const uint16x4_t q0 = vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(u0, 65535.0f)));
						const uint16x4_t q1 = vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(u1, 65535.0f)));
						vst1q_u16(reinterpret_cast<uint16_t *>(aw + size_t(i) * 4), vcombine_u16(q0, q1));
					}
				}
			}
#endif
			for (; i < p_end; i++) {
				const int32_t src_v = identity_mapping ? int32_t(i) : source_vertices[i];
				uint8_t *dst_a = aw + i * attrib_elem_size;
				if (valid_color) {
					const Vector4 c = col_acc.read_vec4(src_v, 1.0f);
					const uint8_t rgba[4] = {
						uint8_t(CLAMP(c.x * 255.0f, 0.0f, 255.0f)),
						uint8_t(CLAMP(c.y * 255.0f, 0.0f, 255.0f)),
						uint8_t(CLAMP(c.z * 255.0f, 0.0f, 255.0f)),
						uint8_t(CLAMP(c.w * 255.0f, 0.0f, 255.0f)),
					};
					std::memcpy(dst_a + offsets[RSE::ARRAY_COLOR], rgba, 4);
				}
				if (has_uv0) {
					Vector2 uv = cached_uv0.is_empty() ? uv0_acc.read_vec2(src_v) : cached_uv0[i];
					if (use_compressed) {
						if (has_nonzero_uv_scale) {
							uv.x = uv_scale.x > 0.0f ? (uv.x / uv_scale.x + 0.5f) : 0.5f;
							uv.y = uv_scale.y > 0.0f ? (uv.y / uv_scale.y + 0.5f) : 0.5f;
						}
						const uint16_t uv16[2] = { pack_unorm16(uv.x), pack_unorm16(uv.y) };
						std::memcpy(dst_a + offsets[RSE::ARRAY_TEX_UV], uv16, 4);
					} else {
						const float uvf[2] = { float(uv.x), float(uv.y) };
						std::memcpy(dst_a + offsets[RSE::ARRAY_TEX_UV], uvf, 8);
					}
				}
				if (has_uv1) {
					Vector2 uv2 = uv1_acc.read_vec2(src_v);
					if (use_compressed) {
						if (has_nonzero_uv_scale) {
							uv2.x = uv_scale.z > 0.0f ? (uv2.x / uv_scale.z + 0.5f) : 0.5f;
							uv2.y = uv_scale.w > 0.0f ? (uv2.y / uv_scale.w + 0.5f) : 0.5f;
						}
						const uint16_t uv16[2] = { pack_unorm16(uv2.x), pack_unorm16(uv2.y) };
						std::memcpy(dst_a + offsets[RSE::ARRAY_TEX_UV2], uv16, 4);
					} else {
						const float uvf[2] = { float(uv2.x), float(uv2.y) };
						std::memcpy(dst_a + offsets[RSE::ARRAY_TEX_UV2], uvf, 8);
					}
				}
				for (int ci = 0; ci < 3 && custom_channels[ci] > 0; ci++) {
					const Vector2 first_uv = has_extra_uv[ci * 2] ? extra_uv_acc[ci * 2].read_vec2(src_v) : Vector2();
					if (custom_channels[ci] == 2) {
						const float f2[2] = { float(first_uv.x), float(first_uv.y) };
						std::memcpy(dst_a + offsets[RSE::ARRAY_CUSTOM0 + ci], f2, 8);
					} else {
						const Vector2 second_uv = has_extra_uv[ci * 2 + 1] ? extra_uv_acc[ci * 2 + 1].read_vec2(src_v) : Vector2();
						const float f4[4] = { float(first_uv.x), float(first_uv.y), float(second_uv.x), float(second_uv.y) };
						std::memcpy(dst_a + offsets[RSE::ARRAY_CUSTOM0 + ci], f4, 16);
					}
				}
			}
		};

		// Pack vertex + normal + tangent + attribute buffers (parallelized across LoadThreadPool <= 3 threads).
		if (use_compressed) {
			const Vector3 aabb_pos = surface_aabb.position;
			const Vector3 inv_aabb_size(
					1.0f / surface_aabb.size.x,
					1.0f / surface_aabb.size.y,
					1.0f / surface_aabb.size.z);
#if defined(__aarch64__) || defined(_M_ARM64)
			const bool can_neon_compressed = identity_mapping && sizeof(Vector3) == 12 &&
					norm_ptr_direct != nullptr && vertex_elem_size == 8 && normal_elem_size == 4 &&
					(!has_effective_tangent || (!generated_tangents.is_empty() && sizeof(Vector4) == 16) || tan_acc.is_tight_f32(4));
			const float *tan_ptr_neon = !generated_tangents.is_empty()
					? reinterpret_cast<const float *>(generated_tangents.ptr())
					: (has_src_tangent ? reinterpret_cast<const float *>(tan_acc.data) : nullptr);
#endif
			auto pack_compressed_range = [&](uint32_t p_begin, uint32_t p_end) {
#if defined(__aarch64__) || defined(_M_ARM64)
				if (can_neon_compressed) {
					pack_compressed_tbn_range_neon(
							vw,
							offsets[RSE::ARRAY_VERTEX],
							offsets[RSE::ARRAY_NORMAL],
							reinterpret_cast<const float *>(pos_ptr),
							reinterpret_cast<const float *>(norm_ptr_direct),
							tan_ptr_neon,
							aabb_pos,
							inv_aabb_size,
							p_begin,
							p_end);
					pack_attrib_range(p_begin, p_end);
					return;
				}
#endif
				for (uint32_t i = p_begin; i < p_end; i++) {
					const int32_t src_v = identity_mapping ? int32_t(i) : source_vertices[i];
					const Vector3 &p = pos_ptr[i];
					const Vector3 n = norm_ptr_direct ? norm_ptr_direct[i] : (cached_normals.is_empty() ? norm_acc.read_vec3(src_v) : cached_normals[i]);
					Vector4 t(0.0f, 0.0f, 0.0f, 1.0f);
					if (!generated_tangents.is_empty()) {
						t = generated_tangents[i];
					} else if (has_src_tangent) {
						t = tan_acc.read_vec4(src_v, 1.0f);
					} else {
						const Vector3 tv = Vector3(n.z, -n.x, n.y).cross(n.normalized()).normalized();
						t = Vector4(tv.x, tv.y, tv.z, 1.0f);
					}
					float angle = 0.0f;
					Vector3 axis;
					compute_tbn_axis_angle(n, t, angle, axis);
					pack_oct_direction(vw + offsets[RSE::ARRAY_NORMAL] + i * normal_elem_size, axis.octahedron_encode(), false);

					const Vector3 rel = (p - aabb_pos) * inv_aabb_size;
					const uint16_t xyza[4] = {
						pack_unorm16(rel.x),
						pack_unorm16(rel.y),
						pack_unorm16(rel.z),
						pack_unorm16(angle),
					};
					std::memcpy(vw + offsets[RSE::ARRAY_VERTEX] + i * vertex_elem_size, xyza, 8);
				}
				pack_attrib_range(p_begin, p_end);
			};
			if (vertex_num >= 16384 && LoadThreadPool::get_singleton()) {
				const uint32_t chunks = CLAMP(uint32_t((vertex_num + 16383) / 16384), 6u, 24u);
				const uint32_t step = ((uint32_t(vertex_num) + chunks - 1) / chunks + 3u) & ~3u;
				LoadThreadPool::get_singleton()->parallel_for(chunks, [&](uint32_t c) {
					const uint32_t b = c * step;
					const uint32_t e = MIN(b + step, uint32_t(vertex_num));
					if (b < e) {
						pack_compressed_range(b, e);
					}
				});
			} else {
				pack_compressed_range(0, uint32_t(vertex_num));
			}
		} else {
			// Uncompressed float3 positions + normals/tangents + attributes (parallelized for >= 8192 vertices).
			if (sizeof(Vector3) == 12) {
				std::memcpy(vw + offsets[RSE::ARRAY_VERTEX], pos_ptr, size_t(vertex_num) * 12);
			} else {
				for (int32_t i = 0; i < vertex_num; i++) {
					const float xyz[3] = { float(pos_ptr[uint32_t(i)].x), float(pos_ptr[uint32_t(i)].y), float(pos_ptr[uint32_t(i)].z) };
					std::memcpy(vw + offsets[RSE::ARRAY_VERTEX] + uint32_t(i) * vertex_elem_size, xyz, 12);
				}
			}

			auto pack_uncompressed_dir_and_attrib_range = [&](uint32_t p_begin, uint32_t p_end) {
				uint32_t cur = p_begin;
#if defined(__aarch64__) || defined(_M_ARM64)
				if (has_normal && has_src_tangent && generated_tangents.is_empty() && identity_mapping &&
						norm_acc.is_tight_f32(3) && tan_acc.is_tight_f32(4) && normal_elem_size == 8) {
					const int done = encode_directions_neon(
							vw + offsets[RSE::ARRAY_NORMAL] + cur * 8,
							reinterpret_cast<const float *>(norm_acc.data) + 3 * size_t(cur),
							reinterpret_cast<const float *>(tan_acc.data) + 4 * size_t(cur),
							int(p_end - cur));
					cur += uint32_t(done);
				}
#endif
				if (has_normal || has_effective_tangent) {
					for (uint32_t i = cur; i < p_end; i++) {
						const int32_t src_v = identity_mapping ? int32_t(i) : source_vertices[i];
						Vector3 n(0.0f, 1.0f, 0.0f);
						if (has_normal) {
							n = norm_ptr_direct ? norm_ptr_direct[i] : (cached_normals.is_empty() ? norm_acc.read_vec3(src_v) : cached_normals[i]);
							pack_oct_direction(vw + offsets[RSE::ARRAY_NORMAL] + i * normal_elem_size, n.octahedron_encode(), false);
						}
						if (format & RSE::ARRAY_FORMAT_TANGENT) {
							Vector4 t(1.0f, 0.0f, 0.0f, 1.0f);
							if (!generated_tangents.is_empty()) {
								t = generated_tangents[i];
							} else if (has_src_tangent) {
								t = tan_acc.read_vec4(src_v, 1.0f);
							} else {
								const Vector3 tv = Vector3(n.z, -n.x, n.y).cross(n.normalized()).normalized();
								t = Vector4(tv.x, tv.y, tv.z, 1.0f);
							}
							const Vector3 tv(t.x, t.y, t.z);
							pack_oct_direction(vw + offsets[RSE::ARRAY_TANGENT] + i * normal_elem_size, tv.octahedron_tangent_encode(t.w), true);
						}
					}
				}
				pack_attrib_range(p_begin, p_end);
			};

			if (vertex_num >= 8192 && LoadThreadPool::get_singleton()) {
				const uint32_t chunks = CLAMP(uint32_t((vertex_num + 8191) / 8192), 6u, 24u);
				const uint32_t step = ((uint32_t(vertex_num) + chunks - 1) / chunks + 3u) & ~3u;
				LoadThreadPool::get_singleton()->parallel_for(chunks, [&](uint32_t c) {
					const uint32_t b = c * step;
					const uint32_t e = MIN(b + step, uint32_t(vertex_num));
					if (b < e) {
						pack_uncompressed_dir_and_attrib_range(b, e);
					}
				});
			} else {
				pack_uncompressed_dir_and_attrib_range(0, uint32_t(vertex_num));
			}
		}

		// Pack skin stream (4 or 8 bone indices & normalized weights) + compute bone AABBs (parallelized for >= 8192 vertices).
		Vector<AABB> bone_aabbs;
		if (skin_elem_size > 0) {
			const uint32_t bone_cnt = use_8_bones ? 8u : 4u;
			const AABB empty_bone_box(Vector3(), Vector3(-1.0f, -1.0f, -1.0f));

			auto pack_skin_slice = [&](uint32_t p_begin, uint32_t p_end, LocalVector<AABB> &r_bounds, int &r_max_bone, bool &r_any_valid) {
				if (has_joints_0 && j0_acc.comp_type == GLTFAccessor::COMPONENT_TYPE_UNSIGNED_BYTE && !use_8_bones) {
					r_bounds.resize(256);
					for (uint32_t b = 0; b < 256; b++) {
						r_bounds[b] = empty_bone_box;
					}
				}
				r_max_bone = -1;
				r_any_valid = false;
				uint32_t i = p_begin;
#if defined(__aarch64__) || defined(_M_ARM64)
				const bool tight_j_u8 = has_joints_0 && !j0_acc.is_materialized_u32 && !j0_acc.is_zero && j0_acc.data &&
						j0_acc.comp_type == GLTFAccessor::COMPONENT_TYPE_UNSIGNED_BYTE && j0_acc.comps == 4 && j0_acc.stride == 4;
				const bool tight_j_u16 = has_joints_0 && !j0_acc.is_materialized_u32 && !j0_acc.is_zero && j0_acc.data &&
						j0_acc.comp_type == GLTFAccessor::COMPONENT_TYPE_UNSIGNED_SHORT && j0_acc.comps == 4 && j0_acc.stride == 8;
				if (!use_8_bones && identity_mapping && (tight_j_u8 || tight_j_u16) &&
						has_weights_0 && w0_acc.is_tight_f32(4) && skin_elem_size == 16 &&
						offsets[RSE::ARRAY_BONES] == 0 && offsets[RSE::ARRAY_WEIGHTS] == 8) {
					struct SimdBoneBox {
						float32x4_t bmin;
						float32x4_t bmax;
						bool valid = false;
					};
					LocalVector<SimdBoneBox> sboxes;
					if (tight_j_u8) {
						sboxes.resize(256);
					}
					const float32x4_t zero = vdupq_n_f32(0.0f);
					const float32x4_t one = vdupq_n_f32(1.0f);
					const float32x4_t eps_v = vdupq_n_f32(CMP_EPSILON);
					const float *w_src = reinterpret_cast<const float *>(w0_acc.data);

					for (; i + 4 <= p_end; i += 4) {
						uint16_t j16[16];
						if (tight_j_u8) {
							const uint8x16_t jb = vld1q_u8(j0_acc.data + size_t(i) * 4);
							const uint16x8_t jlo = vmovl_u8(vget_low_u8(jb));
							const uint16x8_t jhi = vmovl_high_u8(jb);
							const int mb = int(MAX(vmaxvq_u16(jlo), vmaxvq_u16(jhi)));
							if (mb > r_max_bone) {
								r_max_bone = mb;
							}
							vst1q_u16(j16, jlo);
							vst1q_u16(j16 + 8, jhi);
						} else {
							const uint16_t *js = reinterpret_cast<const uint16_t *>(j0_acc.data) + size_t(i) * 4;
							const uint16x8_t jlo = vld1q_u16(js);
							const uint16x8_t jhi = vld1q_u16(js + 8);
							const int mb = int(MAX(vmaxvq_u16(jlo), vmaxvq_u16(jhi)));
							if (mb > r_max_bone) {
								r_max_bone = mb;
							}
							vst1q_u16(j16, jlo);
							vst1q_u16(j16 + 8, jhi);
						}
						if (r_max_bone >= int(sboxes.size())) {
							sboxes.resize(uint32_t(r_max_bone + 1));
						}

						for (uint32_t lane = 0; lane < 4; lane++) {
							const uint32_t vi = i + lane;
							uint8_t *dst_s = sw + size_t(vi) * 16;
							const uint16_t *jl = j16 + lane * 4;
							std::memcpy(dst_s, jl, 8);

							float32x4_t wv = vld1q_f32(w_src + size_t(vi) * 4);
							const float tot = vaddvq_f32(wv);
							if (tot > 0.0f && tot != 1.0f) {
								wv = vdivq_f32(wv, vdupq_n_f32(tot));
							}
							const float32x4_t wcl = vminq_f32(vmaxq_f32(wv, zero), one);
							vst1_u16(reinterpret_cast<uint16_t *>(dst_s + 8), vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(wcl, 65535.0f))));

							float wf_lane[4];
							vst1q_f32(wf_lane, wv);
							const Vector3 &p = pos_ptr[vi];
							const float p4[4] = { p.x, p.y, p.z, 0.0f };
							const float32x4_t pv = vld1q_f32(p4);
							for (uint32_t b = 0; b < 4; b++) {
								if (wf_lane[b] > 0.0f) {
									SimdBoneBox &sb = sboxes[jl[b]];
									if (!sb.valid) {
										sb.bmin = pv;
										sb.bmax = vaddq_f32(pv, eps_v);
										sb.valid = true;
										r_any_valid = true;
									} else {
										sb.bmin = vminq_f32(sb.bmin, pv);
										sb.bmax = vmaxq_f32(sb.bmax, pv);
									}
								}
							}
						}
					}

					if (r_max_bone >= 0) {
						if (r_max_bone >= int(r_bounds.size())) {
							const uint32_t old_sz = r_bounds.size();
							r_bounds.resize(uint32_t(r_max_bone + 1));
							for (uint32_t b = old_sz; b < r_bounds.size(); b++) {
								r_bounds[b] = empty_bone_box;
							}
						}
						const uint32_t sb_lim = MIN(uint32_t(r_max_bone + 1), sboxes.size());
						for (uint32_t b = 0; b < sb_lim; b++) {
							if (sboxes[b].valid) {
								float mn[4], mx[4];
								vst1q_f32(mn, sboxes[b].bmin);
								vst1q_f32(mx, sboxes[b].bmax);
								const Vector3 pmin(mn[0], mn[1], mn[2]);
								const Vector3 pmax(mx[0], mx[1], mx[2]);
								r_bounds[b] = AABB(pmin, (pmax - pmin).max(small_vec3));
							}
						}
					}
				}
#endif
				for (; i < p_end; i++) {
					const int32_t src_v = identity_mapping ? int32_t(i) : source_vertices[i];
					uint8_t *dst_s = sw + i * skin_elem_size;

					uint16_t joints16[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
					if (has_joints_0) {
						j0_acc.read_joints4(src_v, joints16);
						if (use_8_bones) {
							j1_acc.read_joints4(src_v, joints16 + 4);
						}
						for (uint32_t b = 0; b < bone_cnt; b++) {
							if (int(joints16[b]) > r_max_bone) {
								r_max_bone = int(joints16[b]);
							}
						}
						std::memcpy(dst_s + offsets[RSE::ARRAY_BONES], joints16, bone_cnt * sizeof(uint16_t));
					}

					float wf[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
					if (has_weights_0) {
						const Vector4 w0 = w0_acc.read_vec4(src_v, 0.0f);
						wf[0] = w0.x;
						wf[1] = w0.y;
						wf[2] = w0.z;
						wf[3] = w0.w;
						if (use_8_bones) {
							const Vector4 w1 = w1_acc.read_vec4(src_v, 0.0f);
							wf[4] = w1.x;
							wf[5] = w1.y;
							wf[6] = w1.z;
							wf[7] = w1.w;
						}
						float total = 0.0f;
						for (uint32_t b = 0; b < bone_cnt; b++) {
							total += wf[b];
						}
						uint16_t weights16[8];
#if defined(__aarch64__) || defined(_M_ARM64)
						if (!use_8_bones) {
							float32x4_t wvec = vld1q_f32(wf);
							if (total > 0.0f && total != 1.0f) {
								wvec = vdivq_f32(wvec, vdupq_n_f32(total));
								vst1q_f32(wf, wvec);
							}
							const float32x4_t clamped = vminq_f32(vmaxq_f32(wvec, vdupq_n_f32(0.0f)), vdupq_n_f32(1.0f));
							vst1_u16(weights16, vqmovn_u32(vcvtq_u32_f32(vmulq_n_f32(clamped, 65535.0f))));
						} else
#endif
						{
							if (total > 0.0f) {
								const float inv_total = 1.0f / total;
								for (uint32_t b = 0; b < bone_cnt; b++) {
									wf[b] *= inv_total;
								}
							}
							for (uint32_t b = 0; b < bone_cnt; b++) {
								weights16[b] = pack_unorm16(wf[b]);
							}
						}
						std::memcpy(dst_s + offsets[RSE::ARRAY_WEIGHTS], weights16, bone_cnt * sizeof(uint16_t));
					}

					if (has_joints_0 && has_weights_0) {
						if (r_max_bone >= int(r_bounds.size())) {
							const uint32_t old_sz = r_bounds.size();
							r_bounds.resize(uint32_t(r_max_bone + 1));
							for (uint32_t b = old_sz; b < r_bounds.size(); b++) {
								r_bounds[b] = empty_bone_box;
							}
						}
						const Vector3 &p = pos_ptr[i];
						for (uint32_t b = 0; b < bone_cnt; b++) {
							if (wf[b] > 0.0f) {
								AABB &box = r_bounds[joints16[b]];
								if (box.size.x < 0.0f) {
									box = AABB(p, small_vec3);
									r_any_valid = true;
								} else {
									box.expand_to(p);
								}
							}
						}
					}
				}
			};

			if (vertex_num >= 8192 && LoadThreadPool::get_singleton()) {
				constexpr uint32_t kSkinChunks = 12;
				LocalVector<AABB> chunk_bounds[kSkinChunks];
				int chunk_max[kSkinChunks];
				bool chunk_valid[kSkinChunks];
				for (uint32_t c = 0; c < kSkinChunks; c++) {
					chunk_max[c] = -1;
					chunk_valid[c] = false;
				}
				const uint32_t step = ((uint32_t(vertex_num) + kSkinChunks - 1) / kSkinChunks + 3u) & ~3u;
				LoadThreadPool::get_singleton()->parallel_for(kSkinChunks, [&](uint32_t c) {
					const uint32_t b = c * step;
					const uint32_t e = MIN(b + step, uint32_t(vertex_num));
					if (b < e) {
						pack_skin_slice(b, e, chunk_bounds[c], chunk_max[c], chunk_valid[c]);
					}
				});
				int max_bone = -1;
				bool any_valid = false;
				for (uint32_t c = 0; c < kSkinChunks; c++) {
					if (chunk_max[c] > max_bone) {
						max_bone = chunk_max[c];
					}
					any_valid = any_valid || chunk_valid[c];
				}
				if (has_joints_0 && any_valid && max_bone >= 0) {
					bone_aabbs.resize(max_bone + 1);
					AABB *dst_b = bone_aabbs.ptrw();
					for (int b = 0; b <= max_bone; b++) {
						dst_b[b] = empty_bone_box;
					}
					for (uint32_t c = 0; c < kSkinChunks; c++) {
						const int lim = MIN(max_bone + 1, int(chunk_bounds[c].size()));
						for (int b = 0; b < lim; b++) {
							const AABB &sb = chunk_bounds[c][uint32_t(b)];
							if (sb.size.x >= 0.0f) {
								if (dst_b[b].size.x < 0.0f) {
									dst_b[b] = sb;
								} else {
									dst_b[b].merge_with(sb);
								}
							}
						}
					}
				}
			} else {
				LocalVector<AABB> bounds;
				int max_bone = -1;
				bool any_valid = false;
				pack_skin_slice(0, uint32_t(vertex_num), bounds, max_bone, any_valid);
				if (has_joints_0 && any_valid && max_bone >= 0) {
					bone_aabbs.resize(max_bone + 1);
					std::memcpy(bone_aabbs.ptrw(), bounds.ptr(), size_t(max_bone + 1) * sizeof(AABB));
				}
			}
		}

		// Pack index buffer (16-bit when vertex_num <= 65536, else 32-bit) with ARM NEON vld3q/vst3q CW swap + bounds guard.
		if (index_count > 0) {
			uint8_t *iw = index_array.ptrw();
			if (defer_direct_tri_indices) {
				std::atomic<bool> idx_oob{ false };
				auto write_direct_tris = [&](int32_t k_begin, int32_t k_end) {
					uint32_t local_max = 0;
					if (vertex_num <= 65536) {
						uint16_t *dst16 = reinterpret_cast<uint16_t *>(iw);
						if (tight_tri_u16) {
							const uint16_t *src16 = reinterpret_cast<const uint16_t *>(idx_acc.data);
							int32_t k = k_begin;
#if defined(__aarch64__) || defined(_M_ARM64)
							uint16x8_t vmax16 = vdupq_n_u16(0);
							for (; k + 24 <= k_end; k += 24) {
								uint16x8x3_t tri = vld3q_u16(src16 + k);
								vmax16 = vmaxq_u16(vmax16, vmaxq_u16(tri.val[0], vmaxq_u16(tri.val[1], tri.val[2])));
								std::swap(tri.val[1], tri.val[2]);
								vst3q_u16(dst16 + k, tri);
							}
							local_max = MAX(local_max, uint32_t(vmaxvq_u16(vmax16)));
#endif
							for (; k < k_end; k += 3) {
								const uint16_t a = src16[k + 0], b = src16[k + 1], c = src16[k + 2];
								local_max = MAX(local_max, MAX(uint32_t(a), MAX(uint32_t(b), uint32_t(c))));
								dst16[k + 0] = a;
								dst16[k + 1] = c;
								dst16[k + 2] = b;
							}
						} else {
							const uint32_t *src32 = reinterpret_cast<const uint32_t *>(idx_acc.data);
							for (int32_t k = k_begin; k < k_end; k += 3) {
								const uint32_t a = src32[k + 0], b = src32[k + 1], c = src32[k + 2];
								local_max = MAX(local_max, MAX(a, MAX(b, c)));
								dst16[k + 0] = uint16_t(a);
								dst16[k + 1] = uint16_t(c);
								dst16[k + 2] = uint16_t(b);
							}
						}
					} else {
						uint32_t *dst32 = reinterpret_cast<uint32_t *>(iw);
						if (tight_tri_u32) {
							const uint32_t *src32 = reinterpret_cast<const uint32_t *>(idx_acc.data);
							int32_t k = k_begin;
#if defined(__aarch64__) || defined(_M_ARM64)
							uint32x4_t vmax32 = vdupq_n_u32(0);
							for (; k + 12 <= k_end; k += 12) {
								uint32x4x3_t tri = vld3q_u32(src32 + k);
								vmax32 = vmaxq_u32(vmax32, vmaxq_u32(tri.val[0], vmaxq_u32(tri.val[1], tri.val[2])));
								std::swap(tri.val[1], tri.val[2]);
								vst3q_u32(dst32 + k, tri);
							}
							local_max = MAX(local_max, vmaxvq_u32(vmax32));
#endif
							for (; k < k_end; k += 3) {
								const uint32_t a = src32[k + 0], b = src32[k + 1], c = src32[k + 2];
								local_max = MAX(local_max, MAX(a, MAX(b, c)));
								dst32[k + 0] = a;
								dst32[k + 1] = c;
								dst32[k + 2] = b;
							}
						} else {
							const uint16_t *src16 = reinterpret_cast<const uint16_t *>(idx_acc.data);
							for (int32_t k = k_begin; k < k_end; k += 3) {
								const uint16_t a = src16[k + 0], b = src16[k + 1], c = src16[k + 2];
								local_max = MAX(local_max, MAX(uint32_t(a), MAX(uint32_t(b), uint32_t(c))));
								dst32[k + 0] = a;
								dst32[k + 1] = c;
								dst32[k + 2] = b;
							}
						}
					}
					if (local_max >= uint32_t(orig_vertex_num)) {
						idx_oob.store(true, std::memory_order_relaxed);
					}
				};
				if (index_count >= 24576 && LoadThreadPool::get_singleton()) {
					const int32_t tri_total = index_count / 3;
					const uint32_t tri_chunks = CLAMP(uint32_t((tri_total + 8191) / 8192), 6u, 24u);
					const int32_t tri_step = int32_t(((uint32_t(tri_total) + tri_chunks - 1u) / tri_chunks + 7u) & ~7u);
					LoadThreadPool::get_singleton()->parallel_for(tri_chunks, [&](uint32_t c) {
						const int32_t tb = int32_t(c) * tri_step;
						const int32_t te = MIN(tb + tri_step, tri_total);
						if (tb < te) {
							write_direct_tris(tb * 3, te * 3);
						}
					});
				} else {
					write_direct_tris(0, index_count);
				}
				ERR_FAIL_COND_V(idx_oob.load(std::memory_order_relaxed), ERR_INVALID_DATA);
			} else {
				const uint32_t *src_idx = expanded_indices.ptr();
				if (vertex_num <= 65536) {
					uint16_t *dst16 = reinterpret_cast<uint16_t *>(iw);
					for (int32_t k = 0; k < index_count; k++) {
						dst16[k] = uint16_t(src_idx[uint32_t(k)]);
					}
				} else {
					std::memcpy(iw, src_idx, size_t(index_count) * sizeof(uint32_t));
				}
			}
		}

		// Pack morph targets (blend_shape_data) directly into GPU layout without SurfaceTool.
		Vector<uint8_t> blend_shape_data;
		if (mesh_target_count > 0) {
			blend_shape_data.resize(int64_t(mesh_target_count) * int64_t(vertex_array_size));
			uint8_t *bs_all = blend_shape_data.ptrw();

			for (uint32_t t_idx = 0; t_idx < mesh_target_count; t_idx++) {
				uint8_t *dst_shape = bs_all + size_t(t_idx) * size_t(vertex_array_size);
				if (t_idx >= prim.targets.size()) {
					std::memcpy(dst_shape, vw, size_t(vertex_array_size));
					continue;
				}
				const GLTFFastMorphSpec &mspec = prim.targets[t_idx];
				FastAccessorView m_pos, m_norm, m_tan;
				const bool has_m_pos = mspec.position >= 0 && m_pos.init(p_state.ptr(), mspec.position, false);
				const bool has_m_norm = (format & RSE::ARRAY_FORMAT_NORMAL) && mspec.normal >= 0 && m_norm.init(p_state.ptr(), mspec.normal, false);
				const bool has_m_tan = (format & RSE::ARRAY_FORMAT_TANGENT) && mspec.tangent >= 0 && m_tan.init(p_state.ptr(), mspec.tangent, false);

				if (has_m_pos) {
					Vector3 m_min, m_max;
					for (int32_t i = 0; i < vertex_num; i++) {
						const int32_t src_v = identity_mapping ? i : source_vertices[uint32_t(i)];
						Vector3 mp = pos_ptr[uint32_t(i)];
						if (src_v < m_pos.count) {
							mp += m_pos.read_vec3(src_v);
						}
						const float xyz[3] = { float(mp.x), float(mp.y), float(mp.z) };
						std::memcpy(dst_shape + offsets[RSE::ARRAY_VERTEX] + uint32_t(i) * vertex_elem_size, xyz, 12);
						if (i == 0) {
							m_min = m_max = mp;
						} else {
							m_min = m_min.min(mp);
							m_max = m_max.max(mp);
						}
					}
					surface_aabb.merge_with(AABB(m_min, (m_max - m_min).max(small_vec3)));
				} else {
					std::memcpy(dst_shape + offsets[RSE::ARRAY_VERTEX], vw + offsets[RSE::ARRAY_VERTEX], size_t(vertex_elem_size) * size_t(vertex_num));
				}

				if (normal_elem_size > 0) {
					if (!has_m_norm && !has_m_tan) {
						std::memcpy(dst_shape + size_t(vertex_elem_size) * size_t(vertex_num),
								vw + size_t(vertex_elem_size) * size_t(vertex_num),
								size_t(normal_elem_size) * size_t(vertex_num));
					} else {
						for (int32_t i = 0; i < vertex_num; i++) {
							const int32_t src_v = identity_mapping ? i : source_vertices[uint32_t(i)];
							Vector3 mn(0.0f, 1.0f, 0.0f);
							if (format & RSE::ARRAY_FORMAT_NORMAL) {
								mn = norm_ptr_direct ? norm_ptr_direct[i] : cached_normals[uint32_t(i)];
								if (has_m_norm && src_v < m_norm.count) {
									mn += m_norm.read_vec3(src_v);
								}
								pack_oct_direction(dst_shape + offsets[RSE::ARRAY_NORMAL] + uint32_t(i) * normal_elem_size, mn.octahedron_encode(), false);
							}
							if (format & RSE::ARRAY_FORMAT_TANGENT) {
								Vector4 base_t(1.0f, 0.0f, 0.0f, 1.0f);
								if (!generated_tangents.is_empty()) {
									base_t = generated_tangents[uint32_t(i)];
								} else if (has_src_tangent) {
									base_t = tan_acc.read_vec4(src_v, 1.0f);
								} else {
									const Vector3 tv = Vector3(mn.z, -mn.x, mn.y).cross(mn.normalized()).normalized();
									base_t = Vector4(tv.x, tv.y, tv.z, 1.0f);
								}
								Vector3 mt(base_t.x, base_t.y, base_t.z);
								if (has_m_tan && src_v < m_tan.count) {
									mt += m_tan.read_vec3(src_v);
								}
								pack_oct_direction(dst_shape + offsets[RSE::ARRAY_TANGENT] + uint32_t(i) * normal_elem_size, mt.octahedron_tangent_encode(base_t.w), true);
							}
						}
					}
				}
			}
		}

		const int surf_idx = array_mesh->get_surface_count();
		array_mesh->add_surface(
				format,
				godot_prim,
				vertex_array,
				attrib_array,
				skin_array,
				vertex_num,
				index_array,
				index_count,
				surface_aabb,
				blend_shape_data,
				bone_aabbs,
				Vector<RenderingServerTypes::SurfaceData::LOD>(),
				uv_scale);
		if (!mat_name.is_empty()) {
			array_mesh->surface_set_name(surf_idx, mat_name);
		}
		if (mat.is_valid()) {
			array_mesh->surface_set_material(surf_idx, mat);
		}
	}

	Vector<float> blend_weights;
	blend_weights.resize(int(mesh_target_count));
	for (int w_i = 0; w_i < blend_weights.size(); w_i++) {
		blend_weights.write[w_i] = (w_i < p_weights.size()) ? p_weights[w_i] : 0.0f;
	}
	gltf_mesh->set_blend_weights(blend_weights);
	gltf_mesh->set_instance_materials(instance_materials);
	gltf_mesh->set_runtime_mesh(array_mesh);
	gltf_mesh->set_meta(SNAME("_ange_fast_mesh"), true);
	r_out_gltf_mesh = gltf_mesh;
	return OK;
}
