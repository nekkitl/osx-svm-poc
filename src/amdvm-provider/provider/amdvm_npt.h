#ifndef AMDVM_NPT_H
#define AMDVM_NPT_H

#include <stddef.h>
#include <stdint.h>

#define AMDVM_NPT_PAGE_SIZE 4096u
#define AMDVM_NPT_ENTRIES 512u
#define AMDVM_NPT_MAX_GPA_BITS 48u

#define AMDVM_NPT_READ    (1u << 0)
#define AMDVM_NPT_WRITE   (1u << 1)
#define AMDVM_NPT_EXECUTE (1u << 2)

#define AMDVM_NPT_MEMTYPE_UC 0u
#define AMDVM_NPT_MEMTYPE_WC 1u
#define AMDVM_NPT_MEMTYPE_WT 4u
#define AMDVM_NPT_MEMTYPE_WP 5u
#define AMDVM_NPT_MEMTYPE_WB 6u

enum amdvm_npt_result {
	AMDVM_NPT_OK = 0,
	AMDVM_NPT_INVALID = -1,
	AMDVM_NPT_NO_SPACE = -2,
	AMDVM_NPT_CONFLICT = -3,
	AMDVM_NPT_ROLLBACK_FAILED = -4
};

/*
 * PoC implementation: `pool` is kernel-owned, page-aligned, physically
 * contiguous storage and the allocator supplies its physical base. This is
 * an implementation choice, not an NPT or provider ABI requirement; each
 * table link is a physical address, so a future pool can use per-page PAs.
 * Page 0 holds the PML4; remaining pages are consumed for lower-level tables.
 */
typedef struct amdvm_npt {
	uint8_t *pool;
	uint64_t pool_pa;
	size_t pool_pages;
	size_t pages_used;
	uint64_t gpa_limit;
	uint64_t hpa_limit;
} amdvm_npt_t;

typedef struct amdvm_npt_range {
	uint64_t guest_gpa;
	uint64_t size;
} amdvm_npt_range_t;

typedef struct amdvm_npt_pat_range {
	uint64_t guest_gpa;
	uint64_t size;
	uint8_t pat_index;
	uint8_t reserved[7];
} amdvm_npt_pat_range_t;

/* Result of walking the actual four-level table bytes in the NPT pool. */
typedef struct amdvm_npt_translation {
	uint64_t host_physical_address;
	uint64_t leaf_entry;
	uint32_t permissions;
	uint32_t reserved;
} amdvm_npt_translation_t;

typedef int (*amdvm_npt_pat_set_callback_t)(void *context, amdvm_npt_t *npt,
	uint64_t gpa, uint8_t pat_index);

#ifdef __cplusplus
extern "C" {
#endif

int amdvm_npt_init(amdvm_npt_t *npt, void *pool, uint64_t pool_pa,
	size_t pool_pages, uint8_t guest_phys_bits, uint8_t host_phys_bits);
/*
 * Count the exact 4 KiB page-table pool size for sorted, non-overlapping GPA
 * ranges. Includes the root PML4 page; callers allocate this before mapping.
 */
int amdvm_npt_required_pages(const amdvm_npt_range_t *ranges,
	size_t range_count, uint8_t guest_phys_bits, size_t *required_pages);
int amdvm_npt_map_4k(amdvm_npt_t *npt, uint64_t gpa, uint64_t hpa,
	uint32_t permissions);
/*
 * Map a scatter/gather set of guest pages from pinned host pages. On failure,
 * earlier pages may already be present; discard the whole fresh NPT context.
 */
int amdvm_npt_map_4k_pages(amdvm_npt_t *npt, uint64_t guest_gpa,
	const uint64_t *host_page_pas, size_t page_count, uint32_t permissions);
/* Verify that the complete four-page tree maps only this 4 KiB GPA. */
int amdvm_npt_validate_single_4k(const amdvm_npt_t *npt, uint64_t gpa,
	uint64_t hpa, uint32_t permissions);
/* Validate one leaf in a candidate tree; callers validate every input page. */
int amdvm_npt_validate_4k_mapping(const amdvm_npt_t *npt, uint64_t gpa,
	uint64_t hpa, uint32_t permissions);
/* Walk the live NPT table bytes and return the resolved 4 KiB HPA/permissions. */
int amdvm_npt_translate_4k(const amdvm_npt_t *npt, uint64_t gpa,
	amdvm_npt_translation_t *translation);
/* Set the NPT leaf PAT selector (index bits are PAT:PCD:PWT). */
int amdvm_npt_set_4k_pat_index(amdvm_npt_t *npt, uint64_t gpa,
	uint8_t pat_index);
int amdvm_npt_validate_4k_pat_index(const amdvm_npt_t *npt, uint64_t gpa,
	uint8_t pat_index);
int amdvm_npt_get_4k_pat_index(const amdvm_npt_t *npt, uint64_t gpa,
	uint8_t *pat_index);
/* Resolve a requested architectural memory type through the host PAT MSR. */
int amdvm_npt_host_pat_index(uint64_t host_pat, uint8_t memory_type,
	uint8_t *pat_index);
/* Exact AMD APM Table 15-19 composition; no host MSR writes. */
int amdvm_npt_plan_pat(uint64_t host_pat, uint64_t guest_pat,
	const uint8_t *types, size_t count, uint8_t *indexes,
	uint64_t *execution_pat);
/* Atomically update sorted NPT ranges, rolling back on a partial setter error. */
int amdvm_npt_update_pat_ranges_atomic(amdvm_npt_t *npt,
	const amdvm_npt_pat_range_t *ranges, size_t range_count,
	uint8_t *old_pat_indexes, size_t page_count,
	amdvm_npt_pat_set_callback_t set_callback, void *callback_context);
/* Validate sorted keys encoded as ((host_pa >> 12) << 1) | rom_aliasable. */
int amdvm_npt_validate_backing_aliases(uint64_t *encoded_pages,
	size_t page_count);
uint64_t amdvm_npt_root_pa(const amdvm_npt_t *npt);

#ifdef __cplusplus
}
#endif

#endif
