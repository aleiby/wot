#pragma once

#include "CoreMinimal.h"

namespace WoTNetVersion
{
	uint32 ComputeFor(const FString& ReleaseCommit);
	FString GetReleaseCommit();
	uint32 Current();
	void Bind();
}
