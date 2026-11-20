/**************************************************************************/
/*  gltf_fast_access.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#pragma once

#include "gltf_document.h"
#include "gltf_state.h"
#include "structures/gltf_node.h"
#include "structures/gltf_skeleton.h"
#include "structures/gltf_skin.h"

// Shared internal C++ accessor bridge for modules/gltf fast loaders (ODR-clean for LTO).
struct GLTFFastAccess {
	static _FORCE_INLINE_ int &major_version(GLTFState *s) { return s->major_version; }
	static _FORCE_INLINE_ int &minor_version(GLTFState *s) { return s->minor_version; }
	static _FORCE_INLINE_ String &copyright(GLTFState *s) { return s->copyright; }
	static _FORCE_INLINE_ String &filename(GLTFState *s) { return s->filename; }
	static _FORCE_INLINE_ String &scene_name(GLTFState *s) { return s->scene_name; }
	static _FORCE_INLINE_ const uint8_t *glb_bin_ptr(const GLTFState *s) { return s->glb_bin_ptr; }
	static _FORCE_INLINE_ int64_t glb_bin_len(const GLTFState *s) { return s->glb_bin_len; }
	static _FORCE_INLINE_ bool &buffer0_uses_glb_span(GLTFState *s) { return s->buffer0_uses_glb_span; }
	static _FORCE_INLINE_ Vector<uint8_t> &glb_data(GLTFState *s) { return s->glb_data; }
	static _FORCE_INLINE_ bool discard_meshes_and_materials(const GLTFState *s) { return s->discard_meshes_and_materials; }
	static _FORCE_INLINE_ bool force_generate_tangents(const GLTFState *s) { return s->force_generate_tangents; }
	static _FORCE_INLINE_ bool force_disable_compression(const GLTFState *s) { return s->force_disable_compression; }
	static _FORCE_INLINE_ bool use_named_skin_binds(const GLTFState *s) { return s->use_named_skin_binds; }
	static _FORCE_INLINE_ GLTFState::HandleBinaryImageMode handle_binary_image_mode(const GLTFState *s) { return s->handle_binary_image_mode; }

	static _FORCE_INLINE_ Vector<Vector<uint8_t>> &buffers(GLTFState *s) { return s->buffers; }
	static _FORCE_INLINE_ Vector<Ref<GLTFBufferView>> &buffer_views(GLTFState *s) { return s->buffer_views; }
	static _FORCE_INLINE_ Vector<Ref<GLTFAccessor>> &accessors(GLTFState *s) { return s->accessors; }
	static _FORCE_INLINE_ Vector<Ref<GLTFNode>> &nodes(GLTFState *s) { return s->nodes; }
	static _FORCE_INLINE_ Vector<Ref<GLTFMesh>> &meshes(GLTFState *s) { return s->meshes; }
	static _FORCE_INLINE_ Vector<Ref<Material>> &materials(GLTFState *s) { return s->materials; }
	static _FORCE_INLINE_ Vector<Ref<GLTFTexture>> &textures(GLTFState *s) { return s->textures; }
	static _FORCE_INLINE_ Vector<Ref<GLTFTextureSampler>> &texture_samplers(GLTFState *s) { return s->texture_samplers; }
	static _FORCE_INLINE_ Ref<GLTFTextureSampler> &default_texture_sampler(GLTFState *s) { return s->default_texture_sampler; }
	static _FORCE_INLINE_ Vector<Ref<Texture2D>> &images(GLTFState *s) { return s->images; }
	static _FORCE_INLINE_ Vector<Ref<Image>> &source_images(GLTFState *s) { return s->source_images; }
	static _FORCE_INLINE_ Vector<Ref<GLTFSkin>> &skins(GLTFState *s) { return s->skins; }
	static _FORCE_INLINE_ Vector<Ref<GLTFSkeleton>> &skeletons(GLTFState *s) { return s->skeletons; }
	static _FORCE_INLINE_ Vector<Ref<GLTFCamera>> &cameras(GLTFState *s) { return s->cameras; }
	static _FORCE_INLINE_ Vector<Ref<GLTFLight>> &lights(GLTFState *s) { return s->lights; }
	static _FORCE_INLINE_ Vector<Ref<GLTFAnimation>> &animations(GLTFState *s) { return s->animations; }
	static _FORCE_INLINE_ Vector<int> &root_nodes(GLTFState *s) { return s->root_nodes; }
	static _FORCE_INLINE_ Vector<String> &extensions_used(GLTFState *s) { return s->extensions_used; }
	static _FORCE_INLINE_ Vector<String> &extensions_required(GLTFState *s) { return s->extensions_required; }
	static _FORCE_INLINE_ HashSet<String> &unique_names(GLTFState *s) { return s->unique_names; }
	static _FORCE_INLINE_ HashMap<GLTFNodeIndex, Node *> &scene_nodes(GLTFState *s) { return s->scene_nodes; }
	static _FORCE_INLINE_ HashMap<GLTFNodeIndex, ImporterMeshInstance3D *> &scene_mesh_instances(GLTFState *s) { return s->scene_mesh_instances; }
	static _FORCE_INLINE_ HashMap<GLTFNodeIndex, MeshInstance3D *> &scene_runtime_mesh_instances(GLTFState *s) { return s->scene_runtime_mesh_instances; }

	static _FORCE_INLINE_ GLTFNodeIndex &node_parent(GLTFNode *n) { return n->parent; }
	static _FORCE_INLINE_ GLTFNodeIndex node_parent(const GLTFNode *n) { return n->parent; }
	static _FORCE_INLINE_ int &node_height(GLTFNode *n) { return n->height; }
	static _FORCE_INLINE_ Transform3D &node_xform(GLTFNode *n) { return n->transform; }
	static _FORCE_INLINE_ const Transform3D &node_xform(const GLTFNode *n) { return n->transform; }
	static _FORCE_INLINE_ void set_node_rest_xform(GLTFNode *n, const Transform3D &xf) {
		n->rest_transform = xf;
		n->has_rest_transform = true;
	}
	static _FORCE_INLINE_ const Transform3D &node_rest_xform(const GLTFNode *n) { return n->has_rest_transform ? n->rest_transform : n->transform; }
	static _FORCE_INLINE_ Vector<float> &node_weights(GLTFNode *n) { return n->node_weights; }
	static _FORCE_INLINE_ const Vector<float> &node_weights(const GLTFNode *n) { return n->node_weights; }
	static _FORCE_INLINE_ GLTFMeshIndex &node_mesh(GLTFNode *n) { return n->mesh; }
	static _FORCE_INLINE_ GLTFMeshIndex node_mesh(const GLTFNode *n) { return n->mesh; }
	static _FORCE_INLINE_ GLTFCameraIndex &node_camera(GLTFNode *n) { return n->camera; }
	static _FORCE_INLINE_ GLTFSkinIndex &node_skin(GLTFNode *n) { return n->skin; }
	static _FORCE_INLINE_ GLTFSkinIndex node_skin(const GLTFNode *n) { return n->skin; }
	static _FORCE_INLINE_ GLTFSkeletonIndex node_skeleton(const GLTFNode *n) { return n->skeleton; }
	static _FORCE_INLINE_ GLTFLightIndex &node_light(GLTFNode *n) { return n->light; }
	static _FORCE_INLINE_ bool &node_joint(GLTFNode *n) { return n->joint; }
	static _FORCE_INLINE_ bool node_joint(const GLTFNode *n) { return n->joint; }
	static _FORCE_INLINE_ bool &node_visible(GLTFNode *n) { return n->visible; }
	static _FORCE_INLINE_ bool node_visible(const GLTFNode *n) { return n->visible; }
	static _FORCE_INLINE_ Vector<int> &node_children(GLTFNode *n) { return n->children; }
	static _FORCE_INLINE_ const Vector<int> &node_children(const GLTFNode *n) { return n->children; }

	static _FORCE_INLINE_ GLTFNodeIndex &skin_root(GLTFSkin *sk) { return sk->skin_root; }
	static _FORCE_INLINE_ GLTFSkeletonIndex skin_skeleton(const GLTFSkin *sk) { return sk->skeleton; }
	static _FORCE_INLINE_ Vector<GLTFNodeIndex> &skin_joints(GLTFSkin *sk) { return sk->joints; }
	static _FORCE_INLINE_ Vector<GLTFNodeIndex> &skin_joints_original(GLTFSkin *sk) { return sk->joints_original; }
	static _FORCE_INLINE_ const Vector<GLTFNodeIndex> &skin_joints_original(const GLTFSkin *sk) { return sk->joints_original; }
	static _FORCE_INLINE_ Vector<Transform3D> &skin_inverse_binds(GLTFSkin *sk) { return sk->inverse_binds; }
	static _FORCE_INLINE_ HashMap<int, int> &skin_joint_to_bone(GLTFSkin *sk) { return sk->joint_i_to_bone_i; }
	static _FORCE_INLINE_ Ref<Skin> &godot_skin(GLTFSkin *sk) { return sk->godot_skin; }

	static _FORCE_INLINE_ Skeleton3D *godot_skeleton(GLTFSkeleton *sk) { return sk->godot_skeleton; }
	static _FORCE_INLINE_ const Vector<GLTFNodeIndex> &skeleton_roots(const GLTFSkeleton *sk) { return sk->roots; }
	static _FORCE_INLINE_ HashMap<int32_t, GLTFNodeIndex> &skeleton_bone_node(GLTFSkeleton *sk) { return sk->godot_bone_node; }

	static _FORCE_INLINE_ String gen_unique_name(GLTFDocument *doc, Ref<GLTFState> s, const String &name) { return doc->_gen_unique_name(s, name); }
	static _FORCE_INLINE_ String gen_unique_anim_name(GLTFDocument *doc, Ref<GLTFState> s, const String &name) { return doc->_gen_unique_animation_name(s, name); }
	static _FORCE_INLINE_ void assign_node_names(GLTFDocument *doc, Ref<GLTFState> s) { doc->_assign_node_names(s); }
	static _FORCE_INLINE_ Ref<Texture2D> get_texture(GLTFDocument *doc, Ref<GLTFState> s, GLTFTextureIndex ti, int tt) { return doc->_get_texture(s, ti, tt); }
	static _FORCE_INLINE_ Ref<GLTFTextureSampler> get_sampler_for_texture(GLTFDocument *doc, Ref<GLTFState> s, GLTFTextureIndex ti) { return doc->_get_sampler_for_texture(s, ti); }
	static _FORCE_INLINE_ void spec_gloss_to_rough_metal(GLTFDocument *doc, Ref<GLTFSpecGloss> sg, Ref<BaseMaterial3D> mat) { doc->spec_gloss_to_rough_metal(sg, mat); }
	static _FORCE_INLINE_ Ref<Image> &spec_gloss_diffuse_img(GLTFSpecGloss *sg) { return sg->diffuse_img; }
	static _FORCE_INLINE_ Ref<Image> &spec_gloss_img(GLTFSpecGloss *sg) { return sg->spec_gloss_img; }
	static _FORCE_INLINE_ Color &spec_gloss_diffuse_factor(GLTFSpecGloss *sg) { return sg->diffuse_factor; }
	static _FORCE_INLINE_ Color &spec_gloss_specular_factor(GLTFSpecGloss *sg) { return sg->specular_factor; }
	static _FORCE_INLINE_ float &spec_gloss_gloss_factor(GLTFSpecGloss *sg) { return sg->gloss_factor; }
};
