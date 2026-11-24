/**************************************************************************/
/*  perfci_client.h                                                       */
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

#include "core/io/stream_peer_tcp.h"
#include "core/object/object.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"

// PerfCI protocol v1 client. Connects back to the PerfCI host given by the
// PERFCI_* launch environment (or Android perfci_bootstrap.json) and serves
// its requests on a worker thread.
class PerfCIClient : public Object {
	GDCLASS(PerfCIClient, Object);

	String host;
	int port = 0;
	String run_id;
	String token;
	String target_id;
	PackedStringArray scenarios;
	PackedStringArray worlds_available;
	String output_dir;

	Thread thread;
	SafeFlag exit_requested;
	SafeFlag scene_tree_attached;
	Ref<StreamPeerTCP> peer;

	mutable Mutex state_mutex;
	String scenario;
	bool running = false;
	SafeFlag scene_ready;
	String scene_evidence = "waiting for scene";
	uint32_t settled_frames = 0;
	uint64_t settle_start_msec = 0;

	SafeNumeric<uint64_t> last_draw_calls;
	SafeNumeric<uint64_t> last_primitives;

	SafeFlag screenshot_pending;
	Semaphore screenshot_done;
	String screenshot_error;
	int screenshot_width = 0;
	int screenshot_height = 0;

	static void _thread_func(void *p_user);
	void _attach_scene_tree();
	void _on_process_frame();
	void _run();
	void _handle_line(const String &p_line);
	Dictionary _scene_state() const;
	bool _take_snapshot(const Dictionary &p_params, Dictionary &r_result, String &r_error_code, String &r_error_message);
	bool _capture_gpu_frame(const Dictionary &p_params, Dictionary &r_result, String &r_error_code, String &r_error_message);
	void _send(const Dictionary &p_message);

public:
	static bool has_launch_bootstrap();
	void start();
	void stop();
};
