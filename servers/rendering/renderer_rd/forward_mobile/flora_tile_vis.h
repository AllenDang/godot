/**************************************************************************/
/*  flora_tile_vis.h                                                      */
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
