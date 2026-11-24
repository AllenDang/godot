/**************************************************************************/
/*  mobile_taau.h                                                         */
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

#include "servers/rendering/renderer_rd/shaders/effects/mobile_taau.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

class MobileTAAU {
public:
	enum OutputMode {
		OUTPUT_MODE_R11G11B10F = 0,
		OUTPUT_MODE_RGBA16F = 1,
		OUTPUT_MODE_RGB10A2 = 2,
		OUTPUT_MODE_MAX = 3,
	};

	struct PushConstant {
		float reprojection_matrix[16];
		float internal_size[2];
		float target_size[2];
		float inv_internal_size[2];
		float inv_target_size[2];
		float jitter_pixels[2];
		float variance_gamma;
		float reset_history;
	};

	MobileTAAU();
	~MobileTAAU();

	void process(
			Ref<RenderSceneBuffersRD> p_render_buffers,
			const Projection &p_current_projection,
			const Transform3D &p_current_transform,
			const Projection &p_previous_projection,
			const Transform3D &p_previous_transform,
			const Vector2 &p_taa_jitter);

private:
	MobileTaauShaderRD shader;
	RID shader_version;
	RID pipelines[OUTPUT_MODE_MAX];

	RD::DataFormat select_history_format(RD::DataFormat p_preferred, OutputMode &r_mode) const;
};

} // namespace RendererRD
