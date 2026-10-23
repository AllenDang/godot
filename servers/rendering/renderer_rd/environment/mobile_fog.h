/**************************************************************************/
/*  mobile_fog.h                                                          */
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
#include "servers/rendering/renderer_rd/shaders/environment/mobile_fog.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {
// Composes native height fog/AP after screen-space AO, with no depth prepass.
class MobileFog {
public:
	class MobileFogView : public RenderBufferCustomDataRD {
		GDCLASS(MobileFogView, RenderBufferCustomDataRD);

	public:
		MobileFog *owner = nullptr;
		RenderSceneBuffersRD *buffers = nullptr;
		RID scene, radiance, next_radiance, ap[2];
		Projection projection[2];
		bool active = false, array = false, orthographic = false;
		uint32_t applied = 0;
		float luminance = 1, roughness_lod = 7;
		virtual void configure(RenderSceneBuffersRD *p_buffers) override {
			buffers = p_buffers;
			active = false;
		}
		virtual void free_data() override { active = false; }
		virtual bool composite_mobile_ao(RID p_ao, RID p_guide, int p_algorithm, int p_view, float p_radius, float p_intensity, float p_max_distance) override;
	};

private:
	MobileFogShaderRD shader;
	RID version, nearest_sampler, linear_sampler;
	HashMap<uint64_t, RID> pipelines;
	bool initialized = false;
	void initialize();
	bool composite(MobileFogView *p_view, RID p_ao, RID p_guide, int p_algorithm, int p_eye, float p_radius, float p_intensity, float p_max_distance);

public:
	bool prepare(const RenderDataRD *p_data, bool p_requested, RID p_radiance, RID p_next, RID p_ap0, RID p_ap1, bool p_array, int p_roughness_layers, float p_luminance);
	RID get_framebuffer(Ref<RenderSceneBuffersRD> p_buffers, bool p_resolve_depth, bool p_resolve_color);
	void finish(Ref<RenderSceneBuffersRD> p_buffers);
	~MobileFog();
};
} //namespace RendererRD
