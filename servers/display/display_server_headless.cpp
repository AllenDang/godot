/**************************************************************************/
/*  display_server_headless.cpp                                           */
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

#include "display_server_headless.h"

#include "core/input/input.h"
#include "core/input/input_event.h"
#include "servers/display/native_menu.h"
#include "servers/rendering/dummy/rasterizer_dummy.h"

#if defined(METAL_ENABLED) && defined(RD_ENABLED)
#include "core/os/os.h"
#include "drivers/metal/rendering_context_driver_metal.h"
#include "servers/rendering/renderer_rd/renderer_compositor_rd.h"
#include "servers/rendering/rendering_device.h"

namespace {

class HeadlessImageOnlyCompositor final : public RendererCompositorRD {
public:
	void set_boot_image_with_stretch(const Ref<Image> &, const Color &, RSE::SplashStretchMode, bool) override {}
	static void install() {
		low_end = false;
		_create_func = []() -> RendererCompositor * { return memnew(HeadlessImageOnlyCompositor); };
	}
};

class API_AVAILABLE(macos(11.0), ios(14.0), tvos(14.0)) DisplayServerOffscreenMetal final : public DisplayServerHeadless {
	RenderingContextDriverMetal *context = nullptr;
	RenderingDevice *device = nullptr;
	CA::MetalLayer *layer = nullptr;
	Size2i viewport_size = Size2i(1280, 720);

public:
	explicit DisplayServerOffscreenMetal(const Size2i &p_size, Error &r_error) :
			viewport_size(p_size.x > 0 && p_size.y > 0 ? p_size : Size2i(1280, 720)) {
		r_error = ERR_CANT_CREATE;
		context = memnew(RenderingContextDriverMetal);
		if (context->initialize() != OK) {
			return;
		}
		layer = CA::MetalLayer::layer()->retain();
		layer->setDrawableSize(CGSizeMake(viewport_size.x, viewport_size.y));
		RenderingContextDriverMetal::WindowPlatformData surface{ layer };
		if (context->window_create(DisplayServerEnums::MAIN_WINDOW_ID, &surface) != OK) {
			return;
		}
		context->window_set_size(DisplayServerEnums::MAIN_WINDOW_ID, viewport_size.x, viewport_size.y);
		context->window_set_vsync_mode(DisplayServerEnums::MAIN_WINDOW_ID, DisplayServerEnums::VSYNC_DISABLED);
		device = memnew(RenderingDevice);
		if (device->initialize(context, DisplayServerEnums::MAIN_WINDOW_ID) != OK) {
			return;
		}
		HeadlessImageOnlyCompositor::install();
		r_error = OK;
	}

	~DisplayServerOffscreenMetal() override {
		if (device) {
			memdelete(device);
			device = nullptr;
		}
		if (context) {
			context->window_destroy(DisplayServerEnums::MAIN_WINDOW_ID);
			memdelete(context);
			context = nullptr;
		}
		if (layer) {
			layer->release();
			layer = nullptr;
		}
	}

	Size2i window_get_size(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override {
		return viewport_size;
	}
	Size2i window_get_size_with_decorations(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override {
		return viewport_size;
	}
	bool window_is_focused(DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override {
		return false;
	}
	bool window_get_flag(DisplayServerEnums::WindowFlags p_flag, DisplayServerEnums::WindowID p_window = DisplayServerEnums::MAIN_WINDOW_ID) const override {
		return p_flag == DisplayServerEnums::WINDOW_FLAG_NO_FOCUS;
	}
	DisplayServerEnums::VSyncMode window_get_vsync_mode(DisplayServerEnums::WindowID p_window) const override {
		return DisplayServerEnums::VSYNC_DISABLED;
	}
};

} // namespace
#endif

DisplayServer *DisplayServerHeadless::create_func(const String &p_rendering_driver, DisplayServerEnums::WindowMode p_mode, DisplayServerEnums::VSyncMode p_vsync_mode, uint32_t p_flags, const Vector2i *p_position, const Vector2i &p_resolution, int p_screen, DisplayServerEnums::Context p_context, int64_t p_parent_window, Error &r_error) {
	r_error = OK;
#if defined(METAL_ENABLED) && defined(RD_ENABLED)
	if (p_rendering_driver == "metal" && OS::get_singleton() && OS::get_singleton()->get_environment("ANGE_OFFSCREEN_METAL") == "1") {
		if (__builtin_available(macOS 11.0, iOS 14.0, tvOS 14.0, *)) {
			DisplayServerOffscreenMetal *ds = memnew(DisplayServerOffscreenMetal(p_resolution, r_error));
			if (r_error == OK) {
				return ds;
			}
			memdelete(ds);
		}
	}
#endif
	RasterizerDummy::make_current();
	return memnew(DisplayServerHeadless());
}

void DisplayServerHeadless::_dispatch_input_events(const Ref<InputEvent> &p_event) {
	static_cast<DisplayServerHeadless *>(get_singleton())->_dispatch_input_event(p_event);
}

void DisplayServerHeadless::_dispatch_input_event(const Ref<InputEvent> &p_event) {
	if (input_event_callback.is_valid()) {
		input_event_callback.call(p_event);
	}
}

void DisplayServerHeadless::process_events() {
	Input::get_singleton()->flush_buffered_events();
}

DisplayServerHeadless::DisplayServerHeadless() {
	native_menu = memnew(NativeMenu);
	Input::get_singleton()->set_event_dispatch_function(_dispatch_input_events);
}

DisplayServerHeadless::~DisplayServerHeadless() {
	if (native_menu) {
		memdelete(native_menu);
		native_menu = nullptr;
	}
}
