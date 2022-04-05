#ifndef clox_memory_h
#define clox_memory_h

#include "common.h"

#define GROW_CAPACITY(capacity) \
	((capacity) < 8 ? 8 : (capacity) * 2)

#define GROW_ARRAY(sizeof_type, pointer, oldCount, newCount) \
	(type*)reallocate(pointer, sizeof_type * (oldCount), \
		sizeof(type) * (newCount))

void* reallocate(void* pointer, size_t oldSize, size_t newSize);

#endif // !clox_memory_h

