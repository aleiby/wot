#pragma once

#include "CoreMinimal.h"

class FWoTRuntimeClassDefaults;
class UWoTVertexAnimComponent;
struct FWoTRuntimeWorldActor;

/** Effective UE1 mesh-actor presentation decoded from instance + class defaults. */
struct WOT_API FWoTRuntimeActorVisualPresentation
{
	FName MeshName;
	FName MeshPackage;
	FSoftObjectPath Skin;
	TMap<int32, FSoftObjectPath> SkinOverrides;
	float DrawScale = 1.0f;
	float ScaleGlow = 1.0f;
	uint8 AmbientGlow = 0;
	uint8 Fatness = 128;
	uint8 Style = 1;
	bool bUnlit = false;
	bool bMeshCurvy = false;
	bool bStyleTranslucent = false;
	bool bRotatingPickup = true;
	FRotator RotationRate = FRotator(0.0, 5000.0 * 360.0 / 65536.0, 0.0);

	bool HasMesh() const { return !MeshName.IsNone() && !MeshPackage.IsNone(); }
};

WOT_API bool WoTRuntimeReadActorVisualPresentation(
	const FWoTRuntimeClassDefaults& Defaults,
	const FWoTRuntimeWorldActor& Actor,
	FWoTRuntimeActorVisualPresentation& Out, FString& OutError);

WOT_API bool WoTRuntimeReadClassVisualPresentation(
	const FWoTRuntimeClassDefaults& Defaults, FStringView ClassIdentity,
	FWoTRuntimeActorVisualPresentation& Out, FString& OutError);

/** Bind through the one guarded/developer mesh API, then apply authored display. */
WOT_API bool WoTRuntimeApplyActorVisualPresentation(
	UWoTVertexAnimComponent& Component,
	const FWoTRuntimeActorVisualPresentation& Presentation,
	FString& OutError);

#if WITH_AUTOMATION_TESTS
WOT_API bool WoTRuntimeApplyBoundActorVisualPresentationForTesting(
	UWoTVertexAnimComponent& Component,
	const FWoTRuntimeActorVisualPresentation& Presentation,
	FString& OutError);
#endif
