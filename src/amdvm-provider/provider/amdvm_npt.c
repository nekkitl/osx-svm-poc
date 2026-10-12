#include "amdvm_npt.h"
#include "amdvm_vmcb.h"

#include <string.h>

#define AMDVM_PTE_PRESENT  UINT64_C(0x001)
#define AMDVM_PTE_WRITE    UINT64_C(0x002)
#define AMDVM_PTE_USER     UINT64_C(0x004)
#define AMDVM_PTE_LARGE    UINT64_C(0x080)
#define AMDVM_PTE_PWT      UINT64_C(0x008)
#define AMDVM_PTE_PCD      UINT64_C(0x010)
#define AMDVM_PTE_PAT      UINT64_C(0x080)
#define AMDVM_PTE_NX       (UINT64_C(1) << 63)
#define AMDVM_PTE_ADDR     UINT64_C(0x000ffffffffff000)
#define AMDVM_PAGE_MASK    (AMDVM_NPT_PAGE_SIZE - 1u)

static uint64_t *
amdvm_table(amdvm_npt_t *npt, size_t page_index)
{
	return (uint64_t *)(void *)(npt->pool + page_index * AMDVM_NPT_PAGE_SIZE);
}

static void
amdvm_sort_u64(uint64_t *values, size_t count)
{
	size_t start, end;
	if (count < 2u) return;
	for (start = count / 2u; start != 0; ) {
		size_t root = --start;
		for (;;) {
			size_t child = root * 2u + 1u;
			if (child >= count) break;
			if (child + 1u < count && values[child] < values[child + 1u])
				++child;
			if (values[root] >= values[child]) break;
			uint64_t tmp = values[root]; values[root] = values[child];
			values[child] = tmp; root = child;
		}
	}
	for (end = count - 1u; end != 0; --end) {
		uint64_t tmp = values[end]; values[end] = values[0]; values[0] = tmp;
		start = 0;
		for (;;) {
			size_t child = start * 2u + 1u;
			if (child >= end) break;
			if (child + 1u < end && values[child] < values[child + 1u])
				++child;
			if (values[start] >= values[child]) break;
			tmp = values[start]; values[start] = values[child];
			values[child] = tmp; start = child;
		}
	}
}

int
amdvm_npt_validate_backing_aliases(uint64_t *encoded_pages, size_t page_count)
{
	size_t i;
	if (encoded_pages == NULL || page_count == 0) return AMDVM_NPT_INVALID;
	amdvm_sort_u64(encoded_pages, page_count);
	for (i = 1; i < page_count; ++i) {
		if ((encoded_pages[i] >> 1) == (encoded_pages[i - 1u] >> 1) &&
		    ((encoded_pages[i] & 1u) == 0 ||
		    (encoded_pages[i - 1u] & 1u) == 0)) {
			return AMDVM_NPT_CONFLICT;
		}
	}
	return AMDVM_NPT_OK;
}

static int
amdvm_alloc_table(amdvm_npt_t *npt, uint64_t *physical_address,
	uint64_t **virtual_address)
{
	size_t index;
	uint64_t pa;

	if (npt->pages_used >= npt->pool_pages) {
		return AMDVM_NPT_NO_SPACE;
	}
	index = npt->pages_used++;
	pa = npt->pool_pa + (uint64_t)index * AMDVM_NPT_PAGE_SIZE;
	if (pa >= npt->hpa_limit || npt->hpa_limit - pa < AMDVM_NPT_PAGE_SIZE) {
		return AMDVM_NPT_INVALID;
	}
	memset(npt->pool + index * AMDVM_NPT_PAGE_SIZE, 0, AMDVM_NPT_PAGE_SIZE);
	*physical_address = pa;
	*virtual_address = amdvm_table(npt, index);
	return AMDVM_NPT_OK;
}

static int
amdvm_next_table(amdvm_npt_t *npt, uint64_t *parent, size_t index,
	uint64_t **child)
{
	uint64_t entry = parent[index];
	if ((entry & AMDVM_PTE_PRESENT) == 0) {
		uint64_t child_pa;
		int rc = amdvm_alloc_table(npt, &child_pa, child);
		if (rc != AMDVM_NPT_OK) {
			return rc;
		}
		parent[index] = (child_pa & AMDVM_PTE_ADDR) |
		    AMDVM_PTE_PRESENT | AMDVM_PTE_WRITE | AMDVM_PTE_USER;
		return AMDVM_NPT_OK;
	}
	if ((entry & AMDVM_PTE_LARGE) != 0) {
		return AMDVM_NPT_CONFLICT;
	}
	uint64_t child_pa = entry & AMDVM_PTE_ADDR;
	if (child_pa < npt->pool_pa || child_pa >=
	    npt->pool_pa + (uint64_t)npt->pages_used * AMDVM_NPT_PAGE_SIZE ||
	    ((child_pa - npt->pool_pa) & AMDVM_PAGE_MASK) != 0) {
		return AMDVM_NPT_CONFLICT;
	}
	*child = amdvm_table(npt,
	    (size_t)((child_pa - npt->pool_pa) / AMDVM_NPT_PAGE_SIZE));
	return AMDVM_NPT_OK;
}

int
amdvm_npt_init(amdvm_npt_t *npt, void *pool, uint64_t pool_pa,
	size_t pool_pages, uint8_t guest_phys_bits, uint8_t host_phys_bits)
{
	uint64_t gpa_limit, hpa_limit;
	if (npt == NULL || pool == NULL || pool_pages == 0 ||
	    ((uintptr_t)pool & AMDVM_PAGE_MASK) != 0 ||
	    (pool_pa & AMDVM_PAGE_MASK) != 0 ||
	    guest_phys_bits < 32 || guest_phys_bits > AMDVM_NPT_MAX_GPA_BITS ||
	    host_phys_bits < 32 || host_phys_bits > 52 ||
	    pool_pages > (UINT64_MAX / AMDVM_NPT_PAGE_SIZE)) {
		return AMDVM_NPT_INVALID;
	}
	gpa_limit = UINT64_C(1) << guest_phys_bits;
	hpa_limit = UINT64_C(1) << host_phys_bits;
	if (pool_pa >= hpa_limit ||
	    (uint64_t)pool_pages * AMDVM_NPT_PAGE_SIZE > hpa_limit - pool_pa) {
		return AMDVM_NPT_INVALID;
	}
	npt->pool = (uint8_t *)pool;
	npt->pool_pa = pool_pa;
	npt->pool_pages = pool_pages;
	npt->pages_used = 1;
	npt->gpa_limit = gpa_limit;
	npt->hpa_limit = hpa_limit;
	memset(npt->pool, 0, AMDVM_NPT_PAGE_SIZE);
	return AMDVM_NPT_OK;
}

int
amdvm_npt_required_pages(const amdvm_npt_range_t *ranges,
	size_t range_count, uint8_t guest_phys_bits, size_t *required_pages)
{
	static const uint8_t shifts[] = { 39u, 30u, 21u };
	uint64_t gpa_limit;
	uint64_t previous_end = 0;
	uint64_t previous_last[sizeof(shifts)] = { 0 };
	uint8_t have_previous[sizeof(shifts)] = { 0 };
	size_t pages = 1u;
	size_t i, level;

	if (ranges == NULL || range_count == 0 || required_pages == NULL ||
	    guest_phys_bits < 32u || guest_phys_bits > AMDVM_NPT_MAX_GPA_BITS) {
		return AMDVM_NPT_INVALID;
	}
	gpa_limit = UINT64_C(1) << guest_phys_bits;
	for (i = 0; i < range_count; ++i) {
		uint64_t start = ranges[i].guest_gpa;
		uint64_t size = ranges[i].size;
		uint64_t end;

		if (size == 0 || (start & AMDVM_PAGE_MASK) != 0 ||
		    (size & AMDVM_PAGE_MASK) != 0 || start >= gpa_limit ||
		    size > gpa_limit - start || (i != 0 && start < previous_end)) {
			return AMDVM_NPT_INVALID;
		}
		end = start + size;
		for (level = 0; level < sizeof(shifts); ++level) {
			uint64_t first = start >> shifts[level];
			uint64_t last = (end - 1u) >> shifts[level];
			uint64_t add;

			if (!have_previous[level] ||
			    first > previous_last[level]) {
				add = last - first + 1u;
			} else if (last > previous_last[level]) {
				add = last - previous_last[level];
			} else {
				add = 0;
			}
			if (add > SIZE_MAX - pages) {
				return AMDVM_NPT_INVALID;
			}
			pages += (size_t)add;
			if (!have_previous[level] || last > previous_last[level]) {
				previous_last[level] = last;
				have_previous[level] = 1;
			}
		}
		previous_end = end;
	}
	*required_pages = pages;
	return AMDVM_NPT_OK;
}

int
amdvm_npt_map_4k(amdvm_npt_t *npt, uint64_t gpa, uint64_t hpa,
	uint32_t permissions)
{
	uint64_t *pml4, *pdpt, *pd, *pt;
	uint64_t leaf, pool_end;
	int rc;
	if (npt == NULL || npt->pool == NULL ||
	    (gpa & AMDVM_PAGE_MASK) != 0 || (hpa & AMDVM_PAGE_MASK) != 0 ||
	    gpa >= npt->gpa_limit || npt->gpa_limit - gpa < AMDVM_NPT_PAGE_SIZE ||
	    hpa >= npt->hpa_limit || npt->hpa_limit - hpa < AMDVM_NPT_PAGE_SIZE ||
	    (permissions & AMDVM_NPT_READ) == 0 ||
	    (permissions & ~(AMDVM_NPT_READ | AMDVM_NPT_WRITE | AMDVM_NPT_EXECUTE)) != 0) {
		return AMDVM_NPT_INVALID;
	}
	pool_end = npt->pool_pa +
	    (uint64_t)npt->pool_pages * AMDVM_NPT_PAGE_SIZE;
	if (hpa >= npt->pool_pa && hpa < pool_end) {
		/* Never let guest RAM alias its own NPT control structures. */
		return AMDVM_NPT_INVALID;
	}
	pml4 = amdvm_table(npt, 0);
	rc = amdvm_next_table(npt, pml4, (gpa >> 39) & 0x1ffu, &pdpt);
	if (rc != AMDVM_NPT_OK) return rc;
	rc = amdvm_next_table(npt, pdpt, (gpa >> 30) & 0x1ffu, &pd);
	if (rc != AMDVM_NPT_OK) return rc;
	rc = amdvm_next_table(npt, pd, (gpa >> 21) & 0x1ffu, &pt);
	if (rc != AMDVM_NPT_OK) return rc;

	size_t const pt_index = (size_t)((gpa >> 12) & 0x1ffu);
	if ((pt[pt_index] & AMDVM_PTE_PRESENT) != 0) {
		return AMDVM_NPT_CONFLICT;
	}
	leaf = hpa & AMDVM_PTE_ADDR;
	leaf |= AMDVM_PTE_PRESENT | AMDVM_PTE_USER;
	if (permissions & AMDVM_NPT_WRITE) leaf |= AMDVM_PTE_WRITE;
	if ((permissions & AMDVM_NPT_EXECUTE) == 0) leaf |= AMDVM_PTE_NX;
	pt[pt_index] = leaf;
	return AMDVM_NPT_OK;
}

int
amdvm_npt_map_4k_pages(amdvm_npt_t *npt, uint64_t guest_gpa,
	const uint64_t *host_page_pas, size_t page_count, uint32_t permissions)
{
	size_t i;
	uint64_t bytes;

	if (npt == NULL || npt->pool == NULL || host_page_pas == NULL ||
	    page_count == 0 || (guest_gpa & AMDVM_PAGE_MASK) != 0 ||
	    page_count > UINT64_MAX / AMDVM_NPT_PAGE_SIZE ||
	    (permissions & AMDVM_NPT_READ) == 0 ||
	    (permissions & ~(AMDVM_NPT_READ | AMDVM_NPT_WRITE |
	    AMDVM_NPT_EXECUTE)) != 0) {
		return AMDVM_NPT_INVALID;
	}
	bytes = (uint64_t)page_count * AMDVM_NPT_PAGE_SIZE;
	if (guest_gpa >= npt->gpa_limit || bytes > npt->gpa_limit - guest_gpa) {
		return AMDVM_NPT_INVALID;
	}

	/* Validate the complete input before making any leaf visible. */
	for (i = 0; i < page_count; ++i) {
		uint64_t hpa = host_page_pas[i];
		uint64_t pool_end = npt->pool_pa +
		    (uint64_t)npt->pool_pages * AMDVM_NPT_PAGE_SIZE;

		if ((hpa & AMDVM_PAGE_MASK) != 0 || hpa >= npt->hpa_limit ||
		    npt->hpa_limit - hpa < AMDVM_NPT_PAGE_SIZE ||
		    (hpa >= npt->pool_pa && hpa < pool_end)) {
			return AMDVM_NPT_INVALID;
		}
	}

	/*
	 * A table-pool exhaustion can still leave earlier leaves installed. The
	 * only supported caller is construction of a new context which is thrown
	 * away in full if this routine returns an error.
	 */
	for (i = 0; i < page_count; ++i) {
		int rc = amdvm_npt_map_4k(npt,
		    guest_gpa + (uint64_t)i * AMDVM_NPT_PAGE_SIZE,
		    host_page_pas[i], permissions);
		if (rc != AMDVM_NPT_OK) {
			return rc;
		}
	}
	return AMDVM_NPT_OK;
}

static int
amdvm_table_has_only_entry(const uint64_t *table, size_t expected_index,
	uint64_t expected_value)
{
	size_t i;
	for (i = 0; i < AMDVM_NPT_ENTRIES; ++i) {
		if (table[i] != (i == expected_index ? expected_value : 0)) {
			return 0;
		}
	}
	return 1;
}

int
amdvm_npt_validate_single_4k(const amdvm_npt_t *npt, uint64_t gpa,
	uint64_t hpa, uint32_t permissions)
{
	const uint64_t *pml4, *pdpt, *pd, *pt;
	size_t pml4_index, pdpt_index, pd_index, pt_index;
	uint64_t parent_flags, leaf;
	uint64_t pool_end;

	if (npt == NULL || npt->pool == NULL || npt->pool_pages != 4u ||
	    npt->pages_used != 4u ||
	    ((uintptr_t)npt->pool & AMDVM_PAGE_MASK) != 0 ||
	    (npt->pool_pa & AMDVM_PAGE_MASK) != 0 ||
	    (gpa & AMDVM_PAGE_MASK) != 0 ||
	    gpa >= npt->gpa_limit ||
	    npt->gpa_limit - gpa < AMDVM_NPT_PAGE_SIZE ||
	    (hpa & AMDVM_PAGE_MASK) != 0 || hpa >= npt->hpa_limit ||
	    npt->hpa_limit - hpa < AMDVM_NPT_PAGE_SIZE ||
	    (permissions & AMDVM_NPT_READ) == 0 ||
	    (permissions & ~(AMDVM_NPT_READ | AMDVM_NPT_WRITE |
	    AMDVM_NPT_EXECUTE)) != 0) {
		return AMDVM_NPT_INVALID;
	}
	pool_end = npt->pool_pa +
	    (uint64_t)npt->pool_pages * AMDVM_NPT_PAGE_SIZE;
	if (hpa >= npt->pool_pa && hpa < pool_end) {
		return AMDVM_NPT_INVALID;
	}

	pml4_index = (size_t)((gpa >> 39) & 0x1ffu);
	pdpt_index = (size_t)((gpa >> 30) & 0x1ffu);
	pd_index = (size_t)((gpa >> 21) & 0x1ffu);
	pt_index = (size_t)((gpa >> 12) & 0x1ffu);
	parent_flags = AMDVM_PTE_PRESENT | AMDVM_PTE_WRITE | AMDVM_PTE_USER;
	leaf = (hpa & AMDVM_PTE_ADDR) | AMDVM_PTE_PRESENT | AMDVM_PTE_USER;
	if (permissions & AMDVM_NPT_WRITE) leaf |= AMDVM_PTE_WRITE;
	if ((permissions & AMDVM_NPT_EXECUTE) == 0) leaf |= AMDVM_PTE_NX;

	pml4 = (const uint64_t *)(const void *)npt->pool;
	pdpt = (const uint64_t *)(const void *)(npt->pool + AMDVM_NPT_PAGE_SIZE);
	pd = (const uint64_t *)(const void *)(npt->pool +
	    2u * AMDVM_NPT_PAGE_SIZE);
	pt = (const uint64_t *)(const void *)(npt->pool +
	    3u * AMDVM_NPT_PAGE_SIZE);
	if (!amdvm_table_has_only_entry(pml4, pml4_index,
	    (npt->pool_pa + AMDVM_NPT_PAGE_SIZE) | parent_flags) ||
	    !amdvm_table_has_only_entry(pdpt, pdpt_index,
	    (npt->pool_pa + 2u * AMDVM_NPT_PAGE_SIZE) | parent_flags) ||
	    !amdvm_table_has_only_entry(pd, pd_index,
	    (npt->pool_pa + 3u * AMDVM_NPT_PAGE_SIZE) | parent_flags) ||
	    !amdvm_table_has_only_entry(pt, pt_index, leaf)) {
		return AMDVM_NPT_CONFLICT;
	}
	return AMDVM_NPT_OK;
}

int
amdvm_npt_validate_4k_mapping(const amdvm_npt_t *npt, uint64_t gpa,
	uint64_t hpa, uint32_t permissions)
{
	const uint64_t *table;
	uint64_t child_pa, expected, pool_end;
	const size_t shifts[] = { 39u, 30u, 21u };
	size_t level;

	if (npt == NULL || npt->pool == NULL || npt->pages_used == 0 ||
	    npt->pages_used > npt->pool_pages ||
	    (gpa & AMDVM_PAGE_MASK) != 0 || gpa >= npt->gpa_limit ||
	    npt->gpa_limit - gpa < AMDVM_NPT_PAGE_SIZE ||
	    (hpa & AMDVM_PAGE_MASK) != 0 || hpa >= npt->hpa_limit ||
	    npt->hpa_limit - hpa < AMDVM_NPT_PAGE_SIZE ||
	    (permissions & AMDVM_NPT_READ) == 0 ||
	    (permissions & ~(AMDVM_NPT_READ | AMDVM_NPT_WRITE |
	    AMDVM_NPT_EXECUTE)) != 0) {
		return AMDVM_NPT_INVALID;
	}
	pool_end = npt->pool_pa +
	    (uint64_t)npt->pool_pages * AMDVM_NPT_PAGE_SIZE;
	if (hpa >= npt->pool_pa && hpa < pool_end) return AMDVM_NPT_INVALID;
	table = (const uint64_t *)(const void *)npt->pool;
	for (level = 0; level < sizeof(shifts) / sizeof(shifts[0]); ++level) {
		uint64_t entry = table[(gpa >> shifts[level]) & 0x1ffu];
		if ((entry & (AMDVM_PTE_PRESENT | AMDVM_PTE_WRITE |
		    AMDVM_PTE_USER)) != (AMDVM_PTE_PRESENT | AMDVM_PTE_WRITE |
		    AMDVM_PTE_USER) || (entry & AMDVM_PTE_LARGE) != 0) {
			return AMDVM_NPT_CONFLICT;
		}
		child_pa = entry & AMDVM_PTE_ADDR;
		if (child_pa < npt->pool_pa || child_pa >=
		    npt->pool_pa + (uint64_t)npt->pages_used * AMDVM_NPT_PAGE_SIZE ||
		    ((child_pa - npt->pool_pa) & AMDVM_PAGE_MASK) != 0) {
			return AMDVM_NPT_CONFLICT;
		}
		table = (const uint64_t *)(const void *)(npt->pool +
		    (size_t)(child_pa - npt->pool_pa));
	}
	expected = (hpa & AMDVM_PTE_ADDR) | AMDVM_PTE_PRESENT | AMDVM_PTE_USER;
	if (permissions & AMDVM_NPT_WRITE) expected |= AMDVM_PTE_WRITE;
	if ((permissions & AMDVM_NPT_EXECUTE) == 0) expected |= AMDVM_PTE_NX;
	if (table[(gpa >> 12) & 0x1ffu] != expected) {
		return AMDVM_NPT_CONFLICT;
	}
	return AMDVM_NPT_OK;
}

int
amdvm_npt_translate_4k(const amdvm_npt_t *npt, uint64_t gpa,
	amdvm_npt_translation_t *translation)
{
	const uint64_t shifts[] = { 39u, 30u, 21u };
	const uint64_t *table;
	uint64_t child_pa, pool_end, effective;
	size_t level;

	if (npt == NULL || translation == NULL || npt->pool == NULL ||
	    npt->pages_used == 0 || npt->pages_used > npt->pool_pages ||
	    (gpa & AMDVM_PAGE_MASK) != 0 || gpa >= npt->gpa_limit ||
	    npt->gpa_limit - gpa < AMDVM_NPT_PAGE_SIZE ||
	    npt->pool_pages > UINT64_MAX / AMDVM_NPT_PAGE_SIZE ||
	    npt->pool_pa > UINT64_MAX -
	    (uint64_t)npt->pool_pages * AMDVM_NPT_PAGE_SIZE) {
		return AMDVM_NPT_INVALID;
	}
	pool_end = npt->pool_pa +
	    (uint64_t)npt->pages_used * AMDVM_NPT_PAGE_SIZE;
	table = (const uint64_t *)(const void *)npt->pool;
	effective = AMDVM_NPT_READ | AMDVM_NPT_WRITE | AMDVM_NPT_EXECUTE;
	for (level = 0; level < sizeof(shifts) / sizeof(shifts[0]); ++level) {
		uint64_t entry = table[(gpa >> shifts[level]) & 0x1ffu];
		if ((entry & AMDVM_PTE_PRESENT) == 0 ||
		    (entry & AMDVM_PTE_LARGE) != 0) {
			return AMDVM_NPT_CONFLICT;
		}
		if ((entry & AMDVM_PTE_WRITE) == 0) {
			effective &= ~AMDVM_NPT_WRITE;
		}
		if ((entry & AMDVM_PTE_NX) != 0) {
			effective &= ~AMDVM_NPT_EXECUTE;
		}
		child_pa = entry & AMDVM_PTE_ADDR;
		if (child_pa < npt->pool_pa || child_pa >= pool_end ||
		    ((child_pa - npt->pool_pa) & AMDVM_PAGE_MASK) != 0) {
			return AMDVM_NPT_CONFLICT;
		}
		table = (const uint64_t *)(const void *)(npt->pool +
		    (size_t)(child_pa - npt->pool_pa));
	}
	{
		uint64_t leaf = table[(gpa >> 12) & 0x1ffu];
		uint64_t hpa_page;
		if ((leaf & AMDVM_PTE_PRESENT) == 0) {
			return AMDVM_NPT_CONFLICT;
		}
		if ((leaf & AMDVM_PTE_WRITE) == 0) {
			effective &= ~AMDVM_NPT_WRITE;
		}
		if ((leaf & AMDVM_PTE_NX) != 0) {
			effective &= ~AMDVM_NPT_EXECUTE;
		}
		hpa_page = leaf & AMDVM_PTE_ADDR;
		if (hpa_page >= npt->hpa_limit ||
		    npt->hpa_limit - hpa_page < AMDVM_NPT_PAGE_SIZE) {
			return AMDVM_NPT_CONFLICT;
		}
		translation->host_physical_address = hpa_page | (gpa & AMDVM_PAGE_MASK);
		translation->leaf_entry = leaf;
		translation->permissions = (uint32_t)effective;
		translation->reserved = 0;
	}
	return AMDVM_NPT_OK;
}

static int
amdvm_npt_leaf_pointer(amdvm_npt_t *npt, uint64_t gpa, uint64_t **leaf)
{
	uint64_t *table;
	uint64_t child_pa;
	static const unsigned shifts[] = { 39u, 30u, 21u };
	unsigned level;

	if (npt == NULL || npt->pool == NULL || leaf == NULL ||
	    (gpa & AMDVM_PAGE_MASK) != 0 || gpa >= npt->gpa_limit ||
	    npt->gpa_limit - gpa < AMDVM_NPT_PAGE_SIZE ||
	    npt->pages_used == 0 || npt->pages_used > npt->pool_pages) {
		return AMDVM_NPT_INVALID;
	}
	table = (uint64_t *)(void *)npt->pool;
	for (level = 0; level < sizeof(shifts) / sizeof(shifts[0]); ++level) {
		uint64_t entry = table[(gpa >> shifts[level]) & 0x1ffu];
		if ((entry & (AMDVM_PTE_PRESENT | AMDVM_PTE_WRITE |
		    AMDVM_PTE_USER)) != (AMDVM_PTE_PRESENT | AMDVM_PTE_WRITE |
		    AMDVM_PTE_USER) || (entry & AMDVM_PTE_LARGE) != 0) {
			return AMDVM_NPT_CONFLICT;
		}
		child_pa = entry & AMDVM_PTE_ADDR;
		if (child_pa < npt->pool_pa || child_pa >= npt->pool_pa +
		    (uint64_t)npt->pages_used * AMDVM_NPT_PAGE_SIZE ||
		    ((child_pa - npt->pool_pa) & AMDVM_PAGE_MASK) != 0) {
			return AMDVM_NPT_CONFLICT;
		}
		table = (uint64_t *)(void *)(npt->pool +
		    (size_t)(child_pa - npt->pool_pa));
	}
	*leaf = &table[(gpa >> 12) & 0x1ffu];
	/* In a 4 KiB leaf, bit 7 is PAT; LARGE is only meaningful in parents. */
	if ((**leaf & AMDVM_PTE_PRESENT) == 0) {
		return AMDVM_NPT_CONFLICT;
	}
	return AMDVM_NPT_OK;
}

int
amdvm_npt_set_4k_pat_index(amdvm_npt_t *npt, uint64_t gpa,
	uint8_t pat_index)
{
	uint64_t *leaf;
	uint64_t bits = 0;
	int rc;

	if (pat_index > 7u) return AMDVM_NPT_INVALID;
	rc = amdvm_npt_leaf_pointer(npt, gpa, &leaf);
	if (rc != AMDVM_NPT_OK) return rc;
	if (pat_index & 1u) bits |= AMDVM_PTE_PWT;
	if (pat_index & 2u) bits |= AMDVM_PTE_PCD;
	if (pat_index & 4u) bits |= AMDVM_PTE_PAT;
	*leaf = (*leaf & ~(AMDVM_PTE_PWT | AMDVM_PTE_PCD | AMDVM_PTE_PAT)) |
	    bits;
	return AMDVM_NPT_OK;
}

int
amdvm_npt_validate_4k_pat_index(const amdvm_npt_t *npt, uint64_t gpa,
	uint8_t pat_index)
{
	uint64_t *leaf;
	uint64_t bits = 0;
	int rc;

	if (pat_index > 7u) return AMDVM_NPT_INVALID;
	rc = amdvm_npt_leaf_pointer((amdvm_npt_t *)(uintptr_t)npt, gpa, &leaf);
	if (rc != AMDVM_NPT_OK) return rc;
	if (pat_index & 1u) bits |= AMDVM_PTE_PWT;
	if (pat_index & 2u) bits |= AMDVM_PTE_PCD;
	if (pat_index & 4u) bits |= AMDVM_PTE_PAT;
	return ((*leaf & (AMDVM_PTE_PWT | AMDVM_PTE_PCD | AMDVM_PTE_PAT)) == bits) ?
	    AMDVM_NPT_OK : AMDVM_NPT_CONFLICT;
}

int
amdvm_npt_get_4k_pat_index(const amdvm_npt_t *npt, uint64_t gpa,
	uint8_t *pat_index)
{
	uint64_t *leaf;
	uint64_t entry;
	int rc;

	if (pat_index == NULL) return AMDVM_NPT_INVALID;
	rc = amdvm_npt_leaf_pointer((amdvm_npt_t *)(uintptr_t)npt, gpa, &leaf);
	if (rc != AMDVM_NPT_OK) return rc;
	entry = *leaf;
	*pat_index = (uint8_t)(((entry & AMDVM_PTE_PAT) ? 4u : 0u) |
	    ((entry & AMDVM_PTE_PCD) ? 2u : 0u) |
	    ((entry & AMDVM_PTE_PWT) ? 1u : 0u));
	return AMDVM_NPT_OK;
}

/* UC, UC-, WC, WP, WT, WB; 8 denotes the internal WC+ composition. */
static int
amdvm_pat_ordinal(uint8_t type)
{
	switch (type) {
	case 0: return 0; case 7: return 1; case 1: return 2;
	case 5: return 3; case 4: return 4; case 6: return 5;
	default: return -1;
	}
}

static int
amdvm_pat_combine(uint8_t guest, uint8_t host)
{
	static const uint8_t table[6][6] = {
		{0,0,0,0,0,0}, {0,7,1,0,0,0}, {1,1,1,8,8,8},
		{0,0,0,5,0,5}, {0,0,0,0,4,4}, {0,0,1,5,4,6}
	};
	int g = amdvm_pat_ordinal(guest), h = amdvm_pat_ordinal(host);
	return g < 0 || h < 0 ? -1 : table[g][h];
}

int
amdvm_npt_plan_pat(uint64_t host_pat, uint64_t guest_pat,
	const uint8_t *types, size_t count, uint8_t *indexes,
	uint64_t *execution_pat)
{
	static const uint8_t candidates[] = {0,7,1,5,4,6};
	uint64_t result = 0;
	uint8_t wb_index;
	size_t i;
	unsigned slot, candidate;
	int direct = 1;
	if (!types || !count || !indexes || !execution_pat) return AMDVM_NPT_INVALID;
	for (slot = 0; slot < 8u; ++slot) {
		if (amdvm_pat_ordinal((uint8_t)(guest_pat >> (slot * 8u))) < 0 ||
		    amdvm_pat_ordinal((uint8_t)(host_pat >> (slot * 8u))) < 0)
			return AMDVM_NPT_INVALID;
	}
	for (i = 0; i < count; ++i) {
		if (amdvm_pat_ordinal(types[i]) < 0 || types[i] == 7)
			return AMDVM_NPT_INVALID;
		if (amdvm_npt_host_pat_index(host_pat, types[i], &indexes[i]) != AMDVM_NPT_OK)
			direct = 0;
	}
	if (direct) { *execution_pat = guest_pat; return AMDVM_NPT_OK; }
	if (amdvm_npt_host_pat_index(host_pat, 6, &wb_index) != AMDVM_NPT_OK)
		return AMDVM_NPT_INVALID;
	/* A single execution gPAT must preserve each architectural PAT slot for
	 * every NPT range. Compare symbols including WC+; never approximate UC/WB. */
	for (slot = 0; slot < 8u; ++slot) {
		uint8_t original = (uint8_t)(guest_pat >> (slot * 8u));
		int found = 0;
		for (candidate = 0; candidate < sizeof(candidates); ++candidate) {
			int actual = amdvm_pat_combine(candidates[candidate], 6);
			for (i = 0; i < count; ++i) {
				if (actual != amdvm_pat_combine(original, types[i])) break;
			}
			if (i == count) {
				result |= (uint64_t)candidates[candidate] << (slot * 8u);
				found = 1; break;
			}
		}
		if (!found) return AMDVM_NPT_INVALID;
	}
	for (i = 0; i < count; ++i) indexes[i] = wb_index;
	*execution_pat = result;
	return AMDVM_NPT_OK;
}

int
amdvm_npt_host_pat_index(uint64_t host_pat, uint8_t memory_type,
	uint8_t *pat_index)
{
	unsigned i;

	if (pat_index == NULL ||
	    (memory_type != AMDVM_NPT_MEMTYPE_UC &&
	     memory_type != AMDVM_NPT_MEMTYPE_WC &&
	     memory_type != AMDVM_NPT_MEMTYPE_WT &&
	     memory_type != AMDVM_NPT_MEMTYPE_WP &&
	     memory_type != AMDVM_NPT_MEMTYPE_WB)) {
		return AMDVM_NPT_INVALID;
	}
	for (i = 0; i < 8u; ++i) {
		if (((host_pat >> (i * 8u)) & 0xffu) == memory_type) {
			*pat_index = (uint8_t)i;
			return AMDVM_NPT_OK;
		}
	}
	return AMDVM_NPT_INVALID;
}

static int
amdvm_npt_pat_set(amdvm_npt_pat_set_callback_t callback,
	void *callback_context, amdvm_npt_t *npt, uint64_t gpa,
	uint8_t pat_index)
{
	return callback == NULL ? amdvm_npt_set_4k_pat_index(npt, gpa,
	    pat_index) : callback(callback_context, npt, gpa, pat_index);
}

int
amdvm_npt_update_pat_ranges_atomic(amdvm_npt_t *npt,
	const amdvm_npt_pat_range_t *ranges, size_t range_count,
	uint8_t *old_pat_indexes, size_t page_count,
	amdvm_npt_pat_set_callback_t set_callback, void *callback_context)
{
	size_t total_pages = 0, cursor = 0, range_index;
	uint64_t previous_end = 0;
	if (npt == NULL || ranges == NULL || range_count == 0 ||
	    old_pat_indexes == NULL || page_count == 0) return AMDVM_NPT_INVALID;
	for (range_index = 0; range_index < range_count; ++range_index) {
		uint64_t gpa = ranges[range_index].guest_gpa;
		uint64_t size = ranges[range_index].size;
		uint64_t pages;
		if (size == 0 || (gpa & AMDVM_PAGE_MASK) != 0 ||
		    (size & AMDVM_PAGE_MASK) != 0 || gpa > UINT64_MAX - size ||
		    ranges[range_index].pat_index > 7u ||
		    (range_index != 0 && gpa < previous_end) ||
		    memcmp(ranges[range_index].reserved, (uint8_t[7]){0}, 7u) != 0) {
			return AMDVM_NPT_INVALID;
		}
		pages = size / AMDVM_NPT_PAGE_SIZE;
		if (pages > SIZE_MAX - total_pages) return AMDVM_NPT_INVALID;
		total_pages += (size_t)pages;
		previous_end = gpa + size;
	}
	if (total_pages != page_count) return AMDVM_NPT_INVALID;
	for (range_index = 0; range_index < range_count; ++range_index) {
		uint64_t gpa = ranges[range_index].guest_gpa;
		uint64_t pages = ranges[range_index].size / AMDVM_NPT_PAGE_SIZE;
		uint64_t page;
		for (page = 0; page < pages; ++page) {
			uint64_t page_gpa = gpa + page * AMDVM_NPT_PAGE_SIZE;
			if (amdvm_npt_get_4k_pat_index(npt, page_gpa,
			    &old_pat_indexes[cursor++]) != AMDVM_NPT_OK) {
				return AMDVM_NPT_CONFLICT;
			}
		}
	}
	cursor = 0;
	for (range_index = 0; range_index < range_count; ++range_index) {
		uint64_t pages = ranges[range_index].size / AMDVM_NPT_PAGE_SIZE;
		uint64_t page;
		for (page = 0; page < pages; ++page) {
			uint64_t gpa = ranges[range_index].guest_gpa +
			    page * AMDVM_NPT_PAGE_SIZE;
			if (amdvm_npt_pat_set(set_callback, callback_context, npt, gpa,
			    ranges[range_index].pat_index) != AMDVM_NPT_OK) {
				int rollback_failed = 0;
				size_t j = range_index + 1u;
				size_t applied = cursor;
				while (j != 0) {
					size_t current = --j;
					size_t count = (size_t)(ranges[current].size /
					    AMDVM_NPT_PAGE_SIZE);
					size_t limit = current == range_index ? (size_t)page : count;
					while (limit != 0) {
						size_t index = --applied;
						uint64_t rollback_gpa;
						--limit;
						rollback_gpa = ranges[current].guest_gpa +
						    (uint64_t)limit * AMDVM_NPT_PAGE_SIZE;
						if (amdvm_npt_pat_set(set_callback, callback_context,
						    npt, rollback_gpa, old_pat_indexes[index]) !=
						    AMDVM_NPT_OK) rollback_failed = 1;
					}
				}
				return rollback_failed ? AMDVM_NPT_ROLLBACK_FAILED :
				    AMDVM_NPT_CONFLICT;
			}
			++cursor;
		}
	}
	return AMDVM_NPT_OK;
}

uint64_t
amdvm_npt_root_pa(const amdvm_npt_t *npt)
{
	if (npt == NULL || npt->pool == NULL || npt->pages_used == 0) {
		return 0;
	}
	return npt->pool_pa;
}
