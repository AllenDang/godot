/**************************************************************************/
/*  mobile_taau.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
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
