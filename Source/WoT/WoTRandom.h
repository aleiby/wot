#pragma once

#include "CoreMinimal.h"

// The single gameplay random stream. FRandomStream's algorithm is stable
// across the supported platforms; callers must not bypass it with FMath RNGs.
namespace WoTRandom
{
	WOT_API void InitializeFromTime();
	WOT_API void Seed(int32 InSeed);

	WOT_API uint32 GetUnsignedInt();
	WOT_API float FRand();
	WOT_API float FRandRange(float Min, float Max);
	WOT_API double FRandRange(double Min, double Max);
	WOT_API int32 RandHelper(int32 A);
	WOT_API int32 RandRange(int32 Min, int32 Max);
	WOT_API float RandRange(float Min, float Max);
	WOT_API bool RandBool();
	WOT_API FVector VRand();
	WOT_API FVector VRandCone(const FVector& Dir, float ConeHalfAngleRad);
	WOT_API FVector VRandCone(const FVector& Dir, float HorizontalConeHalfAngleRad,
		float VerticalConeHalfAngleRad);
}
