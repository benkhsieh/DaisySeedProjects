#pragma once
#ifndef HEAP_H
#define HEAP_H

#include <cstdint>

// Bounded heap. libnosys' default _sbrk() has no upper limit, so a leak (every
// `new` that is never `delete`d) grows the heap forever until it runs into
// whatever memory happens to sit above it and faults there with no useful
// diagnostic. This overrides _sbrk() with a bump allocator capped at the end
// of the RAM_D2 bank the heap lives in, so exhaustion is caught here and
// reported as a crash record (cfsr 0x48454150, 'HEAP') instead of a random
// bus fault somewhere downstream.

namespace bkshepherd {

/** Bytes handed out so far: current brk minus the heap's start (`end`). */
uint32_t HeapUsedBytes();

/** Total capacity of the heap: the hard limit minus the heap's start (`end`). */
uint32_t HeapTotalBytes();

} // namespace bkshepherd

#endif
