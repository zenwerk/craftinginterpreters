#ifndef clox_memory_h
#define clox_memory_h

#include "common.h"
#include "object.h"

#define ALLOCATE(type, count) \
    (type*)reallocate(NULL, 0, sizeof(type) * (count))

#define FREE(type, pointer) reallocate(pointer, sizeof(type), 0)

// 2倍に新調する
#define GROW_CAPACITY(capacity) \
    ((capacity) < 8 ? 8 : (capacity) * 2)

// newCount の長さにメモリ領域を伸長する
#define GROW_ARRAY(type, pointer, oldCount, newCount) \
    (type*)reallocate(pointer, sizeof(type) * (oldCount), \
        sizeof(type) * (newCount))

#define FREE_ARRAY(type, pointer, oldCount) \
    reallocate(pointer, sizeof(type) * (oldCount), 0)

// メモリ管理関数 (fin-lang の alloc_func に通じるものがある)
void *reallocate(void *pointer, size_t oldSize, size_t newSize);

void markObject(Obj *object);

void markValue(Value value);

void collectGarbage();

void freeObjects();

#endif
