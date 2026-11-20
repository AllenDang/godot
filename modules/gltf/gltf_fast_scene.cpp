/**************************************************************************/
/*  gltf_fast_scene.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "gltf_fast_scene.h"

#include "gltf_document.h"
#include "gltf_fast_access.h"
#include "gltf_state.h"
#include "skin_tool.h"
#include "structures/gltf_animation.h"
#include "structures/gltf_mesh.h"
#include "structures/gltf_node.h"
#include "structures/gltf_skeleton.h"
#include "structures/gltf_skin.h"

#include "core/os/load_thread_pool.h"
#include "scene/3d/importer_mesh_instance_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/animation_library.h"

#include <cmath>

namespace {

struct SampleStep {
	int32_t idx = -1;
	float frac = 0.0f;
	float td = 0.0f;
};

struct SampleSchedule {
	const double *times_ptr = nullptr;
	int32_t times_len = 0;
	LocalVector<SampleStep> steps;
};

void build_sample_times(double p_anim_start, double p_anim_end, double p_increment, LocalVector<double> &r_abs_times, LocalVector<double> &r_rel_times) {
	r_abs_times.clear();
	r_rel_times.clear();
	double time = p_anim_start;
	bool last = false;
	while (true) {
		r_abs_times.push_back(time);
		r_rel_times.push_back(time - p_anim_start);
		if (last) {
			break;
		}
		time += p_increment;
		if (time >= p_anim_end) {
			last = true;
			time = p_anim_end;
		}
	}
}

const SampleSchedule &get_or_build_schedule(
		LocalVector<SampleSchedule> &r_cache,
		const Vector<double> &p_times,
		const LocalVector<double> &p_abs_times) {
	const double *tptr = p_times.ptr();
	const int32_t tlen = p_times.size();
	for (uint32_t i = 0; i < r_cache.size(); i++) {
		if (r_cache[i].times_ptr == tptr && r_cache[i].times_len == tlen) {
			return r_cache[i];
		}
	}

	SampleSchedule sched;
	sched.times_ptr = tptr;
	sched.times_len = tlen;
	const uint32_t scount = p_abs_times.size();
	sched.steps.resize(scount);

	int32_t cursor = -1;
	for (uint32_t s = 0; s < scount; s++) {
		const float query_t = float(p_abs_times[s]);
		// Monotonic advance (falls back to reset only if input times are non-monotonic).
		if (cursor >= 0 && tptr[cursor] > double(query_t)) {
			cursor = -1;
		}
		while (cursor + 1 < tlen && tptr[cursor + 1] <= double(query_t)) {
			cursor++;
		}
		SampleStep step;
		step.idx = cursor;
		if (cursor >= 0 && cursor < tlen - 1) {
			const float td = float(tptr[cursor + 1] - tptr[cursor]);
			step.td = td;
			step.frac = td > 0.0f ? ((query_t - float(tptr[cursor])) / td) : 0.0f;
		}
		sched.steps[s] = step;
	}

	r_cache.push_back(sched);
	return r_cache[r_cache.size() - 1];
}

template <typename T>
_FORCE_INLINE_ T hermite_eval(const T &p_start, const T &p_tan_start, const T &p_end, const T &p_tan_end, float p_t) {
	const real_t t2 = p_t * p_t;
	const real_t t3 = t2 * p_t;
	const real_t h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
	const real_t h10 = t3 - 2.0 * t2 + p_t;
	const real_t h01 = -2.0 * t3 + 3.0 * t2;
	const real_t h11 = t3 - t2;
	return p_start * h00 + p_tan_start * h10 + p_end * h01 + p_tan_end * h11;
}

template <typename T>
_FORCE_INLINE_ T catmull_eval(const T &p0, const T &p1, const T &p2, const T &p3, float p_t) {
	const float t2 = p_t * p_t;
	const float t3 = t2 * p_t;
	return 0.5f * ((2.0f * p1) + (-p0 + p2) * p_t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

void eval_vec3_samples(
		const SampleSchedule &p_sched,
		const Vector<Vector3> &p_values,
		GLTFAnimation::Interpolation p_interp,
		LocalVector<Vector3> &r_out) {
	const uint32_t scount = p_sched.steps.size();
	r_out.resize(scount);
	const int32_t tlen = p_sched.times_len;
	const int32_t vlen = p_values.size();
	if (vlen == 0) {
		for (uint32_t s = 0; s < scount; s++) {
			r_out[s] = Vector3();
		}
		return;
	}
	const Vector3 *vptr = p_values.ptr();
	const int expected_mult = (p_interp == GLTFAnimation::INTERP_CUBIC_SPLINE) ? 3 : 1;
	if (tlen != vlen / expected_mult) {
		for (uint32_t s = 0; s < scount; s++) {
			r_out[s] = vptr[0];
		}
		return;
	}
	const SampleStep *steps = p_sched.steps.ptr();
	Vector3 *dst = r_out.ptr();

	switch (p_interp) {
		case GLTFAnimation::INTERP_LINEAR: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					dst[s] = vptr[0];
				} else if (idx >= tlen - 1) {
					dst[s] = vptr[tlen - 1];
				} else {
					const float f = steps[s].frac;
					if (f <= 1e-6f) {
						dst[s] = vptr[idx];
					} else if (f >= 1.0f - 1e-6f) {
						dst[s] = vptr[idx + 1];
					} else {
						dst[s] = vptr[idx] + (vptr[idx + 1] - vptr[idx]) * f;
					}
				}
			}
		} break;
		case GLTFAnimation::INTERP_STEP: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					dst[s] = vptr[0];
				} else if (idx >= tlen - 1) {
					dst[s] = vptr[tlen - 1];
				} else {
					dst[s] = vptr[idx];
				}
			}
		} break;
		case GLTFAnimation::INTERP_CATMULLROMSPLINE: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					dst[s] = vlen > 1 ? vptr[1] : vptr[0];
				} else if (idx >= tlen - 1 || idx + 3 >= vlen || idx < 1) {
					dst[s] = (1 + tlen - 1 < vlen) ? vptr[1 + tlen - 1] : vptr[vlen - 1];
				} else {
					dst[s] = catmull_eval(vptr[idx - 1], vptr[idx], vptr[idx + 1], vptr[idx + 3], steps[s].frac);
				}
			}
		} break;
		case GLTFAnimation::INTERP_CUBIC_SPLINE: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					dst[s] = vptr[1];
				} else if (idx >= tlen - 1) {
					dst[s] = vptr[(tlen - 1) * 3 + 1];
				} else {
					const float td = steps[s].td;
					dst[s] = hermite_eval(
							vptr[idx * 3 + 1],
							vptr[idx * 3 + 2] * td,
							vptr[idx * 3 + 4],
							vptr[idx * 3 + 3] * td,
							steps[s].frac);
				}
			}
		} break;
	}
}

void eval_quat_samples(
		const SampleSchedule &p_sched,
		const Vector<Quaternion> &p_values,
		GLTFAnimation::Interpolation p_interp,
		LocalVector<Quaternion> &r_out) {
	const uint32_t scount = p_sched.steps.size();
	r_out.resize(scount);
	const int32_t tlen = p_sched.times_len;
	const int32_t vlen = p_values.size();
	if (vlen == 0) {
		for (uint32_t s = 0; s < scount; s++) {
			r_out[s] = Quaternion();
		}
		return;
	}
	const Quaternion *vptr = p_values.ptr();
	const int expected_mult = (p_interp == GLTFAnimation::INTERP_CUBIC_SPLINE) ? 3 : 1;
	if (tlen != vlen / expected_mult) {
		const Quaternion q0 = vptr[0].normalized();
		for (uint32_t s = 0; s < scount; s++) {
			r_out[s] = q0;
		}
		return;
	}
	const SampleStep *steps = p_sched.steps.ptr();
	Quaternion *dst = r_out.ptr();

	auto ensure_unit = [](const Quaternion &p_q) -> Quaternion {
		const float l2 = p_q.x * p_q.x + p_q.y * p_q.y + p_q.z * p_q.z + p_q.w * p_q.w;
		if (std::abs(l2 - 1.0f) <= 1e-5f) {
			return p_q;
		}
		if (l2 > 0.0f) {
			const float inv = 1.0f / std::sqrt(l2);
			return Quaternion(p_q.x * inv, p_q.y * inv, p_q.z * inv, p_q.w * inv);
		}
		return Quaternion();
	};

	auto safe_slerp = [&](const Quaternion &p_a, const Quaternion &p_b, float p_c) -> Quaternion {
		const Quaternion na = ensure_unit(p_a);
		if (p_c <= 1e-6f) {
			return na;
		}
		const Quaternion nb = ensure_unit(p_b);
		if (p_c >= 1.0f - 1e-6f) {
			return nb;
		}
		float cosom = na.x * nb.x + na.y * nb.y + na.z * nb.z + na.w * nb.w;
		Quaternion to1 = nb;
		if (cosom < 0.0f) {
			cosom = -cosom;
			to1 = -to1;
		}
		if (cosom >= 0.999999f) {
			return na;
		}
		if (cosom > 0.9995f) {
			const float s0 = 1.0f - p_c;
			const float rx = s0 * na.x + p_c * to1.x;
			const float ry = s0 * na.y + p_c * to1.y;
			const float rz = s0 * na.z + p_c * to1.z;
			const float rw = s0 * na.w + p_c * to1.w;
			const float inv = 1.0f / std::sqrt(rx * rx + ry * ry + rz * rz + rw * rw);
			return Quaternion(rx * inv, ry * inv, rz * inv, rw * inv);
		}
		const float omega = std::acos(CLAMP(cosom, -1.0f, 1.0f));
		const float inv_sin = 1.0f / std::sin(omega);
		const float s0 = std::sin((1.0f - p_c) * omega) * inv_sin;
		const float s1 = std::sin(p_c * omega) * inv_sin;
		return ensure_unit(Quaternion(
				s0 * na.x + s1 * to1.x,
				s0 * na.y + s1 * to1.y,
				s0 * na.z + s1 * to1.z,
				s0 * na.w + s1 * to1.w));
	};

	switch (p_interp) {
		case GLTFAnimation::INTERP_LINEAR: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					dst[s] = ensure_unit(vptr[0]);
				} else if (idx >= tlen - 1) {
					dst[s] = ensure_unit(vptr[tlen - 1]);
				} else {
					dst[s] = safe_slerp(vptr[idx], vptr[idx + 1], steps[s].frac);
				}
			}
		} break;
		case GLTFAnimation::INTERP_STEP: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				const Quaternion &q = (idx < 0) ? vptr[0] : ((idx >= tlen - 1) ? vptr[tlen - 1] : vptr[idx]);
				dst[s] = ensure_unit(q);
			}
		} break;
		case GLTFAnimation::INTERP_CATMULLROMSPLINE: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					const Quaternion &q = vlen > 1 ? vptr[1] : vptr[0];
					dst[s] = ensure_unit(q);
				} else if (idx >= tlen - 1) {
					const Quaternion &q = (1 + tlen - 1 < vlen) ? vptr[1 + tlen - 1] : vptr[vlen - 1];
					dst[s] = ensure_unit(q);
				} else {
					dst[s] = safe_slerp(vptr[idx], vptr[idx + 1], steps[s].frac);
				}
			}
		} break;
		case GLTFAnimation::INTERP_CUBIC_SPLINE: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					dst[s] = ensure_unit(vptr[1]);
				} else if (idx >= tlen - 1) {
					dst[s] = ensure_unit(vptr[(tlen - 1) * 3 + 1]);
				} else {
					dst[s] = safe_slerp(vptr[idx * 3 + 1], vptr[idx * 3 + 4], steps[s].frac);
				}
			}
		} break;
	}
}

void eval_scalar_samples(
		const SampleSchedule &p_sched,
		const Vector<real_t> &p_values,
		GLTFAnimation::Interpolation p_interp,
		LocalVector<float> &r_out) {
	const uint32_t scount = p_sched.steps.size();
	r_out.resize(scount);
	const int32_t tlen = p_sched.times_len;
	const int32_t vlen = p_values.size();
	if (vlen == 0) {
		for (uint32_t s = 0; s < scount; s++) {
			r_out[s] = 0.0f;
		}
		return;
	}
	const real_t *vptr = p_values.ptr();
	const int expected_mult = (p_interp == GLTFAnimation::INTERP_CUBIC_SPLINE) ? 3 : 1;
	if (tlen != vlen / expected_mult) {
		for (uint32_t s = 0; s < scount; s++) {
			r_out[s] = float(vptr[0]);
		}
		return;
	}
	const SampleStep *steps = p_sched.steps.ptr();
	float *dst = r_out.ptr();

	for (uint32_t s = 0; s < scount; s++) {
		const int32_t idx = steps[s].idx;
		if (p_interp == GLTFAnimation::INTERP_CUBIC_SPLINE) {
			if (idx < 0) {
				dst[s] = float(vptr[1]);
			} else if (idx >= tlen - 1) {
				dst[s] = float(vptr[(tlen - 1) * 3 + 1]);
			} else {
				const float td = steps[s].td;
				dst[s] = float(hermite_eval(vptr[idx * 3 + 1], vptr[idx * 3 + 2] * td, vptr[idx * 3 + 4], vptr[idx * 3 + 3] * td, steps[s].frac));
			}
		} else {
			if (idx < 0) {
				dst[s] = float(vptr[0]);
			} else if (idx >= tlen - 1) {
				dst[s] = float(vptr[tlen - 1]);
			} else {
				dst[s] = float(vptr[idx] + (vptr[idx + 1] - vptr[idx]) * steps[s].frac);
			}
		}
	}
}

NodePath compute_direct_node_path(const Vector<Ref<GLTFNode>> &p_nodes, GLTFNodeIndex p_node_idx, int p_single_root_node_idx) {
	if (p_node_idx == p_single_root_node_idx) {
		return NodePath(".");
	}
	Vector<StringName> names;
	GLTFNodeIndex cur = p_node_idx;
	while (cur >= 0 && cur != p_single_root_node_idx) {
		ERR_FAIL_INDEX_V(cur, p_nodes.size(), NodePath());
		names.push_back(StringName(p_nodes[cur]->get_name()));
		cur = GLTFFastAccess::node_parent(p_nodes[cur].ptr());
	}
	names.reverse();
	return NodePath(names, false);
}

struct NodeBaseTRS {
	Vector3 pos;
	Quaternion rot;
	Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
};

struct DirectAnimContext {
	LocalVector<NodeBaseTRS> base_trs;
	LocalVector<Vector<StringName>> skel_paths;
	LocalVector<Vector<StringName>> scene_paths;
	LocalVector<Vector<StringName>> rmi_paths;
	bool has_vnode_paths = false;
};

void build_node_base_trs(const Vector<Ref<GLTFNode>> &p_nodes, LocalVector<NodeBaseTRS> &r_out) {
	const int n = p_nodes.size();
	r_out.resize(uint32_t(n));
	for (int i = 0; i < n; i++) {
		NodeBaseTRS trs;
		if (p_nodes[i].is_valid()) {
			const Transform3D &xf = GLTFFastAccess::node_xform(p_nodes[i].ptr());
			trs.pos = xf.origin;
			if (xf.basis != Basis()) {
				trs.rot = xf.basis.get_rotation_quaternion();
				trs.scale = xf.basis.get_scale();
			}
		}
		r_out[uint32_t(i)] = trs;
	}
}

Ref<Animation> build_animation_fast_impl(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		Node *p_scene_root,
		GLTFAnimationIndex p_index,
		bool p_trimming,
		bool p_remove_immutable_tracks,
		int p_single_root_node_idx,
		const DirectAnimContext *p_direct_ctx) {
	ERR_FAIL_COND_V(p_state.is_null(), Ref<Animation>());
	Vector<Ref<GLTFAnimation>> &anims = GLTFFastAccess::animations(p_state.ptr());
	ERR_FAIL_INDEX_V(p_index, anims.size(), Ref<Animation>());
	Ref<GLTFAnimation> anim = anims[p_index];
	ERR_FAIL_COND_V(anim.is_null(), Ref<Animation>());

	String anim_name = anim->get_name();
	if (anim_name.is_empty()) {
		anim_name = p_direct_ctx ? (String("Animation_") + itos(p_index)) : GLTFFastAccess::gen_unique_name(p_doc, p_state, "Animation");
	}

	Ref<Animation> animation;
	animation.instantiate();
	animation->set_name(anim_name);
	const double bake_fps = p_state->get_bake_fps() > 0.0 ? p_state->get_bake_fps() : 30.0;
	const double increment = 1.0 / bake_fps;
	animation->set_step(increment);
	if (anim->get_loop()) {
		animation->set_loop_mode(Animation::LOOP_LINEAR);
	}

	// Pass 1: Compute exact [anim_start, anim_end] across ALL tracks first!
	double anim_start = p_trimming ? Math::INF : 0.0;
	double anim_end = 0.0;
	for (const KeyValue<int, GLTFAnimation::NodeTrack> &track_i : anim->get_node_tracks()) {
		const GLTFAnimation::NodeTrack &track = track_i.value;
		auto scan_times = [&](const Vector<double> &tvec) {
			const int n = tvec.size();
			if (n <= 0) {
				return;
			}
			const double *tp = tvec.ptr();
			if (p_trimming) {
				for (int i = 0; i < n; i++) {
					anim_start = MIN(anim_start, tp[i]);
					anim_end = MAX(anim_end, tp[i]);
				}
			} else {
				anim_end = MAX(anim_end, tp[n - 1]);
				for (int i = 0; i < n - 1; i++) {
					if (unlikely(tp[i] > anim_end)) {
						anim_end = tp[i];
					}
				}
			}
		};
		scan_times(track.rotation_track.times);
		scan_times(track.position_track.times);
		scan_times(track.scale_track.times);
		for (int i = 0; i < track.weight_tracks.size(); i++) {
			scan_times(track.weight_tracks[i].times);
		}
	}
	for (const KeyValue<String, GLTFAnimation::Channel<Variant>> &track_iter : anim->get_pointer_tracks()) {
		const Vector<double> &tvec = track_iter.value.times;
		for (int i = 0; i < tvec.size(); i++) {
			if (p_trimming) {
				anim_start = MIN(anim_start, tvec[i]);
			}
			anim_end = MAX(anim_end, tvec[i]);
		}
	}
	if (anim_start == Math::INF) {
		anim_start = 0.0;
	}
	if (anim_end < anim_start) {
		anim_end = anim_start;
	}

	LocalVector<double> abs_sample_times;
	LocalVector<double> rel_sample_times;
	build_sample_times(anim_start, anim_end, increment, abs_sample_times, rel_sample_times);

	LocalVector<SampleSchedule> schedule_cache;
	LocalVector<Vector3> scratch_vec3;
	LocalVector<Quaternion> scratch_quat;
	LocalVector<float> scratch_float;

	Vector<Ref<GLTFNode>> &nodes = GLTFFastAccess::nodes(p_state.ptr());
	Vector<Ref<GLTFMesh>> &meshes = GLTFFastAccess::meshes(p_state.ptr());
	Vector<Ref<GLTFSkeleton>> &skeletons = GLTFFastAccess::skeletons(p_state.ptr());
	auto &scene_nodes = GLTFFastAccess::scene_nodes(p_state.ptr());
	auto &scene_mesh_instances = GLTFFastAccess::scene_mesh_instances(p_state.ptr());
	auto &scene_runtime_mesh_instances = GLTFFastAccess::scene_runtime_mesh_instances(p_state.ptr());

	LocalVector<String> skel_path_cache;
	if (p_scene_root && !skeletons.is_empty()) {
		skel_path_cache.resize(uint32_t(skeletons.size()));
		for (int s_i = 0; s_i < skeletons.size(); s_i++) {
			Skeleton3D *sk = GLTFFastAccess::godot_skeleton(skeletons.write[s_i].ptr());
			if (sk) {
				skel_path_cache[uint32_t(s_i)] = String(p_scene_root->get_path_to(sk));
			}
		}
	}

	// Pass 2: Evaluate and bulk-insert tracks.
	for (const KeyValue<int, GLTFAnimation::NodeTrack> &track_i : anim->get_node_tracks()) {
		const GLTFNodeIndex node_index = track_i.key;
		ERR_CONTINUE(node_index < 0 || node_index >= nodes.size());
		Ref<GLTFNode> gltf_node = nodes[node_index];
		const GLTFAnimation::NodeTrack &track = track_i.value;

		NodePath node_path;
		NodePath transform_node_path;
		Vector<StringName> direct_mesh_path_names;
		NodePath mesh_instance_node_path;

		if (p_scene_root) {
			const GLTFSkeletonIndex skel_idx = GLTFFastAccess::node_skeleton(gltf_node.ptr());
			if (skel_idx >= 0 && skel_idx < int(skel_path_cache.size()) && !skel_path_cache[uint32_t(skel_idx)].is_empty()) {
				transform_node_path = NodePath(skel_path_cache[uint32_t(skel_idx)] + ":" + gltf_node->get_name());
				if (!track.weight_tracks.is_empty()) {
					auto node_el = scene_nodes.find(node_index);
					if (node_el) {
						node_path = p_scene_root->get_path_to(node_el->value);
					}
				}
			} else {
				auto node_el = scene_nodes.find(node_index);
				ERR_CONTINUE_MSG(!node_el, vformat("Unable to find node %d for animation.", node_index));
				node_path = p_scene_root->get_path_to(node_el->value);
				transform_node_path = node_path;
			}
			if (!track.weight_tracks.is_empty()) {
				auto rmi_el = scene_runtime_mesh_instances.find(node_index);
				if (rmi_el && rmi_el->value) {
					mesh_instance_node_path = p_scene_root->get_path_to(rmi_el->value);
				} else {
					auto imi_el = scene_mesh_instances.find(node_index);
					if (imi_el && imi_el->value) {
						mesh_instance_node_path = p_scene_root->get_path_to(imi_el->value);
					} else {
						mesh_instance_node_path = node_path;
					}
				}
			}
		} else if (p_direct_ctx && p_direct_ctx->has_vnode_paths) {
			const GLTFSkeletonIndex skel_idx = GLTFFastAccess::node_skeleton(gltf_node.ptr());
			if (skel_idx >= 0 && uint32_t(skel_idx) < p_direct_ctx->skel_paths.size() && !p_direct_ctx->skel_paths[uint32_t(skel_idx)].is_empty()) {
				Vector<StringName> sub;
				sub.push_back(StringName(gltf_node->get_name()));
				transform_node_path = NodePath(p_direct_ctx->skel_paths[uint32_t(skel_idx)], sub, false);
			} else {
				ERR_CONTINUE_MSG(uint32_t(node_index) >= p_direct_ctx->scene_paths.size() || p_direct_ctx->scene_paths[uint32_t(node_index)].is_empty(),
						vformat("Unable to find node %d for animation.", node_index));
				node_path = NodePath(p_direct_ctx->scene_paths[uint32_t(node_index)], false);
				transform_node_path = node_path;
			}
			if (!track.weight_tracks.is_empty()) {
				if (uint32_t(node_index) < p_direct_ctx->rmi_paths.size() && !p_direct_ctx->rmi_paths[uint32_t(node_index)].is_empty()) {
					direct_mesh_path_names = p_direct_ctx->rmi_paths[uint32_t(node_index)];
				} else if (uint32_t(node_index) < p_direct_ctx->scene_paths.size()) {
					direct_mesh_path_names = p_direct_ctx->scene_paths[uint32_t(node_index)];
				}
			}
		} else {
			node_path = compute_direct_node_path(nodes, node_index, p_single_root_node_idx);
			transform_node_path = node_path;
			mesh_instance_node_path = node_path;
		}

		const bool transform_affects_skinned_mesh_instance =
				GLTFFastAccess::node_skeleton(gltf_node.ptr()) < 0 &&
				GLTFFastAccess::node_skin(gltf_node.ptr()) >= 0;

		if ((track.rotation_track.values.size() || track.position_track.values.size() || track.scale_track.values.size()) &&
				!transform_affects_skinned_mesh_instance) {
			const bool has_pre_trs = (p_direct_ctx && uint32_t(node_index) < p_direct_ctx->base_trs.size());
			if (track.position_track.values.size() && track.position_track.times.size()) {
				bool is_default = true;
				if (p_remove_immutable_tracks) {
					const Vector3 base_pos = has_pre_trs ? p_direct_ctx->base_trs[uint32_t(node_index)].pos : gltf_node->get_position();
					const Vector3 *vp = track.position_track.values.ptr();
					const int vcnt = track.position_track.values.size();
					const int tcnt = track.position_track.times.size();
					const bool cubic = track.position_track.interpolation == GLTFAnimation::INTERP_CUBIC_SPLINE;
					for (int i = 0; i < tcnt; i++) {
						const int vi = cubic ? (1 + i * 3) : i;
						ERR_BREAK(vi >= vcnt);
						if (!vp[vi].is_equal_approx(base_pos)) {
							is_default = false;
							break;
						}
					}
				} else {
					is_default = false;
				}
				if (!is_default) {
					const int pos_idx = animation->get_track_count();
					animation->add_track(Animation::TYPE_POSITION_3D);
					animation->track_set_path(pos_idx, transform_node_path);
					animation->track_set_imported(pos_idx, true);
					if (track.position_track.interpolation == GLTFAnimation::INTERP_STEP) {
						animation->track_set_interpolation_type(pos_idx, Animation::INTERPOLATION_NEAREST);
					}
					const SampleSchedule &sched = get_or_build_schedule(schedule_cache, track.position_track.times, abs_sample_times);
					eval_vec3_samples(sched, track.position_track.values, track.position_track.interpolation, scratch_vec3);
					animation->position_track_set_keys_bulk(pos_idx, rel_sample_times.ptr(), scratch_vec3.ptr(), int(rel_sample_times.size()));
				}
			}

			if (track.rotation_track.values.size() && track.rotation_track.times.size()) {
				bool is_default = true;
				if (p_remove_immutable_tracks) {
					const Quaternion base_rot = has_pre_trs ? p_direct_ctx->base_trs[uint32_t(node_index)].rot : gltf_node->get_rotation();
					const Quaternion *vp = track.rotation_track.values.ptr();
					const int vcnt = track.rotation_track.values.size();
					const int tcnt = track.rotation_track.times.size();
					const bool cubic = track.rotation_track.interpolation == GLTFAnimation::INTERP_CUBIC_SPLINE;
					for (int i = 0; i < tcnt; i++) {
						const int vi = cubic ? (1 + i * 3) : i;
						ERR_BREAK(vi >= vcnt);
						const Quaternion &q = vp[vi];
						const float l2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
						const Quaternion val = (std::abs(l2 - 1.0f) <= 1e-5f) ? q : q.normalized();
						if (!val.is_equal_approx(base_rot)) {
							is_default = false;
							break;
						}
					}
				} else {
					is_default = false;
				}
				if (!is_default) {
					const int rot_idx = animation->get_track_count();
					animation->add_track(Animation::TYPE_ROTATION_3D);
					animation->track_set_path(rot_idx, transform_node_path);
					animation->track_set_imported(rot_idx, true);
					if (track.rotation_track.interpolation == GLTFAnimation::INTERP_STEP) {
						animation->track_set_interpolation_type(rot_idx, Animation::INTERPOLATION_NEAREST);
					}
					const SampleSchedule &sched = get_or_build_schedule(schedule_cache, track.rotation_track.times, abs_sample_times);
					eval_quat_samples(sched, track.rotation_track.values, track.rotation_track.interpolation, scratch_quat);
					animation->rotation_track_set_keys_bulk(rot_idx, rel_sample_times.ptr(), scratch_quat.ptr(), int(rel_sample_times.size()));
				}
			}

			if (track.scale_track.values.size() && track.scale_track.times.size()) {
				bool is_default = true;
				if (p_remove_immutable_tracks) {
					const Vector3 base_scale = has_pre_trs ? p_direct_ctx->base_trs[uint32_t(node_index)].scale : gltf_node->get_scale();
					const Vector3 *vp = track.scale_track.values.ptr();
					const int vcnt = track.scale_track.values.size();
					const int tcnt = track.scale_track.times.size();
					const bool cubic = track.scale_track.interpolation == GLTFAnimation::INTERP_CUBIC_SPLINE;
					for (int i = 0; i < tcnt; i++) {
						const int vi = cubic ? (1 + i * 3) : i;
						ERR_BREAK(vi >= vcnt);
						if (!vp[vi].is_equal_approx(base_scale)) {
							is_default = false;
							break;
						}
					}
				} else {
					is_default = false;
				}
				if (!is_default) {
					const int scl_idx = animation->get_track_count();
					animation->add_track(Animation::TYPE_SCALE_3D);
					animation->track_set_path(scl_idx, transform_node_path);
					animation->track_set_imported(scl_idx, true);
					if (track.scale_track.interpolation == GLTFAnimation::INTERP_STEP) {
						animation->track_set_interpolation_type(scl_idx, Animation::INTERPOLATION_NEAREST);
					}
					const SampleSchedule &sched = get_or_build_schedule(schedule_cache, track.scale_track.times, abs_sample_times);
					eval_vec3_samples(sched, track.scale_track.values, track.scale_track.interpolation, scratch_vec3);
					animation->scale_track_set_keys_bulk(scl_idx, rel_sample_times.ptr(), scratch_vec3.ptr(), int(rel_sample_times.size()));
				}
			}
		}

		const GLTFMeshIndex mesh_idx = GLTFFastAccess::node_mesh(gltf_node.ptr());
		for (int i = 0; i < track.weight_tracks.size(); i++) {
			ERR_CONTINUE(mesh_idx < 0 || mesh_idx >= meshes.size());
			Ref<GLTFMesh> gmesh = meshes[mesh_idx];
			ERR_CONTINUE(gmesh.is_null());

			StringName bs_sname;
			if (gmesh->get_runtime_mesh().is_valid()) {
				ERR_CONTINUE(i >= gmesh->get_runtime_mesh()->get_blend_shape_count());
				bs_sname = gmesh->get_runtime_mesh()->get_blend_shape_name(i);
			} else {
				ERR_CONTINUE(gmesh->get_mesh().is_null() || i >= gmesh->get_mesh()->get_blend_shape_count());
				bs_sname = StringName(gmesh->get_mesh()->get_blend_shape_name(i));
			}

			NodePath blend_np;
			if (!direct_mesh_path_names.is_empty()) {
				Vector<StringName> sub;
				sub.push_back(bs_sname);
				blend_np = NodePath(direct_mesh_path_names, sub, false);
			} else {
				blend_np = NodePath(String(mesh_instance_node_path) + ":" + String(bs_sname));
			}
			const int track_idx = animation->get_track_count();
			animation->add_track(Animation::TYPE_BLEND_SHAPE);
			animation->track_set_path(track_idx, blend_np);
			animation->track_set_imported(track_idx, true);

			const GLTFAnimation::Channel<real_t> &wch = track.weight_tracks[i];
			if (wch.interpolation == GLTFAnimation::INTERP_LINEAR || wch.interpolation == GLTFAnimation::INTERP_STEP) {
				animation->track_set_interpolation_type(
						track_idx,
						wch.interpolation == GLTFAnimation::INTERP_STEP ? Animation::INTERPOLATION_NEAREST : Animation::INTERPOLATION_LINEAR);
				const int kcnt = MIN(wch.times.size(), wch.values.size());
				if (kcnt > 0) {
					bool sorted_strict = true;
					const double *tp = wch.times.ptr();
					for (int k = 1; k < kcnt; k++) {
						if (tp[k] <= tp[k - 1]) {
							sorted_strict = false;
							break;
						}
					}
					if (sorted_strict) {
						scratch_float.resize(uint32_t(kcnt));
						const real_t *vp = wch.values.ptr();
						for (int k = 0; k < kcnt; k++) {
							scratch_float[uint32_t(k)] = float(vp[k]);
						}
						animation->blend_shape_track_set_keys_bulk(track_idx, tp, scratch_float.ptr(), kcnt);
					} else {
						for (int k = 0; k < kcnt; k++) {
							animation->blend_shape_track_insert_key(track_idx, float(wch.times[k]), float(wch.values[k]));
						}
					}
				}
			} else {
				const SampleSchedule &sched = get_or_build_schedule(schedule_cache, wch.times, abs_sample_times);
				eval_scalar_samples(sched, wch.values, wch.interpolation, scratch_float);
				animation->blend_shape_track_set_keys_bulk(track_idx, rel_sample_times.ptr(), scratch_float.ptr(), int(rel_sample_times.size()));
			}
		}
	}

	// KHR_animation_pointer tracks (when p_scene_root is present).
	if (p_scene_root && !anim->get_pointer_tracks().is_empty()) {
		for (const KeyValue<String, GLTFAnimation::Channel<Variant>> &track_iter : anim->get_pointer_tracks()) {
			const String &json_pointer = track_iter.key;
			const Ref<GLTFObjectModelProperty> prop = GLTFDocument::import_object_model_property(p_state, json_pointer);
			ERR_CONTINUE(prop.is_null());
			const GLTFAnimation::Channel<Variant> &channel = track_iter.value;
			ERR_CONTINUE_MSG(channel.times.size() != channel.values.size(), vformat("glTF: Animation pointer '%s' has mismatched keyframe times and values.", json_pointer));
			const Ref<Expression> expr = prop->get_gltf_to_godot_expression();
			const bool has_expr = expr.is_valid();
			for (const NodePath node_path : prop->get_node_paths()) {
				Object *base_instance = nullptr;
				if (has_expr) {
					Ref<Resource> resource;
					Vector<StringName> leftover_subpath;
					base_instance = p_scene_root->get_node_and_resource(node_path, resource, leftover_subpath);
					if (resource.is_valid()) {
						base_instance = resource.ptr();
					}
				}
				const int track_index = animation->get_track_count();
				animation->add_track(Animation::TYPE_VALUE);
				animation->track_set_interpolation_type(track_index, GLTFAnimation::gltf_to_godot_interpolation(channel.interpolation));
				animation->track_set_path(track_index, node_path);
				for (int i = 0; i < channel.times.size(); i++) {
					Variant value = channel.values[i];
					if (has_expr) {
						Array inputs;
						inputs.append(value);
						value = expr->execute(inputs, base_instance);
					}
					animation->track_insert_key(track_index, channel.times[i], value);
				}
			}
		}
	}

	animation->set_length(anim_end - anim_start);
	return animation;
}

struct DirectBoneInfo {
	String name;
	int parent = -1;
	Transform3D rest;
	Vector3 pos;
	Quaternion rot;
	Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
	Variant extras;
	bool has_extras = false;
};

struct DirectSkelInfo {
	LocalVector<DirectBoneInfo> bones;
	HashMap<String, int> bone_name_to_idx;
};

enum VirtNodeKind : uint8_t {
	VKIND_NODE3D = 0,
	VKIND_MESH = 1,
	VKIND_SKELETON = 2,
	VKIND_BONE_ATTACHMENT = 3,
};

struct VirtNode {
	VirtNodeKind kind = VKIND_NODE3D;
	bool use_node_xform = false;
	bool use_node_vis = false;
	bool is_skinned_mesh = false;
	int parent_vidx = -1;
	int gltf_node_idx = -1;
	int skel_idx = -1;
	int skin_idx = -1;
	int bone_attach_idx = -1;
	int merge_meta_node = -1;
	String name;
	String bone_name;
	LocalVector<int> children;
};

} // namespace

Ref<Animation> gltf_build_animation_fast(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		Node *p_scene_root,
		GLTFAnimationIndex p_index,
		bool p_trimming,
		bool p_remove_immutable_tracks,
		int p_single_root_node_idx) {
	return build_animation_fast_impl(p_doc, p_state, p_scene_root, p_index, p_trimming, p_remove_immutable_tracks, p_single_root_node_idx, nullptr);
}

void gltf_import_animation_fast(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		AnimationPlayer *p_animation_player,
		GLTFAnimationIndex p_index,
		bool p_trimming,
		bool p_remove_immutable_tracks) {
	ERR_FAIL_NULL(p_animation_player);
	Node *scene_root = p_animation_player->get_parent();
	ERR_FAIL_NULL(scene_root);
	Ref<Animation> animation = build_animation_fast_impl(p_doc, p_state, scene_root, p_index, p_trimming, p_remove_immutable_tracks, -1, nullptr);
	ERR_FAIL_COND(animation.is_null());

	Ref<AnimationLibrary> library;
	if (!p_animation_player->has_animation_library("")) {
		library.instantiate();
		p_animation_player->add_animation_library("", library);
	} else {
		library = p_animation_player->get_animation_library("");
	}
	library->add_animation(animation->get_name(), animation);
}

Ref<PackedScene> gltf_build_packed_scene_direct(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		bool p_has_custom_extensions,
		float p_bake_fps,
		bool p_trimming,
		bool p_remove_immutable_tracks) {
	ERR_FAIL_NULL_V(p_doc, Ref<PackedScene>());
	ERR_FAIL_COND_V(p_state.is_null(), Ref<PackedScene>());

	p_state->set_bake_fps(p_bake_fps);
	Vector<Ref<GLTFNode>> &nodes = GLTFFastAccess::nodes(p_state.ptr());
	Vector<Ref<GLTFMesh>> &meshes = GLTFFastAccess::meshes(p_state.ptr());
	Vector<Ref<GLTFSkin>> &skins = GLTFFastAccess::skins(p_state.ptr());
	Vector<Ref<GLTFSkeleton>> &skeletons = GLTFFastAccess::skeletons(p_state.ptr());
	Vector<Ref<GLTFAnimation>> &anims = GLTFFastAccess::animations(p_state.ptr());

	bool has_pointer_tracks = false;
	for (int i = 0; i < anims.size(); i++) {
		if (anims[i].is_valid() && !anims[i]->get_pointer_tracks().is_empty()) {
			has_pointer_tracks = true;
			break;
		}
	}

	const bool can_use_direct_state =
			!p_has_custom_extensions &&
			p_doc->get_naming_version() >= 2 &&
			GLTFFastAccess::cameras(p_state.ptr()).is_empty() &&
			GLTFFastAccess::lights(p_state.ptr()).is_empty() &&
			!has_pointer_tracks;

	if (!can_use_direct_state) {
		Node *root = p_doc->generate_scene(p_state, p_bake_fps, p_trimming, p_remove_immutable_tracks);
		ERR_FAIL_NULL_V(root, Ref<PackedScene>());
		Ref<PackedScene> packed;
		packed.instantiate();
		Error err = packed->pack(root);
		memdelete(root);
		ERR_FAIL_COND_V(err != OK, Ref<PackedScene>());
		return packed;
	}

	// Zero-Node* direct SceneState builder (handles both static hierarchies and full Skeleton3D + Skin + BoneAttachment3D rigs).
	Ref<SceneState> ss;
	ss.instantiate();
	HashMap<StringName, int> name_map;
	if (nodes.size() > 32) {
		name_map.reserve(uint32_t(nodes.size() + 64));
	}
	HashMap<Variant, int> resource_val_map;

	auto intern_name = [&](const StringName &p_sname) -> int {
		HashMap<StringName, int>::Iterator E = name_map.find(p_sname);
		if (E) {
			return E->value;
		}
		const int idx = ss->add_name(p_sname);
		name_map.insert(p_sname, idx);
		return idx;
	};

	auto intern_shared_val = [&](const Variant &p_val) -> int {
		HashMap<Variant, int>::Iterator E = resource_val_map.find(p_val);
		if (E) {
			return E->value;
		}
		const int idx = ss->add_value(p_val);
		resource_val_map.insert(p_val, idx);
		return idx;
	};

	const int type_node3d = intern_name(SNAME("Node3D"));
	const int type_mesh_instance = intern_name(SNAME("MeshInstance3D"));
	const int prop_transform = intern_name(SNAME("transform"));
	const int prop_visible = intern_name(SNAME("visible"));
	const int prop_mesh = intern_name(SNAME("mesh"));
	const int val_false = intern_shared_val(false);
	int32_t next_unique_id = 1;

	auto emit_mesh_and_blend_props = [&](int p_ss_node_idx, const Ref<GLTFNode> &p_gnode) {
		const GLTFMeshIndex midx = GLTFFastAccess::node_mesh(p_gnode.ptr());
		if (midx >= 0 && midx < meshes.size() && meshes[midx].is_valid()) {
			Ref<GLTFMesh> gmesh = meshes[midx];
			Ref<Mesh> mres = gmesh->get_runtime_mesh();
			if (mres.is_null() && gmesh->get_mesh().is_valid()) {
				mres = gmesh->get_mesh()->get_mesh();
			}
			if (mres.is_valid()) {
				if (gmesh->has_meta("extras")) {
					mres->set_meta("extras", gmesh->get_meta("extras"));
				}
				ss->add_node_property(p_ss_node_idx, prop_mesh, intern_shared_val(mres));
				Vector<float> weights = gmesh->get_blend_weights();
				const Vector<float> &nw_fast = GLTFFastAccess::node_weights(p_gnode.ptr());
				if (!nw_fast.is_empty()) {
					for (int w = 0; w < MIN(nw_fast.size(), weights.size()); w++) {
						weights.write[w] = nw_fast[w];
					}
				} else if (const_cast<GLTFNode *>(p_gnode.ptr())->has_additional_data(SNAME("_gltf_node_weights"))) {
					Vector<float> nw = const_cast<GLTFNode *>(p_gnode.ptr())->get_additional_data(SNAME("_gltf_node_weights"));
					for (int w = 0; w < MIN(nw.size(), weights.size()); w++) {
						weights.write[w] = nw[w];
					}
				}
				const int bs_cnt = MIN(weights.size(), mres->get_blend_shape_count());
				for (int b = 0; b < bs_cnt; b++) {
					if (weights[b] != 0.0f) {
						const StringName bprop(String("blend_shapes/") + String(mres->get_blend_shape_name(b)));
						ss->add_node_property(p_ss_node_idx, intern_name(bprop), ss->add_value(weights[b]));
					}
				}
			}
		}
	};

	auto emit_node_meta = [&](int p_ss_node_idx, const Ref<GLTFNode> &p_gnode) {
		List<StringName> meta_list;
		p_gnode->get_meta_list(&meta_list);
		for (const StringName &mk : meta_list) {
			const StringName mprop(String("metadata/") + String(mk));
			ss->add_node_property(p_ss_node_idx, intern_name(mprop), ss->add_value(p_gnode->get_meta(mk)));
		}
	};

	const bool use_single_root = GLTFFastAccess::extensions_used(p_state.ptr()).has("GODOT_single_root");
	int single_root_gltf_idx = -1;
	DirectAnimContext anim_ctx;

	if (skeletons.is_empty()) {
		// Ultra-lean zero-allocation stack walk for static / morph / node-animated scenes (e.g. nodes_10000).
		auto emit_static_props = [&](int p_ss_node_idx, const Ref<GLTFNode> &p_gnode) {
			const Transform3D &xf = GLTFFastAccess::node_xform(p_gnode.ptr());
			if (xf != Transform3D()) {
				ss->add_node_property(p_ss_node_idx, prop_transform, ss->add_value(xf));
			}
			if (!GLTFFastAccess::node_visible(p_gnode.ptr())) {
				ss->add_node_property(p_ss_node_idx, prop_visible, val_false);
			}
			emit_mesh_and_blend_props(p_ss_node_idx, p_gnode);
			emit_node_meta(p_ss_node_idx, p_gnode);
		};

		struct StackEntry {
			GLTFNodeIndex gltf_idx = -1;
			int parent_ss_idx = -1;
		};
		LocalVector<StackEntry> stack;

		if (use_single_root) {
			ERR_FAIL_COND_V(nodes.is_empty(), Ref<PackedScene>());
			single_root_gltf_idx = 0;
			Ref<GLTFNode> root_gnode = nodes[0];
			String rname = root_gnode->get_name();
			if (rname.is_empty()) {
				rname = p_state->get_scene_name().is_empty() ? String("Node3D") : GLTFFastAccess::gen_unique_name(p_doc, p_state, p_state->get_scene_name());
			}
			const int rtype = GLTFFastAccess::node_mesh(root_gnode.ptr()) >= 0 ? type_mesh_instance : type_node3d;
			const int root_ss_idx = ss->add_node(-1, -1, rtype, intern_name(StringName(rname)), -1, -1, next_unique_id++);
			emit_static_props(root_ss_idx, root_gnode);
			const Vector<int> &rchildren = GLTFFastAccess::node_children(root_gnode.ptr());
			for (int c = rchildren.size() - 1; c >= 0; c--) {
				stack.push_back(StackEntry{ rchildren[c], root_ss_idx });
			}
		} else {
			String sname = p_state->get_scene_name();
			if (sname.is_empty()) {
				sname = "Node3D";
			} else {
				sname = GLTFFastAccess::gen_unique_name(p_doc, p_state, sname);
			}
			const int root_ss_idx = ss->add_node(-1, -1, type_node3d, intern_name(StringName(sname)), -1, -1, next_unique_id++);
			const Vector<int> &rnodes = GLTFFastAccess::root_nodes(p_state.ptr());
			for (int r = rnodes.size() - 1; r >= 0; r--) {
				stack.push_back(StackEntry{ rnodes[r], root_ss_idx });
			}
		}

		while (!stack.is_empty()) {
			const StackEntry item = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			ERR_CONTINUE(item.gltf_idx < 0 || item.gltf_idx >= nodes.size());
			Ref<GLTFNode> gnode = nodes[item.gltf_idx];
			const int ntype = GLTFFastAccess::node_mesh(gnode.ptr()) >= 0 ? type_mesh_instance : type_node3d;
			String nname = gnode->get_name();
			if (nname.is_empty()) {
				nname = GLTFFastAccess::gen_unique_name(p_doc, p_state, "Node");
			}
			const int ss_idx = ss->add_node(item.parent_ss_idx, 0, ntype, intern_name(StringName(nname)), -1, -1, next_unique_id++);
			emit_static_props(ss_idx, gnode);

			const Vector<int> &children = GLTFFastAccess::node_children(gnode.ptr());
			for (int c = children.size() - 1; c >= 0; c--) {
				stack.push_back(StackEntry{ children[c], ss_idx });
			}
		}
	} else {
		// Direct Skeleton3D + Skin + BoneAttachment3D SceneState builder (zero Node* instantiation or ClassDB reflection).
		HashSet<String> &unique_names = GLTFFastAccess::unique_names(p_state.ptr());
		HashSet<String> unique_node_names(unique_names);
		LocalVector<DirectSkelInfo> direct_skels;
		direct_skels.resize(uint32_t(skeletons.size()));

		for (int skel_i = 0; skel_i < skeletons.size(); skel_i++) {
			Ref<GLTFSkeleton> gltf_skel = skeletons.write[skel_i];
			ERR_FAIL_COND_V(gltf_skel.is_null(), Ref<PackedScene>());
			DirectSkelInfo &dskel = direct_skels[uint32_t(skel_i)];
			HashSet<String> skel_unique_names(unique_node_names);

			Vector<GLTFNodeIndex> roots_sorted = GLTFFastAccess::skeleton_roots(gltf_skel.ptr());
			roots_sorted.sort();
			LocalVector<GLTFNodeIndex> bone_stack;
			for (int r = roots_sorted.size() - 1; r >= 0; r--) {
				bone_stack.push_back(roots_sorted[r]);
			}

			while (!bone_stack.is_empty()) {
				const GLTFNodeIndex node_i = bone_stack[bone_stack.size() - 1];
				bone_stack.resize(bone_stack.size() - 1);
				Ref<GLTFNode> node = nodes[node_i];
				ERR_FAIL_COND_V(GLTFFastAccess::node_skeleton(node.ptr()) != skel_i, Ref<PackedScene>());

				const Vector<int> &ch = GLTFFastAccess::node_children(node.ptr());
				if (!ch.is_empty()) {
					Vector<GLTFNodeIndex> child_bones;
					for (int c = 0; c < ch.size(); c++) {
						if (GLTFFastAccess::node_skeleton(nodes[ch[c]].ptr()) == skel_i) {
							child_bones.push_back(ch[c]);
						}
					}
					child_bones.sort();
					for (int c = child_bones.size() - 1; c >= 0; c--) {
						bone_stack.push_back(child_bones[c]);
					}
				}

				if (node->get_name().is_empty()) {
					node->set_name("bone");
				}
				const String unique_bone_name = SkinTool::_gen_unique_bone_name(skel_unique_names, node->get_name());
				unique_names.insert(unique_bone_name);
				node->set_name(unique_bone_name);

				const int bone_idx = int(dskel.bones.size());
				dskel.bone_name_to_idx.insert(unique_bone_name, bone_idx);
				GLTFFastAccess::skeleton_bone_node(gltf_skel.ptr()).insert(bone_idx, node_i);

				DirectBoneInfo binfo;
				binfo.name = unique_bone_name;
				binfo.rest = GLTFFastAccess::node_rest_xform(node.ptr());
				const Transform3D &xf = GLTFFastAccess::node_xform(node.ptr());
				binfo.pos = xf.origin;
				if (xf.basis != Basis()) {
					binfo.rot = xf.basis.get_rotation_quaternion();
					binfo.scale = xf.basis.get_scale();
				}
				if (node->has_meta("extras")) {
					binfo.extras = node->get_meta("extras");
					binfo.has_extras = true;
				}
				const GLTFNodeIndex par_ni = GLTFFastAccess::node_parent(node.ptr());
				if (par_ni >= 0 && GLTFFastAccess::node_skeleton(nodes[par_ni].ptr()) == skel_i) {
					const int *p_ptr = dskel.bone_name_to_idx.getptr(nodes[par_ni]->get_name());
					ERR_FAIL_COND_V(!p_ptr, Ref<PackedScene>());
					binfo.parent = *p_ptr;
				}
				dskel.bones.push_back(binfo);
			}
		}

		// Map skin joint indices to skeleton bone indices in O(1) and build Godot Skin resources.
		for (int skin_i = 0; skin_i < skins.size(); skin_i++) {
			Ref<GLTFSkin> gskin = skins.write[skin_i];
			ERR_CONTINUE(gskin.is_null());
			const int skel_i = GLTFFastAccess::skin_skeleton(gskin.ptr());
			ERR_FAIL_INDEX_V(skel_i, int(direct_skels.size()), Ref<PackedScene>());
			const DirectSkelInfo &dskel = direct_skels[uint32_t(skel_i)];
			const Vector<GLTFNodeIndex> &j_orig = GLTFFastAccess::skin_joints_original(gskin.ptr());
			HashMap<int, int> &j_to_b = GLTFFastAccess::skin_joint_to_bone(gskin.ptr());
			for (int ji = 0; ji < j_orig.size(); ji++) {
				const int *b_ptr = dskel.bone_name_to_idx.getptr(nodes[j_orig[ji]]->get_name());
				ERR_FAIL_COND_V(!b_ptr, Ref<PackedScene>());
				j_to_b.insert(ji, *b_ptr);
			}
		}
		ERR_FAIL_COND_V(SkinTool::_create_skins(skins, nodes, GLTFFastAccess::use_named_skin_binds(p_state.ptr()), unique_names) != OK, Ref<PackedScene>());

		// Build virtual scene tree mirroring GLTFDocument::_generate_scene_node + _process_mesh_instances.
		LocalVector<VirtNode> vnodes;
		vnodes.reserve(uint32_t(nodes.size() + skeletons.size() + 4));

		auto alloc_vnode = [&](VirtNodeKind p_kind, int p_gltf_idx, const String &p_name) -> int {
			const int idx = int(vnodes.size());
			VirtNode vn;
			vn.kind = p_kind;
			vn.gltf_node_idx = p_gltf_idx;
			vn.name = p_name;
			vnodes.push_back(vn);
			return idx;
		};

		auto attach_vchild = [&](int p_parent_vidx, int p_child_vidx) {
			String cname = vnodes[uint32_t(p_child_vidx)].name.validate_node_name();
			if (cname.is_empty()) {
				cname = "Node3D";
			}
			const LocalVector<int> &sibs = vnodes[uint32_t(p_parent_vidx)].children;
			String final_name = cname;
			int suffix = 2;
			while (true) {
				bool clash = false;
				for (uint32_t s = 0; s < sibs.size(); s++) {
					if (vnodes[uint32_t(sibs[s])].name == final_name) {
						clash = true;
						break;
					}
				}
				if (!clash) {
					break;
				}
				final_name = cname + itos(suffix++);
			}
			vnodes[uint32_t(p_child_vidx)].name = final_name;
			vnodes[uint32_t(p_child_vidx)].parent_vidx = p_parent_vidx;
			vnodes[uint32_t(p_parent_vidx)].children.push_back(p_child_vidx);
		};

		auto detach_vchild = [&](int p_child_vidx) {
			const int pv = vnodes[uint32_t(p_child_vidx)].parent_vidx;
			if (pv >= 0) {
				LocalVector<int> &sibs = vnodes[uint32_t(pv)].children;
				for (uint32_t i = 0; i < sibs.size(); i++) {
					if (sibs[i] == p_child_vidx) {
						sibs.remove_at(i);
						break;
					}
				}
				vnodes[uint32_t(p_child_vidx)].parent_vidx = -1;
			}
		};

		LocalVector<int> skel_vidx;
		skel_vidx.resize(uint32_t(skeletons.size()));
		LocalVector<int> scene_vidx;
		scene_vidx.resize(uint32_t(nodes.size()));
		LocalVector<int> rmi_vidx;
		rmi_vidx.resize(uint32_t(nodes.size()));
		for (int i = 0; i < nodes.size(); i++) {
			scene_vidx[uint32_t(i)] = -1;
			rmi_vidx[uint32_t(i)] = -1;
		}
		for (int s_i = 0; s_i < skeletons.size(); s_i++) {
			const int sv = alloc_vnode(VKIND_SKELETON, -1, "Skeleton3D");
			vnodes[uint32_t(sv)].skel_idx = s_i;
			skel_vidx[uint32_t(s_i)] = sv;
			for (const KeyValue<int32_t, GLTFNodeIndex> &kv : GLTFFastAccess::skeleton_bone_node(skeletons.write[s_i].ptr())) {
				if (kv.value >= 0 && kv.value < nodes.size()) {
					scene_vidx[uint32_t(kv.value)] = sv;
				}
			}
		}

		int root_vidx = -1;

		auto skel_has_parent = [&](int p_sk_i) -> bool {
			const int sv = skel_vidx[uint32_t(p_sk_i)];
			return vnodes[uint32_t(sv)].parent_vidx >= 0 || root_vidx == sv;
		};

		auto needs_skinned_placeholder = [&](const Ref<GLTFNode> &p_gn) -> bool {
			if (GLTFFastAccess::node_skin(p_gn.ptr()) < 0) {
				return false;
			}
			const Vector<int> &ch = GLTFFastAccess::node_children(p_gn.ptr());
			for (int i = 0; i < ch.size(); i++) {
				const Ref<GLTFNode> &cn = nodes[ch[i]];
				if (!GLTFFastAccess::node_joint(cn.ptr())) {
					return true;
				}
				const int csk = GLTFFastAccess::node_skeleton(cn.ptr());
				if (csk >= 0 && csk < skeletons.size() && !skel_has_parent(csk)) {
					return true;
				}
			}
			return false;
		};

		auto gen_vnode = [&](auto &self, GLTFNodeIndex p_ni, int p_parent_vidx) -> void {
			ERR_FAIL_INDEX(p_ni, nodes.size());
			Ref<GLTFNode> gn = nodes[p_ni];
			int cur_v = -1;

			auto attach_to_skel = [&](auto &att_self, GLTFNodeIndex p_ani, int p_cur_v, int p_sk_v, GLTFNodeIndex p_bone_ni) -> void {
				Ref<GLTFNode> agn = nodes[p_ani];
				if (p_cur_v >= 0 && vnodes[uint32_t(p_cur_v)].kind == VKIND_MESH && GLTFFastAccess::node_skin(agn.ptr()) >= 0) {
					attach_vchild(p_sk_v, p_cur_v);
				} else if (p_cur_v >= 0 || !GLTFFastAccess::node_joint(agn.ptr())) {
					const GLTFNodeIndex attach_ni = (p_bone_ni == -1)
							? (GLTFFastAccess::node_joint(agn.ptr()) ? p_ani : GLTFFastAccess::node_parent(agn.ptr()))
							: p_bone_ni;
					ERR_FAIL_INDEX(attach_ni, nodes.size());
					int attach_v = scene_vidx[uint32_t(attach_ni)];
					ERR_FAIL_COND(attach_v < 0);
					if (vnodes[uint32_t(attach_v)].kind == VKIND_SKELETON) {
						const int sk_i = vnodes[uint32_t(attach_v)].skel_idx;
						const String &bname = nodes[attach_ni]->get_name();
						const int ba_v = alloc_vnode(VKIND_BONE_ATTACHMENT, attach_ni, bname);
						vnodes[uint32_t(ba_v)].bone_name = bname;
						const int *bidx_p = direct_skels[uint32_t(sk_i)].bone_name_to_idx.getptr(bname);
						vnodes[uint32_t(ba_v)].bone_attach_idx = bidx_p ? *bidx_p : -1;
						vnodes[uint32_t(ba_v)].merge_meta_node = attach_ni;
						attach_vchild(p_sk_v, ba_v);
						scene_vidx[uint32_t(attach_ni)] = ba_v;
						attach_v = ba_v;
					}
					if (p_cur_v < 0) {
						p_cur_v = alloc_vnode(VKIND_NODE3D, p_ani, agn->get_name());
					} else if (!agn->get_name().is_empty()) {
						vnodes[uint32_t(p_cur_v)].name = agn->get_name();
					}
					if (!GLTFFastAccess::node_joint(agn.ptr())) {
						vnodes[uint32_t(p_cur_v)].use_node_xform = true;
					}
					attach_vchild(attach_v, p_cur_v);
				} else {
					p_cur_v = p_sk_v;
				}
				if (vnodes[uint32_t(p_cur_v)].kind != VKIND_SKELETON) {
					vnodes[uint32_t(p_cur_v)].merge_meta_node = p_ani;
				}
				scene_vidx[uint32_t(p_ani)] = p_cur_v;
				const Vector<int> &ch = GLTFFastAccess::node_children(agn.ptr());
				for (int i = 0; i < ch.size(); i++) {
					self(self, ch[i], p_cur_v);
				}
			};

			if (GLTFFastAccess::node_mesh(gn.ptr()) >= 0) {
				const int mv = alloc_vnode(VKIND_MESH, p_ni, gn->get_name());
				rmi_vidx[uint32_t(p_ni)] = mv;
				cur_v = mv;
				if (needs_skinned_placeholder(gn)) {
					const int sk_i = GLTFFastAccess::node_skeleton(gn.ptr());
					int ph_v = -1;
					if (sk_i >= 0 && sk_i < skeletons.size() && !skel_has_parent(sk_i)) {
						ph_v = skel_vidx[uint32_t(sk_i)];
					} else {
						ph_v = alloc_vnode(VKIND_NODE3D, p_ni, gn->get_name());
					}
					attach_vchild(ph_v, cur_v);
					cur_v = ph_v;
				}
			}

			if (cur_v < 0 || vnodes[uint32_t(cur_v)].kind != VKIND_SKELETON) {
				if (cur_v >= 0 && !gn->get_name().is_empty()) {
					vnodes[uint32_t(cur_v)].name = gn->get_name();
				}
				const int sk_i = GLTFFastAccess::node_skeleton(gn.ptr());
				if (sk_i >= 0) {
					const int sk_v = skel_vidx[uint32_t(sk_i)];
					if (!skel_has_parent(sk_i)) {
						if (root_vidx >= 0) {
							if (p_parent_vidx >= 0 && vnodes[uint32_t(p_parent_vidx)].kind == VKIND_SKELETON) {
								attach_to_skel(attach_to_skel, p_ni, sk_v, p_parent_vidx, GLTFFastAccess::node_parent(gn.ptr()));
							} else if (p_parent_vidx >= 0) {
								attach_vchild(p_parent_vidx, sk_v);
							}
						} else {
							root_vidx = sk_v;
						}
					}
					attach_to_skel(attach_to_skel, p_ni, cur_v, sk_v, -1);
					return;
				}
			}

			if (p_parent_vidx >= 0 && vnodes[uint32_t(p_parent_vidx)].kind == VKIND_SKELETON) {
				attach_to_skel(attach_to_skel, p_ni, cur_v, p_parent_vidx, -1);
				return;
			}

			if (cur_v < 0) {
				cur_v = alloc_vnode(VKIND_NODE3D, p_ni, gn->get_name());
			}
			if (p_parent_vidx >= 0) {
				attach_vchild(p_parent_vidx, cur_v);
			}
			if (root_vidx < 0) {
				root_vidx = cur_v;
			}
			vnodes[uint32_t(cur_v)].use_node_xform = true;
			vnodes[uint32_t(cur_v)].use_node_vis = true;
			vnodes[uint32_t(cur_v)].merge_meta_node = p_ni;
			scene_vidx[uint32_t(p_ni)] = cur_v;

			const Vector<int> &ch = GLTFFastAccess::node_children(gn.ptr());
			for (int i = 0; i < ch.size(); i++) {
				self(self, ch[i], cur_v);
			}
		};

		if (use_single_root) {
			ERR_FAIL_COND_V(nodes.is_empty(), Ref<PackedScene>());
			single_root_gltf_idx = 0;
			gen_vnode(gen_vnode, 0, -1);
			if (scene_vidx[0] >= 0 && root_vidx < 0) {
				root_vidx = scene_vidx[0];
			}
		} else {
			root_vidx = alloc_vnode(VKIND_NODE3D, -1, "");
			const Vector<int> &rnodes = GLTFFastAccess::root_nodes(p_state.ptr());
			for (int r = 0; r < rnodes.size(); r++) {
				gen_vnode(gen_vnode, rnodes[r], root_vidx);
			}
		}
		ERR_FAIL_COND_V(root_vidx < 0, Ref<PackedScene>());

		if (p_state->get_scene_name().is_empty()) {
			GLTFFastAccess::scene_name(p_state.ptr()) = vnodes[uint32_t(root_vidx)].name;
		} else if (vnodes[uint32_t(root_vidx)].name.is_empty()) {
			vnodes[uint32_t(root_vidx)].name = GLTFFastAccess::gen_unique_name(p_doc, p_state, p_state->get_scene_name());
		}
		if (vnodes[uint32_t(root_vidx)].name.is_empty()) {
			vnodes[uint32_t(root_vidx)].name = "Node3D";
		}

		// Reparent skinned MeshInstance3D nodes directly under their Skeleton3D (_process_mesh_instances).
		for (int node_i = 0; node_i < nodes.size(); node_i++) {
			Ref<GLTFNode> gn = nodes[node_i];
			const int skin_i = GLTFFastAccess::node_skin(gn.ptr());
			if (skin_i >= 0 && GLTFFastAccess::node_mesh(gn.ptr()) >= 0) {
				ERR_FAIL_INDEX_V(skin_i, skins.size(), Ref<PackedScene>());
				const int skel_i = GLTFFastAccess::skin_skeleton(skins[skin_i].ptr());
				ERR_FAIL_INDEX_V(skel_i, skeletons.size(), Ref<PackedScene>());
				const int sk_v = skel_vidx[uint32_t(skel_i)];
				const int mi_v = rmi_vidx[uint32_t(node_i)];
				ERR_FAIL_COND_V(mi_v < 0 || !vnodes[uint32_t(mi_v)].children.is_empty(), Ref<PackedScene>());
				if (vnodes[uint32_t(mi_v)].parent_vidx != sk_v) {
					detach_vchild(mi_v);
					attach_vchild(sk_v, mi_v);
				}
				vnodes[uint32_t(mi_v)].skin_idx = skin_i;
				vnodes[uint32_t(mi_v)].is_skinned_mesh = true;
				vnodes[uint32_t(mi_v)].use_node_xform = false;
			}
		}

		// Emit vnodes in topological pre-order into SceneState.
		const int type_skeleton3d = intern_name(SNAME("Skeleton3D"));
		const int type_bone_attach = intern_name(SNAME("BoneAttachment3D"));
		const int prop_skin = intern_name(SNAME("skin"));
		const int prop_skeleton = intern_name(SNAME("skeleton"));
		const int prop_bone_name = intern_name(SNAME("bone_name"));
		const int prop_bone_idx = intern_name(SNAME("bone_idx"));
		const int val_parent_np = intern_shared_val(NodePath(".."));

		LocalVector<Vector<StringName>> vnode_paths;
		vnode_paths.resize(vnodes.size());

		struct EmitItem {
			int vidx = -1;
			int parent_ss_idx = -1;
		};
		LocalVector<EmitItem> emit_stack;
		emit_stack.push_back(EmitItem{ root_vidx, -1 });

		while (!emit_stack.is_empty()) {
			const EmitItem item = emit_stack[emit_stack.size() - 1];
			emit_stack.resize(emit_stack.size() - 1);
			const VirtNode &vn = vnodes[uint32_t(item.vidx)];

			int ntype = type_node3d;
			if (vn.kind == VKIND_MESH) {
				ntype = type_mesh_instance;
			} else if (vn.kind == VKIND_SKELETON) {
				ntype = type_skeleton3d;
			} else if (vn.kind == VKIND_BONE_ATTACHMENT) {
				ntype = type_bone_attach;
			}

			const StringName sname(vn.name);
			const int owner_ss = (item.parent_ss_idx < 0) ? -1 : 0;
			const int ss_idx = ss->add_node(item.parent_ss_idx, owner_ss, ntype, intern_name(sname), -1, -1, next_unique_id++);

			if (item.parent_ss_idx < 0) {
				Vector<StringName> dot_p;
				dot_p.push_back(SNAME("."));
				vnode_paths[uint32_t(item.vidx)] = dot_p;
			} else {
				Vector<StringName> p = vnode_paths[uint32_t(vn.parent_vidx)];
				if (p.size() == 1 && p[0] == SNAME(".")) {
					p.clear();
				}
				p.push_back(sname);
				vnode_paths[uint32_t(item.vidx)] = p;
			}

			if (vn.kind == VKIND_SKELETON) {
				const DirectSkelInfo &dskel = direct_skels[uint32_t(vn.skel_idx)];
				const uint32_t bcnt = dskel.bones.size();
				for (uint32_t b = 0; b < bcnt; b++) {
					const DirectBoneInfo &bi = dskel.bones[b];
					const String prefix = String("bones/") + itos(b) + "/";
					ss->add_node_property(ss_idx, intern_name(StringName(prefix + "name")), ss->add_value(bi.name));
					if (bi.parent >= 0) {
						ss->add_node_property(ss_idx, intern_name(StringName(prefix + "parent")), ss->add_value(bi.parent));
					}
					if (bi.rest != Transform3D()) {
						ss->add_node_property(ss_idx, intern_name(StringName(prefix + "rest")), ss->add_value(bi.rest));
					}
					if (bi.pos != Vector3()) {
						ss->add_node_property(ss_idx, intern_name(StringName(prefix + "position")), ss->add_value(bi.pos));
					}
					if (bi.rot != Quaternion()) {
						ss->add_node_property(ss_idx, intern_name(StringName(prefix + "rotation")), ss->add_value(bi.rot));
					}
					if (!bi.scale.is_equal_approx(Vector3(1.0f, 1.0f, 1.0f))) {
						ss->add_node_property(ss_idx, intern_name(StringName(prefix + "scale")), ss->add_value(bi.scale));
					}
					if (bi.has_extras) {
						ss->add_node_property(ss_idx, intern_name(StringName(prefix + "bone_meta/extras")), ss->add_value(bi.extras));
					}
				}
			} else if (vn.kind == VKIND_BONE_ATTACHMENT) {
				ss->add_node_property(ss_idx, prop_bone_name, ss->add_value(StringName(vn.bone_name)));
				if (vn.bone_attach_idx >= 0) {
					ss->add_node_property(ss_idx, prop_bone_idx, ss->add_value(vn.bone_attach_idx));
				}
			}

			if (vn.gltf_node_idx >= 0) {
				Ref<GLTFNode> gn = nodes[vn.gltf_node_idx];
				if (vn.use_node_xform) {
					const Transform3D &xf = GLTFFastAccess::node_xform(gn.ptr());
					if (xf != Transform3D()) {
						ss->add_node_property(ss_idx, prop_transform, ss->add_value(xf));
					}
				}
				if (vn.use_node_vis && !GLTFFastAccess::node_visible(gn.ptr())) {
					ss->add_node_property(ss_idx, prop_visible, val_false);
				}
				if (vn.kind == VKIND_MESH) {
					emit_mesh_and_blend_props(ss_idx, gn);
					if (vn.is_skinned_mesh && vn.skin_idx >= 0) {
						Ref<Skin> gskin = GLTFFastAccess::godot_skin(skins.write[vn.skin_idx].ptr());
						if (gskin.is_valid()) {
							ss->add_node_property(ss_idx, prop_skin, intern_shared_val(gskin));
						}
						ss->add_node_property(ss_idx, prop_skeleton, val_parent_np);
					}
				}
			}
			if (vn.merge_meta_node >= 0) {
				emit_node_meta(ss_idx, nodes[vn.merge_meta_node]);
			}

			for (int c = int(vn.children.size()) - 1; c >= 0; c--) {
				emit_stack.push_back(EmitItem{ vn.children[uint32_t(c)], ss_idx });
			}
		}

		if (p_state->get_create_animations() && !anims.is_empty()) {
			anim_ctx.has_vnode_paths = true;
			anim_ctx.skel_paths.resize(uint32_t(skeletons.size()));
			for (int s_i = 0; s_i < skeletons.size(); s_i++) {
				const int sv = skel_vidx[uint32_t(s_i)];
				if (sv >= 0 && uint32_t(sv) < vnode_paths.size()) {
					anim_ctx.skel_paths[uint32_t(s_i)] = vnode_paths[uint32_t(sv)];
				}
			}
			anim_ctx.scene_paths.resize(uint32_t(nodes.size()));
			anim_ctx.rmi_paths.resize(uint32_t(nodes.size()));
			for (int ni = 0; ni < nodes.size(); ni++) {
				const int sv = scene_vidx[uint32_t(ni)];
				if (sv >= 0 && uint32_t(sv) < vnode_paths.size()) {
					anim_ctx.scene_paths[uint32_t(ni)] = vnode_paths[uint32_t(sv)];
				}
				const int rv = rmi_vidx[uint32_t(ni)];
				if (rv >= 0 && uint32_t(rv) < vnode_paths.size()) {
					anim_ctx.rmi_paths[uint32_t(ni)] = vnode_paths[uint32_t(rv)];
				}
			}
		}
	}

	if (p_state->get_create_animations() && !anims.is_empty()) {
		if (p_remove_immutable_tracks) {
			build_node_base_trs(nodes, anim_ctx.base_trs);
		}
		for (int a_i = 0; a_i < anims.size(); a_i++) {
			if (anims[a_i].is_valid() && anims[a_i]->get_name().is_empty()) {
				anims.write[a_i]->set_name(GLTFFastAccess::gen_unique_name(p_doc, p_state, "Animation"));
			}
		}
		LocalVector<Ref<Animation>> built_anims;
		built_anims.resize(uint32_t(anims.size()));
		if (anims.size() >= 2 && LoadThreadPool::get_singleton()) {
			LoadThreadPool::get_singleton()->parallel_for(uint32_t(anims.size()), [&](uint32_t a_i) {
				built_anims[a_i] = build_animation_fast_impl(
						p_doc, p_state, nullptr, int(a_i), p_trimming, p_remove_immutable_tracks, single_root_gltf_idx, &anim_ctx);
			});
		} else {
			for (int a_i = 0; a_i < anims.size(); a_i++) {
				built_anims[uint32_t(a_i)] = build_animation_fast_impl(
						p_doc, p_state, nullptr, a_i, p_trimming, p_remove_immutable_tracks, single_root_gltf_idx, &anim_ctx);
			}
		}

		Ref<AnimationLibrary> lib;
		lib.instantiate();
		for (uint32_t a_i = 0; a_i < built_anims.size(); a_i++) {
			if (built_anims[a_i].is_valid()) {
				lib->add_animation(built_anims[a_i]->get_name(), built_anims[a_i]);
			}
		}
		const String ap_name = GLTFFastAccess::unique_names(p_state.ptr()).has("AnimationPlayer")
				? GLTFFastAccess::gen_unique_name(p_doc, p_state, "AnimationPlayer")
				: String("AnimationPlayer");
		const int ap_ss_idx = ss->add_node(0, 0, intern_name(SNAME("AnimationPlayer")), intern_name(StringName(ap_name)), -1, -1, next_unique_id++);
		ss->add_node_property(ap_ss_idx, intern_name(SNAME("libraries/")), intern_shared_val(lib));
	}

	Ref<PackedScene> packed;
	packed.instantiate();
	packed->replace_state(ss);
	return packed;
}

Ref<Resource> ResourceFormatLoaderGLB::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	if (r_error) {
		*r_error = ERR_FILE_CANT_OPEN;
	}
	Ref<GLTFDocument> doc;
	doc.instantiate();
	Ref<GLTFState> state;
	state.instantiate();
	state->set_handle_binary_image_mode(GLTFState::HANDLE_BINARY_IMAGE_MODE_EMBED_AS_UNCOMPRESSED);
	state->set_additional_data(SNAME("_ange_want_packed_scene"), true);
	state->set_additional_data(SNAME("_ange_strict_external_ktx2"), true);

	Error err = doc->append_from_file(p_path, state);
	if (err != OK) {
		if (r_error) {
			*r_error = err;
		}
		return Ref<Resource>();
	}

	Ref<PackedScene> packed = state->get_additional_data(SNAME("_ange_packed_scene"));
	if (packed.is_null()) {
		packed = gltf_build_packed_scene_direct(doc.ptr(), state, false);
	}
	if (r_error) {
		*r_error = packed.is_valid() ? OK : ERR_CANT_CREATE;
	}
	return packed;
}

void ResourceFormatLoaderGLB::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("glb");
}

bool ResourceFormatLoaderGLB::handles_type(const String &p_type) const {
	return p_type == "PackedScene" || p_type == "Resource";
}

String ResourceFormatLoaderGLB::get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "glb") {
		return "PackedScene";
	}
	return "";
}
