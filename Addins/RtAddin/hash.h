#pragma once

#include <xxhash/xxhash.h>

#include <stdint.h>

namespace hash
{
	uint64_t hash(void *data, size_t size);

	template<typename type>
	uint64_t hash(const type &t)
	{
		return hash(&t, sizeof(t));
	}
}
