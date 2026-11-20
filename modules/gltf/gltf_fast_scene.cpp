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
#include "structures/gltf_animation.h"
#include "structures/gltf_mesh.h"
#include "structures/gltf_node.h"
#include "structures/gltf_skeleton.h"

#include "scene/3d/importer_mesh_instance_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/animation/animation_player.h"
#include "scene/resources/animation_library.h"

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
					dst[s] = vptr[idx] + (vptr[idx + 1] - vptr[idx]) * steps[s].frac;
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

	auto safe_slerp = [](const Quaternion &p_a, const Quaternion &p_b, float p_c) -> Quaternion {
		const Quaternion na = p_a.is_normalized() ? p_a : p_a.normalized();
		const Quaternion nb = p_b.is_normalized() ? p_b : p_b.normalized();
		return na.slerp(nb, p_c).normalized();
	};

	switch (p_interp) {
		case GLTFAnimation::INTERP_LINEAR: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					dst[s] = vptr[0].is_normalized() ? vptr[0] : vptr[0].normalized();
				} else if (idx >= tlen - 1) {
					const Quaternion &q = vptr[tlen - 1];
					dst[s] = q.is_normalized() ? q : q.normalized();
				} else {
					dst[s] = safe_slerp(vptr[idx], vptr[idx + 1], steps[s].frac);
				}
			}
		} break;
		case GLTFAnimation::INTERP_STEP: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				const Quaternion &q = (idx < 0) ? vptr[0] : ((idx >= tlen - 1) ? vptr[tlen - 1] : vptr[idx]);
				dst[s] = q.is_normalized() ? q : q.normalized();
			}
		} break;
		case GLTFAnimation::INTERP_CATMULLROMSPLINE: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					const Quaternion &q = vlen > 1 ? vptr[1] : vptr[0];
					dst[s] = q.is_normalized() ? q : q.normalized();
				} else if (idx >= tlen - 1) {
					const Quaternion &q = (1 + tlen - 1 < vlen) ? vptr[1 + tlen - 1] : vptr[vlen - 1];
					dst[s] = q.is_normalized() ? q : q.normalized();
				} else {
					dst[s] = safe_slerp(vptr[idx], vptr[idx + 1], steps[s].frac);
				}
			}
		} break;
		case GLTFAnimation::INTERP_CUBIC_SPLINE: {
			for (uint32_t s = 0; s < scount; s++) {
				const int32_t idx = steps[s].idx;
				if (idx < 0) {
					const Quaternion &q = vptr[1];
					dst[s] = q.is_normalized() ? q : q.normalized();
				} else if (idx >= tlen - 1) {
					const Quaternion &q = vptr[(tlen - 1) * 3 + 1];
					dst[s] = q.is_normalized() ? q : q.normalized();
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

} // namespace

Ref<Animation> gltf_build_animation_fast(
		GLTFDocument *p_doc,
		Ref<GLTFState> p_state,
		Node *p_scene_root,
		GLTFAnimationIndex p_index,
		bool p_trimming,
		bool p_remove_immutable_tracks,
		int p_single_root_node_idx) {
	ERR_FAIL_COND_V(p_state.is_null(), Ref<Animation>());
	Vector<Ref<GLTFAnimation>> &anims = GLTFFastAccess::animations(p_state.ptr());
	ERR_FAIL_INDEX_V(p_index, anims.size(), Ref<Animation>());
	Ref<GLTFAnimation> anim = anims[p_index];
	ERR_FAIL_COND_V(anim.is_null(), Ref<Animation>());

	String anim_name = anim->get_name();
	if (anim_name.is_empty()) {
		anim_name = GLTFFastAccess::gen_unique_name(p_doc, p_state, "Animation");
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
				// Fast endpoint check + interior safety.
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

	// CacheSkeleton NodePath strings so 68 bones don't call scene_root->get_path_to(sk) 68 times!
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
			if (track.position_track.values.size() && track.position_track.times.size()) {
				bool is_default = true;
				if (p_remove_immutable_tracks) {
					const Vector3 base_pos = gltf_node->get_position();
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
					const Quaternion base_rot = gltf_node->get_rotation();
					const Quaternion *vp = track.rotation_track.values.ptr();
					const int vcnt = track.rotation_track.values.size();
					const int tcnt = track.rotation_track.times.size();
					const bool cubic = track.rotation_track.interpolation == GLTFAnimation::INTERP_CUBIC_SPLINE;
					for (int i = 0; i < tcnt; i++) {
						const int vi = cubic ? (1 + i * 3) : i;
						ERR_BREAK(vi >= vcnt);
						const Quaternion val = vp[vi].is_normalized() ? vp[vi] : vp[vi].normalized();
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
					const Vector3 base_scale = gltf_node->get_scale();
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

			String bs_name;
			if (gmesh->get_runtime_mesh().is_valid()) {
				ERR_CONTINUE(i >= gmesh->get_runtime_mesh()->get_blend_shape_count());
				bs_name = String(gmesh->get_runtime_mesh()->get_blend_shape_name(i));
			} else {
				ERR_CONTINUE(gmesh->get_mesh().is_null() || i >= gmesh->get_mesh()->get_blend_shape_count());
				bs_name = gmesh->get_mesh()->get_blend_shape_name(i);
			}

			const String blend_path = String(mesh_instance_node_path) + ":" + bs_name;
			const int track_idx = animation->get_track_count();
			animation->add_track(Animation::TYPE_BLEND_SHAPE);
			animation->track_set_path(track_idx, blend_path);
			animation->track_set_imported(track_idx, true);

			const GLTFAnimation::Channel<real_t> &wch = track.weight_tracks[i];
			if (wch.interpolation == GLTFAnimation::INTERP_LINEAR || wch.interpolation == GLTFAnimation::INTERP_STEP) {
				animation->track_set_interpolation_type(
						track_idx,
						wch.interpolation == GLTFAnimation::INTERP_STEP ? Animation::INTERPOLATION_NEAREST : Animation::INTERPOLATION_LINEAR);
				const int kcnt = MIN(wch.times.size(), wch.values.size());
				if (kcnt > 0) {
					// Check if times are strictly sorted for O(N) bulk insert; otherwise fall back to per-key insert.
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
	Ref<Animation> animation = gltf_build_animation_fast(p_doc, p_state, scene_root, p_index, p_trimming, p_remove_immutable_tracks, -1);
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
			GLTFFastAccess::skeletons(p_state.ptr()).is_empty() &&
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

	// Zero-Node* direct SceneState builder.
	Ref<SceneState> ss;
	ss.instantiate();
	HashMap<StringName, int> name_map;
	HashMap<Variant, int> variant_map;

	auto intern_name = [&](const StringName &p_sname) -> int {
		HashMap<StringName, int>::Iterator E = name_map.find(p_sname);
		if (E) {
			return E->value;
		}
		const int idx = ss->add_name(p_sname);
		name_map.insert(p_sname, idx);
		return idx;
	};

	auto intern_val = [&](const Variant &p_val) -> int {
		HashMap<Variant, int>::Iterator E = variant_map.find(p_val);
		if (E) {
			return E->value;
		}
		const int idx = ss->add_value(p_val);
		variant_map.insert(p_val, idx);
		return idx;
	};

	const int type_node3d = intern_name(SNAME("Node3D"));
	const int type_mesh_instance = intern_name(SNAME("MeshInstance3D"));
	const int prop_transform = intern_name(SNAME("transform"));
	const int prop_visible = intern_name(SNAME("visible"));
	const int prop_mesh = intern_name(SNAME("mesh"));
	int32_t next_unique_id = 1;

	auto emit_gltf_node_props = [&](int p_ss_node_idx, const Ref<GLTFNode> &p_gnode) {
		const Transform3D &xf = GLTFFastAccess::node_xform(p_gnode.ptr());
		if (xf != Transform3D()) {
			ss->add_node_property(p_ss_node_idx, prop_transform, intern_val(xf));
		}
		if (!GLTFFastAccess::node_visible(p_gnode.ptr())) {
			ss->add_node_property(p_ss_node_idx, prop_visible, intern_val(false));
		}
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
				ss->add_node_property(p_ss_node_idx, prop_mesh, intern_val(mres));
				Vector<float> weights = gmesh->get_blend_weights();
				if (const_cast<GLTFNode *>(p_gnode.ptr())->has_additional_data(SNAME("_gltf_node_weights"))) {
					Vector<float> nw = const_cast<GLTFNode *>(p_gnode.ptr())->get_additional_data(SNAME("_gltf_node_weights"));
					for (int w = 0; w < MIN(nw.size(), weights.size()); w++) {
						weights.write[w] = nw[w];
					}
				}
				const int bs_cnt = MIN(weights.size(), mres->get_blend_shape_count());
				for (int b = 0; b < bs_cnt; b++) {
					if (weights[b] != 0.0f) {
						const StringName bprop(String("blend_shapes/") + String(mres->get_blend_shape_name(b)));
						ss->add_node_property(p_ss_node_idx, intern_name(bprop), intern_val(weights[b]));
					}
				}
			}
		}
		List<StringName> meta_list;
		p_gnode->get_meta_list(&meta_list);
		for (const StringName &mk : meta_list) {
			const StringName mprop(String("metadata/") + String(mk));
			ss->add_node_property(p_ss_node_idx, intern_name(mprop), intern_val(p_gnode->get_meta(mk)));
		}
	};

	struct StackEntry {
		GLTFNodeIndex gltf_idx = -1;
		int parent_ss_idx = -1;
	};
	LocalVector<StackEntry> stack;

	const bool use_single_root = GLTFFastAccess::extensions_used(p_state.ptr()).has("GODOT_single_root");
	int single_root_gltf_idx = -1;

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
		emit_gltf_node_props(root_ss_idx, root_gnode);
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
		emit_gltf_node_props(ss_idx, gnode);

		const Vector<int> &children = GLTFFastAccess::node_children(gnode.ptr());
		for (int c = children.size() - 1; c >= 0; c--) {
			stack.push_back(StackEntry{ children[c], ss_idx });
		}
	}

	if (p_state->get_create_animations() && !anims.is_empty()) {
		Ref<AnimationLibrary> lib;
		lib.instantiate();
		for (int a_i = 0; a_i < anims.size(); a_i++) {
			Ref<Animation> godot_anim = gltf_build_animation_fast(
					p_doc, p_state, nullptr, a_i, p_trimming, p_remove_immutable_tracks, single_root_gltf_idx);
			if (godot_anim.is_valid()) {
				lib->add_animation(godot_anim->get_name(), godot_anim);
			}
		}
		const String ap_name = GLTFFastAccess::unique_names(p_state.ptr()).has("AnimationPlayer")
				? GLTFFastAccess::gen_unique_name(p_doc, p_state, "AnimationPlayer")
				: String("AnimationPlayer");
		const int ap_ss_idx = ss->add_node(0, 0, intern_name(SNAME("AnimationPlayer")), intern_name(StringName(ap_name)), -1, -1, next_unique_id++);
		ss->add_node_property(ap_ss_idx, intern_name(SNAME("libraries/")), intern_val(lib));
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
