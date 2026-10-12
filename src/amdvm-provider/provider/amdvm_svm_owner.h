#ifndef AMDVM_SVM_OWNER_H
#define AMDVM_SVM_OWNER_H

#include <stdint.h>

#include "amdvm_physmem.h"

typedef struct amdvm_svm_lease {
	uint64_t original_efer;
	uint64_t original_hsave_pa;
	uint64_t owned_hsave_pa;
	uint32_t cpu_index;
	uint32_t active;
} amdvm_svm_lease_t;

enum amdvm_svm_owner_status {
	AMDVM_SVM_OWNER_OK = 0,
	AMDVM_SVM_OWNER_INVALID = -1,
	AMDVM_SVM_OWNER_UNSUPPORTED = -2,
	AMDVM_SVM_OWNER_DISABLED_BY_FIRMWARE = -3,
	AMDVM_SVM_OWNER_BUSY = -4,
	AMDVM_SVM_OWNER_VERIFY_FAILED = -5,
	AMDVM_SVM_OWNER_CPU_MISMATCH = -6,
	AMDVM_SVM_OWNER_STATE_CHANGED = -7,
	AMDVM_SVM_OWNER_ROLLBACK_FAILED = -8
};

/*
 * The caller must zero-initialize the lease and pin the current CPU for its
 * entire lifetime, normally by
 * disabling preemption before acquire and reenabling it after release.
 * The supplied region must remain allocated until release succeeds.
 */
int amdvm_svm_owner_acquire_current_cpu(amdvm_svm_lease_t *lease,
	const amdvm_physmem_region_t *hsave_region);
int amdvm_svm_owner_release_current_cpu(amdvm_svm_lease_t *lease);

#endif
