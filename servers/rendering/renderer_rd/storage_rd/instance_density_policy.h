/**************************************************************************/
/*  instance_density_policy.h                                             */
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

#include <cstdint>
#include <cstring>

// Pinned Unreal Engine FRandomStream / FClusterBuilder admission policy. Spatial order and
// authored instance records are independent of this original-ID selection.
namespace InstanceDensityPolicy {
inline float next_fraction(uint32_t &r_state) {
	r_state = r_state * 196314165U + 907633515U;
	uint32_t bits = 0x3f800000U | (r_state >> 9);
	float result;
	std::memcpy(&result, &bits, sizeof(result));
	return result - 1.0f;
}
template <typename Append>
uint32_t admit(int32_t p_seed, float p_density, uint32_t p_count, Append p_append) {
	uint32_t state = uint32_t(p_seed);
	for (uint32_t original = 0; original < p_count; original++) {
		if (p_density < 1.0f && next_fraction(state) > p_density) {
			continue;
		}
		p_append(original);
	}
	return state;
}
} // namespace InstanceDensityPolicy
