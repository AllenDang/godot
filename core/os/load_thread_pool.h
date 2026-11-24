/**************************************************************************/
/*  load_thread_pool.h                                                    */
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

#include "core/os/condition_variable.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"

#include <atomic>
#include <type_traits>

// Global low-priority asset loading pool strictly capped at <= 3 worker threads.
// All worker threads run at Thread::PRIORITY_LOW (QOS_CLASS_UTILITY on Apple
// E-cores, nice +10 on Linux/Android, BELOW_NORMAL on Windows) to prevent
// P-core thermal throttling and frame-time spikes on mobile devices.
class LoadThreadPool {
public:
	static constexpr int MAX_LOAD_THREADS = 3;
	using TaskCallback = void (*)(void *p_userdata, int p_index);

private:
	struct WorkerSlot {
		LoadThreadPool *pool = nullptr;
		int slot_index = 0;
		Thread thread;
	};

	BinaryMutex dispatch_mutex;
	BinaryMutex queue_mutex;
	ConditionVariable worker_cv;
	ConditionVariable done_cv;

	LocalVector<WorkerSlot *> workers;
	bool initialized = false;
	bool shutting_down = false;

	uint64_t job_generation = 0;
	int job_count = 0;
	int active_worker_limit = 0;
	TaskCallback job_callback = nullptr;
	void *job_userdata = nullptr;
	std::atomic<int> next_index{ 0 };
	std::atomic<int> remaining_workers{ 0 };

	static void _worker_entry(void *p_userdata);
	void _worker_loop(int p_slot_index);
	void _ensure_initialized();

public:
	static LoadThreadPool *get_singleton();
	static void cleanup();

	int get_concurrency_budget(bool p_gameplay_throttle = false) const;
	void parallel_for_raw(int p_count, TaskCallback p_callback, void *p_userdata, bool p_gameplay_throttle = false);

	template <typename Fn>
	void parallel_for(int p_count, Fn &&p_fn, bool p_gameplay_throttle = false) {
		if (p_count <= 0) {
			return;
		}
		if (p_count == 1) {
			p_fn(0);
			return;
		}
		auto thunk = [](void *p_ud, int p_idx) {
			(*static_cast<typename std::remove_reference<Fn>::type *>(p_ud))(p_idx);
		};
		parallel_for_raw(p_count, thunk, &p_fn, p_gameplay_throttle);
	}
};
