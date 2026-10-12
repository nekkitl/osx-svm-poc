/* Offline NPT sizing check for an 8 GiB guest and the reset-ROM page. */
#include "../provider/amdvm_npt.h"
#include "../amdvm_probe_abi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
	const uint64_t four_gib = UINT64_C(4) * 1024u * 1024u * 1024u;
	const amdvm_npt_range_t ranges[] = {
		{ .guest_gpa = 0,
		  .size = AMDVM_VCPU_RESET_ROM_GPA },
		{ .guest_gpa = AMDVM_VCPU_RESET_ROM_GPA,
		  .size = AMDVM_NPT_PAGE_SIZE },
		{ .guest_gpa = four_gib,
		  .size = AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES -
		      AMDVM_VCPU_RESET_ROM_GPA },
	};
	size_t required_pages = 0;

	assert(AMDVM_VCPU_MEMORY_MAX_TOTAL_BYTES ==
	    UINT64_C(8) * 1024u * 1024u * 1024u);
	assert(amdvm_npt_required_pages(ranges,
	    sizeof(ranges) / sizeof(ranges[0]), 48, &required_pages) ==
	    AMDVM_NPT_OK);
	assert(required_pages > 1u);
	assert(required_pages <= UINT32_MAX / AMDVM_NPT_PAGE_SIZE);
	printf("8 GiB guest NPT sizing: %zu pages (%zu bytes): PASS\n",
	    required_pages, required_pages * AMDVM_NPT_PAGE_SIZE);
	return 0;
}
