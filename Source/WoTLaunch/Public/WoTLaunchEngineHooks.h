#pragma once

#include "CoreMinimal.h"
#include "WoTEarlyUI.h"

class UWorld;

/** Every guarded engine route that can expose a materialized world. */
enum class EWoTWorldStartPath : uint8
{
	NormalInitialLoad,
	ListenHostTravel,
	RemoteClientTravel,
	ListenHostSeamlessTravel,
	RemoteClientSeamlessTravel,
};

using FWoTGuardedWorldMaterializer = bool (*)(
	UWorld* World, EWoTWorldStartPath Path, FString* Error);
using FWoTGuardedWorldExposureCommitter = bool (*)(
	UWorld* World, FString* Error);

/** Apply the retained bounded projection without consuming the handoff. */
WOTLAUNCH_API bool
WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
	const FWoTEarlyUINativeProjectionSink& Sink, FString* Error);

/**
 * ABI-thin engine bridge. Engine supplies its real system-resolution path and
 * remains the owner of EWindowMode/FSystemResolution; WoTLaunch never imports
 * Engine just to configure a window.
 */
using FWoTEarlyUIEngineProjection = bool (*)(
	uint16 Width, uint16 Height, uint8 WindowMode, void* Context);
extern "C" WOTLAUNCH_API bool
WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration_EngineBridge(
	FWoTEarlyUIEngineProjection Apply, void* Context, FString* Error);

/** Atomically publish the sole process-lifetime prepare/commit callback pair. */
WOTLAUNCH_API bool WoTRegisterGuardedWorldMaterializer(
	FWoTGuardedWorldMaterializer Materializer,
	FWoTGuardedWorldExposureCommitter ExposureCommitter, FString* Error);

/**
 * The one permanent engine-to-project world transition.
 *
 * It deliberately has no package cache or exception. Before registration it
 * fails closed; after registration the callback decides every world separately.
 */
extern "C" WOTLAUNCH_API bool WoTGuardedStartup_MaterializeWorld(
	UWorld* World, EWoTWorldStartPath Path, FString* Error);

/** Commit a prepared candidate only after the engine exposes its context. */
extern "C" WOTLAUNCH_API bool WoTGuardedStartup_CommitWorldExposure(
	UWorld* World, FString* Error);
