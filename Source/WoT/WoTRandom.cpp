#include "WoTRandom.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

namespace
{
	FCriticalSection GWoTRandomLock;
	FRandomStream GWoTRandomStream(0);

	// Retail appFrand() is rand() / (FLOAT)RAND_MAX
	// with the shipped Win32 CRT's RAND_MAX = 32767: 32768 evenly spaced
	// values in [0,1], each endpoint drawn 1 time in 32768. Draw on that same
	// 15-bit grid from the platform-independent stream, so truncated ranges
	// (for example the rare maximum initial artifact charge) keep retail odds.
	constexpr uint32 RetailRandMax = 32767u;

	double NextInclusiveFraction()
	{
		return static_cast<double>(GWoTRandomStream.GetUnsignedInt() >> 17)
			/ static_cast<double>(RetailRandMax);
	}
}

void WoTRandom::InitializeFromTime()
{
	Seed(static_cast<int32>(FPlatformTime::Cycles()));
}

void WoTRandom::Seed(const int32 InSeed)
{
	FScopeLock Lock(&GWoTRandomLock);
	GWoTRandomStream.Initialize(InSeed);
}

uint32 WoTRandom::GetUnsignedInt()
{
	FScopeLock Lock(&GWoTRandomLock);
	return GWoTRandomStream.GetUnsignedInt();
}

float WoTRandom::FRand()
{
	FScopeLock Lock(&GWoTRandomLock);
	return static_cast<float>(NextInclusiveFraction());
}

float WoTRandom::FRandRange(const float Min, const float Max)
{
	FScopeLock Lock(&GWoTRandomLock);
	return static_cast<float>(static_cast<double>(Min)
		+ static_cast<double>(Max - Min) * NextInclusiveFraction());
}

double WoTRandom::FRandRange(const double Min, const double Max)
{
	FScopeLock Lock(&GWoTRandomLock);
	return Min + (Max - Min) * NextInclusiveFraction();
}

int32 WoTRandom::RandHelper(const int32 A)
{
	FScopeLock Lock(&GWoTRandomLock);
	return GWoTRandomStream.RandHelper(A);
}

int32 WoTRandom::RandRange(const int32 Min, const int32 Max)
{
	FScopeLock Lock(&GWoTRandomLock);
	return GWoTRandomStream.RandRange(Min, Max);
}

float WoTRandom::RandRange(const float Min, const float Max)
{
	return FRandRange(Min, Max);
}

bool WoTRandom::RandBool()
{
	FScopeLock Lock(&GWoTRandomLock);
	return GWoTRandomStream.RandBool();
}

FVector WoTRandom::VRand()
{
	FScopeLock Lock(&GWoTRandomLock);
	return GWoTRandomStream.VRand();
}

FVector WoTRandom::VRandCone(const FVector& Dir, const float ConeHalfAngleRad)
{
	FScopeLock Lock(&GWoTRandomLock);
	return GWoTRandomStream.VRandCone(Dir, ConeHalfAngleRad);
}

FVector WoTRandom::VRandCone(const FVector& Dir,
	const float HorizontalConeHalfAngleRad, const float VerticalConeHalfAngleRad)
{
	FScopeLock Lock(&GWoTRandomLock);
	return GWoTRandomStream.VRandCone(
		Dir, HorizontalConeHalfAngleRad, VerticalConeHalfAngleRad);
}
