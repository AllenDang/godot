#pragma once

#include <cstdint>
#include <cstring>

// Pinned UE FRandomStream / FClusterBuilder admission policy. Spatial order and
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
