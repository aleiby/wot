#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "WoTDynamicLightSubsystem.generated.h"

class AActor;
class UMaterialParameterCollection;
class UPointLightComponent;
class UWoTDynamicLightSubsystem;
class UWoTMapLightTable;

// The receiver's spatial function. The value travels to the materials as the
// slot colour's alpha (0, 1, 2); see Tools/import/dynamic_light_material.py.
UENUM()
enum class EWoTDynLightShape : uint8
{
	NonIncidence,
	Incidence,
	// LE_Shell: only 0.8 < d/R < 1
	// is lit, peaking at 0.9. AMAPearl's dome light is the one retail user.
	Shell,
};

UENUM()
enum class EWoTDynLightType : uint8
{
	Steady,
	Pulse,
	SubtlePulse,
	Blink,
	Flicker,
};

/** One display-ready live light, shared by selection, MPC packing, and specs. */
struct FWoTDynamicLightSample
{
	TWeakObjectPtr<UPointLightComponent> Component;
	FVector Position = FVector::ZeroVector;
	float Radius = 0.0f;
	FLinearColor Color = FLinearColor::Black;
	float LiveScale = 0.0f;
	uint8 Brightness = 0;
	uint8 Hue = 0;
	uint8 Saturation = 255;
	EWoTDynLightShape Shape = EWoTDynLightShape::NonIncidence;
	EWoTDynLightType Type = EWoTDynLightType::Steady;
	uint64 RegistrationId = 0;
};

/** Eight material-parameter-collection slots. */
struct FWoTDynamicLightSlotValues
{
	TArray<FLinearColor> PosR;
	TArray<FLinearColor> Color;
};

USTRUCT()
struct FWoTDynamicLightTickFunction : public FTickFunction
{
	GENERATED_BODY()

	UWoTDynamicLightSubsystem* Target = nullptr;

	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType,
		ENamedThreads::Type CurrentThread,
		const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override;
	virtual FName DiagnosticContext(bool bDetailed) override;
};

template<>
struct TStructOpsTypeTraits<FWoTDynamicLightTickFunction>
	: public TStructOpsTypeTraitsBase2<FWoTDynamicLightTickFunction>
{
	enum
	{
		WithCopy = false
	};
};

/**
 * World-local registry and bounded material transport for UE1 moving actor
 * lights. Components retain their existing visual-light setup; this subsystem
 * samples their live intensity/radius after script-side animation.
 */
UCLASS()
class WOT_API UWoTDynamicLightSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static constexpr int32 MaxDynamicLights = 8;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	void Register(UPointLightComponent* Comp, uint8 Brightness, uint8 Hue,
		uint8 Saturation,
		EWoTDynLightShape Shape = EWoTDynLightShape::NonIncidence,
		EWoTDynLightType Type = EWoTDynLightType::Steady,
		uint8 Period = 0, uint8 Phase = 0);

	/**
	 * Constructor-safe registration seam. The component tag is a deferred
	 * retry contract for actors whose world subsystem is not reachable until
	 * BeginPlay; an immediately reachable world is registered at once.
	 */
	static void RegisterOrDefer(UPointLightComponent* Comp, uint8 Brightness,
		uint8 Hue, uint8 Saturation,
		EWoTDynLightShape Shape = EWoTDynLightShape::NonIncidence,
		EWoTDynLightType Type = EWoTDynLightType::Steady,
		uint8 Period = 0, uint8 Phase = 0);
	static void RegisterTaggedComponents(AActor* Owner);

	static FLinearColor FGetHSV(uint8 Hue, uint8 Saturation, uint8 Value = 255);
	// Shared authority for retail light types. World, receiver, and mover
	// light-map paths all consume these exact values; callers must not grow a
	// parallel sine/blink implementation.
	static float RetailWaveMean(uint8 RetailType);
	static float RetailWaveAmplitude(uint8 RetailType);
	static float EvaluateRetailWaveform(uint8 RetailType, float TimeSeconds,
		uint8 Period, uint8 Phase, uint64 RegistrationId, uint64 Frame);
	static float EvaluateWaveform(EWoTDynLightType Type, float TimeSeconds,
		uint8 Period, uint8 Phase, uint64 RegistrationId, uint64 Frame);
	static TArray<FWoTDynamicLightSample> SelectDynamicLights(
		const TArray<FWoTDynamicLightSample>& Records,
		const FVector& ViewLocation, int32 MaxSlots = MaxDynamicLights);
	static FWoTDynamicLightSlotValues BuildSlotValues(
		const TArray<FWoTDynamicLightSample>& Records,
		const FVector& ViewLocation, bool bEnabled,
		int32 MaxSlots = MaxDynamicLights);
	static bool AreDynamicActorLightsEnabled();

	// Pure/headless seams: no camera or material asset is required.
	TArray<FWoTDynamicLightSample> GatherForTesting(float TimeSeconds,
		float LevelBrightness);
	TArray<FWoTDynamicLightSample> GatherForTesting(float TimeSeconds);
	int32 GetRegisteredCountForTesting() const { return Records.Num(); }
	void SetMapLightTableForTesting(UWoTMapLightTable* Table);

private:
	friend struct FWoTDynamicLightTickFunction;

	struct FRecord
	{
		TWeakObjectPtr<UPointLightComponent> Component;
		FLinearColor HSVColor = FLinearColor::Black;
		float BaseIntensity = 0.0f;
		uint8 Brightness = 0;
		uint8 Hue = 0;
		uint8 Saturation = 255;
		EWoTDynLightShape Shape = EWoTDynLightShape::NonIncidence;
		EWoTDynLightType Type = EWoTDynLightType::Steady;
		uint8 Period = 0;
		uint8 Phase = 0;
		uint64 RegistrationId = 0;
	};

	void RegisterTickFunction();
	void TickSubsystem(float DeltaSeconds);
	TArray<FWoTDynamicLightSample> Gather(float TimeSeconds,
		float LevelBrightness);
	float ResolveLevelBrightness();
	FVector ResolveViewLocation() const;
	void WriteSlots(const FWoTDynamicLightSlotValues& Slots);

	UPROPERTY(Transient)
	TObjectPtr<UMaterialParameterCollection> DynamicLightCollection;

	UPROPERTY(Transient)
	TObjectPtr<UWoTMapLightTable> MapLightTable;

	UPROPERTY(Transient)
	FWoTDynamicLightTickFunction DynamicLightTick;

	TArray<FRecord> Records;
	uint64 NextRegistrationId = 1;
	uint64 GatherFrame = 0;
	bool bMapLightTableResolved = false;
};
