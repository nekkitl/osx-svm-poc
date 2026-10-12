#ifndef AMDVM_TEST_TASK_REF_H
#define AMDVM_TEST_TASK_REF_H

typedef void *task_t;
void task_reference(task_t);
void task_deallocate(task_t);

#endif
