#ifndef AMDVM_TEST_IOMEMORYDESCRIPTOR_H
#define AMDVM_TEST_IOMEMORYDESCRIPTOR_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t IOByteCount;
typedef uint64_t IOByteCount64;
typedef uint64_t addr64_t;
typedef int IOReturn;
typedef int IODirection;

enum {
	kIOReturnSuccess = 0,
	kIODirectionInOut = 3,
	kIOMemoryMapperNone = 0,
	kIOMemoryHostPhysicallyContiguous = 1u << 0,
};

class IOMemoryDescriptor {
public:
	static IOMemoryDescriptor *withAddressRange(uint64_t, size_t,
	    IODirection, void *);
	virtual IOReturn complete(IODirection);
	virtual IOReturn prepare(IODirection);
	virtual addr64_t getPhysicalSegment(IOByteCount, IOByteCount *,
	    uint32_t);
	virtual IOByteCount getLength();
	virtual void release();
};

#endif
