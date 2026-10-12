#ifndef AMDVM_TEST_IOBUFFERMEMORYDESCRIPTOR_H
#define AMDVM_TEST_IOBUFFERMEMORYDESCRIPTOR_H

#include "IOMemoryDescriptor.h"

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
	static IOBufferMemoryDescriptor *inTaskWithOptions(void *, uint32_t,
	    size_t, size_t);
	virtual void *getBytesNoCopy();
};

#endif
