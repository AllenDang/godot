/**************************************************************************/
/*  flora_tile_vis.h                                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

struct FloraTileVisPassConfig {
	RID framebuffer;
	Vector<Color> clear_colors;
	uint32_t clear_flags = 0;
	bool desktop_tile_compute = false;
	bool has_velocity = false;
	bool defer_fog = false;
	bool has_post_subpass = false;
};

class FloraTileVis {
public:
	static constexpr uint32_t FLORA_STENCIL_BIT = 0x80u;

	static bool is_supported(const Ref<RenderSceneBuffersRD> &p_rb, bool p_is_reflection_probe, bool p_use_msaa);

	static FloraTileVisPassConfig prepare_pass(
			Ref<RenderSceneBuffersRD> p_rb,
			const Color &p_linear_clear_color,
			bool p_load_color,
			bool p_defer_fog,
			bool p_has_velocity,
			bool p_using_subpass_post_process);
};

} // namespace RendererRD
