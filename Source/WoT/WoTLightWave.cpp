#include "WoTLightWave.h"
#include "WoTDynamicLightSubsystem.h"
#include "WoTPresentationPolicy.h"

#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"

static TAutoConsoleVariable<int32> CVarWoTLightWaveFreeze(
	TEXT("wot.LightWaveFreeze"), 0,
	TEXT("1 = pin animated lightmaps to the literal mean atlas."));

namespace
{
const FName WaveParameterNames[AWoTLightWave::MaxWaveformGroups] = {
	TEXT("Wave0"), TEXT("Wave1"), TEXT("Wave2"), TEXT("Wave3")};
const FName NoiseFrameParameterName(TEXT("NoiseFrame"));
const FName FreezeParameterName(TEXT("Freeze"));
const FName LegacyWaveParameterName(TEXT("LightWave"));
}

AWoTLightWave::AWoTLightWave()
{
	PrimaryActorTick.bCanEverTick = true;
}

float AWoTLightWave::EvaluateWaveScalar(
	float TimeSeconds, float Period, float Phase)
{
	// Compatibility seam for existing specs/callers: normalized Pulse through
	// the one shared retail evaluator.
	const float Contribution =
		UWoTDynamicLightSubsystem::EvaluateRetailWaveform(2, TimeSeconds,
			static_cast<uint8>(FMath::Clamp(FMath::TruncToInt(Period), 0, 255)),
			static_cast<uint8>(FMath::Clamp(FMath::TruncToInt(Phase), 0, 255)), 0, 0);
	return 0.5f + 0.5f * ((Contribution - 0.60f) / 0.39f);
}

FWoTLightWaveParameterValues AWoTLightWave::EvaluateParameters(
	float TimeSeconds, bool bFrozen,
	const TArray<FWoTLightWaveGroup>& WaveGroups,
	uint32& InOutNoiseFrame)
{
	FWoTLightWaveParameterValues Values;
	if (bFrozen)
	{
		// These are also the MPC asset defaults. Holding the internal frame lets
		// live animation resume without a hidden frozen-time advance, while the
		// transported frame is zero and the material takes its explicit mean path.
		return Values;
	}

	const int32 GroupCount = FMath::Min(WaveGroups.Num(), MaxWaveformGroups);
	for (int32 Index = 0; Index < GroupCount; ++Index)
	{
		const FWoTLightWaveGroup& Group = WaveGroups[Index];
		const uint8 Type = Group.Type;
		const float Mean = UWoTDynamicLightSubsystem::RetailWaveMean(Type);
		const float Amplitude =
			UWoTDynamicLightSubsystem::RetailWaveAmplitude(Type);
		const float Contribution =
			UWoTDynamicLightSubsystem::EvaluateRetailWaveform(Type, TimeSeconds,
				static_cast<uint8>(FMath::Clamp(Group.Period, 0, 255)),
				static_cast<uint8>(FMath::Clamp(Group.Phase, 0, 255)), 0, 0);
		Values.Waves[Index] = Amplitude > 0.0f
			? 0.5f + 0.5f * ((Contribution - Mean) / Amplitude)
			: 0.5f;
	}

	InOutNoiseFrame = (InOutNoiseFrame + 1u) & NoiseFrameMask;
	Values.NoiseFrame = static_cast<float>(InOutNoiseFrame);
	Values.Freeze = 0.0f;
	return Values;
}

bool AWoTLightWave::IsFreezeEnabled()
{
	return CVarWoTLightWaveFreeze.GetValueOnGameThread() != 0;
}

void AWoTLightWave::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!WoTPresentationAnimationEnabled(GetWorld()))
	{
		// Nothing samples the collection on a dedicated server.
		SetActorTickEnabled(false);
		return;
	}
	if (!Collection)
	{
		Collection = LoadObject<UMaterialParameterCollection>(
			nullptr, TEXT("/Game/WoT/Materials/MPC_WoTLightWave.MPC_WoTLightWave"));
		if (!Collection)
		{
			return;
		}
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const bool bFrozen = IsFreezeEnabled();
	const FWoTLightWaveParameterValues Values = EvaluateParameters(
		World->GetTimeSeconds(), bFrozen, Groups, NoiseFrame);
	for (int32 Index = 0; Index < MaxWaveformGroups; ++Index)
	{
		UKismetMaterialLibrary::SetScalarParameterValue(
			this, Collection, WaveParameterNames[Index], Values.Waves[Index]);
	}
	UKismetMaterialLibrary::SetScalarParameterValue(
		this, Collection, NoiseFrameParameterName, Values.NoiseFrame);
	UKismetMaterialLibrary::SetScalarParameterValue(
		this, Collection, FreezeParameterName, Values.Freeze);
	if (Groups.IsEmpty())
	{
		// Compatibility shim for the five shipped endpoint maps. Continue
		// writing their serialized Period/Phase contract until the MPC, masters,
		// response assets, and maps are replaced in one atomic landing.
		const float LegacyWave = bFrozen
			? 0.5f
			: EvaluateWaveScalar(
				World->GetTimeSeconds(), Period, Phase);
		UKismetMaterialLibrary::SetScalarParameterValue(
			this, Collection, LegacyWaveParameterName, LegacyWave);
	}
}
