#include "WoTPresentationPolicy.h"

#include "Engine/World.h"
#include "Misc/CoreMisc.h"

namespace
{
#if WITH_AUTOMATION_TESTS
TOptional<bool> GWoTPresentationDedicatedServerOverride;
#endif
}

bool WoTPresentationAnimationEnabled(const UWorld* World)
{
#if WITH_AUTOMATION_TESTS
	if (GWoTPresentationDedicatedServerOverride.IsSet())
	{
		return !GWoTPresentationDedicatedServerOverride.GetValue();
	}
#endif
	// The Server target answers at compile time; a listen host or a client
	// answers through the world's net driver (the same check the engine's
	// AActor::GetNetMode performs).
	if (IsRunningDedicatedServer())
	{
		return false;
	}
	return World == nullptr || World->GetNetMode() != NM_DedicatedServer;
}

#if WITH_AUTOMATION_TESTS
void WoTSetPresentationDedicatedServerOverrideForTesting(
	const TOptional<bool> bDedicatedServer)
{
	GWoTPresentationDedicatedServerOverride = bDedicatedServer;
}
#endif
