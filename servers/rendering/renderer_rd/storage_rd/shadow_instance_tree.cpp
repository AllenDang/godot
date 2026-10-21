#include "shadow_instance_tree.h"

#include "core/io/json.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include <cmath> // hierarchical LOD hook

// BEGIN hierarchical LOD environment
bool ShadowInstanceTree::lod_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("ANGE_SHADOW_HIERARCHICAL_LOD") == "1";
	return enabled;
}
float ShadowInstanceTree::lod_basis_scale(const Basis &p_basis) {
	double maximum = 0;
	for (int row = 0; row < 3; row++) {
		double sum = 0;
		for (int column = 0; column < 3; column++) {
			double dot = 0;
			for (int axis = 0; axis < 3; axis++) {
				dot += double(p_basis[axis][row]) * double(p_basis[axis][column]);
			}
			sum += Math::abs(dot);
		}
		maximum = MAX(maximum, sum);
	}
	return maximum == 0 ? 0 : std::nextafter(float(std::sqrt(maximum)), INFINITY);
}
// END hierarchical LOD environment

namespace {
// BEGIN in-place environment
bool in_place_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("ANGE_SHADOW_CULL_IN_PLACE") == "1";
	return enabled;
}
// END in-place environment
bool async_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("ANGE_SHADOW_CULL_ASYNC") == "1";
	return enabled;
}
bool trace_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("ANGE_SHADOW_TREE_TRACE") == "1";
	return enabled;
}
uint32_t minimum_instances() {
	static const uint32_t count = trace_enabled() && OS::get_singleton()->has_environment("ANGE_SHADOW_TREE_MIN_INSTANCES") ?
			MAX(1, OS::get_singleton()->get_environment("ANGE_SHADOW_TREE_MIN_INSTANCES").to_int()) : 1024;
	return count;
}
void trace_event(const char *p_event, const void *p_tree, uint64_t p_revision, uint64_t p_job_revision, uint32_t p_count, uint64_t p_usec = 0, bool p_worker = false, uint64_t p_owner_usec = 0) {
	if (!trace_enabled()) {
		return;
	}
	Dictionary row;
	row["event"] = p_event;
	row["tree"] = int64_t(uintptr_t(p_tree));
	row["revision"] = int64_t(p_revision);
	row["job_revision"] = int64_t(p_job_revision);
	row["instances"] = p_count;
	row["usec"] = int64_t(p_usec);
	row["worker_thread"] = p_worker;
	row["owner_usec"] = int64_t(p_owner_usec);
	print_line("SHADOW_TREE_ASYNC " + JSON::stringify(row));
}
} // namespace

// Only the owner thread touches task_id and reaps the shared job. The callback
// uses only its private tree. It cannot retain this job through that tree.
struct ShadowInstanceTree::BuildJob {
	ShadowInstanceTree tree;
	WorkerThreadPool::TaskID task_id = WorkerThreadPool::INVALID_TASK_ID;
	Thread::ID caller_thread;
	const void *owner_key = nullptr;
	uint64_t build_usec = 0;
	bool on_worker = false;
	bool pending_traced = false;
	uint32_t delay_msec = 0;

	explicit BuildJob(const ShadowInstanceTree &p_source) {
		owner_key = &p_source;
		tree.local_bounds = p_source.local_bounds;
		tree.transforms = p_source.transforms;
		tree.snapshot = p_source.snapshot;
		tree.revision = p_source.revision;
		// BEGIN density snapshot
		tree.density = p_source.density;
		tree.density_seed = p_source.density_seed;
		tree.density_source_count = p_source.density_source_count;
		tree.density_external = p_source.density_external;
		tree.density_membership = p_source.density_membership;
		tree.density_ready = p_source.density_ready;
		// END density snapshot
		caller_thread = Thread::get_caller_id();
		if (trace_enabled()) {
			delay_msec = CLAMP(OS::get_singleton()->get_environment("ANGE_SHADOW_TREE_DELAY_MSEC").to_int(), 0, 1000);
		}
	}
	static void run(void *p_data) {
		BuildJob *job = static_cast<BuildJob *>(p_data);
		job->on_worker = Thread::get_caller_id() != job->caller_thread;
		if (job->delay_msec) {
			OS::get_singleton()->delay_usec(job->delay_msec * 1000);
		}
		uint64_t started = OS::get_singleton()->get_ticks_usec();
		job->tree.rebuild_sync();
		job->build_usec = OS::get_singleton()->get_ticks_usec() - started;
	}
	bool complete() const {
		return task_id == WorkerThreadPool::INVALID_TASK_ID || WorkerThreadPool::get_singleton()->is_task_completed(task_id);
	}
	void reap() {
		if (task_id != WorkerThreadPool::INVALID_TASK_ID) {
			const Error error = WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
			CRASH_COND_MSG(error != OK, "Cannot release a live shadow hierarchy job.");
			task_id = WorkerThreadPool::INVALID_TASK_ID;
		}
	}
	~BuildJob() {
		const uint64_t started = OS::get_singleton()->get_ticks_usec();
		reap();
		trace_event("reaped", owner_key, tree.revision, tree.revision, tree.transforms.size(), OS::get_singleton()->get_ticks_usec() - started, on_worker);
	}
};

void ShadowInstanceTree::rebuild() {
	// Only the frame preparation call may publish a density selection. Ordinary
	// shadow/LOD queries later in the frame observe that same published snapshot.
	if (density != 1 && !density_preparing) { return; }
	if (async_job) {
		valid = false;
		if (!async_job->complete()) {
			if (!async_job->pending_traced) {
				trace_event("pending", this, revision, async_job->tree.revision, transforms.size());
				async_job->pending_traced = true;
			}
			return;
		}
		const uint64_t owner_started = trace_enabled() ? OS::get_singleton()->get_ticks_usec() : 0;
		async_job->reap();
		const ShadowInstanceTree &built = async_job->tree;
		const bool mutant = trace_enabled() && OS::get_singleton()->get_environment("ANGE_SHADOW_TREE_STALE_MUTANT") == "1";
		if (built.revision == revision || mutant) {
			// BEGIN density snapshot publication
			density_ids = built.density_ids;
			density_membership = built.density_membership;
			density_ready = built.density_ready;
			// END density snapshot publication
			boxes = built.boxes;
			order = built.order;
			nodes = built.nodes;
			lod_scales = built.lod_scales; // hierarchical LOD hook
			update_parents = built.update_parents; // in-place hook
			update_leaves = built.update_leaves; // in-place hook
			valid = built.valid;
			dirty = false;
			trace_event("published", this, revision, built.revision, transforms.size(), async_job->build_usec, async_job->on_worker, owner_started ? OS::get_singleton()->get_ticks_usec() - owner_started : 0);
		} else {
			trace_event("discarded", this, revision, built.revision, transforms.size(), async_job->build_usec, async_job->on_worker, owner_started ? OS::get_singleton()->get_ticks_usec() - owner_started : 0);
		}
		async_job.reset();
	}
	if (!dirty) {
		return;
	}
	if (async_enabled() && WorkerThreadPool::get_singleton() && ((requested() && snapshot && uint32_t(transforms.size()) >= minimum_instances()) || (density != 1 && density_source_count >= minimum_instances()))) {
		valid = false;
		const uint64_t started = OS::get_singleton()->get_ticks_usec();
		async_job = std::make_shared<BuildJob>(*this);
		async_job->task_id = WorkerThreadPool::get_singleton()->add_native_task(&BuildJob::run, async_job.get(), false, "Shadow instance hierarchy");
		trace_event("scheduled", this, revision, revision, transforms.size(), OS::get_singleton()->get_ticks_usec() - started);
		return;
	}
	const uint64_t started = trace_enabled() ? OS::get_singleton()->get_ticks_usec() : 0;
	rebuild_sync();
	trace_event("sync", this, revision, revision, transforms.size(), started ? OS::get_singleton()->get_ticks_usec() - started : 0);
}

bool ShadowInstanceTree::get_conservative_aggregate(AABB &r_bounds) {
	if (!async_enabled() && density == 1) {
		if (!active()) {
			return false;
		}
		r_bounds = get_aggregate();
		return true;
	}
	if (trace_enabled() && OS::get_singleton()->get_environment("ANGE_SHADOW_TREE_BOUNDS_MUTANT") == "1") {
		return false;
	}
	if (aggregate_revision == revision) {
		r_bounds = conservative_aggregate;
		return aggregate_valid;
	}
	const uint64_t started = trace_enabled() ? OS::get_singleton()->get_ticks_usec() : 0;
	aggregate_revision = revision;
	conservative_aggregate = AABB();
	aggregate_valid = false;
	if (!requested() || !snapshot || transforms.is_empty()) {
		return false;
	}
	// Broadphase cannot depend on background completion. Use current transforms
	// and full deformation bounds; sorting is unnecessary for their union.
	for (int i = 0; i < transforms.size(); i++) {
		const AABB box = transforms[i].xform(local_bounds);
		if (!transforms[i].is_finite() || !box.is_finite()) {
			return false;
		}
		if (i == 0) {
			conservative_aggregate = box;
		} else {
			conservative_aggregate.merge_with(box);
		}
	}
	aggregate_valid = true;
	r_bounds = conservative_aggregate;
	trace_event("aggregate", this, revision, revision, transforms.size(), started ? OS::get_singleton()->get_ticks_usec() - started : 0);
	return true;
}

// BEGIN in-place implementation
void ShadowInstanceTree::build_update_maps() {
	if (!in_place_enabled()) {
		return;
	}
	update_parents.resize(nodes.size());
	update_parents.fill(-1);
	update_leaves.resize(transforms.size());
	for (int i = 0; i < nodes.size(); i++) {
		const Node &node = nodes[i];
		if (node.left < 0) {
			for (uint32_t slot = node.from; slot < node.from + node.count; slot++) {
				update_leaves.write[order[slot]] = i;
			}
		} else {
			update_parents.write[node.left] = i;
			update_parents.write[node.right] = i;
		}
	}
}

bool ShadowInstanceTree::update_in_place(uint32_t p_index, const Transform3D &p_transform) {
	if (!in_place_enabled()) {
		return false;
	}
	// A pending worker owns a different snapshot. Never refit that topology or
	// publish old instance-to-leaf mappings after a structural change.
	if (!valid || dirty || async_job || update_leaves.size() != transforms.size() || update_parents.size() != nodes.size() ||
			transforms[p_index].origin != p_transform.origin || !p_transform.is_finite()) {
		trace_event("in_place_fallback", this, revision, revision, transforms.size());
		return false;
	}
	const AABB box = p_transform.xform(local_bounds);
	if (!box.is_finite()) {
		trace_event("in_place_fallback", this, revision, revision, transforms.size());
		return false;
	}
	const uint64_t started = trace_enabled() ? OS::get_singleton()->get_ticks_usec() : 0;
	int leaf = update_leaves[p_index];
	if (trace_enabled() && OS::get_singleton()->get_environment("ANGE_SHADOW_TREE_MAP_MUTANT") == "1") {
		leaf = update_leaves[(p_index + transforms.size() / 2) % transforms.size()];
	}
	revision++;
	transforms.write[p_index] = p_transform;
	boxes.write[p_index] = box;
	if (lod_enabled()) { lod_scales.write[p_index] = lod_basis_scale(p_transform.basis); } // hierarchical LOD hook
	const Node node = nodes[leaf];
	AABB leaf_bounds = boxes[order[node.from]];
	float min_scale = 0, max_scale = 0; if (lod_enabled()) { min_scale = max_scale = lod_scales[order[node.from]]; } // hierarchical LOD hook
	for (uint32_t slot = node.from + 1; slot < node.from + node.count; slot++) {
		leaf_bounds.merge_with(boxes[order[slot]]);
		if (lod_enabled()) { min_scale = MIN(min_scale, lod_scales[order[slot]]); max_scale = MAX(max_scale, lod_scales[order[slot]]); } // hierarchical LOD hook
	}
	nodes.write[leaf].bounds = leaf_bounds;
	if (lod_enabled()) { nodes.write[leaf].min_lod_scale = min_scale; nodes.write[leaf].max_lod_scale = max_scale; } // hierarchical LOD hook
	const bool mutant = trace_enabled() && OS::get_singleton()->get_environment("ANGE_SHADOW_TREE_PARENT_MUTANT") == "1";
	if (!mutant) {
		for (int parent = update_parents[leaf]; parent >= 0; parent = update_parents[parent]) {
			const Node ancestor = nodes[parent];
			const AABB combined = nodes[ancestor.left].bounds.merge(nodes[ancestor.right].bounds);
			nodes.write[parent].bounds = combined;
			if (lod_enabled()) { nodes.write[parent].min_lod_scale = MIN(nodes[ancestor.left].min_lod_scale, nodes[ancestor.right].min_lod_scale); nodes.write[parent].max_lod_scale = MAX(nodes[ancestor.left].max_lod_scale, nodes[ancestor.right].max_lod_scale); } // hierarchical LOD hook
		}
	}
	conservative_aggregate = nodes[0].bounds;
	aggregate_revision = revision;
	aggregate_valid = true;
	trace_event("in_place", this, revision, revision, transforms.size(), started ? OS::get_singleton()->get_ticks_usec() - started : 0);
	return true;
}
// END in-place implementation
