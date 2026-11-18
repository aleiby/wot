#pragma once

#include "CoreMinimal.h"

namespace WoTSha256
{
	// Hashes a contiguous byte range and returns the 32-byte digest as lowercase
	// hexadecimal. A null pointer is valid only for an empty range.
	FString HashBuffer(const void* Data, uint64 Size);
}
