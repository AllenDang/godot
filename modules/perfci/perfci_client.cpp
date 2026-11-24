/**************************************************************************/
/*  perfci_client.cpp                                                     */
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

#include "perfci_client.h"

#include "perfci_gpu_capture.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/ip.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "scene/gui/texture_rect.h"
#include "scene/main/canvas_layer.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "servers/rendering/rendering_server.h"

#ifndef _3D_DISABLED
#include "scene/3d/node_3d.h"
#include "scene/3d/physics/character_body_3d.h"
#endif

namespace {

String slugify_target_id(const String &p_raw) {
	String out;
	bool prev_dash = false;
	const String lower = p_raw.strip_edges().to_lower();
	for (int i = 0; i < lower.length(); i++) {
		const char32_t c = lower[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
			out += c;
			prev_dash = false;
		} else if (!prev_dash && !out.is_empty()) {
			out += '-';
			prev_dash = true;
		}
	}
	while (out.ends_with("-")) {
		out = out.left(out.length() - 1);
	}
	return out.is_empty() ? String("godot") : out;
}

#ifdef ANDROID_ENABLED
String android_bootstrap_path() {
	const String ext_files = OS::get_singleton()->get_system_dir(OS::SYSTEM_DIR_DESKTOP, false);
	if (ext_files.is_empty()) {
		return String();
	}
	return ext_files.path_join("perfci/perfci_bootstrap.json");
}
#endif

bool are_canvas_layers_visible(const Node *p_node) {
	for (const Node *cur = p_node; cur != nullptr; cur = cur->get_parent()) {
		if (const CanvasLayer *layer = Object::cast_to<CanvasLayer>(cur)) {
			if (!layer->is_visible()) {
				return false;
			}
		}
	}
	return true;
}

bool is_subviewport_presented(Node *p_node, const Texture2D *p_vp_tex, int p_depth) {
	if (p_node == nullptr || p_vp_tex == nullptr || p_depth > 5) {
		return false;
	}
	if (TextureRect *rect = Object::cast_to<TextureRect>(p_node)) {
		if (rect->get_texture().ptr() == p_vp_tex && rect->is_visible_in_tree() && are_canvas_layers_visible(rect)) {
			return true;
		}
	}
	const int count = p_node->get_child_count();
	for (int i = 0; i < count; i++) {
		if (is_subviewport_presented(p_node->get_child(i), p_vp_tex, p_depth + 1)) {
			return true;
		}
	}
	return false;
}

#ifndef _3D_DISABLED
CharacterBody3D *find_grounded_player(Node *p_node) {
	if (p_node == nullptr) {
		return nullptr;
	}
	if (CharacterBody3D *body = Object::cast_to<CharacterBody3D>(p_node)) {
		if (body->is_visible_in_tree() && body->is_on_floor()) {
			return body;
		}
	}
	const int count = p_node->get_child_count();
	for (int i = 0; i < count; i++) {
		if (CharacterBody3D *found = find_grounded_player(p_node->get_child(i))) {
			return found;
		}
	}
	return nullptr;
}

CharacterBody3D *find_ready_world_player(Node *p_root, Node *p_node, const String &p_world_filter, int p_depth) {
	if (p_node == nullptr || p_depth > 3) {
		return nullptr;
	}
	if (SubViewport *vp = Object::cast_to<SubViewport>(p_node)) {
		if (vp->get_update_mode() == SubViewport::UPDATE_ALWAYS && is_subviewport_presented(p_root, vp->get_texture().ptr(), 0)) {
			const int child_count = vp->get_child_count();
			for (int i = 0; i < child_count; i++) {
				Node3D *root_3d = Object::cast_to<Node3D>(vp->get_child(i));
				if (root_3d == nullptr || !root_3d->is_visible_in_tree()) {
					continue;
				}
				if (!p_world_filter.is_empty() && !String(root_3d->get_name()).to_lower().contains(p_world_filter)) {
					continue;
				}
				if (CharacterBody3D *player = find_grounded_player(root_3d)) {
					return player;
				}
			}
		}
		return nullptr;
	}
	const int count = p_node->get_child_count();
	for (int i = 0; i < count; i++) {
		if (CharacterBody3D *found = find_ready_world_player(p_root, p_node->get_child(i), p_world_filter, p_depth + 1)) {
			return found;
		}
	}
	return nullptr;
}
#endif

bool is_nonempty_file(const String &p_path) {
	if (!FileAccess::exists(p_path)) {
		return false;
	}
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	return f.is_valid() && f->get_length() > 0;
}

} // namespace

bool PerfCIClient::has_launch_bootstrap() {
	if (!OS::get_singleton()->get_environment("PERFCI_PORT").is_empty()) {
		return true;
	}
#ifdef ANDROID_ENABLED
	const String boot_path = android_bootstrap_path();
	if (!boot_path.is_empty() && FileAccess::exists(boot_path)) {
		return true;
	}
#endif
	return false;
}

void PerfCIClient::start() {
	OS *os = OS::get_singleton();
	const String env_port = os->get_environment("PERFCI_PORT");
	if (!env_port.is_empty()) {
		host = os->get_environment("PERFCI_HOST");
		port = env_port.to_int();
		run_id = os->get_environment("PERFCI_RUN_ID");
		token = os->get_environment("PERFCI_TOKEN");
		output_dir = os->get_environment("PERFCI_OUTPUT_DIR");
	}
#ifdef ANDROID_ENABLED
	else {
		const String boot_path = android_bootstrap_path();
		if (!boot_path.is_empty() && FileAccess::exists(boot_path)) {
			const String raw_json = FileAccess::get_file_as_string(boot_path);
			// Delete immediately so manual app launches do not reconnect to a stale port.
			DirAccess::remove_absolute(boot_path);
			const Variant parsed = JSON::parse_string(raw_json);
			if (parsed.get_type() == Variant::DICTIONARY) {
				const Dictionary boot = parsed;
				host = String(boot.get("PERFCI_HOST", "127.0.0.1"));
				port = String(boot.get("PERFCI_PORT", "")).to_int();
				run_id = String(boot.get("PERFCI_RUN_ID", ""));
				token = String(boot.get("PERFCI_TOKEN", ""));
				output_dir = String(boot.get("PERFCI_OUTPUT_DIR", ""));
			}
		}
	}
#endif
	if (port <= 0) {
		return;
	}
	if (host.is_empty()) {
		host = "127.0.0.1";
	}

	if (ProjectSettings::get_singleton() != nullptr) {
		target_id = slugify_target_id(String(ProjectSettings::get_singleton()->get_setting("application/config/name", "godot")));
	}
	if (target_id.is_empty()) {
		target_id = "godot";
	}

	Ref<DirAccess> worlds_dir = DirAccess::open("res://worlds");
	if (worlds_dir.is_valid()) {
		for (const String &dir_name : worlds_dir->get_directories()) {
			if (!dir_name.begins_with(".")) {
				worlds_available.push_back(dir_name);
				scenarios.push_back(dir_name);
			}
		}
	}
	if (scenarios.is_empty()) {
		scenarios.push_back("idle");
	}

	if (output_dir.is_empty()) {
#ifdef ANDROID_ENABLED
		output_dir = os->get_system_dir(OS::SYSTEM_DIR_DESKTOP, false).path_join("perfci");
#else
		output_dir = "Documents/perfci";
#endif
	}
	if (output_dir.is_relative_path()) {
		output_dir = os->get_environment("HOME").path_join(output_dir);
	}

	thread.start(_thread_func, this);
}

void PerfCIClient::stop() {
	exit_requested.set();
	screenshot_done.post();
	if (SceneTree *tree = SceneTree::get_singleton()) {
		const Callable cb = callable_mp(this, &PerfCIClient::_on_process_frame);
		if (tree->is_connected(SNAME("process_frame"), cb)) {
			tree->disconnect(SNAME("process_frame"), cb);
		}
	}
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	if (peer.is_valid()) {
		peer->disconnect_from_host();
		peer.unref();
	}
}

void PerfCIClient::_attach_scene_tree() {
	if (exit_requested.is_set()) {
		return;
	}
	if (SceneTree *tree = SceneTree::get_singleton()) {
		const Callable cb = callable_mp(this, &PerfCIClient::_on_process_frame);
		if (!tree->is_connected(SNAME("process_frame"), cb)) {
			tree->connect(SNAME("process_frame"), cb);
		}
		scene_tree_attached.set();
	}
}

void PerfCIClient::_on_process_frame() {
	if (RenderingServer *rs = RenderingServer::get_singleton()) {
		last_draw_calls.set(rs->get_rendering_info(RSE::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME));
		last_primitives.set(rs->get_rendering_info(RSE::RENDERING_INFO_TOTAL_PRIMITIVES_IN_FRAME));
	}

	SceneTree *tree = SceneTree::get_singleton();
	Window *root = tree ? tree->get_root() : nullptr;

	if (screenshot_pending.is_set()) {
		String err_msg;
		int w = 0;
		int h = 0;
		Ref<Image> img = (root != nullptr && root->get_texture().is_valid()) ? root->get_texture()->get_image() : Ref<Image>();
		if (img.is_null() || img->is_empty()) {
			err_msg = "Root viewport image is empty.";
		} else {
			DirAccess::make_dir_recursive_absolute(output_dir);
			const String png_path = output_dir.path_join("scene.png");
			if (img->save_png(png_path) != OK) {
				err_msg = vformat("Failed saving PNG screenshot to %s.", png_path);
			} else {
				w = img->get_width();
				h = img->get_height();
			}
		}
		{
			MutexLock lock(state_mutex);
			screenshot_error = err_msg;
			screenshot_width = w;
			screenshot_height = h;
			screenshot_pending.clear();
		}
		screenshot_done.post();
	}

	if (scene_ready.is_set() || root == nullptr) {
		return;
	}
	String current_scenario;
	{
		MutexLock lock(state_mutex);
		current_scenario = scenario;
	}
	if (current_scenario.is_empty()) {
		return;
	}

	const uint64_t now_msec = OS::get_singleton()->get_ticks_msec();
	const uint64_t drawn = Engine::get_singleton()->get_frames_drawn();

#ifndef _3D_DISABLED
	if (!worlds_available.is_empty()) {
		const String world_filter = worlds_available.has(current_scenario) ? current_scenario.to_lower() : String();
		CharacterBody3D *player = find_ready_world_player(root, root, world_filter, 0);

		MutexLock lock(state_mutex);
		if (player != nullptr) {
			if (settled_frames == 0) {
				settle_start_msec = now_msec;
			}
			settled_frames++;
			const uint64_t elapsed_ms = now_msec - settle_start_msec;
			const Vector3 pos = player->get_global_position();
			if (settled_frames >= 45 && elapsed_ms >= 600) {
				scene_evidence = vformat(
						"world '%s' SubViewport presented, player grounded at (%.2f, %.2f, %.2f) for %d frames",
						current_scenario, pos.x, pos.y, pos.z, settled_frames);
				scene_ready.set();
			} else {
				scene_evidence = vformat(
						"settling '%s': player grounded (%d/45 frames, %d/600 ms)",
						current_scenario, settled_frames, elapsed_ms);
			}
		} else {
			settled_frames = 0;
			scene_evidence = vformat("waiting for presented '%s' SubViewport and grounded player (%d frames drawn)", current_scenario, drawn);
		}
		return;
	}
#endif

	if (drawn >= 60) {
		MutexLock lock(state_mutex);
		scene_evidence = vformat("scene rendered %d frames", drawn);
		scene_ready.set();
	}
}

void PerfCIClient::_thread_func(void *p_user) {
	static_cast<PerfCIClient *>(p_user)->_run();
}

void PerfCIClient::_run() {
	while (!exit_requested.is_set() && SceneTree::get_singleton() == nullptr) {
		OS::get_singleton()->delay_usec(10000);
	}
	if (exit_requested.is_set()) {
		return;
	}
	callable_mp(this, &PerfCIClient::_attach_scene_tree).call_deferred();

	IPAddress address;
	if (host.is_valid_ip_address()) {
		address = IPAddress(host);
	} else if (IP::get_singleton() != nullptr) {
		address = IP::get_singleton()->resolve_hostname(host);
	}
	ERR_FAIL_COND_MSG(!address.is_valid(), vformat("PerfCI: cannot resolve PERFCI_HOST '%s'.", host));

	peer.instantiate();
	ERR_FAIL_COND_MSG(peer->connect_to_host(address, port) != OK, vformat("PerfCI: cannot connect to %s:%d.", host, port));
	peer->set_no_delay(true);
	while (!exit_requested.is_set()) {
		peer->poll();
		const StreamPeerTCP::Status status = peer->get_status();
		if (status == StreamPeerTCP::STATUS_CONNECTED) {
			break;
		}
		ERR_FAIL_COND_MSG(status != StreamPeerTCP::STATUS_CONNECTING, vformat("PerfCI: cannot connect to %s:%d.", host, port));
		OS::get_singleton()->delay_usec(10000);
	}
	if (exit_requested.is_set()) {
		return;
	}

	Dictionary hello;
	hello["protocol"] = "perfci";
	hello["version"] = 1;
	hello["run_id"] = run_id;
	hello["token"] = token;
	hello["pid"] = OS::get_singleton()->get_process_id();
	_send(hello);

	PackedByteArray buffer;
	uint8_t chunk[4096];
	while (!exit_requested.is_set()) {
		peer->poll();
		if (peer->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			return;
		}
		int received = 0;
		if (peer->get_partial_data(chunk, sizeof(chunk), received) != OK) {
			return;
		}
		if (received == 0) {
			OS::get_singleton()->delay_usec(10000);
			continue;
		}
		const int64_t old_size = buffer.size();
		buffer.resize(old_size + received);
		memcpy(buffer.ptrw() + old_size, chunk, received);
		int64_t newline = buffer.find('\n');
		while (newline >= 0) {
			_handle_line(String::utf8(reinterpret_cast<const char *>(buffer.ptr()), newline));
			buffer = buffer.slice(newline + 1);
			newline = buffer.find('\n');
		}
	}
}

Dictionary PerfCIClient::_scene_state() const {
	MutexLock lock(state_mutex);
	Dictionary state;
	state["scenario"] = scenario;
	state["valid"] = true;
	if (running) {
		state["state"] = "running";
		state["evidence"] = scene_evidence;
	} else if (scene_ready.is_set()) {
		state["state"] = "ready";
		state["evidence"] = scene_evidence;
	} else {
		state["state"] = "preparing";
		state["evidence"] = scene_evidence;
	}
	return state;
}

bool PerfCIClient::_take_snapshot(const Dictionary &p_params, Dictionary &r_result, String &r_error_code, String &r_error_message) {
	Dictionary metrics;
	metrics["draw_calls"] = static_cast<int64_t>(last_draw_calls.get());
	metrics["primitives"] = static_cast<int64_t>(last_primitives.get());
	r_result["metrics"] = metrics;

	if (bool(p_params.get("include_screenshot", false))) {
		{
			MutexLock lock(state_mutex);
			while (screenshot_done.try_wait()) {
			}
			screenshot_error = String();
			screenshot_width = 0;
			screenshot_height = 0;
			screenshot_pending.set();
		}
		while (!screenshot_done.try_wait()) {
			if (exit_requested.is_set()) {
				screenshot_pending.clear();
				r_error_code = "aborted";
				r_error_message = "PerfCI client stopped during screenshot.";
				return false;
			}
			OS::get_singleton()->delay_usec(2000);
		}
		MutexLock lock(state_mutex);
		if (!screenshot_error.is_empty()) {
			r_error_code = "screenshot_failed";
			r_error_message = screenshot_error;
			return false;
		}
		Dictionary shot;
		shot["path"] = "scene.png";
		shot["width"] = screenshot_width;
		shot["height"] = screenshot_height;
		shot["rendered"] = true;
		r_result["screenshot"] = shot;
	}
	return true;
}

bool PerfCIClient::_capture_gpu_frame(const Dictionary &p_params, Dictionary &r_result, String &r_error_code, String &r_error_message) {
	if (RenderingServer::get_singleton()->get_current_rendering_driver_name() != "metal") {
		r_error_code = "unsupported_capability";
		r_error_message = "Programmatic in-process GPU frame capture is supported on Metal only.";
		return false;
	}
	const String name = p_params.get("output_name", String());
	if (!name.ends_with(".gputrace") || name.begins_with(".") || name.contains("/") || name.contains("\\")) {
		r_error_code = "invalid_params";
		r_error_message = vformat("output_name must be a plain *.gputrace file name: '%s'.", name);
		return false;
	}
	const String path = output_dir.path_join(name);
	DirAccess::make_dir_recursive_absolute(output_dir);
	// A trace left by an earlier run would make MTLCaptureManager refuse the path.
	if (DirAccess::dir_exists_absolute(path)) {
		Ref<DirAccess> trace_dir = DirAccess::open(path);
		if (trace_dir.is_valid()) {
			trace_dir->erase_contents_recursive();
		}
	}
	if (DirAccess::dir_exists_absolute(path) || FileAccess::exists(path)) {
		DirAccess::remove_absolute(path);
	}

	uint64_t frame = 0;
	const String error = PerfCIGpuCapture::capture_next_frame(path, frame, exit_requested);
	if (!error.is_empty()) {
		r_error_code = "capture_failed";
		r_error_message = error;
		return false;
	}

	// Wait until the .gputrace bundle metadata and capture stream are flushed to disk.
	const uint64_t deadline_msec = OS::get_singleton()->get_ticks_msec() + 30000;
	bool complete = false;
	while (!exit_requested.is_set() && OS::get_singleton()->get_ticks_msec() < deadline_msec) {
		if (DirAccess::dir_exists_absolute(path)) {
			if (is_nonempty_file(path.path_join("metadata")) &&
					(is_nonempty_file(path.path_join("capture")) || is_nonempty_file(path.path_join("unsorted-capture")))) {
				complete = true;
				break;
			}
		} else if (is_nonempty_file(path)) {
			complete = true;
			break;
		}
		OS::get_singleton()->delay_usec(20000);
	}
	if (!complete) {
		r_error_code = "capture_failed";
		r_error_message = vformat("Capture stopped on frame %d, but %s was not finalized on disk.", frame, path);
		return false;
	}

	r_result["saved_path"] = name;
	r_result["frame"] = static_cast<int64_t>(frame);
	return true;
}

void PerfCIClient::_handle_line(const String &p_line) {
	const Variant parsed = JSON::parse_string(p_line);
	ERR_FAIL_COND_MSG(parsed.get_type() != Variant::DICTIONARY, "PerfCI: request is not a JSON object.");
	const Dictionary request = parsed;
	const int64_t id = request.get("id", 0);
	const String method = request.get("method", String());
	const Dictionary params = request.get("params", Dictionary());

	Dictionary result;
	String error_code;
	String error_message;
	if (String(request.get("run_id", String())) != run_id) {
		error_code = "invalid_request";
		error_message = "run_id does not belong to this PerfCI session.";
	} else if (method == "describe") {
		const String api = RenderingServer::get_singleton()->get_current_rendering_driver_name();
		Array capabilities;
		capabilities.push_back("screenshot");
		capabilities.push_back("render_metrics");
		if (api == "metal") {
			capabilities.push_back("gpu_frame_capture");
		}
		Array scenario_ids;
		for (const String &id_name : scenarios) {
			scenario_ids.push_back(id_name);
		}
		result["target_id"] = target_id;
		result["pid"] = OS::get_singleton()->get_process_id();
		result["rendering_api"] = api;
		result["capabilities"] = capabilities;
		result["scenarios"] = scenario_ids;
	} else if (method == "prepare") {
		const String requested = params.get("scenario", String());
		{
			MutexLock lock(state_mutex);
			if (scenario != requested) {
				scenario = requested;
				scene_ready.clear();
				settled_frames = 0;
				settle_start_msec = 0;
				scene_evidence = vformat("preparing scenario '%s'", scenario);
			}
			running = false;
		}
		result = _scene_state();
	} else if (method == "status") {
		result = _scene_state();
	} else if (method == "begin") {
		{
			MutexLock lock(state_mutex);
			scenario = params.get("scenario", String());
			running = true;
			if (scene_evidence.is_empty()) {
				scene_evidence = "scenario running";
			}
		}
		result = _scene_state();
	} else if (method == "end") {
		{
			MutexLock lock(state_mutex);
			running = false;
		}
		result = _scene_state();
	} else if (method == "snapshot") {
		_take_snapshot(params, result, error_code, error_message);
	} else if (method == "capture_gpu_frame") {
		_capture_gpu_frame(params, result, error_code, error_message);
	} else if (method == "shutdown") {
		result["accepted"] = true;
		if (SceneTree *tree = SceneTree::get_singleton()) {
			callable_mp(tree, &SceneTree::quit).call_deferred(EXIT_SUCCESS);
		}
	} else {
		error_code = "unsupported_method";
		error_message = vformat("Unsupported PerfCI method '%s'.", method);
	}

	Dictionary response;
	response["protocol"] = "perfci";
	response["version"] = 1;
	response["run_id"] = run_id;
	response["id"] = id;
	response["ok"] = error_code.is_empty();
	if (error_code.is_empty()) {
		response["result"] = result;
	} else {
		Dictionary error;
		error["code"] = error_code;
		error["message"] = error_message;
		response["error"] = error;
	}
	_send(response);
}

void PerfCIClient::_send(const Dictionary &p_message) {
	const CharString line = (JSON::stringify(p_message, "", false) + "\n").utf8();
	peer->put_data(reinterpret_cast<const uint8_t *>(line.get_data()), line.length());
}
