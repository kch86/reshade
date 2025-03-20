#include "hash.h"

#define XXH_STATIC_LINKING_ONLY   /* access advanced declarations */
#define XXH_IMPLEMENTATION
#include <xxhash/xxhash.h>

namespace hash
{
	uint64_t hash(const void* data, size_t size)
	{
		return XXH3_64bits(data, size);
	}
}

