/**************************************************************************/
/*  flora_tile_vis.cpp                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "flora_tile_vis.h"

#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

using namespace RendererRD;

bool FloraTileVis::is_supported(const Ref<RenderSceneBuffersRD> &p_rb, bool p_is_reflection_probe, bool p_use_msaa) {
	if (p_rb.is_null() || p_is_reflection_probe || p_use_msaa) {
		return false;
	}
	if (!p_rb->is_flora_tile_vis_active()) {
		return false;
	}
	return p_rb->get_flora_subpass_vis_callback().is_valid() && p_rb->get_flora_subpass_resolve_callback().is_valid();
}

FloraTileVisPassConfig FloraTileVis::prepare_pass(
		Ref<RenderSceneBuffersRD> p_rb,
		const Color &p_linear_clear_color,
		bool p_load_color,
		bool p_defer_fog,
		bool p_has_velocity,
		bool p_using_subpass_post_process) {
	FloraTileVisPassConfig cfg;
	ERR_FAIL_COND_V(p_rb.is_null(), cfg);

	const bool desktop_compute = p_rb->is_flora_desktop_tile_compute();
	const uint32_t view_count = p_rb->get_view_count();
	TextureStorage *texture_storage = TextureStorage::get_singleton();

	RID color_tex = p_rb->get_internal_texture();
	RID depth_tex = p_rb->get_depth_texture();
	RID vis_tex = p_rb->ensure_flora_vis_texture();
	ERR_FAIL_COND_V(color_tex.is_null() || depth_tex.is_null() || vis_tex.is_null(), cfg);

	Vector<RID> textures;
	textures.push_back(color_tex); // 0: color (color_index = 0)
	textures.push_back(depth_tex); // 1: depth + stencil (not a color_index)

	cfg.clear_flags = RD::DRAW_CLEAR_DEPTH | RD::DRAW_CLEAR_STENCIL;
	Color c0 = p_linear_clear_color;
	c0.a = 0.0f;
	cfg.clear_colors.push_back(c0); // color_index 0
	if (!p_load_color) {
		cfg.clear_flags |= RD::DRAW_CLEAR_COLOR_0;
	}

	uint32_t next_color_idx = 1;

	int fog_att = -1;
	if (p_defer_fog && p_rb->has_texture(SNAME("mobile_fog"), SNAME("mask"))) {
		fog_att = textures.size();
		textures.push_back(p_rb->get_texture(SNAME("mobile_fog"), SNAME("mask")));
		cfg.clear_colors.push_back(Color(0, 0, 0, 0));
		cfg.clear_flags |= (RD::DRAW_CLEAR_COLOR_0 << next_color_idx);
		next_color_idx++;
	}

	const int vis_att = textures.size();
	textures.push_back(vis_tex);
	cfg.clear_colors.push_back(Color(0, 0, 0, 0)); // R32_UINT clears to 0u
	cfg.clear_flags |= (RD::DRAW_CLEAR_COLOR_0 << next_color_idx);
	next_color_idx++;

	int vel_att = -1;
	if (p_has_velocity && !desktop_compute) {
		p_rb->ensure_velocity();
		RID vel_tex = p_rb->get_velocity_buffer(false);
		if (vel_tex.is_valid()) {
			vel_att = textures.size();
			textures.push_back(vel_tex);
			// (-1, -1) is Godot's static-camera motion vector derivation sentinel.
			cfg.clear_colors.push_back(Color(-1.0f, -1.0f, 0.0f, 0.0f));
			cfg.clear_flags |= (RD::DRAW_CLEAR_COLOR_0 << next_color_idx);
			next_color_idx++;
		}
	}

	RID vrs_tex;
	if (p_rb->has_texture(RB_SCOPE_VRS, RB_TEXTURE)) {
		vrs_tex = p_rb->get_texture(RB_SCOPE_VRS, RB_TEXTURE);
		if (vrs_tex.is_valid()) {
			textures.push_back(vrs_tex);
		}
	}

	int post_target_att = -1;
	if (p_using_subpass_post_process && !desktop_compute && !p_defer_fog) {
		RID rt = p_rb->get_render_target();
		if (rt.is_valid() && p_rb->get_target_size() == p_rb->get_internal_size()) {
			RID target_buf = (view_count > 1 || texture_storage->render_target_get_msaa(rt) == RSE::VIEWPORT_MSAA_DISABLED)
					? texture_storage->render_target_get_rd_texture(rt)
					: texture_storage->render_target_get_rd_texture_msaa(rt);
			if (target_buf.is_valid()) {
				if (view_count == 1 && texture_storage->render_target_get_msaa(rt) != RSE::VIEWPORT_MSAA_DISABLED) {
					texture_storage->render_target_set_msaa_needs_resolve(rt, true);
				}
				post_target_att = textures.size();
				textures.push_back(target_buf);
				cfg.clear_colors.push_back(Color(0, 0, 0, 0));
				next_color_idx++;
			}
		}
	}

	Vector<RD::FramebufferPass> passes;

	// Subpass 0: Standard Opaque Geometry (subpass == 0, matching precompiled SceneShaderForwardMobile).
	{
		RD::FramebufferPass pass0;
		pass0.color_attachments.push_back(0);
		if (fog_att >= 0) {
			pass0.color_attachments.push_back(fog_att);
		}
		pass0.depth_attachment = 1;
		if (vel_att >= 0) {
			pass0.preserve_attachments.push_back(vel_att);
		}
		passes.push_back(pass0);
	}

	// Subpass 1: Flora Visibility (Subpass 0a pure opaque clusters/grass + Subpass 0b uvec2 3D DDA voxel hulls).
	// Writes R32_UINT VisID + Depth + Hardware Stencil FLORA_STENCIL_BIT (0x80).
	{
		RD::FramebufferPass pass1;
		pass1.color_attachments.push_back(vis_att);
		pass1.depth_attachment = 1;
		pass1.preserve_attachments.push_back(0);
		if (fog_att >= 0) {
			pass1.preserve_attachments.push_back(fog_att);
		}
		if (vel_att >= 0) {
			pass1.preserve_attachments.push_back(vel_att);
		}
		passes.push_back(pass1);
	}

	if (!desktop_compute) {
		// Subpass 2: Flora Single-Sample Visible-Only Resolve (subpassLoad(vis_id) + Early-Stencil == 0x80).
		{
			RD::FramebufferPass pass2;
			pass2.input_attachments.push_back(vis_att);
			pass2.color_attachments.push_back(0);
			if (fog_att >= 0) {
				pass2.color_attachments.push_back(fog_att);
			}
			if (vel_att >= 0) {
				pass2.color_attachments.push_back(vel_att);
			}
			pass2.depth_attachment = 1;
			passes.push_back(pass2);
		}

		// Subpass 3: Sky + Merged Transparent (single color attachment [0] + depth [1]).
		{
			RD::FramebufferPass pass3;
			pass3.color_attachments.push_back(0);
			pass3.depth_attachment = 1;
			if (fog_att >= 0) {
				pass3.preserve_attachments.push_back(fog_att);
			}
			if (vel_att >= 0) {
				pass3.preserve_attachments.push_back(vel_att);
			}
			passes.push_back(pass3);
		}

		if (post_target_att >= 0) {
			// Subpass 4: Direct Tonemap Blit Subpass.
			RD::FramebufferPass pass4;
			pass4.input_attachments.push_back(0);
			pass4.color_attachments.push_back(post_target_att);
			if (vel_att >= 0) {
				pass4.preserve_attachments.push_back(vel_att);
			}
			passes.push_back(pass4);
			cfg.has_post_subpass = true;
		}
	}

	cfg.framebuffer = FramebufferCacheRD::get_singleton()->get_cache_multipass(textures, passes, view_count);
	cfg.desktop_tile_compute = desktop_compute;
	cfg.has_velocity = (vel_att >= 0) || (p_has_velocity && desktop_compute);
	cfg.defer_fog = (fog_att >= 0);
	return cfg;
}
