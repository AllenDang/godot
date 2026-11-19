/**************************************************************************/
/*  gltf_yyjson.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "gltf_yyjson.h"

#include "gltf_document.h"
#include "gltf_fast_access.h"
#include "gltf_fast_mesh.h"
#include "gltf_state.h"
#include "skin_tool.h"

#include "core/config/engine.h"
#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/marshalls.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/os/load_thread_pool.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/placeholder_textures.h"
#include "scene/resources/portable_compressed_texture.h"

#include <atomic>
#include <cstring>

namespace {

_FORCE_INLINE_ String yj_str(yyjson_val *p_val) {
	if (!p_val || !yyjson_is_str(p_val)) {
		return String();
	}
	return String::utf8(yyjson_get_str(p_val), static_cast<int>(yyjson_get_len(p_val)));
}

_FORCE_INLINE_ bool yj_str_eq(yyjson_val *p_val, const char *p_lit) {
	return p_val && yyjson_is_str(p_val) && yyjson_equals_str(p_val, p_lit);
}

_FORCE_INLINE_ int64_t yj_int(yyjson_val *p_val, int64_t p_default = 0) {
	if (!p_val) {
		return p_default;
	}
	if (yyjson_is_sint(p_val)) {
		return yyjson_get_sint(p_val);
	}
	if (yyjson_is_uint(p_val)) {
		return static_cast<int64_t>(yyjson_get_uint(p_val));
	}
	if (yyjson_is_real(p_val)) {
		return static_cast<int64_t>(yyjson_get_real(p_val));
	}
	return p_default;
}

_FORCE_INLINE_ double yj_num(yyjson_val *p_val, double p_default = 0.0) {
	return (p_val && yyjson_is_num(p_val)) ? yyjson_get_num(p_val) : p_default;
}

_FORCE_INLINE_ bool yj_bool(yyjson_val *p_val, bool p_default = false) {
	return (p_val && yyjson_is_bool(p_val)) ? yyjson_get_bool(p_val) : p_default;
}

_FORCE_INLINE_ bool yj_vec3(yyjson_val *p_arr, Vector3 &r_out) {
	if (!p_arr || !yyjson_is_arr(p_arr) || yyjson_arr_size(p_arr) != 3) {
		return false;
	}
	yyjson_arr_iter it = yyjson_arr_iter_with(p_arr);
	yyjson_val *v0 = yyjson_arr_iter_next(&it);
	yyjson_val *v1 = yyjson_arr_iter_next(&it);
	yyjson_val *v2 = yyjson_arr_iter_next(&it);
	if (!yyjson_is_num(v0) || !yyjson_is_num(v1) || !yyjson_is_num(v2)) {
		return false;
	}
	r_out = Vector3(
			static_cast<real_t>(yyjson_get_num(v0)),
			static_cast<real_t>(yyjson_get_num(v1)),
			static_cast<real_t>(yyjson_get_num(v2)));
	return true;
}

_FORCE_INLINE_ bool yj_quat(yyjson_val *p_arr, Quaternion &r_out) {
	if (!p_arr || !yyjson_is_arr(p_arr) || yyjson_arr_size(p_arr) != 4) {
		return false;
	}
	yyjson_arr_iter it = yyjson_arr_iter_with(p_arr);
	yyjson_val *v0 = yyjson_arr_iter_next(&it);
	yyjson_val *v1 = yyjson_arr_iter_next(&it);
	yyjson_val *v2 = yyjson_arr_iter_next(&it);
	yyjson_val *v3 = yyjson_arr_iter_next(&it);
	if (!yyjson_is_num(v0) || !yyjson_is_num(v1) || !yyjson_is_num(v2) || !yyjson_is_num(v3)) {
		return false;
	}
	r_out = Quaternion(
			static_cast<real_t>(yyjson_get_num(v0)),
			static_cast<real_t>(yyjson_get_num(v1)),
			static_cast<real_t>(yyjson_get_num(v2)),
			static_cast<real_t>(yyjson_get_num(v3)));
	return true;
}

_FORCE_INLINE_ bool yj_color3(yyjson_val *p_arr, Color &r_out) {
	if (!p_arr || !yyjson_is_arr(p_arr) || yyjson_arr_size(p_arr) < 3) {
		return false;
	}
	yyjson_arr_iter it = yyjson_arr_iter_with(p_arr);
	yyjson_val *v0 = yyjson_arr_iter_next(&it);
	yyjson_val *v1 = yyjson_arr_iter_next(&it);
	yyjson_val *v2 = yyjson_arr_iter_next(&it);
	if (!yyjson_is_num(v0) || !yyjson_is_num(v1) || !yyjson_is_num(v2)) {
		return false;
	}
	r_out = Color(
			static_cast<float>(yyjson_get_num(v0)),
			static_cast<float>(yyjson_get_num(v1)),
			static_cast<float>(yyjson_get_num(v2)),
			1.0f);
	return true;
}

_FORCE_INLINE_ bool yj_color4(yyjson_val *p_arr, Color &r_out) {
	if (!p_arr || !yyjson_is_arr(p_arr) || yyjson_arr_size(p_arr) != 4) {
		return false;
	}
	yyjson_arr_iter it = yyjson_arr_iter_with(p_arr);
	yyjson_val *v0 = yyjson_arr_iter_next(&it);
	yyjson_val *v1 = yyjson_arr_iter_next(&it);
	yyjson_val *v2 = yyjson_arr_iter_next(&it);
	yyjson_val *v3 = yyjson_arr_iter_next(&it);
	if (!yyjson_is_num(v0) || !yyjson_is_num(v1) || !yyjson_is_num(v2) || !yyjson_is_num(v3)) {
		return false;
	}
	r_out = Color(
			static_cast<float>(yyjson_get_num(v0)),
			static_cast<float>(yyjson_get_num(v1)),
			static_cast<float>(yyjson_get_num(v2)),
			static_cast<float>(yyjson_get_num(v3)));
	return true;
}

_FORCE_INLINE_ bool yj_xform(yyjson_val *p_arr, Transform3D &r_out) {
	if (!p_arr || !yyjson_is_arr(p_arr) || yyjson_arr_size(p_arr) != 16) {
		return false;
	}
	real_t m[16];
	yyjson_arr_iter it = yyjson_arr_iter_with(p_arr);
	for (int i = 0; i < 16; i++) {
		yyjson_val *v = yyjson_arr_iter_next(&it);
		if (!yyjson_is_num(v)) {
			return false;
		}
		m[i] = static_cast<real_t>(yyjson_get_num(v));
	}
	r_out.basis.set_column(Vector3::AXIS_X, Vector3(m[0], m[1], m[2]));
	r_out.basis.set_column(Vector3::AXIS_Y, Vector3(m[4], m[5], m[6]));
	r_out.basis.set_column(Vector3::AXIS_Z, Vector3(m[8], m[9], m[10]));
	r_out.set_origin(Vector3(m[12], m[13], m[14]));
	return true;
}

Vector<uint8_t> decode_base64_uri(const String &p_uri) {
	const int start = p_uri.find_char(',');
	ERR_FAIL_COND_V(start == -1, Vector<uint8_t>());
	const CharString substr = p_uri.substr(start + 1).ascii();
	const int strlen = substr.length();
	if (strlen == 0) {
		return Vector<uint8_t>();
	}
	Vector<uint8_t> buf;
	buf.resize(strlen / 4 * 3 + 1 + 1);
	size_t len = 0;
	ERR_FAIL_COND_V(CryptoCore::b64_decode(buf.ptrw(), buf.size(), &len, (unsigned char *)substr.get_data(), strlen) != OK, Vector<uint8_t>());
	buf.resize(len);
	return buf;
}

void attach_yyjson_extras_to_meta(yyjson_val *p_extras, Ref<Resource> p_res) {
	if (!p_extras || !yyjson_is_obj(p_extras) || yyjson_obj_size(p_extras) == 0 || p_res.is_null()) {
		return;
	}
	p_res->set_meta("extras", gltf_yyjson_val_to_variant(p_extras));
}

void apply_yyjson_texture_transform_uv1(yyjson_val *p_tex_info, Ref<BaseMaterial3D> p_material) {
	if (!p_tex_info || !yyjson_is_obj(p_tex_info) || p_material.is_null()) {
		return;
	}
	yyjson_val *exts = yyjson_obj_get(p_tex_info, "extensions");
	if (!exts || !yyjson_is_obj(exts)) {
		return;
	}
	yyjson_val *tt = yyjson_obj_get(exts, "KHR_texture_transform");
	if (!tt || !yyjson_is_obj(tt)) {
		return;
	}
	yyjson_val *off = yyjson_obj_get(tt, "offset");
	if (off && yyjson_is_arr(off) && yyjson_arr_size(off) == 2) {
		yyjson_val *o0 = yyjson_arr_get(off, 0);
		yyjson_val *o1 = yyjson_arr_get(off, 1);
		if (yyjson_is_num(o0) && yyjson_is_num(o1)) {
			p_material->set_uv1_offset(Vector3(static_cast<real_t>(yyjson_get_num(o0)), static_cast<real_t>(yyjson_get_num(o1)), 0.0f));
		}
	}
	yyjson_val *sc = yyjson_obj_get(tt, "scale");
	if (sc && yyjson_is_arr(sc) && yyjson_arr_size(sc) == 2) {
		yyjson_val *s0 = yyjson_arr_get(sc, 0);
		yyjson_val *s1 = yyjson_arr_get(sc, 1);
		if (yyjson_is_num(s0) && yyjson_is_num(s1)) {
			p_material->set_uv1_scale(Vector3(static_cast<real_t>(yyjson_get_num(s0)), static_cast<real_t>(yyjson_get_num(s1)), 1.0f));
		}
	}
}

GLTFAccessor::GLTFAccessorType parse_accessor_type(yyjson_val *p_val, bool &r_ok) {
	r_ok = true;
	if (!p_val || !yyjson_is_str(p_val)) {
		r_ok = false;
		return GLTFAccessor::TYPE_SCALAR;
	}
	const char *s = yyjson_get_str(p_val);
	const size_t n = yyjson_get_len(p_val);
	if (n == 6 && std::memcmp(s, "SCALAR", 6) == 0) {
		return GLTFAccessor::TYPE_SCALAR;
	}
	if (n == 4) {
		if (std::memcmp(s, "VEC3", 4) == 0) {
			return GLTFAccessor::TYPE_VEC3;
		}
		if (std::memcmp(s, "VEC2", 4) == 0) {
			return GLTFAccessor::TYPE_VEC2;
		}
		if (std::memcmp(s, "VEC4", 4) == 0) {
			return GLTFAccessor::TYPE_VEC4;
		}
		if (std::memcmp(s, "MAT4", 4) == 0) {
			return GLTFAccessor::TYPE_MAT4;
		}
		if (std::memcmp(s, "MAT3", 4) == 0) {
			return GLTFAccessor::TYPE_MAT3;
		}
		if (std::memcmp(s, "MAT2", 4) == 0) {
			return GLTFAccessor::TYPE_MAT2;
		}
	}
	r_ok = false;
	return GLTFAccessor::TYPE_SCALAR;
}

} // namespace

Error GLTFYyjsonDoc::parse(const char *p_utf8, size_t p_len, String *r_err_msg, int *r_err_line) {
	clear();
	if (!p_utf8 || p_len == 0) {
		if (r_err_msg) {
			*r_err_msg = "Empty JSON input";
		}
		return ERR_PARSE_ERROR;
	}

	const size_t pool_bytes = yyjson_read_max_memory_usage(p_len, 0);
	if (pool_bytes > 0 && pool_bytes <= (256ULL * 1024ULL * 1024ULL)) {
		pool_mem = Memory::alloc_static(pool_bytes);
	}
	yyjson_read_err read_err = {};
	if (pool_mem && yyjson_alc_pool_init(&alc, pool_mem, pool_bytes)) {
		doc = yyjson_read_opts(const_cast<char *>(p_utf8), p_len, 0, &alc, &read_err);
	} else {
		if (pool_mem) {
			Memory::free_static(pool_mem);
			pool_mem = nullptr;
		}
		doc = yyjson_read_opts(const_cast<char *>(p_utf8), p_len, 0, nullptr, &read_err);
	}

	if (!doc) {
		if (r_err_msg) {
			*r_err_msg = read_err.msg ? String::utf8(read_err.msg) : String("yyjson parse error");
		}
		if (r_err_line) {
			size_t line = 1, col = 1, chr = 0;
			if (yyjson_locate_pos(p_utf8, p_len, read_err.pos, &line, &col, &chr)) {
				*r_err_line = static_cast<int>(line);
			} else {
				*r_err_line = 0;
			}
		}
		clear();
		return ERR_PARSE_ERROR;
	}
	return OK;
}

Variant gltf_yyjson_val_to_variant(yyjson_val *p_val) {
	if (!p_val) {
		return Variant();
	}
	switch (yyjson_get_type(p_val)) {
		case YYJSON_TYPE_NULL:
			return Variant();
		case YYJSON_TYPE_BOOL:
			return yyjson_get_bool(p_val);
		case YYJSON_TYPE_NUM: {
			// Match Godot JSON::parse behavior (numbers are stored as double unless exact int).
			if (yyjson_is_sint(p_val)) {
				return static_cast<double>(yyjson_get_sint(p_val));
			}
			if (yyjson_is_uint(p_val)) {
				return static_cast<double>(yyjson_get_uint(p_val));
			}
			return yyjson_get_real(p_val);
		}
		case YYJSON_TYPE_STR:
			return String::utf8(yyjson_get_str(p_val), static_cast<int>(yyjson_get_len(p_val)));
		case YYJSON_TYPE_ARR: {
			const size_t n = yyjson_arr_size(p_val);
			Array arr;
			arr.resize(static_cast<int>(n));
			yyjson_arr_iter it = yyjson_arr_iter_with(p_val);
			int idx = 0;
			yyjson_val *elem = nullptr;
			while ((elem = yyjson_arr_iter_next(&it)) != nullptr) {
				arr[idx++] = gltf_yyjson_val_to_variant(elem);
			}
			return arr;
		}
		case YYJSON_TYPE_OBJ: {
			Dictionary dict;
			yyjson_obj_iter it = yyjson_obj_iter_with(p_val);
			yyjson_val *key = nullptr;
			while ((key = yyjson_obj_iter_next(&it)) != nullptr) {
				yyjson_val *val = yyjson_obj_iter_get_val(key);
				String kstr = String::utf8(yyjson_get_str(key), static_cast<int>(yyjson_get_len(key)));
				dict[kstr] = gltf_yyjson_val_to_variant(val);
			}
			return dict;
		}
		default:
			return Variant();
	}
}

Variant gltf_yyjson_parse_to_variant(const char *p_utf8, size_t p_len, Error *r_err, String *r_err_msg) {
	GLTFYyjsonDoc jdoc;
	Error err = jdoc.parse(p_utf8, p_len, r_err_msg, nullptr);
	if (r_err) {
		*r_err = err;
	}
	if (err != OK || !jdoc.is_valid()) {
		return Variant();
	}
	return gltf_yyjson_val_to_variant(jdoc.root());
}

namespace {

bool is_builtin_standard_doc_ext(const Ref<GLTFDocumentExtension> &p_ext) {
	if (p_ext.is_null()) {
		return true;
	}
	const String cname = p_ext->get_class();
	return cname == "GLTFDocumentExtensionPhysics" ||
			cname == "GLTFDocumentExtensionTextureKTX" ||
			cname == "GLTFDocumentExtensionTextureWebP" ||
			cname == "GLTFDocumentExtensionConvertImporterMesh";
}

bool is_meshopt_fallback_buffer(yyjson_val *p_buf_obj) {
	yyjson_val *exts = yyjson_obj_get(p_buf_obj, "extensions");
	if (!exts || !yyjson_is_obj(exts)) {
		return false;
	}
	yyjson_val *mext = yyjson_obj_get(exts, "KHR_meshopt_compression");
	if (!mext) {
		mext = yyjson_obj_get(exts, "EXT_meshopt_compression");
	}
	if (!mext || !yyjson_is_obj(mext)) {
		return false;
	}
	return yj_bool(yyjson_obj_get(mext, "fallback"), false);
}

void parse_yyjson_meshopt_bv(yyjson_val *p_bv_obj, Ref<GLTFBufferView> &r_bv) {
	yyjson_val *exts = yyjson_obj_get(p_bv_obj, "extensions");
	if (!exts || !yyjson_is_obj(exts)) {
		return;
	}
	yyjson_val *mext = yyjson_obj_get(exts, "KHR_meshopt_compression");
	if (!mext) {
		mext = yyjson_obj_get(exts, "EXT_meshopt_compression");
	}
	if (!mext || !yyjson_is_obj(mext)) {
		return;
	}
	yyjson_val *v_buf = yyjson_obj_get(mext, "buffer");
	yyjson_val *v_len = yyjson_obj_get(mext, "byteLength");
	yyjson_val *v_str = yyjson_obj_get(mext, "byteStride");
	yyjson_val *v_cnt = yyjson_obj_get(mext, "count");
	yyjson_val *v_mod = yyjson_obj_get(mext, "mode");
	if (!v_buf || !v_len || !v_str || !v_cnt || !v_mod || !yyjson_is_str(v_mod)) {
		return;
	}
	GLTFBufferView::MeshoptMode mode = GLTFBufferView::MESHOPT_MODE_NONE;
	if (yj_str_eq(v_mod, "ATTRIBUTES")) {
		mode = GLTFBufferView::MESHOPT_MODE_ATTRIBUTES;
	} else if (yj_str_eq(v_mod, "TRIANGLES")) {
		mode = GLTFBufferView::MESHOPT_MODE_TRIANGLES;
	} else if (yj_str_eq(v_mod, "INDICES")) {
		mode = GLTFBufferView::MESHOPT_MODE_INDICES;
	} else {
		return;
	}

	GLTFBufferView::MeshoptFilter filter = GLTFBufferView::MESHOPT_FILTER_NONE;
	yyjson_val *v_flt = yyjson_obj_get(mext, "filter");
	if (v_flt && yyjson_is_str(v_flt)) {
		if (yj_str_eq(v_flt, "OCTAHEDRAL")) {
			filter = GLTFBufferView::MESHOPT_FILTER_OCTAHEDRAL;
		} else if (yj_str_eq(v_flt, "QUATERNION")) {
			filter = GLTFBufferView::MESHOPT_FILTER_QUATERNION;
		} else if (yj_str_eq(v_flt, "EXPONENTIAL")) {
			filter = GLTFBufferView::MESHOPT_FILTER_EXPONENTIAL;
		} else if (yj_str_eq(v_flt, "COLOR")) {
			filter = GLTFBufferView::MESHOPT_FILTER_COLOR;
		}
	}

	r_bv->configure_meshopt(
			static_cast<GLTFBufferIndex>(yj_int(v_buf, -1)),
			yj_int(yyjson_obj_get(mext, "byteOffset"), 0),
			yj_int(v_len, 0),
			yj_int(v_str, 0),
			yj_int(v_cnt, 0),
			mode,
			filter);
}

Ref<Image> decode_raw_image_bytes(const PackedByteArray &p_bytes, const String &p_mime_type, int p_index) {
	Ref<Image> img;
	img.instantiate();
	if (p_mime_type == "image/ktx2" || p_mime_type == "image/ktx") {
		img->load_ktx_from_buffer(p_bytes);
	} else if (p_mime_type == "image/webp") {
		img->load_webp_from_buffer(p_bytes);
	} else if (p_mime_type == "image/png") {
		img->load_png_from_buffer(p_bytes);
	} else if (p_mime_type == "image/jpeg" || p_mime_type == "image/jpg") {
		img->load_jpg_from_buffer(p_bytes);
	}
	if (img->is_empty()) {
		// Magic-byte fallback for untyped or mislabeled URIs/bufferViews.
		if (p_bytes.size() >= 12 && p_bytes[0] == 0xAB && p_bytes[1] == 'K' && p_bytes[2] == 'T' && p_bytes[3] == 'X') {
			img->load_ktx_from_buffer(p_bytes);
		} else if (p_bytes.size() >= 12 && p_bytes[0] == 'R' && p_bytes[1] == 'I' && p_bytes[2] == 'F' && p_bytes[3] == 'F' &&
				p_bytes[8] == 'W' && p_bytes[9] == 'E' && p_bytes[10] == 'B' && p_bytes[11] == 'P') {
			img->load_webp_from_buffer(p_bytes);
		} else {
			img->load_png_from_buffer(p_bytes);
			if (img->is_empty()) {
				img->load_jpg_from_buffer(p_bytes);
			}
		}
	}
	if (img->is_empty()) {
		ERR_PRINT(vformat("glTF: Couldn't load image index '%d' with mimetype '%s'.", p_index, p_mime_type));
	}
	return img;
}

void apply_yyjson_khr_material_extensions(GLTFDocument *p_doc, Ref<GLTFState> p_state, yyjson_val *p_exts, Ref<StandardMaterial3D> &r_mat) {
	if (!p_exts || !yyjson_is_obj(p_exts)) {
		return;
	}

	if (yyjson_val *ior_obj = yyjson_obj_get(p_exts, "KHR_materials_ior")) {
		if (yyjson_is_obj(ior_obj)) {
			const float ior = float(yj_num(yyjson_obj_get(ior_obj, "ior"), 1.5));
			if (ior > 0.0f) {
				const float r = (ior - 1.0f) / (ior + 1.0f);
				const float f0 = r * r;
				r_mat->set_specular(CLAMP(f0 / 0.08f, 0.0f, 1.0f));
			}
		}
	}

	if (yyjson_val *spec_obj = yyjson_obj_get(p_exts, "KHR_materials_specular")) {
		if (yyjson_is_obj(spec_obj)) {
			if (yyjson_val *sf = yyjson_obj_get(spec_obj, "specularFactor")) {
				r_mat->set_specular(CLAMP(float(yj_num(sf, 1.0)) * r_mat->get_specular(), 0.0f, 1.0f));
			}
		}
	}

	if (yyjson_val *cc_obj = yyjson_obj_get(p_exts, "KHR_materials_clearcoat")) {
		if (yyjson_is_obj(cc_obj)) {
			const float cc_factor = float(yj_num(yyjson_obj_get(cc_obj, "clearcoatFactor"), 0.0));
			const float cc_rough = float(yj_num(yyjson_obj_get(cc_obj, "clearcoatRoughnessFactor"), 0.0));
			if (cc_factor > 0.0f) {
				r_mat->set_feature(BaseMaterial3D::FEATURE_CLEARCOAT, true);
				r_mat->set_clearcoat(CLAMP(cc_factor, 0.0f, 1.0f));
				r_mat->set_clearcoat_roughness(CLAMP(cc_rough, 0.0f, 1.0f));
				if (yyjson_val *cct = yyjson_obj_get(cc_obj, "clearcoatTexture")) {
					const GLTFTextureIndex ti = GLTFTextureIndex(yj_int(yyjson_obj_get(cct, "index"), -1));
					if (ti >= 0) {
						r_mat->set_texture(BaseMaterial3D::TEXTURE_CLEARCOAT, GLTFFastAccess::get_texture(p_doc, p_state, ti, GLTFDocument::TEXTURE_TYPE_GENERIC));
					}
				}
			}
		}
	}

	if (yyjson_val *aniso_obj = yyjson_obj_get(p_exts, "KHR_materials_anisotropy")) {
		if (yyjson_is_obj(aniso_obj)) {
			const float strength = float(yj_num(yyjson_obj_get(aniso_obj, "anisotropyStrength"), 0.0));
			if (strength > 0.0f) {
				r_mat->set_feature(BaseMaterial3D::FEATURE_ANISOTROPY, true);
				r_mat->set_anisotropy(CLAMP(strength, -1.0f, 1.0f));
				if (yyjson_val *at = yyjson_obj_get(aniso_obj, "anisotropyTexture")) {
					const GLTFTextureIndex ti = GLTFTextureIndex(yj_int(yyjson_obj_get(at, "index"), -1));
					if (ti >= 0) {
						r_mat->set_texture(BaseMaterial3D::TEXTURE_FLOWMAP, GLTFFastAccess::get_texture(p_doc, p_state, ti, GLTFDocument::TEXTURE_TYPE_GENERIC));
					}
				}
			}
		}
	}

	if (yyjson_val *sheen_obj = yyjson_obj_get(p_exts, "KHR_materials_sheen")) {
		if (yyjson_is_obj(sheen_obj)) {
			Color sc(0.0f, 0.0f, 0.0f);
			if (yj_color3(yyjson_obj_get(sheen_obj, "sheenColorFactor"), sc) && (sc.r > 0.0f || sc.g > 0.0f || sc.b > 0.0f)) {
				r_mat->set_feature(BaseMaterial3D::FEATURE_RIM, true);
				r_mat->set_rim(CLAMP(MAX(sc.r, MAX(sc.g, sc.b)), 0.0f, 1.0f));
				r_mat->set_rim_tint(float(yj_num(yyjson_obj_get(sheen_obj, "sheenRoughnessFactor"), 0.5)));
			}
		}
	}

	if (yyjson_val *trans_obj = yyjson_obj_get(p_exts, "KHR_materials_transmission")) {
		if (yyjson_is_obj(trans_obj)) {
			const float tf = float(yj_num(yyjson_obj_get(trans_obj, "transmissionFactor"), 0.0));
			if (tf > 0.0f) {
				r_mat->set_feature(BaseMaterial3D::FEATURE_REFRACTION, true);
				r_mat->set_refraction(0.05f * tf);
			}
		}
	}

	if (yyjson_val *dt_obj = yyjson_obj_get(p_exts, "KHR_materials_diffuse_transmission")) {
		if (yyjson_is_obj(dt_obj)) {
			const float dtf = float(yj_num(yyjson_obj_get(dt_obj, "diffuseTransmissionFactor"), 0.0));
			if (dtf > 0.0f) {
				r_mat->set_feature(BaseMaterial3D::FEATURE_BACKLIGHT, true);
				Color dtc(1.0f, 1.0f, 1.0f);
				yj_color3(yyjson_obj_get(dt_obj, "diffuseTransmissionColorFactor"), dtc);
				r_mat->set_backlight(dtc.linear_to_srgb() * dtf);
			}
		}
	}
}

bool fast_decode_accessor_f64(const Ref<GLTFState> &p_state, int p_acc_idx, PackedFloat64Array &r_out) {
	const Vector<Ref<GLTFAccessor>> &accs = p_state->get_accessors();
	ERR_FAIL_INDEX_V(p_acc_idx, accs.size(), false);
	const Ref<GLTFAccessor> &acc = accs[p_acc_idx];
	ERR_FAIL_COND_V(acc.is_null(), false);
	if (acc->get_sparse_count() == 0 && acc->get_buffer_view() >= 0 &&
			acc->get_accessor_type() == GLTFAccessor::TYPE_SCALAR &&
			acc->get_component_type() == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT) {
		const Vector<Ref<GLTFBufferView>> &bvs = p_state->get_buffer_views();
		const GLTFBufferViewIndex bvi = acc->get_buffer_view();
		if (bvi >= 0 && bvi < bvs.size()) {
			const uint8_t *vptr = nullptr;
			int64_t vlen = 0;
			if (bvs[bvi]->get_view_span(p_state.ptr(), vptr, vlen)) {
				const int64_t cnt = acc->get_count();
				const int64_t stride = bvs[bvi]->get_byte_stride() > 0 ? bvs[bvi]->get_byte_stride() : 4;
				const int64_t off = acc->get_byte_offset();
				if (off >= 0 && (cnt == 0 || off + (cnt - 1) * stride + 4 <= vlen)) {
					r_out.resize(cnt);
					double *dst = r_out.ptrw();
					if (stride == 4) {
						const float *src = reinterpret_cast<const float *>(vptr + off);
						for (int64_t i = 0; i < cnt; i++) {
							dst[i] = double(src[i]);
						}
					} else {
						for (int64_t i = 0; i < cnt; i++) {
							float f;
							std::memcpy(&f, vptr + off + i * stride, 4);
							dst[i] = double(f);
						}
					}
					return true;
				}
			}
		}
	}
	r_out = acc->decode_as_float64s(p_state);
	return true;
}

bool fast_decode_accessor_vec3(const Ref<GLTFState> &p_state, int p_acc_idx, PackedVector3Array &r_out) {
	const Vector<Ref<GLTFAccessor>> &accs = p_state->get_accessors();
	ERR_FAIL_INDEX_V(p_acc_idx, accs.size(), false);
	const Ref<GLTFAccessor> &acc = accs[p_acc_idx];
	ERR_FAIL_COND_V(acc.is_null(), false);
	if (acc->get_sparse_count() == 0 && acc->get_buffer_view() >= 0 &&
			acc->get_accessor_type() == GLTFAccessor::TYPE_VEC3 &&
			acc->get_component_type() == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT) {
		const Vector<Ref<GLTFBufferView>> &bvs = p_state->get_buffer_views();
		const GLTFBufferViewIndex bvi = acc->get_buffer_view();
		if (bvi >= 0 && bvi < bvs.size()) {
			const uint8_t *vptr = nullptr;
			int64_t vlen = 0;
			if (bvs[bvi]->get_view_span(p_state.ptr(), vptr, vlen)) {
				const int64_t cnt = acc->get_count();
				const int64_t stride = bvs[bvi]->get_byte_stride() > 0 ? bvs[bvi]->get_byte_stride() : 12;
				const int64_t off = acc->get_byte_offset();
				if (off >= 0 && (cnt == 0 || off + (cnt - 1) * stride + 12 <= vlen)) {
					r_out.resize(cnt);
					Vector3 *dst = r_out.ptrw();
					if (stride == 12 && sizeof(Vector3) == 12) {
						if (cnt > 0) {
							std::memcpy(dst, vptr + off, size_t(cnt) * 12);
						}
					} else {
						for (int64_t i = 0; i < cnt; i++) {
							float f[3];
							std::memcpy(f, vptr + off + i * stride, 12);
							dst[i] = Vector3(f[0], f[1], f[2]);
						}
					}
					return true;
				}
			}
		}
	}
	r_out = acc->decode_as_vector3s(p_state);
	return true;
}

bool fast_decode_accessor_quat(const Ref<GLTFState> &p_state, int p_acc_idx, Vector<Quaternion> &r_out) {
	const Vector<Ref<GLTFAccessor>> &accs = p_state->get_accessors();
	ERR_FAIL_INDEX_V(p_acc_idx, accs.size(), false);
	const Ref<GLTFAccessor> &acc = accs[p_acc_idx];
	ERR_FAIL_COND_V(acc.is_null(), false);
	if (acc->get_sparse_count() == 0 && acc->get_buffer_view() >= 0 &&
			acc->get_accessor_type() == GLTFAccessor::TYPE_VEC4 &&
			acc->get_component_type() == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT) {
		const Vector<Ref<GLTFBufferView>> &bvs = p_state->get_buffer_views();
		const GLTFBufferViewIndex bvi = acc->get_buffer_view();
		if (bvi >= 0 && bvi < bvs.size()) {
			const uint8_t *vptr = nullptr;
			int64_t vlen = 0;
			if (bvs[bvi]->get_view_span(p_state.ptr(), vptr, vlen)) {
				const int64_t cnt = acc->get_count();
				const int64_t stride = bvs[bvi]->get_byte_stride() > 0 ? bvs[bvi]->get_byte_stride() : 16;
				const int64_t off = acc->get_byte_offset();
				if (off >= 0 && (cnt == 0 || off + (cnt - 1) * stride + 16 <= vlen)) {
					r_out.resize(cnt);
					Quaternion *dst = r_out.ptrw();
					for (int64_t i = 0; i < cnt; i++) {
						float f[4];
						std::memcpy(f, vptr + off + i * stride, 16);
						dst[i] = Quaternion(f[0], f[1], f[2], f[3]).normalized();
					}
					return true;
				}
			}
		}
	}
	r_out = acc->decode_as_quaternions(p_state);
	return true;
}

bool fast_decode_accessor_mat4(const Ref<GLTFState> &p_state, int p_acc_idx, Vector<Transform3D> &r_out) {
	const Vector<Ref<GLTFAccessor>> &accs = p_state->get_accessors();
	ERR_FAIL_INDEX_V(p_acc_idx, accs.size(), false);
	const Ref<GLTFAccessor> &acc = accs[p_acc_idx];
	ERR_FAIL_COND_V(acc.is_null(), false);
	if (acc->get_sparse_count() == 0 && acc->get_buffer_view() >= 0 &&
			acc->get_accessor_type() == GLTFAccessor::TYPE_MAT4 &&
			acc->get_component_type() == GLTFAccessor::COMPONENT_TYPE_SINGLE_FLOAT) {
		const Vector<Ref<GLTFBufferView>> &bvs = p_state->get_buffer_views();
		const GLTFBufferViewIndex bvi = acc->get_buffer_view();
		if (bvi >= 0 && bvi < bvs.size()) {
			const uint8_t *vptr = nullptr;
			int64_t vlen = 0;
			if (bvs[bvi]->get_view_span(p_state.ptr(), vptr, vlen)) {
				const int64_t cnt = acc->get_count();
				const int64_t stride = bvs[bvi]->get_byte_stride() > 0 ? bvs[bvi]->get_byte_stride() : 64;
				const int64_t off = acc->get_byte_offset();
				if (off >= 0 && (cnt == 0 || off + (cnt - 1) * stride + 64 <= vlen)) {
					r_out.resize(cnt);
					Transform3D *dst = r_out.ptrw();
					for (int64_t i = 0; i < cnt; i++) {
						float m[16];
						std::memcpy(m, vptr + off + i * stride, 64);
						Transform3D xf;
						xf.basis.set_column(Vector3::AXIS_X, Vector3(m[0], m[1], m[2]));
						xf.basis.set_column(Vector3::AXIS_Y, Vector3(m[4], m[5], m[6]));
						xf.basis.set_column(Vector3::AXIS_Z, Vector3(m[8], m[9], m[10]));
						xf.set_origin(Vector3(m[12], m[13], m[14]));
						dst[i] = xf;
					}
					return true;
				}
			}
		}
	}
	Array arr = acc->decode_as_variants(p_state, Variant::TRANSFORM3D);
	r_out.resize(arr.size());
	for (int i = 0; i < arr.size(); i++) {
		r_out.write[i] = arr[i];
	}
	return true;
}

} // namespace

Error gltf_yyjson_populate_state(
		const GLTFYyjsonDoc &p_jdoc,
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		const String &p_search_path,
		bool &r_used_direct_pipeline) {
	r_used_direct_pipeline = false;
	ERR_FAIL_NULL_V(p_doc, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(p_state.is_null() || !p_jdoc.is_valid(), ERR_INVALID_PARAMETER);

	yyjson_val *root = p_jdoc.root();
	ERR_FAIL_COND_V(!root || !yyjson_is_obj(root), ERR_PARSE_ERROR);

	// 1. Parse asset header.
	yyjson_val *asset_obj = yyjson_obj_get(root, "asset");
	ERR_FAIL_COND_V(!asset_obj || !yyjson_is_obj(asset_obj), ERR_PARSE_ERROR);
	yyjson_val *ver_val = yyjson_obj_get(asset_obj, "version");
	ERR_FAIL_COND_V(!ver_val || !yyjson_is_str(ver_val), ERR_PARSE_ERROR);
	const String ver_str = yj_str(ver_val);
	GLTFFastAccess::major_version(p_state.ptr()) = ver_str.get_slicec('.', 0).to_int();
	GLTFFastAccess::minor_version(p_state.ptr()) = ver_str.get_slicec('.', 1).to_int();
	if (yyjson_val *cp_val = yyjson_obj_get(asset_obj, "copyright")) {
		GLTFFastAccess::copyright(p_state.ptr()) = yj_str(cp_val);
	}

	// 2. Parse extensionsUsed & extensionsRequired and check supported set.
	Vector<String> &exts_used = GLTFFastAccess::extensions_used(p_state.ptr());
	Vector<String> &exts_req = GLTFFastAccess::extensions_required(p_state.ptr());
	exts_used.clear();
	exts_req.clear();

	bool needs_extension_fallback = false;
	if (yyjson_val *eu_arr = yyjson_obj_get(root, "extensionsUsed")) {
		if (yyjson_is_arr(eu_arr)) {
			yyjson_arr_iter it = yyjson_arr_iter_with(eu_arr);
			yyjson_val *el = nullptr;
			while ((el = yyjson_arr_iter_next(&it)) != nullptr) {
				if (yyjson_is_str(el)) {
					const String s = yj_str(el);
					exts_used.push_back(s);
					if (s == "OMI_physics_body" || s == "OMI_physics_shape" || s == "GODOT_physics_body" || s == "KHR_animation_pointer") {
						needs_extension_fallback = true;
					}
				}
			}
		}
	}

	if (yyjson_val *er_arr = yyjson_obj_get(root, "extensionsRequired")) {
		if (yyjson_is_arr(er_arr)) {
			yyjson_arr_iter it = yyjson_arr_iter_with(er_arr);
			yyjson_val *el = nullptr;
			while ((el = yyjson_arr_iter_next(&it)) != nullptr) {
				if (yyjson_is_str(el)) {
					exts_req.push_back(yj_str(el));
				}
			}
		}
	}

	if (!exts_req.is_empty()) {
		const HashSet<String> supported = GLTFDocument::get_supported_gltf_extensions_hashset();
		Error req_err = OK;
		for (int i = 0; i < exts_req.size(); i++) {
			if (!supported.has(exts_req[i])) {
				ERR_PRINT("glTF: Can't import file '" + GLTFFastAccess::filename(p_state.ptr()) + "', required extension '" + exts_req[i] + "' is not supported. Are you missing a GLTFDocumentExtension plugin?");
				req_err = ERR_UNAVAILABLE;
			}
		}
		if (req_err != OK) {
			return req_err;
		}
	}

	// Check if editor import or custom user GLTFDocumentExtensions require Dictionary fallback.
	if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
		needs_extension_fallback = true;
	}
	if (!needs_extension_fallback) {
		const Vector<Ref<GLTFDocumentExtension>> all_exts = GLTFDocument::get_all_gltf_document_extensions();
		for (int i = 0; i < all_exts.size(); i++) {
			if (!is_builtin_standard_doc_ext(all_exts[i])) {
				needs_extension_fallback = true;
				break;
			}
		}
	}
	if (needs_extension_fallback) {
		r_used_direct_pipeline = false;
		return OK;
	}

	// 3. Parse buffers (keeping buffer 0 zero-copy on glb_bin_ptr when possible).
	Vector<Vector<uint8_t>> &state_buffers = GLTFFastAccess::buffers(p_state.ptr());
	state_buffers.clear();
	if (yyjson_val *bufs_arr = yyjson_obj_get(root, "buffers")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(bufs_arr), ERR_PARSE_ERROR);
		const size_t buf_cnt = yyjson_arr_size(bufs_arr);
		state_buffers.resize(int(buf_cnt));
		yyjson_arr_iter it = yyjson_arr_iter_with(bufs_arr);
		size_t bi = 0;
		yyjson_val *bobj = nullptr;
		while ((bobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(bobj), ERR_PARSE_ERROR);
			yyjson_val *uri_val = yyjson_obj_get(bobj, "uri");
			if (uri_val && yyjson_is_str(uri_val)) {
				if (bi == 0) {
					GLTFFastAccess::buffer0_uses_glb_span(p_state.ptr()) = false;
				}
				String uri = yj_str(uri_val);
				Vector<uint8_t> buf_data;
				if (uri.begins_with("data:")) {
					buf_data = decode_base64_uri(uri);
				} else {
					ERR_FAIL_COND_V(p_search_path.is_empty(), ERR_INVALID_PARAMETER);
					uri = p_search_path.path_join(uri.uri_file_decode()).replace_char('\\', '/');
					ERR_FAIL_COND_V_MSG(!FileAccess::exists(uri), ERR_FILE_NOT_FOUND, "glTF: Binary file not found: " + uri);
					buf_data = FileAccess::get_file_as_bytes(uri);
					ERR_FAIL_COND_V_MSG(buf_data.is_empty(), ERR_PARSE_ERROR, "glTF: Couldn't load binary file: " + uri);
				}
				const int64_t declared_len = yj_int(yyjson_obj_get(bobj, "byteLength"), -1);
				ERR_FAIL_COND_V(declared_len < 0 || buf_data.size() < declared_len, ERR_PARSE_ERROR);
				state_buffers.write[int(bi)] = buf_data;
			} else if (bi == 0 && GLTFFastAccess::glb_bin_ptr(p_state.ptr()) != nullptr) {
				// Zero-copy: leave state_buffers[0] empty; GLTFState::get_raw_buffer_span(0) serves glb_bin_ptr directly!
				GLTFFastAccess::buffer0_uses_glb_span(p_state.ptr()) = true;
			} else if (bi == 0 && GLTFFastAccess::glb_data(p_state.ptr()).size() > 0) {
				state_buffers.write[0] = GLTFFastAccess::glb_data(p_state.ptr());
			} else if (is_meshopt_fallback_buffer(bobj)) {
				// Valid EXT/KHR_meshopt_compression dummy fallback buffer: leave empty silently.
			} else {
				ERR_PRINT("glTF: Buffer " + itos(int64_t(bi)) + " has no data and cannot be loaded.");
			}
			bi++;
		}
	}

	// 4. Parse bufferViews & parallel-decode meshopt views (<= 3 threads via LoadThreadPool).
	Vector<Ref<GLTFBufferView>> &state_bvs = GLTFFastAccess::buffer_views(p_state.ptr());
	state_bvs.clear();
	LocalVector<int> meshopt_bv_indices;
	if (yyjson_val *bvs_arr = yyjson_obj_get(root, "bufferViews")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(bvs_arr), ERR_PARSE_ERROR);
		const size_t bv_cnt = yyjson_arr_size(bvs_arr);
		state_bvs.resize(int(bv_cnt));
		yyjson_arr_iter it = yyjson_arr_iter_with(bvs_arr);
		int bvi = 0;
		yyjson_val *bvobj = nullptr;
		while ((bvobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(bvobj), ERR_PARSE_ERROR);
			Ref<GLTFBufferView> bv;
			bv.instantiate();
			bv->set_buffer(GLTFBufferIndex(yj_int(yyjson_obj_get(bvobj, "buffer"), -1)));
			bv->set_byte_length(yj_int(yyjson_obj_get(bvobj, "byteLength"), 0));
			bv->set_byte_offset(yj_int(yyjson_obj_get(bvobj, "byteOffset"), 0));
			if (yyjson_val *bs_val = yyjson_obj_get(bvobj, "byteStride")) {
				bv->set_byte_stride(yj_int(bs_val, -1));
			}
			if (yyjson_val *tgt_val = yyjson_obj_get(bvobj, "target")) {
				const int tgt = int(yj_int(tgt_val, 0));
				bv->set_indices(tgt == GLTFBufferView::TARGET_ELEMENT_ARRAY_BUFFER);
				bv->set_vertex_attributes(tgt == GLTFBufferView::TARGET_ARRAY_BUFFER);
			}
			parse_yyjson_meshopt_bv(bvobj, bv);
			if (bv->has_meshopt_compression()) {
				meshopt_bv_indices.push_back(bvi);
			}
			state_bvs.write[bvi++] = bv;
		}
	}
	if (!meshopt_bv_indices.is_empty()) {
		std::atomic<int> meshopt_err{ int(OK) };
		auto decode_one_bv = [&](uint32_t idx) {
			if (meshopt_err.load(std::memory_order_relaxed) != int(OK)) {
				return;
			}
			const int bvi = meshopt_bv_indices[idx];
			const Error e = state_bvs.write[bvi]->decode_meshopt(p_state.ptr());
			if (e != OK) {
				meshopt_err.store(int(e), std::memory_order_relaxed);
			}
		};
		if (meshopt_bv_indices.size() >= 2 && LoadThreadPool::get_singleton()) {
			LoadThreadPool::get_singleton()->parallel_for(meshopt_bv_indices.size(), decode_one_bv);
		} else {
			for (uint32_t idx = 0; idx < meshopt_bv_indices.size(); idx++) {
				decode_one_bv(idx);
			}
		}
		const Error m_err = Error(meshopt_err.load(std::memory_order_relaxed));
		ERR_FAIL_COND_V(m_err != OK, m_err);
	}

	// 5. Parse accessors (including sparse metadata).
	Vector<Ref<GLTFAccessor>> &state_accs = GLTFFastAccess::accessors(p_state.ptr());
	state_accs.clear();
	if (yyjson_val *accs_arr = yyjson_obj_get(root, "accessors")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(accs_arr), ERR_PARSE_ERROR);
		const size_t acc_cnt = yyjson_arr_size(accs_arr);
		state_accs.resize(int(acc_cnt));
		yyjson_arr_iter it = yyjson_arr_iter_with(accs_arr);
		int ai = 0;
		yyjson_val *aobj = nullptr;
		while ((aobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(aobj), ERR_PARSE_ERROR);
			Ref<GLTFAccessor> acc;
			acc.instantiate();
			acc->set_buffer_view(GLTFBufferViewIndex(yj_int(yyjson_obj_get(aobj, "bufferView"), -1)));
			acc->set_byte_offset(yj_int(yyjson_obj_get(aobj, "byteOffset"), 0));
			yyjson_val *ct_val = yyjson_obj_get(aobj, "componentType");
			yyjson_val *cnt_val = yyjson_obj_get(aobj, "count");
			yyjson_val *tp_val = yyjson_obj_get(aobj, "type");
			ERR_FAIL_COND_V(!ct_val || !cnt_val || !tp_val, ERR_PARSE_ERROR);
			acc->set_component_type(GLTFAccessor::GLTFComponentType(yj_int(ct_val, 0)));
			acc->set_normalized(yj_bool(yyjson_obj_get(aobj, "normalized"), false));
			acc->set_count(yj_int(cnt_val, 0));
			bool type_ok = false;
			acc->set_accessor_type(parse_accessor_type(tp_val, type_ok));
			ERR_FAIL_COND_V(!type_ok, ERR_PARSE_ERROR);

			if (yyjson_val *min_arr = yyjson_obj_get(aobj, "min")) {
				if (yyjson_is_arr(min_arr)) {
					Vector<double> v;
					v.resize(int(yyjson_arr_size(min_arr)));
					yyjson_arr_iter mit = yyjson_arr_iter_with(min_arr);
					int mi = 0;
					yyjson_val *mv = nullptr;
					while ((mv = yyjson_arr_iter_next(&mit)) != nullptr) {
						v.write[mi++] = yj_num(mv, 0.0);
					}
					acc->set_min(v);
				}
			}
			if (yyjson_val *max_arr = yyjson_obj_get(aobj, "max")) {
				if (yyjson_is_arr(max_arr)) {
					Vector<double> v;
					v.resize(int(yyjson_arr_size(max_arr)));
					yyjson_arr_iter mit = yyjson_arr_iter_with(max_arr);
					int mi = 0;
					yyjson_val *mv = nullptr;
					while ((mv = yyjson_arr_iter_next(&mit)) != nullptr) {
						v.write[mi++] = yj_num(mv, 0.0);
					}
					acc->set_max(v);
				}
			}
			if (yyjson_val *sp_obj = yyjson_obj_get(aobj, "sparse")) {
				ERR_FAIL_COND_V(!yyjson_is_obj(sp_obj), ERR_PARSE_ERROR);
				const int64_t sp_cnt = yj_int(yyjson_obj_get(sp_obj, "count"), 0);
				acc->set_sparse_count(sp_cnt);
				if (sp_cnt > 0) {
					yyjson_val *sp_idx = yyjson_obj_get(sp_obj, "indices");
					yyjson_val *sp_val = yyjson_obj_get(sp_obj, "values");
					ERR_FAIL_COND_V(!sp_idx || !yyjson_is_obj(sp_idx) || !sp_val || !yyjson_is_obj(sp_val), ERR_PARSE_ERROR);
					acc->set_sparse_indices_buffer_view(GLTFBufferViewIndex(yj_int(yyjson_obj_get(sp_idx, "bufferView"), -1)));
					acc->set_sparse_indices_byte_offset(yj_int(yyjson_obj_get(sp_idx, "byteOffset"), 0));
					acc->set_sparse_indices_component_type(GLTFAccessor::GLTFComponentType(yj_int(yyjson_obj_get(sp_idx, "componentType"), 0)));
					acc->set_sparse_values_buffer_view(GLTFBufferViewIndex(yj_int(yyjson_obj_get(sp_val, "bufferView"), -1)));
					acc->set_sparse_values_byte_offset(yj_int(yyjson_obj_get(sp_val, "byteOffset"), 0));
				}
			}
			state_accs.write[ai++] = acc;
		}
	}

	// 6. Parse scenes.
	GLTFFastAccess::unique_names(p_state.ptr()).insert("Skeleton3D");
	Vector<int> &rnodes = GLTFFastAccess::root_nodes(p_state.ptr());
	rnodes.clear();
	if (yyjson_val *scenes_arr = yyjson_obj_get(root, "scenes")) {
		if (yyjson_is_arr(scenes_arr) && yyjson_arr_size(scenes_arr) > 0) {
			const int loaded_scene = int(yj_int(yyjson_obj_get(root, "scene"), 0));
			ERR_FAIL_COND_V(loaded_scene < 0 || size_t(loaded_scene) >= yyjson_arr_size(scenes_arr), ERR_FILE_CORRUPT);
			yyjson_val *sobj = yyjson_arr_get(scenes_arr, size_t(loaded_scene));
			ERR_FAIL_COND_V(!sobj || !yyjson_is_obj(sobj), ERR_PARSE_ERROR);
			if (yyjson_val *snodes = yyjson_obj_get(sobj, "nodes")) {
				if (yyjson_is_arr(snodes)) {
					rnodes.resize(int(yyjson_arr_size(snodes)));
					yyjson_arr_iter nit = yyjson_arr_iter_with(snodes);
					int ri = 0;
					yyjson_val *nv = nullptr;
					while ((nv = yyjson_arr_iter_next(&nit)) != nullptr) {
						rnodes.write[ri++] = int(yj_int(nv, 0));
					}
				}
			}
			const String sname = yj_str(yyjson_obj_get(sobj, "name"));
			if (!sname.is_empty() && !sname.begins_with("Scene")) {
				GLTFFastAccess::scene_name(p_state.ptr()) = sname;
			} else if (GLTFFastAccess::scene_name(p_state.ptr()).is_empty()) {
				GLTFFastAccess::scene_name(p_state.ptr()) = GLTFFastAccess::filename(p_state.ptr());
			}
			if (p_doc->get_naming_version() == 0) {
				GLTFFastAccess::scene_name(p_state.ptr()) = GLTFFastAccess::gen_unique_name(p_doc, p_state, GLTFFastAccess::scene_name(p_state.ptr()));
			}
		}
	}

	// 7. Parse nodes & hierarchy.
	Vector<Ref<GLTFNode>> &state_nodes = GLTFFastAccess::nodes(p_state.ptr());
	state_nodes.clear();
	if (yyjson_val *nodes_arr = yyjson_obj_get(root, "nodes")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(nodes_arr), ERR_PARSE_ERROR);
		const int node_cnt = int(yyjson_arr_size(nodes_arr));
		state_nodes.resize(node_cnt);
		yyjson_arr_iter it = yyjson_arr_iter_with(nodes_arr);
		int ni = 0;
		yyjson_val *nobj = nullptr;
		while ((nobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(nobj), ERR_PARSE_ERROR);
			Ref<GLTFNode> node;
			node.instantiate();
			if (yyjson_val *nm = yyjson_obj_get(nobj, "name")) {
				const String ns = yj_str(nm);
				node->set_original_name(ns);
				node->set_name(ns);
			}
			if (yyjson_val *cv = yyjson_obj_get(nobj, "camera")) {
				GLTFFastAccess::node_camera(node.ptr()) = GLTFCameraIndex(yj_int(cv, -1));
			}
			if (yyjson_val *mv = yyjson_obj_get(nobj, "mesh")) {
				GLTFFastAccess::node_mesh(node.ptr()) = GLTFMeshIndex(yj_int(mv, -1));
			}
			if (yyjson_val *sv = yyjson_obj_get(nobj, "skin")) {
				GLTFFastAccess::node_skin(node.ptr()) = GLTFSkinIndex(yj_int(sv, -1));
			}
			if (yyjson_val *mat_arr = yyjson_obj_get(nobj, "matrix")) {
				Transform3D xf;
				ERR_FAIL_COND_V(!yj_xform(mat_arr, xf), ERR_PARSE_ERROR);
				GLTFFastAccess::node_xform(node.ptr()) = xf;
			} else {
				Vector3 pos(0.0f, 0.0f, 0.0f);
				Quaternion rot;
				Vector3 scl(1.0f, 1.0f, 1.0f);
				bool has_trs = false;
				if (yyjson_val *tv = yyjson_obj_get(nobj, "translation")) {
					ERR_FAIL_COND_V(!yj_vec3(tv, pos), ERR_PARSE_ERROR);
					has_trs = true;
				}
				if (yyjson_val *rv = yyjson_obj_get(nobj, "rotation")) {
					ERR_FAIL_COND_V(!yj_quat(rv, rot), ERR_PARSE_ERROR);
					has_trs = true;
				}
				if (yyjson_val *sv = yyjson_obj_get(nobj, "scale")) {
					ERR_FAIL_COND_V(!yj_vec3(sv, scl), ERR_PARSE_ERROR);
					has_trs = true;
				}
				if (has_trs) {
					Transform3D xf;
					xf.basis.set_quaternion_scale(rot.normalized(), scl);
					xf.origin = pos;
					GLTFFastAccess::node_xform(node.ptr()) = xf;
				}
			}
			node->set_additional_data(SNAME("GODOT_rest_transform"), GLTFFastAccess::node_xform(node.ptr()));

			if (yyjson_val *w_arr = yyjson_obj_get(nobj, "weights")) {
				if (yyjson_is_arr(w_arr) && yyjson_arr_size(w_arr) > 0) {
					Vector<float> nw;
					nw.resize(int(yyjson_arr_size(w_arr)));
					yyjson_arr_iter wit = yyjson_arr_iter_with(w_arr);
					int wi = 0;
					yyjson_val *wv = nullptr;
					while ((wv = yyjson_arr_iter_next(&wit)) != nullptr) {
						nw.write[wi++] = float(yj_num(wv, 0.0));
					}
					node->set_additional_data(SNAME("_gltf_node_weights"), nw);
				}
			}

			if (yyjson_val *nexts = yyjson_obj_get(nobj, "extensions")) {
				if (yyjson_is_obj(nexts)) {
					if (yyjson_val *lp = yyjson_obj_get(nexts, "KHR_lights_punctual")) {
						if (yyjson_is_obj(lp)) {
							if (yyjson_val *lv = yyjson_obj_get(lp, "light")) {
								GLTFFastAccess::node_light(node.ptr()) = GLTFLightIndex(yj_int(lv, -1));
							}
						}
					}
					if (yyjson_val *vis = yyjson_obj_get(nexts, "KHR_node_visibility")) {
						if (yyjson_is_obj(vis)) {
							if (yyjson_val *vv = yyjson_obj_get(vis, "visible")) {
								GLTFFastAccess::node_visible(node.ptr()) = yj_bool(vv, true);
							}
						}
					}
				}
			}

			attach_yyjson_extras_to_meta(yyjson_obj_get(nobj, "extras"), node);

			if (yyjson_val *ch_arr = yyjson_obj_get(nobj, "children")) {
				if (yyjson_is_arr(ch_arr) && yyjson_arr_size(ch_arr) > 0) {
					Vector<int> &children = GLTFFastAccess::node_children(node.ptr());
					children.resize(int(yyjson_arr_size(ch_arr)));
					yyjson_arr_iter cit = yyjson_arr_iter_with(ch_arr);
					int ci = 0;
					yyjson_val *cv = nullptr;
					while ((cv = yyjson_arr_iter_next(&cit)) != nullptr) {
						children.write[ci++] = int(yj_int(cv, -1));
					}
				}
			}
			state_nodes.write[ni++] = node;
		}

		// Link parents and compute heights (with cycle guard).
		for (int i = 0; i < node_cnt; i++) {
			const Vector<int> &ch = GLTFFastAccess::node_children(state_nodes[i].ptr());
			for (int j = 0; j < ch.size(); j++) {
				const int child_i = ch[j];
				ERR_FAIL_INDEX_V(child_i, node_cnt, ERR_FILE_CORRUPT);
				ERR_CONTINUE(GLTFFastAccess::node_parent(state_nodes[child_i].ptr()) != -1);
				GLTFFastAccess::node_parent(state_nodes.write[child_i].ptr()) = i;
			}
		}
		if (p_doc->get_naming_version() < 2) {
			rnodes.clear();
		}
		for (int i = 0; i < node_cnt; i++) {
			int h = 0;
			int cur = GLTFFastAccess::node_parent(state_nodes[i].ptr());
			while (cur >= 0 && h <= node_cnt) {
				h++;
				cur = GLTFFastAccess::node_parent(state_nodes[cur].ptr());
			}
			ERR_FAIL_COND_V(h > node_cnt, ERR_FILE_CORRUPT);
			GLTFFastAccess::node_height(state_nodes.write[i].ptr()) = h;
			if (p_doc->get_naming_version() < 2 && h == 0) {
				rnodes.push_back(i);
			}
		}
	}

	// 8. Parse images, samplers, textures, and materials (unless discarded).
	if (!GLTFFastAccess::discard_meshes_and_materials(p_state.ptr())) {
		Vector<Ref<Texture2D>> &state_images = GLTFFastAccess::images(p_state.ptr());
		Vector<Ref<Image>> &state_src_images = GLTFFastAccess::source_images(p_state.ptr());

		if (yyjson_val *imgs_arr = yyjson_obj_get(root, "images")) {
			ERR_FAIL_COND_V(!yyjson_is_arr(imgs_arr), ERR_PARSE_ERROR);
			const int img_cnt = int(yyjson_arr_size(imgs_arr));
			// Preserve caller-supplied textures when pre-populated (e.g. ANGE prepared textures).
			if (state_images.size() != img_cnt && img_cnt > 0) {
				state_images.resize(img_cnt);
				state_src_images.resize(img_cnt);

				struct PendingImgJob {
					PackedByteArray raw_bytes;
					String mime_type;
					String name;
					Ref<Texture2D> external_tex;
					Ref<Image> decoded_img;
					bool needs_decode = false;
				};
				LocalVector<PendingImgJob> jobs;
				jobs.resize(uint32_t(img_cnt));
				HashSet<String> used_img_names;
				const GLTFState::HandleBinaryImageMode img_mode = GLTFFastAccess::handle_binary_image_mode(p_state.ptr());

				yyjson_arr_iter it = yyjson_arr_iter_with(imgs_arr);
				int ii = 0;
				yyjson_val *iobj = nullptr;
				while ((iobj = yyjson_arr_iter_next(&it)) != nullptr) {
					ERR_FAIL_COND_V(!yyjson_is_obj(iobj), ERR_PARSE_ERROR);
					PendingImgJob &job = jobs[uint32_t(ii)];
					job.mime_type = yj_str(yyjson_obj_get(iobj, "mimeType"));
					String iname = yj_str(yyjson_obj_get(iobj, "name")).get_file().get_basename().validate_filename();
					if (iname.is_empty()) {
						iname = itos(ii);
					}
					while (used_img_names.has(iname)) {
						iname += "_" + itos(ii);
					}
					used_img_names.insert(iname);
					job.name = iname;

					yyjson_val *uri_val = yyjson_obj_get(iobj, "uri");
					yyjson_val *bv_val = yyjson_obj_get(iobj, "bufferView");
					if (img_mode == GLTFState::HANDLE_BINARY_IMAGE_MODE_DISCARD_TEXTURES) {
						ii++;
						continue;
					}
					if (uri_val && yyjson_is_str(uri_val)) {
						String uri = yj_str(uri_val);
						if (uri.begins_with("data:")) {
							job.raw_bytes = decode_base64_uri(uri);
							if (job.mime_type.is_empty() && uri.contains_char(';')) {
								job.mime_type = uri.substr(5, uri.find(";base64") - 5);
							}
							job.needs_decode = !job.raw_bytes.is_empty();
						} else {
							ERR_FAIL_COND_V(p_search_path.is_empty(), ERR_INVALID_PARAMETER);
							const String res_uri = p_search_path.path_join(uri.uri_file_decode()).replace_char('\\', '/').simplify_path();
							if (ResourceLoader::exists(res_uri)) {
								job.external_tex = ResourceLoader::load(res_uri, "Texture2D");
							}
							if (job.external_tex.is_null()) {
								if (job.mime_type.is_empty()) {
									job.mime_type = "image/" + res_uri.get_extension().to_lower();
								}
								job.raw_bytes = FileAccess::get_file_as_bytes(res_uri);
								job.needs_decode = !job.raw_bytes.is_empty();
							}
						}
					} else if (bv_val) {
						ERR_FAIL_COND_V_MSG(job.mime_type.is_empty(), ERR_FILE_CORRUPT, vformat("glTF: Image index '%d' specifies 'bufferView' without 'mimeType'.", ii));
						const int bvi = int(yj_int(bv_val, -1));
						ERR_FAIL_INDEX_V(bvi, state_bvs.size(), ERR_PARAMETER_RANGE_ERROR);
						const uint8_t *vptr = nullptr;
						int64_t vlen = 0;
						ERR_FAIL_COND_V(!state_bvs[bvi]->get_view_span(p_state.ptr(), vptr, vlen), ERR_FILE_CORRUPT);
						if (vlen > 0) {
							job.raw_bytes.resize(vlen);
							std::memcpy(job.raw_bytes.ptrw(), vptr, size_t(vlen));
							job.needs_decode = true;
						}
					}
					ii++;
				}

				// Decode raw image buffers in parallel on LoadThreadPool (<= 3 low-priority workers).
				uint32_t decode_job_count = 0;
				for (int i = 0; i < img_cnt; i++) {
					if (jobs[uint32_t(i)].needs_decode) {
						decode_job_count++;
					}
				}
				if (decode_job_count >= 2 && LoadThreadPool::get_singleton()) {
					LoadThreadPool::get_singleton()->parallel_for(uint32_t(img_cnt), [&](uint32_t idx) {
						PendingImgJob &job = jobs[idx];
						if (job.needs_decode) {
							job.decoded_img = decode_raw_image_bytes(job.raw_bytes, job.mime_type, int(idx));
							if (job.decoded_img.is_valid()) {
								job.decoded_img->set_name(job.name);
							}
						}
					});
				} else {
					for (int i = 0; i < img_cnt; i++) {
						PendingImgJob &job = jobs[uint32_t(i)];
						if (job.needs_decode) {
							job.decoded_img = decode_raw_image_bytes(job.raw_bytes, job.mime_type, i);
							if (job.decoded_img.is_valid()) {
								job.decoded_img->set_name(job.name);
							}
						}
					}
				}

				// Upload / wrap Texture2D objects on the caller thread.
				const bool defer_uncompressed = bool(p_state->get_additional_data(SNAME("_ange_defer_uncompressed_images")));
				for (int i = 0; i < img_cnt; i++) {
					PendingImgJob &job = jobs[uint32_t(i)];
					if (job.external_tex.is_valid()) {
						state_images.write[i] = job.external_tex;
						state_src_images.write[i] = job.external_tex->has_meta(SNAME("_ange_gpu_source_image"))
								? Ref<Image>(job.external_tex->get_meta(SNAME("_ange_gpu_source_image")))
								: job.external_tex->get_image();
					} else if (job.decoded_img.is_valid() && !job.decoded_img->is_empty()) {
						if (img_mode == GLTFState::HANDLE_BINARY_IMAGE_MODE_EMBED_AS_BASISU) {
							Ref<PortableCompressedTexture2D> ptex;
							ptex.instantiate();
							ptex->set_name(job.name);
							ptex->set_keep_compressed_buffer(true);
							ptex->create_from_image(job.decoded_img, PortableCompressedTexture2D::COMPRESSION_MODE_BASIS_UNIVERSAL);
							state_images.write[i] = ptex;
							state_src_images.write[i] = job.decoded_img;
						} else if (defer_uncompressed && !job.decoded_img->is_compressed()) {
							Ref<PlaceholderTexture2D> ptex;
							ptex.instantiate();
							ptex->set_name(job.name);
							ptex->set_size(Vector2i(job.decoded_img->get_width(), job.decoded_img->get_height()));
							ptex->set_meta(SNAME("_ange_gpu_source_image"), job.decoded_img);
							ptex->set_meta(SNAME("_ange_gltf_placeholder"), true);
							state_images.write[i] = ptex;
							state_src_images.write[i] = job.decoded_img;
						} else {
							const bool authored_blocks = job.decoded_img->is_compressed() && job.decoded_img->has_mipmaps();
							if (authored_blocks) {
								job.decoded_img->set_meta(SNAME("_ange_authored_block_texture"), true);
							}
							Ref<ImageTexture> itex;
							itex.instantiate();
							itex->set_name(job.name);
							itex->set_image(job.decoded_img);
							if (authored_blocks) {
								itex->set_meta(SNAME("_ange_authored_block_texture"), true);
							}
							state_images.write[i] = itex;
							state_src_images.write[i] = job.decoded_img;
						}
					}
				}
			}
		}

		// Parse texture samplers.
		Ref<GLTFTextureSampler> &def_sampler = GLTFFastAccess::default_texture_sampler(p_state.ptr());
		def_sampler.instantiate();
		def_sampler->set_min_filter(GLTFTextureSampler::FilterMode::LINEAR_MIPMAP_LINEAR);
		def_sampler->set_mag_filter(GLTFTextureSampler::FilterMode::LINEAR);
		def_sampler->set_wrap_s(GLTFTextureSampler::WrapMode::REPEAT);
		def_sampler->set_wrap_t(GLTFTextureSampler::WrapMode::REPEAT);

		Vector<Ref<GLTFTextureSampler>> &state_samplers = GLTFFastAccess::texture_samplers(p_state.ptr());
		state_samplers.clear();
		if (yyjson_val *samp_arr = yyjson_obj_get(root, "samplers")) {
			ERR_FAIL_COND_V(!yyjson_is_arr(samp_arr), ERR_PARSE_ERROR);
			state_samplers.resize(int(yyjson_arr_size(samp_arr)));
			yyjson_arr_iter it = yyjson_arr_iter_with(samp_arr);
			int si = 0;
			yyjson_val *sobj = nullptr;
			while ((sobj = yyjson_arr_iter_next(&it)) != nullptr) {
				ERR_FAIL_COND_V(!yyjson_is_obj(sobj), ERR_PARSE_ERROR);
				Ref<GLTFTextureSampler> s;
				s.instantiate();
				s->set_min_filter(int(yj_int(yyjson_obj_get(sobj, "minFilter"), GLTFTextureSampler::FilterMode::LINEAR_MIPMAP_LINEAR)));
				s->set_mag_filter(int(yj_int(yyjson_obj_get(sobj, "magFilter"), GLTFTextureSampler::FilterMode::LINEAR)));
				s->set_wrap_s(int(yj_int(yyjson_obj_get(sobj, "wrapS"), GLTFTextureSampler::WrapMode::DEFAULT)));
				s->set_wrap_t(int(yj_int(yyjson_obj_get(sobj, "wrapT"), GLTFTextureSampler::WrapMode::DEFAULT)));
				state_samplers.write[si++] = s;
			}
		}

		// Parse textures (including KHR_texture_basisu & EXT_texture_webp source overrides).
		Vector<Ref<GLTFTexture>> &state_textures = GLTFFastAccess::textures(p_state.ptr());
		state_textures.clear();
		if (yyjson_val *tex_arr = yyjson_obj_get(root, "textures")) {
			ERR_FAIL_COND_V(!yyjson_is_arr(tex_arr), ERR_PARSE_ERROR);
			state_textures.resize(int(yyjson_arr_size(tex_arr)));
			yyjson_arr_iter it = yyjson_arr_iter_with(tex_arr);
			int ti = 0;
			yyjson_val *tobj = nullptr;
			while ((tobj = yyjson_arr_iter_next(&it)) != nullptr) {
				ERR_FAIL_COND_V(!yyjson_is_obj(tobj), ERR_PARSE_ERROR);
				Ref<GLTFTexture> gtex;
				gtex.instantiate();
				int src_img = -1;
				if (yyjson_val *texts = yyjson_obj_get(tobj, "extensions")) {
					if (yyjson_is_obj(texts)) {
						if (yyjson_val *kt = yyjson_obj_get(texts, "KHR_texture_basisu")) {
							if (yyjson_is_obj(kt)) {
								src_img = int(yj_int(yyjson_obj_get(kt, "source"), -1));
							}
						}
						if (src_img < 0) {
							if (yyjson_val *wt = yyjson_obj_get(texts, "EXT_texture_webp")) {
								if (yyjson_is_obj(wt)) {
									src_img = int(yj_int(yyjson_obj_get(wt, "source"), -1));
								}
							}
						}
					}
				}
				if (src_img < 0) {
					src_img = int(yj_int(yyjson_obj_get(tobj, "source"), -1));
				}
				ERR_FAIL_COND_V(src_img < 0, ERR_PARSE_ERROR);
				gtex->set_src_image(src_img);
				gtex->set_sampler(GLTFTextureSamplerIndex(yj_int(yyjson_obj_get(tobj, "sampler"), -1)));
				state_textures.write[ti++] = gtex;
			}
		}

		// Parse materials (core PBR + KHR_materials_* suite).
		Vector<Ref<Material>> &state_materials = GLTFFastAccess::materials(p_state.ptr());
		state_materials.clear();
		if (yyjson_val *mats_arr = yyjson_obj_get(root, "materials")) {
			ERR_FAIL_COND_V(!yyjson_is_arr(mats_arr), ERR_PARSE_ERROR);
			state_materials.resize(int(yyjson_arr_size(mats_arr)));
			yyjson_arr_iter it = yyjson_arr_iter_with(mats_arr);
			int mi = 0;
			yyjson_val *mobj = nullptr;
			while ((mobj = yyjson_arr_iter_next(&it)) != nullptr) {
				ERR_FAIL_COND_V(!yyjson_is_obj(mobj), ERR_PARSE_ERROR);
				Ref<StandardMaterial3D> mat;
				mat.instantiate();
				const String mname = yj_str(yyjson_obj_get(mobj, "name"));
				mat->set_name(!mname.is_empty() ? mname : vformat("material_%d", mi));

				yyjson_val *mexts = yyjson_obj_get(mobj, "extensions");
				if (mexts && yyjson_is_obj(mexts)) {
					if (yyjson_obj_get(mexts, "KHR_materials_unlit")) {
						mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
					}
					if (yyjson_val *es = yyjson_obj_get(mexts, "KHR_materials_emissive_strength")) {
						if (yyjson_is_obj(es)) {
							if (yyjson_val *esv = yyjson_obj_get(es, "emissiveStrength")) {
								mat->set_emission_energy_multiplier(float(yj_num(esv, 1.0)));
							}
						}
					}
				}

				int primary_uv = -1;
				yyjson_val *sg_ext = (mexts && yyjson_is_obj(mexts)) ? yyjson_obj_get(mexts, "KHR_materials_pbrSpecularGlossiness") : nullptr;
				if (sg_ext && yyjson_is_obj(sg_ext)) {
					Ref<GLTFSpecGloss> sg;
					sg.instantiate();
					if (yyjson_val *dt = yyjson_obj_get(sg_ext, "diffuseTexture")) {
						if (yyjson_is_obj(dt)) {
							const int dti = int(yj_int(yyjson_obj_get(dt, "index"), -1));
							if (dti >= 0) {
								Ref<GLTFTextureSampler> ds = GLTFFastAccess::get_sampler_for_texture(p_doc, p_state, dti);
								if (ds.is_valid()) {
									mat->set_texture_filter(ds->get_filter_mode());
									mat->set_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, ds->get_wrap_mode());
								}
								Ref<Texture2D> dtex = GLTFFastAccess::get_texture(p_doc, p_state, dti, GLTFDocument::TEXTURE_TYPE_GENERIC);
								if (dtex.is_valid()) {
									GLTFFastAccess::spec_gloss_diffuse_img(sg.ptr()) = dtex->has_meta(SNAME("_ange_gpu_source_image"))
											? Ref<Image>(dtex->get_meta(SNAME("_ange_gpu_source_image")))
											: dtex->get_image();
									mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, dtex);
								}
							}
							primary_uv = int(yj_int(yyjson_obj_get(dt, "texCoord"), 0));
						}
					}
					Color df;
					if (yj_color4(yyjson_obj_get(sg_ext, "diffuseFactor"), df)) {
						GLTFFastAccess::spec_gloss_diffuse_factor(sg.ptr()) = df.linear_to_srgb();
						mat->set_albedo(GLTFFastAccess::spec_gloss_diffuse_factor(sg.ptr()));
					}
					Color sf;
					if (yj_color3(yyjson_obj_get(sg_ext, "specularFactor"), sf)) {
						GLTFFastAccess::spec_gloss_specular_factor(sg.ptr()) = sf;
					}
					if (yyjson_val *gf = yyjson_obj_get(sg_ext, "glossinessFactor")) {
						const float g = float(yj_num(gf, 1.0));
						GLTFFastAccess::spec_gloss_gloss_factor(sg.ptr()) = g;
						mat->set_roughness(1.0f - CLAMP(g, 0.0f, 1.0f));
					}
					if (yyjson_val *sgt = yyjson_obj_get(sg_ext, "specularGlossinessTexture")) {
						if (yyjson_is_obj(sgt)) {
							const int sgti = int(yj_int(yyjson_obj_get(sgt, "index"), -1));
							if (sgti >= 0) {
								Ref<Texture2D> otex = GLTFFastAccess::get_texture(p_doc, p_state, sgti, GLTFDocument::TEXTURE_TYPE_GENERIC);
								if (otex.is_valid()) {
									GLTFFastAccess::spec_gloss_img(sg.ptr()) = otex->has_meta(SNAME("_ange_gpu_source_image"))
											? Ref<Image>(otex->get_meta(SNAME("_ange_gpu_source_image")))
											: otex->get_image();
								}
							}
							if (primary_uv == -1 && yyjson_obj_get(sgt, "texCoord")) {
								primary_uv = int(yj_int(yyjson_obj_get(sgt, "texCoord"), 0));
							}
						}
					}
					GLTFFastAccess::spec_gloss_to_rough_metal(p_doc, sg, mat);
				} else if (yyjson_val *pbr = yyjson_obj_get(mobj, "pbrMetallicRoughness")) {
					if (yyjson_is_obj(pbr)) {
						Color bcf;
						const bool has_bcf = yj_color4(yyjson_obj_get(pbr, "baseColorFactor"), bcf);
						if (has_bcf) {
							mat->set_albedo(bcf.linear_to_srgb());
						}
						if (yyjson_val *bct = yyjson_obj_get(pbr, "baseColorTexture")) {
							if (yyjson_is_obj(bct)) {
								const int bcti = int(yj_int(yyjson_obj_get(bct, "index"), -1));
								if (bcti >= 0) {
									mat->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, GLTFFastAccess::get_texture(p_doc, p_state, bcti, GLTFDocument::TEXTURE_TYPE_GENERIC));
									Ref<GLTFTextureSampler> bs = GLTFFastAccess::get_sampler_for_texture(p_doc, p_state, bcti);
									if (bs.is_valid()) {
										mat->set_texture_filter(bs->get_filter_mode());
										mat->set_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, bs->get_wrap_mode());
									}
								}
								primary_uv = int(yj_int(yyjson_obj_get(bct, "texCoord"), 0));
								if (!has_bcf) {
									mat->set_albedo(Color(1.0f, 1.0f, 1.0f));
								}
								apply_yyjson_texture_transform_uv1(bct, mat);
							}
						}
						mat->set_metallic(float(yj_num(yyjson_obj_get(pbr, "metallicFactor"), 1.0)));
						mat->set_roughness(float(yj_num(yyjson_obj_get(pbr, "roughnessFactor"), 1.0)));
						if (yyjson_val *mrt = yyjson_obj_get(pbr, "metallicRoughnessTexture")) {
							if (yyjson_is_obj(mrt)) {
								const int mrti = int(yj_int(yyjson_obj_get(mrt, "index"), -1));
								if (mrti >= 0) {
									Ref<Texture2D> t = GLTFFastAccess::get_texture(p_doc, p_state, mrti, GLTFDocument::TEXTURE_TYPE_GENERIC);
									mat->set_texture(BaseMaterial3D::TEXTURE_METALLIC, t);
									mat->set_metallic_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_BLUE);
									mat->set_texture(BaseMaterial3D::TEXTURE_ROUGHNESS, t);
									mat->set_roughness_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_GREEN);
								}
								if (primary_uv == -1 && yyjson_obj_get(mrt, "texCoord")) {
									primary_uv = int(yj_int(yyjson_obj_get(mrt, "texCoord"), 0));
								}
							}
						}
					}
				}

				if (yyjson_val *nt = yyjson_obj_get(mobj, "normalTexture")) {
					if (yyjson_is_obj(nt)) {
						const int nti = int(yj_int(yyjson_obj_get(nt, "index"), -1));
						if (nti >= 0) {
							mat->set_texture(BaseMaterial3D::TEXTURE_NORMAL, GLTFFastAccess::get_texture(p_doc, p_state, nti, GLTFDocument::TEXTURE_TYPE_NORMAL));
							mat->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
						}
						if (primary_uv == -1 && yyjson_obj_get(nt, "texCoord")) {
							primary_uv = int(yj_int(yyjson_obj_get(nt, "texCoord"), 0));
						}
						if (yyjson_val *nsc = yyjson_obj_get(nt, "scale")) {
							mat->set_normal_scale(float(yj_num(nsc, 1.0)));
						}
					}
				}

				int secondary_uv = -1;
				if (yyjson_val *ot = yyjson_obj_get(mobj, "occlusionTexture")) {
					if (yyjson_is_obj(ot)) {
						const int oti = int(yj_int(yyjson_obj_get(ot, "index"), -1));
						if (oti >= 0) {
							mat->set_texture(BaseMaterial3D::TEXTURE_AMBIENT_OCCLUSION, GLTFFastAccess::get_texture(p_doc, p_state, oti, GLTFDocument::TEXTURE_TYPE_GENERIC));
							mat->set_ao_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_RED);
							mat->set_feature(BaseMaterial3D::FEATURE_AMBIENT_OCCLUSION, true);
						}
						if (yyjson_val *otc = yyjson_obj_get(ot, "texCoord")) {
							const int uv = int(yj_int(otc, 0));
							if (primary_uv == -1) {
								primary_uv = uv;
							} else if (uv != primary_uv) {
								secondary_uv = uv;
								mat->set_flag(BaseMaterial3D::FLAG_AO_ON_UV2, true);
							}
						}
					}
				}

				Color ef;
				const bool has_ef = yj_color3(yyjson_obj_get(mobj, "emissiveFactor"), ef);
				if (has_ef) {
					mat->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
					mat->set_emission(ef.linear_to_srgb());
				}
				if (yyjson_val *et = yyjson_obj_get(mobj, "emissiveTexture")) {
					if (yyjson_is_obj(et)) {
						const int eti = int(yj_int(yyjson_obj_get(et, "index"), -1));
						if (eti >= 0) {
							mat->set_texture(BaseMaterial3D::TEXTURE_EMISSION, GLTFFastAccess::get_texture(p_doc, p_state, eti, GLTFDocument::TEXTURE_TYPE_GENERIC));
							mat->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
							mat->set_emission_operator(BaseMaterial3D::EMISSION_OP_MULTIPLY);
							if (!has_ef) {
								mat->set_emission(Color(1.0f, 1.0f, 1.0f));
							}
						}
						if (yyjson_val *etc = yyjson_obj_get(et, "texCoord")) {
							const int uv = int(yj_int(etc, 0));
							if (primary_uv == -1) {
								primary_uv = uv;
							} else if (uv != primary_uv && (secondary_uv == -1 || secondary_uv == uv)) {
								secondary_uv = uv;
								mat->set_flag(BaseMaterial3D::FLAG_EMISSION_ON_UV2, true);
							}
						}
					}
				}

				if (yj_bool(yyjson_obj_get(mobj, "doubleSided"), false)) {
					mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
				}
				if (yyjson_val *am = yyjson_obj_get(mobj, "alphaMode")) {
					if (yj_str_eq(am, "BLEND")) {
						mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
					} else if (yj_str_eq(am, "MASK")) {
						mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR);
					}
				}
				mat->set_alpha_scissor_threshold(float(yj_num(yyjson_obj_get(mobj, "alphaCutoff"), 0.5)));

				apply_yyjson_khr_material_extensions(p_doc, p_state, mexts, mat);
				attach_yyjson_extras_to_meta(yyjson_obj_get(mobj, "extras"), mat);

				if (p_doc->get_texture_map_mode() == GLTFDocument::TEXTURE_MAP_MODE_REMAP_TO_STANDARD_MATERIAL) {
					if (primary_uv != -1) {
						mat->set_meta("_gltf_primary_texture_coord", primary_uv);
					}
					if (secondary_uv != -1) {
						mat->set_meta("_gltf_secondary_texture_coord", secondary_uv);
					}
				}

				state_materials.write[mi++] = mat;
			}
		}
	}

	// 9. Parse skins, determine skeletons, and assign scene node names.
	Vector<Ref<GLTFSkin>> &state_skins = GLTFFastAccess::skins(p_state.ptr());
	state_skins.clear();
	if (yyjson_val *skins_arr = yyjson_obj_get(root, "skins")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(skins_arr), ERR_PARSE_ERROR);
		state_skins.resize(int(yyjson_arr_size(skins_arr)));
		yyjson_arr_iter it = yyjson_arr_iter_with(skins_arr);
		int si = 0;
		yyjson_val *sobj = nullptr;
		while ((sobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(sobj), ERR_PARSE_ERROR);
			yyjson_val *j_arr = yyjson_obj_get(sobj, "joints");
			ERR_FAIL_COND_V(!j_arr || !yyjson_is_arr(j_arr), ERR_PARSE_ERROR);

			Ref<GLTFSkin> skin;
			skin.instantiate();
			const int j_cnt = int(yyjson_arr_size(j_arr));
			if (yyjson_val *ibm_val = yyjson_obj_get(sobj, "inverseBindMatrices")) {
				Vector<Transform3D> &ibms = GLTFFastAccess::skin_inverse_binds(skin.ptr());
				ERR_FAIL_COND_V(!fast_decode_accessor_mat4(p_state, int(yj_int(ibm_val, -1)), ibms), ERR_PARSE_ERROR);
				ERR_FAIL_COND_V(ibms.size() != j_cnt, ERR_PARSE_ERROR);
			}

			Vector<GLTFNodeIndex> &joints = GLTFFastAccess::skin_joints(skin.ptr());
			Vector<GLTFNodeIndex> &joints_orig = GLTFFastAccess::skin_joints_original(skin.ptr());
			joints.resize(j_cnt);
			joints_orig.resize(j_cnt);
			yyjson_arr_iter jit = yyjson_arr_iter_with(j_arr);
			int ji = 0;
			yyjson_val *jv = nullptr;
			while ((jv = yyjson_arr_iter_next(&jit)) != nullptr) {
				const GLTFNodeIndex nidx = GLTFNodeIndex(yj_int(jv, -1));
				ERR_FAIL_INDEX_V(nidx, state_nodes.size(), ERR_PARSE_ERROR);
				joints.write[ji] = nidx;
				joints_orig.write[ji] = nidx;
				GLTFFastAccess::node_joint(state_nodes.write[nidx].ptr()) = true;
				ji++;
			}

			const String sk_name = yj_str(yyjson_obj_get(sobj, "name"));
			skin->set_name(!sk_name.is_empty() ? sk_name : vformat("skin_%d", si));
			if (yyjson_val *sr = yyjson_obj_get(sobj, "skeleton")) {
				GLTFFastAccess::skin_root(skin.ptr()) = GLTFNodeIndex(yj_int(sr, -1));
			}
			state_skins.write[si++] = skin;
		}
		for (int i = 0; i < state_skins.size(); i++) {
			ERR_FAIL_COND_V(SkinTool::_expand_skin(state_nodes, state_skins.write[i]) != OK, ERR_PARSE_ERROR);
			ERR_FAIL_COND_V(SkinTool::_verify_skin(state_nodes, state_skins.write[i]) != OK, ERR_PARSE_ERROR);
		}
	}

	Vector<Ref<GLTFSkeleton>> &state_skeletons = GLTFFastAccess::skeletons(p_state.ptr());
	if (p_state->get_import_as_skeleton_bones()) {
		ERR_FAIL_COND_V(SkinTool::_determine_skeletons(state_skins, state_nodes, state_skeletons, rnodes, true) != OK, ERR_PARSE_ERROR);
	} else {
		ERR_FAIL_COND_V(SkinTool::_determine_skeletons(state_skins, state_nodes, state_skeletons, Vector<GLTFNodeIndex>(), p_doc->get_naming_version() < 2) != OK, ERR_PARSE_ERROR);
	}
	GLTFFastAccess::assign_node_names(p_doc, p_state);

	// 10. Parse meshes directly into runtime ArrayMeshes!
	Vector<Ref<GLTFMesh>> &state_meshes = GLTFFastAccess::meshes(p_state.ptr());
	state_meshes.clear();
	if (yyjson_val *meshes_arr = yyjson_obj_get(root, "meshes")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(meshes_arr), ERR_PARSE_ERROR);
		state_meshes.resize(int(yyjson_arr_size(meshes_arr)));
		yyjson_arr_iter it = yyjson_arr_iter_with(meshes_arr);
		int mesh_i = 0;
		yyjson_val *mobj = nullptr;
		while ((mobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(mobj), ERR_PARSE_ERROR);
			yyjson_val *prims_arr = yyjson_obj_get(mobj, "primitives");
			ERR_FAIL_COND_V(!prims_arr || !yyjson_is_arr(prims_arr), ERR_PARSE_ERROR);

			const String orig_mname = yj_str(yyjson_obj_get(mobj, "name"));
			const String base_mname = !orig_mname.is_empty() ? orig_mname : String("mesh");
			const String unique_mname = GLTFFastAccess::gen_unique_name(
					p_doc, p_state, vformat("%s_%s", GLTFFastAccess::scene_name(p_state.ptr()), base_mname));

			Vector<String> target_names;
			Variant extras_var;
			if (yyjson_val *mextras = yyjson_obj_get(mobj, "extras")) {
				if (yyjson_is_obj(mextras) && yyjson_obj_size(mextras) > 0) {
					if (yyjson_val *tn_arr = yyjson_obj_get(mextras, "targetNames")) {
						if (yyjson_is_arr(tn_arr)) {
							target_names.resize(int(yyjson_arr_size(tn_arr)));
							yyjson_arr_iter tnit = yyjson_arr_iter_with(tn_arr);
							int ti = 0;
							yyjson_val *tv = nullptr;
							while ((tv = yyjson_arr_iter_next(&tnit)) != nullptr) {
								target_names.write[ti++] = yj_str(tv);
							}
						}
					}
					extras_var = gltf_yyjson_val_to_variant(mextras);
				}
			}

			Vector<float> mesh_weights;
			if (yyjson_val *mw_arr = yyjson_obj_get(mobj, "weights")) {
				if (yyjson_is_arr(mw_arr)) {
					mesh_weights.resize(int(yyjson_arr_size(mw_arr)));
					yyjson_arr_iter wit = yyjson_arr_iter_with(mw_arr);
					int wi = 0;
					yyjson_val *wv = nullptr;
					while ((wv = yyjson_arr_iter_next(&wit)) != nullptr) {
						mesh_weights.write[wi++] = float(yj_num(wv, 0.0));
					}
				}
			}

			LocalVector<GLTFFastPrimSpec> prim_specs;
			prim_specs.resize(uint32_t(yyjson_arr_size(prims_arr)));
			yyjson_arr_iter pit = yyjson_arr_iter_with(prims_arr);
			uint32_t pi = 0;
			yyjson_val *pobj = nullptr;
			while ((pobj = yyjson_arr_iter_next(&pit)) != nullptr) {
				ERR_FAIL_COND_V(!yyjson_is_obj(pobj), ERR_PARSE_ERROR);
				GLTFFastPrimSpec &ps = prim_specs[pi++];
				ps.mode = int32_t(yj_int(yyjson_obj_get(pobj, "mode"), 4));
				ps.indices = int32_t(yj_int(yyjson_obj_get(pobj, "indices"), -1));
				ps.material = int32_t(yj_int(yyjson_obj_get(pobj, "material"), -1));

				yyjson_val *attrs = yyjson_obj_get(pobj, "attributes");
				ERR_FAIL_COND_V(!attrs || !yyjson_is_obj(attrs), ERR_PARSE_ERROR);
				yyjson_obj_iter ait = yyjson_obj_iter_with(attrs);
				yyjson_val *akey = nullptr;
				while ((akey = yyjson_obj_iter_next(&ait)) != nullptr) {
					yyjson_val *aval = yyjson_obj_iter_get_val(akey);
					const int32_t acc_i = int32_t(yj_int(aval, -1));
					const char *ks = yyjson_get_str(akey);
					const size_t kl = yyjson_get_len(akey);
					if (kl == 8 && std::memcmp(ks, "POSITION", 8) == 0) {
						ps.position = acc_i;
					} else if (kl == 6 && std::memcmp(ks, "NORMAL", 6) == 0) {
						ps.normal = acc_i;
					} else if (kl == 7 && std::memcmp(ks, "TANGENT", 7) == 0) {
						ps.tangent = acc_i;
					} else if (kl == 7 && std::memcmp(ks, "COLOR_0", 7) == 0) {
						ps.color_0 = acc_i;
					} else if (kl == 10 && std::memcmp(ks, "TEXCOORD_", 9) == 0 && ks[9] >= '0' && ks[9] <= '7') {
						ps.texcoord[ks[9] - '0'] = acc_i;
					} else if (kl == 8 && std::memcmp(ks, "JOINTS_0", 8) == 0) {
						ps.joints_0 = acc_i;
					} else if (kl == 8 && std::memcmp(ks, "JOINTS_1", 8) == 0) {
						ps.joints_1 = acc_i;
					} else if (kl == 9 && std::memcmp(ks, "WEIGHTS_0", 9) == 0) {
						ps.weights_0 = acc_i;
					} else if (kl == 9 && std::memcmp(ks, "WEIGHTS_1", 9) == 0) {
						ps.weights_1 = acc_i;
					}
				}

				if (yyjson_val *tgts = yyjson_obj_get(pobj, "targets")) {
					if (yyjson_is_arr(tgts) && yyjson_arr_size(tgts) > 0) {
						ps.targets.resize(uint32_t(yyjson_arr_size(tgts)));
						yyjson_arr_iter tit = yyjson_arr_iter_with(tgts);
						uint32_t ti = 0;
						yyjson_val *tobj = nullptr;
						while ((tobj = yyjson_arr_iter_next(&tit)) != nullptr) {
							ERR_FAIL_COND_V(!yyjson_is_obj(tobj), ERR_PARSE_ERROR);
							GLTFFastMorphSpec &ms = ps.targets[ti++];
							ms.position = int32_t(yj_int(yyjson_obj_get(tobj, "POSITION"), -1));
							ms.normal = int32_t(yj_int(yyjson_obj_get(tobj, "NORMAL"), -1));
							ms.tangent = int32_t(yj_int(yyjson_obj_get(tobj, "TANGENT"), -1));
						}
					}
				}
			}

			Ref<GLTFMesh> out_mesh;
			const Error merr = gltf_build_fast_array_mesh(
					p_doc,
					p_state,
					unique_mname,
					orig_mname,
					target_names,
					mesh_weights,
					extras_var,
					GLTFFastAccess::discard_meshes_and_materials(p_state.ptr()),
					GLTFFastAccess::force_generate_tangents(p_state.ptr()),
					GLTFFastAccess::force_disable_compression(p_state.ptr()),
					prim_specs,
					out_mesh);
			ERR_FAIL_COND_V(merr != OK, merr);
			state_meshes.write[mesh_i++] = out_mesh;
		}
	}

	// 11. Parse KHR_lights_punctual & cameras.
	Vector<Ref<GLTFLight>> &state_lights = GLTFFastAccess::lights(p_state.ptr());
	state_lights.clear();
	if (yyjson_val *rexts = yyjson_obj_get(root, "extensions")) {
		if (yyjson_is_obj(rexts)) {
			if (yyjson_val *lp = yyjson_obj_get(rexts, "KHR_lights_punctual")) {
				if (yyjson_is_obj(lp)) {
					if (yyjson_val *l_arr = yyjson_obj_get(lp, "lights")) {
						if (yyjson_is_arr(l_arr)) {
							state_lights.resize(int(yyjson_arr_size(l_arr)));
							yyjson_arr_iter it = yyjson_arr_iter_with(l_arr);
							int li = 0;
							yyjson_val *lobj = nullptr;
							while ((lobj = yyjson_arr_iter_next(&it)) != nullptr) {
								ERR_FAIL_COND_V(!yyjson_is_obj(lobj) || !yyjson_obj_get(lobj, "type"), ERR_PARSE_ERROR);
								Ref<GLTFLight> light;
								light.instantiate();
								const String ltype = yj_str(yyjson_obj_get(lobj, "type"));
								light->set_light_type(ltype);
								Color lc;
								if (yj_color3(yyjson_obj_get(lobj, "color"), lc)) {
									light->set_color(lc);
								}
								if (yyjson_val *iv = yyjson_obj_get(lobj, "intensity")) {
									light->set_intensity(float(yj_num(iv, 1.0)));
								}
								if (yyjson_val *rv = yyjson_obj_get(lobj, "range")) {
									light->set_range(float(yj_num(rv, Math::INF)));
								}
								if (ltype == "spot") {
									if (yyjson_val *sp = yyjson_obj_get(lobj, "spot")) {
										if (yyjson_is_obj(sp)) {
											if (yyjson_val *ic = yyjson_obj_get(sp, "innerConeAngle")) {
												light->set_inner_cone_angle(float(yj_num(ic, 0.0)));
											}
											if (yyjson_val *oc = yyjson_obj_get(sp, "outerConeAngle")) {
												light->set_outer_cone_angle(float(yj_num(oc, Math::TAU / 8.0)));
											}
										}
									}
								}
								state_lights.write[li++] = light;
							}
						}
					}
				}
			}
		}
	}

	Vector<Ref<GLTFCamera>> &state_cameras = GLTFFastAccess::cameras(p_state.ptr());
	state_cameras.clear();
	if (yyjson_val *cams_arr = yyjson_obj_get(root, "cameras")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(cams_arr), ERR_PARSE_ERROR);
		state_cameras.resize(int(yyjson_arr_size(cams_arr)));
		yyjson_arr_iter it = yyjson_arr_iter_with(cams_arr);
		int ci = 0;
		yyjson_val *cobj = nullptr;
		while ((cobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(cobj) || !yyjson_obj_get(cobj, "type"), ERR_PARSE_ERROR);
			Ref<GLTFCamera> cam;
			cam.instantiate();
			const String ctype = yj_str(yyjson_obj_get(cobj, "type"));
			if (ctype == "perspective") {
				cam->set_perspective(true);
				if (yyjson_val *p = yyjson_obj_get(cobj, "perspective")) {
					if (yyjson_is_obj(p)) {
						cam->set_fov(real_t(yj_num(yyjson_obj_get(p, "yfov"), Math::deg_to_rad(75.0))));
						if (yyjson_val *zf = yyjson_obj_get(p, "zfar")) {
							cam->set_depth_far(real_t(yj_num(zf, 4000.0)));
						}
						cam->set_depth_near(real_t(yj_num(yyjson_obj_get(p, "znear"), 0.05)));
					}
				}
			} else if (ctype == "orthographic") {
				cam->set_perspective(false);
				if (yyjson_val *o = yyjson_obj_get(cobj, "orthographic")) {
					if (yyjson_is_obj(o)) {
						cam->set_size_mag(real_t(yj_num(yyjson_obj_get(o, "ymag"), 0.5)));
						cam->set_depth_far(real_t(yj_num(yyjson_obj_get(o, "zfar"), 4000.0)));
						cam->set_depth_near(real_t(yj_num(yyjson_obj_get(o, "znear"), 0.05)));
					}
				}
			}
			state_cameras.write[ci++] = cam;
		}
	}

	// 12. Parse animations (memoizing input time accessors so shared timelines decode once and share CoW pointers!).
	Vector<Ref<GLTFAnimation>> &state_anims = GLTFFastAccess::animations(p_state.ptr());
	state_anims.clear();
	if (yyjson_val *anims_arr = yyjson_obj_get(root, "animations")) {
		ERR_FAIL_COND_V(!yyjson_is_arr(anims_arr), ERR_PARSE_ERROR);
		HashMap<int, PackedFloat64Array> time_accessor_cache;

		yyjson_arr_iter it = yyjson_arr_iter_with(anims_arr);
		yyjson_val *aobj = nullptr;
		while ((aobj = yyjson_arr_iter_next(&it)) != nullptr) {
			ERR_FAIL_COND_V(!yyjson_is_obj(aobj), ERR_PARSE_ERROR);
			yyjson_val *ch_arr = yyjson_obj_get(aobj, "channels");
			yyjson_val *sp_arr = yyjson_obj_get(aobj, "samplers");
			if (!ch_arr || !yyjson_is_arr(ch_arr) || !sp_arr || !yyjson_is_arr(sp_arr)) {
				continue;
			}

			Ref<GLTFAnimation> anim;
			anim.instantiate();
			if (yyjson_val *anm = yyjson_obj_get(aobj, "name")) {
				const String anim_name = yj_str(anm);
				const String anim_lower = anim_name.to_lower();
				if (anim_lower.begins_with("loop") || anim_lower.ends_with("loop") ||
						anim_lower.begins_with("cycle") || anim_lower.ends_with("cycle")) {
					anim->set_loop(true);
				}
				anim->set_original_name(anim_name);
				anim->set_name(GLTFFastAccess::gen_unique_anim_name(p_doc, p_state, anim_name));
			}

			struct AnimSamplerSpec {
				int input = -1;
				int output = -1;
				GLTFAnimation::Interpolation interp = GLTFAnimation::INTERP_LINEAR;
				int output_count = 1;
			};
			const size_t sp_cnt = yyjson_arr_size(sp_arr);
			LocalVector<AnimSamplerSpec> samplers;
			samplers.resize(uint32_t(sp_cnt));
			{
				yyjson_arr_iter sit = yyjson_arr_iter_with(sp_arr);
				uint32_t si = 0;
				yyjson_val *sobj = nullptr;
				while ((sobj = yyjson_arr_iter_next(&sit)) != nullptr) {
					ERR_FAIL_COND_V(!yyjson_is_obj(sobj), ERR_PARSE_ERROR);
					yyjson_val *in_v = yyjson_obj_get(sobj, "input");
					yyjson_val *out_v = yyjson_obj_get(sobj, "output");
					ERR_FAIL_COND_V(!in_v || !out_v, ERR_PARSE_ERROR);
					AnimSamplerSpec &ss = samplers[si++];
					ss.input = int(yj_int(in_v, -1));
					ss.output = int(yj_int(out_v, -1));
					if (yyjson_val *iv = yyjson_obj_get(sobj, "interpolation")) {
						if (yj_str_eq(iv, "STEP")) {
							ss.interp = GLTFAnimation::INTERP_STEP;
						} else if (yj_str_eq(iv, "CATMULLROMSPLINE")) {
							ss.interp = GLTFAnimation::INTERP_CATMULLROMSPLINE;
							ss.output_count = 3;
						} else if (yj_str_eq(iv, "CUBICSPLINE")) {
							ss.interp = GLTFAnimation::INTERP_CUBIC_SPLINE;
							ss.output_count = 3;
						}
					}
				}
			}

			HashMap<int, GLTFAnimation::NodeTrack> &node_tracks = anim->get_node_tracks();
			yyjson_arr_iter cit = yyjson_arr_iter_with(ch_arr);
			yyjson_val *cobj = nullptr;
			while ((cobj = yyjson_arr_iter_next(&cit)) != nullptr) {
				ERR_FAIL_COND_V(!yyjson_is_obj(cobj), ERR_PARSE_ERROR);
				yyjson_val *sv = yyjson_obj_get(cobj, "sampler");
				yyjson_val *tv = yyjson_obj_get(cobj, "target");
				ERR_FAIL_COND_V(!sv || !tv || !yyjson_is_obj(tv), ERR_PARSE_ERROR);
				const int s_idx = int(yj_int(sv, -1));
				ERR_FAIL_COND_V(s_idx < 0 || uint32_t(s_idx) >= samplers.size(), ERR_PARSE_ERROR);
				const AnimSamplerSpec &ss = samplers[uint32_t(s_idx)];

				yyjson_val *path_v = yyjson_obj_get(tv, "path");
				ERR_FAIL_COND_V(!path_v || !yyjson_is_str(path_v), ERR_PARSE_ERROR);
				yyjson_val *node_v = yyjson_obj_get(tv, "node");
				if (!node_v) {
					continue;
				}
				const GLTFNodeIndex nidx = GLTFNodeIndex(yj_int(node_v, -1));
				ERR_FAIL_INDEX_V(nidx, state_nodes.size(), ERR_PARSE_ERROR);

				PackedFloat64Array times;
				auto t_it = time_accessor_cache.find(ss.input);
				if (t_it) {
					times = t_it->value;
				} else {
					ERR_FAIL_COND_V(!fast_decode_accessor_f64(p_state, ss.input, times), ERR_PARSE_ERROR);
					time_accessor_cache.insert(ss.input, times);
				}

				GLTFAnimation::NodeTrack &track = node_tracks[nidx];
				if (yj_str_eq(path_v, "translation")) {
					PackedVector3Array vals;
					ERR_FAIL_COND_V(!fast_decode_accessor_vec3(p_state, ss.output, vals), ERR_PARSE_ERROR);
					track.position_track.interpolation = ss.interp;
					track.position_track.times = times;
					track.position_track.values = vals;
				} else if (yj_str_eq(path_v, "rotation")) {
					Vector<Quaternion> vals;
					ERR_FAIL_COND_V(!fast_decode_accessor_quat(p_state, ss.output, vals), ERR_PARSE_ERROR);
					track.rotation_track.interpolation = ss.interp;
					track.rotation_track.times = times;
					track.rotation_track.values = vals;
				} else if (yj_str_eq(path_v, "scale")) {
					PackedVector3Array vals;
					ERR_FAIL_COND_V(!fast_decode_accessor_vec3(p_state, ss.output, vals), ERR_PARSE_ERROR);
					track.scale_track.interpolation = ss.interp;
					track.scale_track.times = times;
					track.scale_track.values = vals;
				} else if (yj_str_eq(path_v, "weights")) {
					ERR_FAIL_INDEX_V(ss.output, state_accs.size(), ERR_PARSE_ERROR);
					const PackedFloat32Array weights = state_accs[ss.output]->decode_as_float32s(p_state);
					const GLTFMeshIndex midx = GLTFFastAccess::node_mesh(state_nodes[nidx].ptr());
					ERR_FAIL_INDEX_V(midx, state_meshes.size(), ERR_PARSE_ERROR);
					const int wc = state_meshes[midx]->get_blend_weights().size();
					ERR_CONTINUE(wc <= 0 || weights.size() != times.size() * ss.output_count * wc);
					track.weight_tracks.resize(wc);
					const int wlen = weights.size() / wc;
					const float *wp = weights.ptr();
					for (int k = 0; k < wc; k++) {
						GLTFAnimation::Channel<real_t> cf;
						cf.interpolation = ss.interp;
						cf.times = times;
						cf.values.resize(wlen);
						real_t *dst_w = cf.values.ptrw();
						for (int l = 0; l < wlen; l++) {
							dst_w[l] = wp[l * wc + k];
						}
						track.weight_tracks.write[k] = cf;
					}
				}
			}
			state_anims.push_back(anim);
		}
	}

	r_used_direct_pipeline = true;
	return OK;
}
