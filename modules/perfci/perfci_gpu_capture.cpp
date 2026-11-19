/**************************************************************************/
/*  perfci_gpu_capture.cpp                                                */
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

#include "perfci_gpu_capture.h"

#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/semaphore.h"

namespace {

Mutex mutex;
SafeFlag pending;
Semaphore done;
String request_path;
String result_error;
uint64_t result_frame = 0;

} // namespace

String PerfCIGpuCapture::capture_next_frame(const String &p_path, uint64_t &r_frame, const SafeFlag &p_abort) {
	{
		MutexLock lock(mutex);
		while (done.try_wait()) {
		}
		request_path = p_path;
		result_error = String();
		result_frame = 0;
		pending.set();
	}
	while (!done.try_wait()) {
		if (p_abort.is_set()) {
			pending.clear();
			return "PerfCI client stopped before the capture finished.";
		}
		OS::get_singleton()->delay_usec(2000);
	}
	MutexLock lock(mutex);
	r_frame = result_frame;
	return result_error;
}

bool PerfCIGpuCapture::has_pending_request() {
	return pending.is_set();
}

bool PerfCIGpuCapture::take_request(String &r_path) {
	if (!pending.is_set()) {
		return false;
	}
	MutexLock lock(mutex);
	if (!pending.is_set()) {
		return false;
	}
	pending.clear();
	r_path = request_path;
	return true;
}

void PerfCIGpuCapture::finish(const String &p_error, uint64_t p_frame) {
	{
		MutexLock lock(mutex);
		result_error = p_error;
		result_frame = p_frame;
	}
	done.post();
}
