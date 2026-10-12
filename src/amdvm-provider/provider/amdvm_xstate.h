#ifndef AMDVM_XSTATE_H
#define AMDVM_XSTATE_H

#include <stddef.h>
#include <stdint.h>

/* Validate the standard-format XSAVE header against a vCPU's enabled XCR0. */
int amdvm_xstate_validate_standard(const void *area, size_t size,
	uint64_t xcr0);

/* Validate and copy a standard-format XSAVE image to/from provider storage. */
int amdvm_xstate_copy_standard(void *destination, size_t destination_size,
	const void *source, size_t source_size, uint64_t xcr0);

#endif
