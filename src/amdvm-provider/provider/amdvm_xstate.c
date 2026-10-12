#include "amdvm_xstate.h"
#include "../amdvm_probe_abi.h"

#include <string.h>

#define AMDVM_XSTATE_BV_OFFSET 512u
#define AMDVM_XCOMP_BV_OFFSET 520u
#define AMDVM_XSTATE_RESERVED_OFFSET 528u
#define AMDVM_XSTATE_RESERVED_SIZE 48u

int
amdvm_xstate_validate_standard(const void *area, size_t size, uint64_t xcr0)
{
	const uint8_t *bytes = area;
	uint64_t xstate_bv, xcomp_bv;
	size_t i;

	if (area == NULL || size != AMDVM_X86_XSTATE_MAX_SIZE ||
	    (xcr0 & 1u) == 0 ||
	    ((xcr0 & (1ull << 2)) != 0 && (xcr0 & (1ull << 1)) == 0)) {
		return -1;
	}
	memcpy(&xstate_bv, bytes + AMDVM_XSTATE_BV_OFFSET, sizeof(xstate_bv));
	memcpy(&xcomp_bv, bytes + AMDVM_XCOMP_BV_OFFSET, sizeof(xcomp_bv));
	if ((xstate_bv & ~xcr0) != 0 || xcomp_bv != 0) {
		return -1;
	}
	for (i = 0; i < AMDVM_XSTATE_RESERVED_SIZE; i++) {
		if (bytes[AMDVM_XSTATE_RESERVED_OFFSET + i] != 0) {
			return -1;
		}
	}
	return 0;
}

int
amdvm_xstate_copy_standard(void *destination, size_t destination_size,
	const void *source, size_t source_size, uint64_t xcr0)
{
	if (destination == NULL || source == NULL ||
	    destination_size != AMDVM_X86_XSTATE_MAX_SIZE ||
	    source_size != AMDVM_X86_XSTATE_MAX_SIZE ||
	    amdvm_xstate_validate_standard(source, source_size, xcr0) != 0) {
		return -1;
	}
	memmove(destination, source, AMDVM_X86_XSTATE_MAX_SIZE);
	return 0;
}
