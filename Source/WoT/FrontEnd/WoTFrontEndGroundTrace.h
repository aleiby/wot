#pragma once

#include "CoreMinimal.h"

struct FWoTRuntimeWorldNode;

enum class EWoTEntryTraceResult : uint8
{
	Hit,
	Miss,
	Invalid,
};

/** Traces a point segment through authenticated UE1 BSP solid space.
 * Successful hits are raw geometric boundaries; caller-specific pullback is
 * deliberately outside this helper. */
WOT_API EWoTEntryTraceResult WoTTraceEntryGround(
	TConstArrayView<FWoTRuntimeWorldNode> Nodes, bool bRootOutside,
	FVector Start, FVector End, FVector& Hit, FString& Error);
