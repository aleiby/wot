#pragma once
#if WITH_AUTOMATION_TESTS && !WOT_GUARDED_BOOTSTRAP
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "IpNetDriver.h"

namespace WoTBotTest
{
// No clients or sockets are needed; the ordinary server gameplay branches are.
struct FSoakServer
{
	UWorld* World;
	UIpNetDriver* Driver = nullptr;
	bool bWasDedicated = false;
	explicit FSoakServer(UWorld* W) : World(W)
	{
		FWorldContext& Context = GEngine->GetWorldContextFromWorldChecked(W);
		bWasDedicated = Context.RunAsDedicated;
		Context.RunAsDedicated = true;
		Driver = NewObject<UIpNetDriver>(W);
		Driver->SetWorld(W);
		// This driver supplies server identity only. It has no connections or
		// transport initialization, so network dispatch/flush must not tick.
		W->OnTickDispatch().RemoveAll(Driver);
		W->OnPostTickDispatch().RemoveAll(Driver);
		W->OnTickFlush().RemoveAll(Driver);
		W->OnPostTickFlush().RemoveAll(Driver);
		World->SetNetDriver(Driver);
	}
	~FSoakServer()
	{
		if (Driver) { World->SetNetDriver(nullptr); Driver->SetWorld(nullptr); }
		GEngine->GetWorldContextFromWorldChecked(World).RunAsDedicated = bWasDedicated;
	}
};
}
#endif
