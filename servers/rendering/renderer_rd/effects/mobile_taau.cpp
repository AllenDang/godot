/**************************************************************************/
/*  mobile_taau.cpp                                                       */
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

#include "mobile_taau.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

MobileTAAU::MobileTAAU() {
	Vector<String> modes;
	modes.push_back("\n#define OUTPUT_R11G11B10F\n");
	modes.push_back("\n#define OUTPUT_RGBA16F\n");
	modes.push_back("\n#define OUTPUT_RGB10A2\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < OUTPUT_MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

MobileTAAU::~MobileTAAU() {
	RD *rd = RD::get_singleton();
	for (int i = 0; i < OUTPUT_MODE_MAX; i++) {
		if (pipelines[i].is_valid()) {
			rd->free_rid(pipelines[i]);
		}
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
}

RD::DataFormat MobileTAAU::select_history_format(RD::DataFormat p_preferred, OutputMode &r_mode) const {
	const uint32_t req_usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	RD *rd = RD::get_singleton();
	if (p_preferred == RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32 && rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32, req_usage)) {
		r_mode = OUTPUT_MODE_R11G11B10F;
		return RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32;
	}
	if (p_preferred == RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32 && rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32, req_usage)) {
		r_mode = OUTPUT_MODE_RGB10A2;
		return RD::DATA_FORMAT_A2B10G10R10_UNORM_PACK32;
	}
	r_mode = OUTPUT_MODE_RGBA16F;
	return RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
}

void MobileTAAU::process(
		Ref<RenderSceneBuffersRD> p_render_buffers,
		const Projection &p_current_projection,
		const Transform3D &p_current_transform,
		const Projection &p_previous_projection,
		const Transform3D &p_previous_transform,
		const Vector2 &p_taa_jitter) {
	ERR_FAIL_COND(p_render_buffers.is_null());
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	ERR_FAIL_NULL(material_storage);

	const Size2i internal_size = p_render_buffers->get_internal_size();
	const Size2i target_size = p_render_buffers->get_target_size();
	const uint32_t view_count = p_render_buffers->get_view_count();
	if (internal_size.x <= 0 || internal_size.y <= 0 || target_size.x <= 0 || target_size.y <= 0) {
		return;
	}

	OutputMode mode = OUTPUT_MODE_RGBA16F;
	const RD::DataFormat history_fmt = select_history_format(p_render_buffers->get_base_data_format(), mode);
	const uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;

	bool just_allocated = !p_render_buffers->has_taau_output();
	if (!p_render_buffers->has_texture(SNAME("taau"), SNAME("history_0"))) {
		p_render_buffers->create_texture(SNAME("taau"), SNAME("history_0"), history_fmt, usage_bits, RD::TEXTURE_SAMPLES_1, target_size);
		p_render_buffers->create_texture(SNAME("taau"), SNAME("history_1"), history_fmt, usage_bits, RD::TEXTURE_SAMPLES_1, target_size);
		just_allocated = true;
	}

	const uint32_t prev_idx = p_render_buffers->get_taau_write_index();
	const uint32_t write_idx = just_allocated ? 0u : (prev_idx ^ 1u);
	const StringName read_name = (write_idx == 0u) ? SNAME("history_1") : SNAME("history_0");
	const StringName write_name = (write_idx == 0u) ? SNAME("history_0") : SNAME("history_1");

	Projection correction;
	correction.set_depth_correction(true, true, false);
	Projection reprojection = (correction * p_previous_projection) * p_previous_transform.affine_inverse() * p_current_transform * (correction * p_current_projection).inverse();

	PushConstant push = {};
	MaterialStorage::store_camera(reprojection, push.reprojection_matrix);
	push.internal_size[0] = float(internal_size.x);
	push.internal_size[1] = float(internal_size.y);
	push.target_size[0] = float(target_size.x);
	push.target_size[1] = float(target_size.y);
	push.inv_internal_size[0] = 1.0f / float(internal_size.x);
	push.inv_internal_size[1] = 1.0f / float(internal_size.y);
	push.inv_target_size[0] = 1.0f / float(target_size.x);
	push.inv_target_size[1] = 1.0f / float(target_size.y);
	// Convert NDC taa_jitter (±1/size) to internal pixel offset (±0.5).
	push.jitter_pixels[0] = p_taa_jitter.x * 0.5f * float(internal_size.x);
	push.jitter_pixels[1] = -p_taa_jitter.y * 0.5f * float(internal_size.y);
	push.variance_gamma = CLAMP(1.15f * (1080.0f / float(MAX(target_size.y, 1))), 0.85f, 1.25f);
	push.reset_history = just_allocated ? 1.0f : 0.0f;

	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = shader.version_get_shader(shader_version, mode);
	ERR_FAIL_COND(shader_rid.is_null());

	RD *rd = RD::get_singleton();
	rd->draw_command_begin_label("Mobile TAAU");
	RD::ComputeListID cl = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(cl, pipelines[mode]);

	const uint32_t gx = uint32_t((target_size.x + 7) / 8);
	const uint32_t gy = uint32_t((target_size.y + 7) / 8);

	for (uint32_t v = 0; v < view_count; v++) {
		RID color_tex = p_render_buffers->get_internal_texture(v);
		RID depth_tex = p_render_buffers->get_depth_texture(v);
		RID vel_tex = p_render_buffers->get_velocity_buffer(false, v);
		RID hist_read = p_render_buffers->get_texture_slice(SNAME("taau"), read_name, v, 0);
		RID hist_write = p_render_buffers->get_texture_slice(SNAME("taau"), write_name, v, 0);

		RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, { nearest_sampler, color_tex });
		RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, { nearest_sampler, depth_tex });
		RD::Uniform u_vel(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, { nearest_sampler, vel_tex });
		RD::Uniform u_hist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, { linear_sampler, hist_read });
		RD::Uniform u_out(RD::UNIFORM_TYPE_IMAGE, 4, { hist_write });

		RID set = uniform_set_cache->get_cache(shader_rid, 0, u_color, u_depth, u_vel, u_hist, u_out);
		rd->compute_list_bind_uniform_set(cl, set, 0);
		rd->compute_list_set_push_constant(cl, &push, sizeof(PushConstant));
		rd->compute_list_dispatch(cl, gx, gy, 1);
	}

	rd->compute_list_end();
	rd->draw_command_end_label();

	p_render_buffers->set_taau_output_state(write_idx, true);
}
