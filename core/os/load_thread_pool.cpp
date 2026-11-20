/**************************************************************************/
/*  load_thread_pool.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "load_thread_pool.h"

#include "core/config/engine.h"
#include "core/os/memory.h"
#include "core/os/os.h"
#include "core/string/ustring.h"

#include <cstdlib>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

thread_local bool tl_is_load_pool_worker = false;
std::mutex g_pool_singleton_mutex;
LoadThreadPool *g_load_pool_singleton = nullptr;

int query_env_max_threads() {
	const char *env = std::getenv("ANGE_GLB_MAX_THREADS");
	if (!env || env[0] == '\0') {
		return 0;
	}
	const int val = std::atoi(env);
	return CLAMP(val, 1, LoadThreadPool::MAX_LOAD_THREADS);
}

int query_platform_thermal_state() {
#if defined(__APPLE__)
	// Query NSProcessInfo.processInfo.thermalState via Objective-C runtime:
	// 0 = Nominal, 1 = Fair, 2 = Serious, 3 = Critical.
	Class cls = objc_getClass("NSProcessInfo");
	if (!cls) {
		return 0;
	}
	SEL proc_sel = sel_registerName("processInfo");
	SEL therm_sel = sel_registerName("thermalState");
	id proc_info = ((id(*)(id, SEL))objc_msgSend)((id)cls, proc_sel);
	if (!proc_info) {
		return 0;
	}
	return static_cast<int>(((long (*)(id, SEL))objc_msgSend)(proc_info, therm_sel));
#else
	return 0;
#endif
}

struct ScopedPoolParticipant {
	bool prev;
	ScopedPoolParticipant() :
			prev(tl_is_load_pool_worker) {
		tl_is_load_pool_worker = true;
	}
	~ScopedPoolParticipant() {
		tl_is_load_pool_worker = prev;
	}
};

} // namespace

LoadThreadPool *LoadThreadPool::get_singleton() {
	LoadThreadPool *pool = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_pool_singleton_mutex);
		if (!g_load_pool_singleton) {
			g_load_pool_singleton = memnew(LoadThreadPool);
		}
		pool = g_load_pool_singleton;
	}
	pool->_ensure_initialized();
	return pool;
}

void LoadThreadPool::cleanup() {
	LoadThreadPool *instance = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_pool_singleton_mutex);
		instance = g_load_pool_singleton;
		g_load_pool_singleton = nullptr;
	}
	if (!instance) {
		return;
	}
	{
		std::lock_guard<std::mutex> qlock(instance->queue_mutex);
		instance->shutting_down = true;
	}
	instance->worker_cv.notify_all();
	for (uint32_t i = 0; i < instance->workers.size(); i++) {
		if (instance->workers[i]->thread.is_started()) {
			instance->workers[i]->thread.wait_to_finish();
		}
		memdelete(instance->workers[i]);
	}
	instance->workers.clear();
	memdelete(instance);
}

int LoadThreadPool::get_concurrency_budget(bool p_gameplay_throttle) const {
	static const int env_cap = query_env_max_threads();
	const int cpu_count = OS::get_singleton() ? MAX(1, OS::get_singleton()->get_processor_count()) : 1;
	int budget = MIN(MAX_LOAD_THREADS, cpu_count);
	if (env_cap > 0) {
		budget = MIN(budget, env_cap);
	}
	const int thermal = query_platform_thermal_state();
	if (thermal >= 3) {
		return 1;
	}
	if (thermal == 2) {
		budget = MIN(budget, 2);
	}
	if (p_gameplay_throttle) {
		const Engine *eng = Engine::get_singleton();
		if (eng && !eng->is_editor_hint() && eng->get_process_frames() > 8) {
			budget = MIN(budget, cpu_count <= 4 ? 1 : 2);
		}
	}
	return CLAMP(budget, 1, MAX_LOAD_THREADS);
}

void LoadThreadPool::_ensure_initialized() {
	std::lock_guard<std::mutex> qlock(queue_mutex);
	if (initialized || shutting_down) {
		return;
	}
	const int cpu_count = OS::get_singleton() ? MAX(1, OS::get_singleton()->get_processor_count()) : 1;
	// Calling thread participates as 1 worker, so at most (MAX_LOAD_THREADS - 1) helper threads are needed.
	const int num_workers = CLAMP(cpu_count - 1, 1, MAX_LOAD_THREADS - 1);
	Thread::Settings settings;
	settings.priority = Thread::PRIORITY_LOW;

	workers.reserve(num_workers);
	for (int i = 0; i < num_workers; i++) {
		WorkerSlot *slot = memnew(WorkerSlot);
		slot->pool = this;
		slot->slot_index = i;
		slot->thread.start(_worker_entry, slot, settings);
		workers.push_back(slot);
	}
	initialized = true;
}

void LoadThreadPool::_worker_entry(void *p_userdata) {
	WorkerSlot *slot = static_cast<WorkerSlot *>(p_userdata);
	tl_is_load_pool_worker = true;
	Thread::set_name(vformat("GLBLoadPool-%d", slot->slot_index));
	slot->pool->_worker_loop(slot->slot_index);
}

void LoadThreadPool::_worker_loop(int p_slot_index) {
	uint64_t seen_generation = 0;
	for (;;) {
		TaskCallback cb = nullptr;
		void *ud = nullptr;
		int count = 0;
		{
			std::unique_lock<std::mutex> qlock(queue_mutex);
			worker_cv.wait(qlock, [&]() {
				return shutting_down ||
						(job_generation != seen_generation &&
								p_slot_index < active_worker_limit &&
								next_index.load(std::memory_order_relaxed) < job_count);
			});
			if (shutting_down) {
				return;
			}
			seen_generation = job_generation;
			cb = job_callback;
			ud = job_userdata;
			count = job_count;
			remaining_workers.fetch_add(1, std::memory_order_relaxed);
		}

		for (;;) {
			const int idx = next_index.fetch_add(1, std::memory_order_relaxed);
			if (idx >= count) {
				break;
			}
			cb(ud, idx);
		}

		{
			std::lock_guard<std::mutex> qlock(queue_mutex);
			if (remaining_workers.fetch_sub(1, std::memory_order_acq_rel) == 1 && active_worker_limit == 0) {
				done_cv.notify_one();
			}
		}
	}
}

void LoadThreadPool::parallel_for_raw(int p_count, TaskCallback p_callback, void *p_userdata, bool p_gameplay_throttle) {
	if (p_count <= 0 || !p_callback) {
		return;
	}
	if (tl_is_load_pool_worker) {
		for (int i = 0; i < p_count; i++) {
			p_callback(p_userdata, i);
		}
		return;
	}

	const int budget = get_concurrency_budget(p_gameplay_throttle);
	if (budget <= 1 || p_count == 1) {
		for (int i = 0; i < p_count; i++) {
			p_callback(p_userdata, i);
		}
		return;
	}

	std::lock_guard<std::mutex> dlock(dispatch_mutex);
	_ensure_initialized();
	const int avail = static_cast<int>(workers.size());
	const int total_threads = CLAMP(MIN(p_count, budget), 1, MAX_LOAD_THREADS);
	const int helpers = CLAMP(total_threads - 1, 0, avail);
	if (helpers <= 0) {
		for (int i = 0; i < p_count; i++) {
			p_callback(p_userdata, i);
		}
		return;
	}

	{
		std::lock_guard<std::mutex> qlock(queue_mutex);
		job_callback = p_callback;
		job_userdata = p_userdata;
		job_count = p_count;
		active_worker_limit = helpers;
		next_index.store(0, std::memory_order_relaxed);
		remaining_workers.store(0, std::memory_order_relaxed);
		job_generation++;
	}
	worker_cv.notify_all();

	{
		ScopedPoolParticipant participant_guard;
		for (;;) {
			const int idx = next_index.fetch_add(1, std::memory_order_relaxed);
			if (idx >= p_count) {
				break;
			}
			p_callback(p_userdata, idx);
		}
	}

	{
		std::unique_lock<std::mutex> qlock(queue_mutex);
		active_worker_limit = 0;
		done_cv.wait(qlock, [&]() {
			return remaining_workers.load(std::memory_order_acquire) == 0;
		});
		job_callback = nullptr;
		job_userdata = nullptr;
		job_count = 0;
	}
}
