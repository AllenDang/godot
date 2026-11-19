/**************************************************************************/
/*  gltf_fast_scene.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "gltf_defines.h"

#include "core/io/resource_loader.h"
#include "scene/resources/animation.h"
#include "scene/resources/packed_scene.h"

class AnimationPlayer;
class GLTFDocument;
class GLTFState;

// Fast O(N) animation builder using shared SampleSchedule + bulk track setters.
Ref<Animation> gltf_build_animation_fast(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		Node *p_scene_root,
		GLTFAnimationIndex p_index,
		bool p_trimming,
		bool p_remove_immutable_tracks,
		int p_single_root_node_idx = -1);

void gltf_import_animation_fast(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		AnimationPlayer *p_animation_player,
		GLTFAnimationIndex p_index,
		bool p_trimming,
		bool p_remove_immutable_tracks);

// Direct PackedScene builder: builds SceneState with zero temporary Node* allocations
// for static/morph/animated hierarchies, and delegates skinned/camera/light/extension
// scenes to GLTFDocument::generate_scene + pack.
Ref<PackedScene> gltf_build_packed_scene_direct(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		bool p_has_custom_extensions,
		float p_bake_fps = 30.0f,
		bool p_trimming = false,
		bool p_remove_immutable_tracks = true);

class ResourceFormatLoaderGLB : public ResourceFormatLoader {
	GDCLASS(ResourceFormatLoaderGLB, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
