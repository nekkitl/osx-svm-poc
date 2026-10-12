/* Verify kick generation ordering and same-generation timeout retries. */
#include "../svm-kext/provider/amdvm_kick_logic.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int
main(void)
{
	assert(amdvm_generation_newer(12, 11));
	assert(!amdvm_generation_newer(11, 11));
	assert(!amdvm_generation_newer(10, 11));
	assert(amdvm_generation_newer(1, UINT64_MAX));
	assert(!amdvm_generation_newer(UINT64_MAX, 1));

	/* First delivery and retry after timeout share the latched generation. */
	assert(amdvm_kick_generation_should_dispatch(12, 11));
	assert(amdvm_kick_generation_should_dispatch(11, 11));
	assert(!amdvm_kick_generation_should_dispatch(10, 11));
	assert(amdvm_kick_generation_should_dispatch(1, UINT64_MAX));

	puts("amdvm kick generation retry logic: PASS");
	return 0;
}
