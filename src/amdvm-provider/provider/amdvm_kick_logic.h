#ifndef AMDVM_KICK_LOGIC_H
#define AMDVM_KICK_LOGIC_H

#include <stdint.h>

static inline int
amdvm_generation_newer(uint64_t candidate, uint64_t current)
{
	return candidate != current && (candidate - current) < (1ull << 63);
}

/* Equal means a retry of an already latched generation is still actionable. */
static inline int
amdvm_kick_generation_should_dispatch(uint64_t request, uint64_t current)
{
	return request == current || amdvm_generation_newer(request, current);
}

#endif /* AMDVM_KICK_LOGIC_H */
