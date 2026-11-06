#pragma once

#include "CoreMinimal.h"

namespace WoTPasswordToken
{
	// Join credentials use a versioned, project-specific hash domain. Host
	// passwords remain plaintext in host state so the RPC AdminLogin contract is
	// unchanged; only the URL login boundary derives this token.
	FString Derive(const FString& Password);
}
