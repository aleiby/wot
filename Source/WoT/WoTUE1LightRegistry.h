#pragma once

#include "CoreMinimal.h"
#include "RetailData/WoTRetailBuildContext.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTDynamicLightSubsystem.h"

class FWoTRuntimeClassDefaults;
class UPointLightComponent;

// The authored UE1 LightEffect; each maps to the receiver shape of the same name
// (None is UE1's default incidence lighting).
enum class EWoTUE1LightShape : uint8
{
	None,
	NonIncidence,
	Shell,
};

struct FWoTUE1LightRow
{
	uint8 Brightness = 0;
	uint8 Hue = 0;
	uint8 Saturation = 0;
	float Radius = 0.0f;
	EWoTUE1LightShape Shape = EWoTUE1LightShape::None;
	EWoTDynLightType Type = EWoTDynLightType::Steady;
	uint8 Period = 0;
	uint8 Phase = 0;

	EWoTDynLightShape DynamicShape() const;
};

struct FWoTUE1LightSet
{
	TMap<FName, FWoTUE1LightRow> Rows;
};

struct FWoTUE1LightRegistryBuilder
{
	static constexpr const TCHAR* FamilyName = TEXT("ue1-lights");

	static bool Build(const FWoTRetailBuildContext& Context,
		FWoTUE1LightSet& Out, FString& OutError);
	static FString CanonicalDigest(const FWoTUE1LightSet& Set);
	static void Install(FWoTUE1LightSet&& Set);
	static void SerializeCanonical(const FWoTUE1LightSet& Set,
		TArray<uint8>& Out);
	static bool AreEquivalent(const FWoTUE1LightSet& Built,
		const FWoTUE1LightSet& Expected, FString& OutError);

#if WITH_AUTOMATION_TESTS
	static void Reset();
#endif
};

/** Cited presentation constants for every G45 retail emitter census class. */
namespace WoTUE1LightRegistry
{
	WOT_API const TMap<FName, FWoTUE1LightRow>& All();
	WOT_API const FWoTUE1LightRow* Find(FName ClassName);

	// Shared seam: applies the UE1 component conversion and registers the
	// authored bytes with the dynamic-light receiver subsystem.
	WOT_API bool Configure(UPointLightComponent* Light, FName ClassName);
	WOT_API bool Configure(UPointLightComponent* Light,
		const FWoTUE1LightRow& Row);
}
