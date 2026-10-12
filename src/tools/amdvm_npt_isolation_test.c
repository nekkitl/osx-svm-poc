/* Userspace-only test: simulated physical addresses, no privileged instructions. */
#include "amdvm_npt.h"
#include "../svm-kext/amdvm_probe_abi.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decode hardware-format entries independently of the builder/validator. */
#define PAGE_BYTES 4096u
#define POOL_BYTES (4u * PAGE_BYTES)
#define LARGE_POOL_PAGES 16u
#define LARGE_POOL_BYTES (LARGE_POOL_PAGES * PAGE_BYTES)
#define MAX_EXPECTED_PAGES 16u
#define POOL_PA UINT64_C(0x100000)
#define PAYLOAD_PA UINT64_C(0x104000)
#define ADDRESS_MASK UINT64_C(0x000ffffffffff000)

static uint64_t expected_gpas[MAX_EXPECTED_PAGES];
static uint64_t expected_hpas[MAX_EXPECTED_PAGES];
static uint64_t expected_leaf_flags[MAX_EXPECTED_PAGES];
static unsigned expected_count;
static uint64_t expected_pool_pa;
static size_t expected_pool_pages;

struct walk_result {
	size_t leaves;
	unsigned visited;
	int invalid;
};

static void
walk(const uint8_t *pool, uint64_t table_pa, unsigned level,
	uint64_t guest_base, struct walk_result *result)
{
	if (table_pa < expected_pool_pa || table_pa >= expected_pool_pa +
	    expected_pool_pages * PAGE_BYTES ||
	    (table_pa & (PAGE_BYTES - 1u)) != 0 || level == 0) {
		result->invalid = 1;
		return;
	}
	unsigned index = (unsigned)((table_pa - expected_pool_pa) / PAGE_BYTES);
	if (result->visited & (1u << index)) {
		result->invalid = 1;
		return;
	}
	result->visited |= 1u << index;
	const uint64_t *entries = (const uint64_t *)(pool + table_pa - expected_pool_pa);
	for (unsigned i = 0; i < 512u; ++i) {
		uint64_t entry = entries[i];
		if ((entry & 1u) == 0) {
			/* This fixed profile has no non-present metadata entries. */
			if (entry != 0) result->invalid = 1;
			continue;
		}
		uint64_t address = entry & ADDRESS_MASK;
		uint64_t gpa = guest_base | ((uint64_t)i << (12u + 9u * (level - 1u)));
		if (level == 1u) {
			++result->leaves;
			/* Match each allowed guest page to its pinned host page. */
			unsigned match;
			for (match = 0; match < expected_count; ++match) {
				if (gpa == expected_gpas[match]) break;
			}
			if (match == expected_count || address != expected_hpas[match] ||
			    (entry & ~ADDRESS_MASK) != expected_leaf_flags[match] ||
			    result->invalid) {
				result->invalid = 1;
			}
		} else {
			/* Only ordinary four-level tables, with no large-page leaves. */
			if ((entry & ~ADDRESS_MASK) != UINT64_C(7)) {
				result->invalid = 1;
				continue;
			}
			walk(pool, address, level - 1u, gpa, result);
		}
	}
}

static int
isolated(const uint8_t *pool)
{
	struct walk_result result = {0};
	walk(pool, POOL_PA, 4u, 0, &result);
	return !result.invalid && result.leaves == expected_count &&
	    result.visited != 0;
}

static void
assert_rejected(const amdvm_npt_t *npt)
{
	assert(!isolated(npt->pool));
	assert(amdvm_npt_validate_single_4k(npt, 0, PAYLOAD_PA,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) != AMDVM_NPT_OK);
}

static void
assert_npt_pool_size_matches_mapping(const amdvm_npt_range_t *ranges,
	unsigned range_count)
{
	uint8_t *pool = NULL;
	uint64_t host_pages[4];
	amdvm_npt_t npt = {0};
	size_t required_pages;
	unsigned i, host_page_count = 0;

	assert(amdvm_npt_required_pages(ranges, range_count, 48,
	    &required_pages) == AMDVM_NPT_OK);
	assert(required_pages <= LARGE_POOL_PAGES);
	assert(posix_memalign((void **)&pool, PAGE_BYTES,
	    LARGE_POOL_BYTES) == 0);
	assert(amdvm_npt_init(&npt, pool, POOL_PA, required_pages, 48, 48) ==
	    AMDVM_NPT_OK);
	for (i = 0; i < range_count; ++i) {
		unsigned pages = (unsigned)(ranges[i].size / PAGE_BYTES);
		unsigned j;

		assert(pages <= 4u - host_page_count);
		for (j = 0; j < pages; ++j) {
			host_pages[host_page_count] = UINT64_C(0x900000) +
			    (uint64_t)host_page_count * PAGE_BYTES;
			host_page_count++;
		}
		assert(amdvm_npt_map_4k_pages(&npt, ranges[i].guest_gpa,
		    &host_pages[host_page_count - pages], pages,
		    AMDVM_NPT_READ | AMDVM_NPT_WRITE | AMDVM_NPT_EXECUTE) ==
		    AMDVM_NPT_OK);
	}
	assert(npt.pages_used == required_pages);
	free(pool);
}

int
main(void)
{
	uint8_t *pool = NULL;
	uint8_t *large_pool = NULL;
	uint64_t scattered_pages[8];
	uint64_t invalid_scattered_pages[8];
	uint8_t reset_rom_page[PAGE_BYTES] = {0};
	const uint64_t reset_linear_address = AMDVM_VCPU_RESET_VECTOR_LINEAR;
	static const uint8_t reset_rom_code[] = {
		0x0f, 0xa2,             /* CPUID */
		0xb0, 0x41,             /* MOV AL, 'A' */
		0xe6, 0xe9,             /* OUT 0xe9, AL */
		0xf4                    /* HLT */
	};
	amdvm_npt_t npt = {0};
	amdvm_npt_range_t npt_ranges[3];
	size_t required_npt_pages = 0;

	/* Exact pool sizing for one leaf, split FlatView ranges and boundaries. */
	npt_ranges[0] = (amdvm_npt_range_t) { 0, PAGE_BYTES };
	assert(amdvm_npt_required_pages(npt_ranges, 1, 48,
	    &required_npt_pages) == AMDVM_NPT_OK);
	assert(required_npt_pages == 4u);
	assert_npt_pool_size_matches_mapping(npt_ranges, 1);
	npt_ranges[0] = (amdvm_npt_range_t) { 0, PAGE_BYTES };
	npt_ranges[1] = (amdvm_npt_range_t) { 0x100000u, PAGE_BYTES };
	assert(amdvm_npt_required_pages(npt_ranges, 2, 48,
	    &required_npt_pages) == AMDVM_NPT_OK);
	assert(required_npt_pages == 4u);
	assert_npt_pool_size_matches_mapping(npt_ranges, 2);
	npt_ranges[0] = (amdvm_npt_range_t) { 0x1ff000u, 2u * PAGE_BYTES };
	assert(amdvm_npt_required_pages(npt_ranges, 1, 48,
	    &required_npt_pages) == AMDVM_NPT_OK);
	assert(required_npt_pages == 5u);
	assert_npt_pool_size_matches_mapping(npt_ranges, 1);
	npt_ranges[0] = (amdvm_npt_range_t) { 0x3ffff000u, 2u * PAGE_BYTES };
	assert(amdvm_npt_required_pages(npt_ranges, 1, 48,
	    &required_npt_pages) == AMDVM_NPT_OK);
	assert(required_npt_pages == 6u);
	assert_npt_pool_size_matches_mapping(npt_ranges, 1);
	npt_ranges[0] = (amdvm_npt_range_t) { 0x7ffffff000u, 2u * PAGE_BYTES };
	assert(amdvm_npt_required_pages(npt_ranges, 1, 48,
	    &required_npt_pages) == AMDVM_NPT_OK);
	assert(required_npt_pages == 7u);
	assert_npt_pool_size_matches_mapping(npt_ranges, 1);
	npt_ranges[0] = (amdvm_npt_range_t) { 0, 2u * PAGE_BYTES };
	npt_ranges[1] = (amdvm_npt_range_t) { PAGE_BYTES, PAGE_BYTES };
	assert(amdvm_npt_required_pages(npt_ranges, 2, 48,
	    &required_npt_pages) == AMDVM_NPT_INVALID);
	npt_ranges[0] = (amdvm_npt_range_t) { PAGE_BYTES, PAGE_BYTES };
	npt_ranges[1] = (amdvm_npt_range_t) { 0, PAGE_BYTES };
	assert(amdvm_npt_required_pages(npt_ranges, 2, 48,
	    &required_npt_pages) == AMDVM_NPT_INVALID);
	npt_ranges[0] = (amdvm_npt_range_t) { 0, PAGE_BYTES + 1u };
	assert(amdvm_npt_required_pages(npt_ranges, 1, 48,
	    &required_npt_pages) == AMDVM_NPT_INVALID);

	assert(posix_memalign((void **)&pool, PAGE_BYTES, POOL_BYTES) == 0);
	expected_pool_pa = POOL_PA;
	expected_pool_pages = POOL_BYTES / PAGE_BYTES;
	memset(pool, 0xa5, POOL_BYTES);
	assert(amdvm_npt_init(&npt, pool, POOL_PA, 4, 32, 48) == AMDVM_NPT_OK);
	/* The builder must reject each page in its own control-table allocation. */
	for (unsigned i = 0; i < 4u; ++i) {
		assert(amdvm_npt_map_4k(&npt, 0, POOL_PA + i * PAGE_BYTES,
		    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_INVALID);
	}
	expected_gpas[0] = 0;
	expected_hpas[0] = PAYLOAD_PA;
	expected_count = 1;
	expected_leaf_flags[0] = UINT64_C(5);
	assert(amdvm_npt_map_4k(&npt, 0, PAYLOAD_PA,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK);
	assert(isolated(pool));
	assert(amdvm_npt_validate_single_4k(&npt, 0, PAYLOAD_PA,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK);
	amdvm_npt_translation_t translation = {0};
	assert(amdvm_npt_translate_4k(&npt, 0, &translation) == AMDVM_NPT_OK);
	assert(translation.host_physical_address == PAYLOAD_PA);
	assert(translation.permissions == (AMDVM_NPT_READ | AMDVM_NPT_EXECUTE));
	assert(amdvm_npt_map_4k(&npt, 0, PAYLOAD_PA,
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_CONFLICT);

	uint64_t *root = (uint64_t *)pool;
	uint64_t *leaf = (uint64_t *)(pool + 3u * PAGE_BYTES);
	uint64_t valid_leaf = leaf[0], valid_root = root[0];
	leaf[1] = UINT64_C(0x107000) | 5u; /* Inject a host XSTATE/control page. */
	assert_rejected(&npt);
	leaf[1] = 0;
	leaf[0] |= 2u; /* Guest write permission is forbidden for this payload. */
	assert_rejected(&npt);
	leaf[0] = valid_leaf | (UINT64_C(1) << 63); /* Payload must execute. */
	assert_rejected(&npt);
	leaf[0] = POOL_PA | 5u; /* Substitute an NPT control page as guest RAM. */
	assert_rejected(&npt);
	leaf[0] = valid_leaf;
	root[0] = POOL_PA | 7u; /* Circular table chain. */
	assert_rejected(&npt);
	root[0] = UINT64_C(0x200000) | 7u; /* Outside the retained pool. */
	assert_rejected(&npt);
	root[0] = valid_root | (UINT64_C(1) << 7); /* Unexpected large-page bit. */
	assert_rejected(&npt);
	root[0] = valid_root;
	assert(isolated(pool));

	/* QEMU's reset vector 0xfffffff0 is in this final 4 KiB GPA page. */
	memset(pool, 0, POOL_BYTES);
	expected_gpas[0] = AMDVM_VCPU_RESET_ROM_GPA;
	expected_hpas[0] = PAYLOAD_PA;
	expected_count = 1;
	expected_leaf_flags[0] = UINT64_C(5);
	assert(reset_linear_address == UINT64_C(0xfffffff0));
	assert((reset_linear_address & ~(uint64_t)(PAGE_BYTES - 1u)) == expected_gpas[0]);
	assert((reset_linear_address & (PAGE_BYTES - 1u)) ==
	    AMDVM_VCPU_RESET_ROM_OFFSET);
	memcpy(reset_rom_page + AMDVM_VCPU_RESET_ROM_OFFSET,
	    reset_rom_code, sizeof(reset_rom_code));
	assert(memcmp(reset_rom_page + AMDVM_VCPU_RESET_ROM_OFFSET, reset_rom_code,
	    sizeof(reset_rom_code)) == 0);
	assert(amdvm_npt_init(&npt, pool, POOL_PA, 4, 32, 48) == AMDVM_NPT_OK);
	assert(amdvm_npt_map_4k(&npt, expected_gpas[0], expected_hpas[0],
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK);
	assert(isolated(pool));
	assert(amdvm_npt_validate_single_4k(&npt, expected_gpas[0], expected_hpas[0],
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK);
	assert(amdvm_npt_translate_4k(&npt, expected_gpas[0], &translation) ==
	    AMDVM_NPT_OK);
	assert(translation.host_physical_address == expected_hpas[0]);
	assert(translation.permissions == (AMDVM_NPT_READ | AMDVM_NPT_EXECUTE));
	/* The entry GPA resolves to the actual backing page plus ROM offset 0xff0. */
	assert((expected_gpas[0] + AMDVM_VCPU_RESET_ROM_OFFSET) ==
	    UINT64_C(0xfffffff0));
	assert((translation.host_physical_address + AMDVM_VCPU_RESET_ROM_OFFSET) ==
	    (PAYLOAD_PA + AMDVM_VCPU_RESET_ROM_OFFSET));

	/* Map RAM from non-contiguous pinned pages alongside the reset ROM GPA. */
	assert(posix_memalign((void **)&large_pool, PAGE_BYTES,
	    LARGE_POOL_BYTES) == 0);
	memset(large_pool, 0, LARGE_POOL_BYTES);
	assert(amdvm_npt_init(&npt, large_pool, UINT64_C(0x300000),
	    LARGE_POOL_PAGES, 48, 48) == AMDVM_NPT_OK);
	expected_pool_pa = UINT64_C(0x300000);
	expected_pool_pages = LARGE_POOL_PAGES;
	for (unsigned i = 0; i < 8u; ++i) {
		expected_gpas[i] = UINT64_C(0x100000) +
		    (uint64_t)i * PAGE_BYTES;
		scattered_pages[i] = UINT64_C(0x400000) +
		    (uint64_t)i * 2u * PAGE_BYTES;
		invalid_scattered_pages[i] = scattered_pages[i];
	}
	invalid_scattered_pages[7] += 1u; /* Reject before installing any leaf. */
	assert(amdvm_npt_map_4k_pages(&npt, UINT64_C(0x100000),
	    invalid_scattered_pages, 8, AMDVM_NPT_READ | AMDVM_NPT_WRITE |
	    AMDVM_NPT_EXECUTE) == AMDVM_NPT_INVALID);
	assert(npt.pages_used == 1u);
	assert(((const uint64_t *)large_pool)[0] == 0);
	assert(amdvm_npt_init(&npt, large_pool, UINT64_C(0x300000),
	    LARGE_POOL_PAGES, 48, 48) == AMDVM_NPT_OK);
	expected_gpas[8] = UINT64_C(0xfffff000);
	expected_hpas[8] = UINT64_C(0x500000);
	memcpy(expected_hpas, scattered_pages, sizeof(scattered_pages));
	expected_count = 9;
	for (unsigned i = 0; i < 8u; ++i) {
		expected_leaf_flags[i] = UINT64_C(7); /* Guest RAM is RWX. */
	}
	expected_leaf_flags[8] = UINT64_C(5); /* Reset ROM is RX. */
	assert(amdvm_npt_map_4k_pages(&npt, expected_gpas[0], scattered_pages,
	    8, AMDVM_NPT_READ | AMDVM_NPT_WRITE | AMDVM_NPT_EXECUTE) ==
	    AMDVM_NPT_OK);
	assert(amdvm_npt_map_4k(&npt, expected_gpas[8], expected_hpas[8],
	    AMDVM_NPT_READ | AMDVM_NPT_EXECUTE) == AMDVM_NPT_OK);
	/* The independent walk checks only these eight RAM pages and reset page. */
	struct walk_result combined = {0};
	walk(large_pool, UINT64_C(0x300000), 4u, 0, &combined);
	assert(!combined.invalid && combined.leaves == expected_count);
	for (unsigned i = 0; i < expected_count; ++i) {
		/* Ensure every expected GPA was found by the independent tree walk. */
		int found = 0;
		/* A direct lookup cross-checks GPA-to-HPA leaves. */
		uint64_t gpa = expected_gpas[i];
		uint64_t table_pa = UINT64_C(0x300000);
		for (unsigned level = 4; level > 1; --level) {
			const uint64_t *table = (const uint64_t *)(large_pool +
			    table_pa - UINT64_C(0x300000));
			unsigned index = (unsigned)((gpa >> (12u + 9u * (level - 1u))) & 0x1ffu);
			uint64_t entry = table[index];
			assert((entry & 1u) != 0 && (entry & 0x80u) == 0);
			table_pa = entry & ADDRESS_MASK;
		}
		const uint64_t *leaf_table = (const uint64_t *)(large_pool +
		    table_pa - UINT64_C(0x300000));
		uint64_t entry = leaf_table[(gpa >> 12) & 0x1ffu];
		found = (entry & 1u) != 0 &&
		    (entry & ADDRESS_MASK) == expected_hpas[i];
		assert(found);
	}

	/* NPT PAT selector bits must encode PAT:PCD:PWT and preserve the leaf. */
	uint8_t pat_index = 0xff;
	const uint64_t default_host_pat = UINT64_C(0x0007040600070406);
	assert(amdvm_npt_host_pat_index(default_host_pat,
	    AMDVM_NPT_MEMTYPE_WB, &pat_index) == AMDVM_NPT_OK && pat_index == 0);
	assert(amdvm_npt_host_pat_index(default_host_pat,
	    AMDVM_NPT_MEMTYPE_UC, &pat_index) == AMDVM_NPT_OK && pat_index == 3);
	assert(amdvm_npt_host_pat_index(default_host_pat,
	    AMDVM_NPT_MEMTYPE_WC, &pat_index) == AMDVM_NPT_INVALID);
	assert(amdvm_npt_set_4k_pat_index(&npt, expected_gpas[0], 3) ==
	    AMDVM_NPT_OK);
	assert(amdvm_npt_validate_4k_pat_index(&npt, expected_gpas[0], 3) ==
	    AMDVM_NPT_OK);
	assert(amdvm_npt_validate_4k_pat_index(&npt, expected_gpas[0], 0) ==
	    AMDVM_NPT_CONFLICT);
	assert(amdvm_npt_set_4k_pat_index(&npt, UINT64_C(0x200000), 3) ==
	    AMDVM_NPT_CONFLICT); /* Cannot change a missing guest mapping. */
	assert(amdvm_npt_set_4k_pat_index(&npt, expected_gpas[0], 8) ==
	    AMDVM_NPT_INVALID);

	free(pool);
	free(large_pool);
	puts("PASS: independent NPT walks validate the isolated RX payload, the QEMU reset page at GPA 0xfffff000 with ROM bytes at offset 0xff0, and scatter/gather RWX RAM pages; control aliases, extra mappings, permission corruption, circular/out-of-pool tables and large-page entries are rejected.");
	return 0;
}
