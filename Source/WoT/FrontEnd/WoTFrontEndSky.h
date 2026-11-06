#pragma once

#include "CoreMinimal.h"
#include "../WoTBSPPayload.h"

class FWoTRuntimeWorldResource;
struct FWoTRetailSkySelection;

enum class EWoTEntryBlend : uint8
{
	Opaque,
	Masked,
	Translucent,
	Modulated,
};

struct FWoTEntrySectionStyle
{
	FString TextureIdentity;
	EWoTEntryBlend Blend = EWoTEntryBlend::Opaque;
	FVector2f PanUV = FVector2f::ZeroVector;
	bool bUnlit = false;
	bool bTwoSided = false;
};

struct FWoTFrontEndSkyPlan
{
	FWoTBSPPayloadData WorldMesh;
	FWoTBSPPayloadData SkyMesh;
	TArray<FWoTEntrySectionStyle> WorldStyles;
	TArray<FWoTEntrySectionStyle> SkyStyles;
	FVector Origin = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float Scale = 12.0f;
};

WOT_API bool WoTBuildFrontEndSkyPlan(
	const FWoTRuntimeWorldResource& World,
	const FWoTBSPPayloadData& BSP,
	FWoTFrontEndSkyPlan& Out,
	FString& Error);

WOT_API float WoTEntrySkyScale(float WorldRadius, float MinFaceDistance);

/** The entry scene's single-sky policy over the shared retail selection
 * (WoTSelectRetailSky): empty when acceptable, else the entry's exact error. */
WOT_API FString WoTEntrySkySelectionPolicyError(const FWoTRetailSkySelection& Selection);
