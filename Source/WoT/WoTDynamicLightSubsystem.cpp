#include "WoTDynamicLightSubsystem.h"
#include "Payload/WoTPayloadActivation.h"

#include "WoTMapLightTable.h"
#include "WoTMeshLight.h"

#include "Components/PointLightComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Materials/MaterialParameterCollection.h"
#include "RHIGlobals.h"

static TAutoConsoleVariable<int32> CVarWoTDynamicActorLights(
	TEXT("wot.DynamicActorLights"), 1,
	TEXT("Enable the bounded UE1 dynamic actor-light material term."),
	ECVF_Default);

namespace
{
	constexpr int32 RetailSinTableSize = 16384;
	constexpr int32 RetailSinAngleShift = 2;
	constexpr double RetailWavePi = 3.1415926535897932;

	struct FRetailSinTable
	{
		float Values[RetailSinTableSize];

		FRetailSinTable()
		{
			// UE1's global math tables build TrigFLOAT over this exact float
			// domain. SinTab truncates the rotator units, shifts, and masks.
			for (int32 Index = 0; Index < RetailSinTableSize; ++Index)
			{
				const double Angle = static_cast<double>(static_cast<float>(Index))
					* 2.0 * RetailWavePi
					/ static_cast<double>(static_cast<float>(RetailSinTableSize));
				Values[Index] = static_cast<float>(FMath::Sin(Angle));
			}
		}
	};

	const FRetailSinTable& RetailSinTable()
	{
		static const FRetailSinTable Table;
		return Table;
	}

	EWoTDynLightType RetailLightType(const uint8 Type)
	{
		switch (Type)
		{
		case 2: return EWoTDynLightType::Pulse;
		case 3: return EWoTDynLightType::Blink;
		case 4: return EWoTDynLightType::Flicker;
		case 7: return EWoTDynLightType::SubtlePulse;
		case 1:
		default: return EWoTDynLightType::Steady;
		}
	}

const TCHAR* DynamicLightCollectionPath =
	TEXT("/Game/WoT/Materials/MPC_WoTDynamicLights.MPC_WoTDynamicLights");
const TCHAR* RegistrationTagPrefix = TEXT("WoTDynLight=");

bool DynamicLightWorldReady()
{
#if WOT_GUARDED_BOOTSTRAP
	return IsWoTBootstrapWorldReady();
#else
	// Direct editor/game runs have no payload bootstrap, but still need the
	// retail dynamic-light material path for faithful development captures.
	return true;
#endif
}

uint32 DynamicLightHash(uint64 RegistrationId, uint64 Frame)
{
	uint32 Value = static_cast<uint32>(RegistrationId)
		^ static_cast<uint32>(RegistrationId >> 32)
		^ static_cast<uint32>(Frame)
		^ static_cast<uint32>(Frame >> 32)
		^ 0x9e3779b9u;
	Value ^= Value >> 16;
	Value *= 0x7feb352du;
	Value ^= Value >> 15;
	Value *= 0x846ca68bu;
	Value ^= Value >> 16;
	return Value;
}

bool ParseRegistrationTag(const FName Tag, uint8& OutBrightness, uint8& OutHue,
	uint8& OutSaturation, EWoTDynLightShape& OutShape,
	EWoTDynLightType& OutType, uint8& OutPeriod, uint8& OutPhase)
{
	const FString Text = Tag.ToString();
	if (!Text.StartsWith(RegistrationTagPrefix))
	{
		return false;
	}
	TArray<FString> Fields;
	Text.RightChop(FCString::Strlen(RegistrationTagPrefix)).ParseIntoArray(
		Fields, TEXT(","), false);
	if (Fields.Num() != 7)
	{
		return false;
	}
	const int32 Brightness = FCString::Atoi(*Fields[0]);
	const int32 Hue = FCString::Atoi(*Fields[1]);
	const int32 Saturation = FCString::Atoi(*Fields[2]);
	const int32 Shape = FCString::Atoi(*Fields[3]);
	const int32 Type = FCString::Atoi(*Fields[4]);
	const int32 Period = FCString::Atoi(*Fields[5]);
	const int32 Phase = FCString::Atoi(*Fields[6]);
	if (!FMath::IsWithinInclusive(Brightness, 0, 255)
		|| !FMath::IsWithinInclusive(Hue, 0, 255)
		|| !FMath::IsWithinInclusive(Saturation, 0, 255)
		|| !FMath::IsWithinInclusive(Shape, 0, 2)
		|| !FMath::IsWithinInclusive(Type, 0, 4)
		|| !FMath::IsWithinInclusive(Period, 0, 255)
		|| !FMath::IsWithinInclusive(Phase, 0, 255))
	{
		return false;
	}
	OutBrightness = static_cast<uint8>(Brightness);
	OutHue = static_cast<uint8>(Hue);
	OutSaturation = static_cast<uint8>(Saturation);
	OutShape = static_cast<EWoTDynLightShape>(Shape);
	OutType = static_cast<EWoTDynLightType>(Type);
	OutPeriod = static_cast<uint8>(Period);
	OutPhase = static_cast<uint8>(Phase);
	return true;
}
}

float UWoTDynamicLightSubsystem::RetailWaveMean(const uint8 RetailType)
{
	switch (RetailType)
	{
	case 2: return 0.60f;
	case 3: return 0.50f;
	case 7: return 0.90f;
	default: return 1.0f;
	}
}

float UWoTDynamicLightSubsystem::RetailWaveAmplitude(const uint8 RetailType)
{
	switch (RetailType)
	{
	case 2: return 0.39f;
	case 3: return 0.50f;
	case 7: return 0.09f;
	default: return 0.0f;
	}
}

float UWoTDynamicLightSubsystem::EvaluateRetailWaveform(
	const uint8 RetailType, const float TimeSeconds, const uint8 Period,
	const uint8 Phase, const uint64 RegistrationId, const uint64 Frame)
{
	return EvaluateWaveform(RetailLightType(RetailType), TimeSeconds,
		Period, Phase, RegistrationId, Frame);
}

void FWoTDynamicLightTickFunction::ExecuteTick(float DeltaTime,
	ELevelTick, ENamedThreads::Type, const FGraphEventRef&)
{
	if (Target)
	{
		Target->TickSubsystem(DeltaTime);
	}
}

FString FWoTDynamicLightTickFunction::DiagnosticMessage()
{
	return TEXT("UWoTDynamicLightSubsystem::TickSubsystem");
}

FName FWoTDynamicLightTickFunction::DiagnosticContext(bool)
{
	return FName(TEXT("WoTDynamicActorLights"));
}

void UWoTDynamicLightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	DynamicLightTick.Target = this;
	DynamicLightTick.TickGroup = TG_PostUpdateWork;
	DynamicLightTick.bCanEverTick = true;
	DynamicLightTick.bStartWithTickEnabled = false;
	DynamicLightTick.SetTickFunctionEnable(false);
	if (DynamicLightWorldReady())
	{
		DynamicLightTick.SetTickFunctionEnable(true);
		RegisterTickFunction();
	}
}

void UWoTDynamicLightSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (DynamicLightWorldReady())
	{
		DynamicLightTick.SetTickFunctionEnable(true);
		RegisterTickFunction();
	}
}

void UWoTDynamicLightSubsystem::RegisterTickFunction()
{
	if (!DynamicLightWorldReady())
	{
		return;
	}
	if (!DynamicLightTick.IsTickFunctionRegistered())
	{
		if (UWorld* World = GetWorld(); IsValid(World) && World->PersistentLevel)
		{
			DynamicLightTick.RegisterTickFunction(World->PersistentLevel);
		}
	}
}

void UWoTDynamicLightSubsystem::Deinitialize()
{
	if (DynamicLightTick.IsTickFunctionRegistered())
	{
		DynamicLightTick.UnRegisterTickFunction();
	}
	DynamicLightTick.Target = nullptr;
	Records.Reset();
	DynamicLightCollection = nullptr;
	MapLightTable = nullptr;
	Super::Deinitialize();
}

void UWoTDynamicLightSubsystem::Register(UPointLightComponent* Comp,
	uint8 Brightness, uint8 Hue, uint8 Saturation, EWoTDynLightShape Shape,
	EWoTDynLightType Type, uint8 Period, uint8 Phase)
{
	if (!DynamicLightWorldReady() || !IsValid(Comp))
	{
		return;
	}

	FRecord* Record = Records.FindByPredicate(
		[Comp](const FRecord& Candidate)
		{
			return Candidate.Component.Get() == Comp;
		});
	if (!Record)
	{
		FRecord& NewRecord = Records.AddDefaulted_GetRef();
		NewRecord.Component = Comp;
		NewRecord.RegistrationId = NextRegistrationId++;
		Record = &NewRecord;
	}
	Record->Brightness = Brightness;
	Record->Hue = Hue;
	Record->Saturation = Saturation;
	Record->Shape = Shape;
	Record->Type = Type;
	Record->Period = Period;
	Record->Phase = Phase;
	Record->BaseIntensity = static_cast<float>(Brightness) / 64.0f * 5.0f;
	Record->HSVColor = FGetHSV(Hue, Saturation, 255);
}

void UWoTDynamicLightSubsystem::RegisterOrDefer(UPointLightComponent* Comp,
	uint8 Brightness, uint8 Hue, uint8 Saturation, EWoTDynLightShape Shape,
	EWoTDynLightType Type, uint8 Period, uint8 Phase)
{
	if (!IsValid(Comp))
	{
		return;
	}

	Comp->ComponentTags.RemoveAll(
		[](const FName Tag)
		{
			return Tag.ToString().StartsWith(RegistrationTagPrefix);
		});
	Comp->ComponentTags.Add(FName(*FString::Printf(
		TEXT("%s%u,%u,%u,%u,%u,%u,%u"), RegistrationTagPrefix,
		static_cast<uint32>(Brightness), static_cast<uint32>(Hue),
		static_cast<uint32>(Saturation), static_cast<uint32>(Shape),
		static_cast<uint32>(Type), static_cast<uint32>(Period),
		static_cast<uint32>(Phase))));

	if (UWorld* World = Comp->GetWorld())
	{
		if (UWoTDynamicLightSubsystem* Subsystem =
			World->GetSubsystem<UWoTDynamicLightSubsystem>())
		{
			Subsystem->Register(Comp, Brightness, Hue, Saturation, Shape,
				Type, Period, Phase);
		}
	}
}

void UWoTDynamicLightSubsystem::RegisterTaggedComponents(AActor* Owner)
{
	if (!IsValid(Owner) || !Owner->GetWorld())
	{
		return;
	}
	UWoTDynamicLightSubsystem* Subsystem =
		Owner->GetWorld()->GetSubsystem<UWoTDynamicLightSubsystem>();
	if (!Subsystem)
	{
		return;
	}

	TArray<UPointLightComponent*> Components;
	Owner->GetComponents<UPointLightComponent>(Components);
	for (UPointLightComponent* Comp : Components)
	{
		if (!IsValid(Comp))
		{
			continue;
		}
		for (const FName Tag : Comp->ComponentTags)
		{
			uint8 Brightness = 0;
			uint8 Hue = 0;
			uint8 Saturation = 255;
			uint8 Period = 0;
			uint8 Phase = 0;
			EWoTDynLightShape Shape = EWoTDynLightShape::NonIncidence;
			EWoTDynLightType Type = EWoTDynLightType::Steady;
			if (ParseRegistrationTag(Tag, Brightness, Hue, Saturation, Shape,
				Type, Period, Phase))
			{
				Subsystem->Register(Comp, Brightness, Hue, Saturation, Shape,
					Type, Period, Phase);
				break;
			}
		}
	}
}

FLinearColor UWoTDynamicLightSubsystem::FGetHSV(
	uint8 Hue, uint8 Saturation, uint8 Value)
{
	float Brightness = 1.4f * static_cast<float>(Value) / 255.0f;
	Brightness *= 0.70f / (0.01f + FMath::Sqrt(Brightness));
	Brightness = FMath::Clamp(Brightness, 0.0f, 1.0f);

	FVector HueTriangle;
	if (Hue < 86)
	{
		HueTriangle = FVector(
			(85.0f - static_cast<float>(Hue)) / 85.0f,
			static_cast<float>(Hue) / 85.0f, 0.0f);
	}
	else if (Hue < 171)
	{
		HueTriangle = FVector(
			0.0f, (170.0f - static_cast<float>(Hue)) / 85.0f,
			(static_cast<float>(Hue) - 85.0f) / 85.0f);
	}
	else
	{
		HueTriangle = FVector(
			(static_cast<float>(Hue) - 170.0f) / 85.0f, 0.0f,
			(255.0f - static_cast<float>(Hue)) / 84.0f);
	}
	const float White = static_cast<float>(Saturation) / 255.0f;
	const FVector RGB =
		(HueTriangle + White * (FVector::OneVector - HueTriangle)) * Brightness;
	return FLinearColor(RGB.X, RGB.Y, RGB.Z, 1.0f);
}

float UWoTDynamicLightSubsystem::EvaluateWaveform(EWoTDynLightType Type,
	float TimeSeconds, uint8 Period, uint8 Phase, uint64 RegistrationId,
	uint64 Frame)
{
	const double ThetaUnits =
		static_cast<double>(TimeSeconds) * 35.0 * 65536.0
			/ static_cast<double>(FMath::Max(static_cast<int32>(Period), 1))
		+ static_cast<double>(static_cast<uint32>(Phase) << 8);
	const int32 RotatorUnits = static_cast<int32>(ThetaUnits);
	const int32 TableIndex =
		(RotatorUnits >> RetailSinAngleShift) & (RetailSinTableSize - 1);
	const float SinTheta = RetailSinTable().Values[TableIndex];
	switch (Type)
	{
	case EWoTDynLightType::Pulse:
		return 0.6f + 0.39f * SinTheta;
	case EWoTDynLightType::SubtlePulse:
		return 0.9f + 0.09f * SinTheta;
	case EWoTDynLightType::Blink:
	{
		// Retail global_Blink: parity of the integer angle
		// units with divisor LightPeriod+1 (no max clamp) — not a sine sign.
		const double BlinkUnits =
			static_cast<double>(TimeSeconds) * 35.0 * 65536.0
				/ (static_cast<double>(Period) + 1.0)
			+ static_cast<double>(static_cast<uint32>(Phase) << 8);
		return (static_cast<int64>(BlinkUnits) & 1) ? 0.0f : 1.0f;
	}
	case EWoTDynLightType::Flicker:
	{
		const float Random = static_cast<float>(
			DynamicLightHash(RegistrationId, Frame) & 0x00ffffffu)
			/ static_cast<float>(0x01000000u);
		return Random < 0.5f ? 0.0f : Random;
	}
	case EWoTDynLightType::Steady:
	default:
		return 1.0f;
	}
}

TArray<FWoTDynamicLightSample>
UWoTDynamicLightSubsystem::SelectDynamicLights(
	const TArray<FWoTDynamicLightSample>& Records,
	const FVector& ViewLocation, int32 MaxSlots)
{
	struct FRanked
	{
		FWoTDynamicLightSample Sample;
		float Priority = 0.0f;
	};
	TArray<FRanked> Ranked;
	Ranked.Reserve(Records.Num());
	for (const FWoTDynamicLightSample& Record : Records)
	{
		const float Distance = FVector::Distance(Record.Position, ViewLocation);
		const float Nearness = FMath::Max(
			0.0f, 1.0f - Distance / (Record.Radius + 1024.0f));
		FRanked& Row = Ranked.AddDefaulted_GetRef();
		Row.Sample = Record;
		Row.Priority =
			static_cast<float>(Record.Brightness) * Record.LiveScale * Nearness;
	}
	Ranked.Sort(
		[](const FRanked& A, const FRanked& B)
		{
			if (A.Priority != B.Priority)
			{
				return A.Priority > B.Priority;
			}
			return A.Sample.RegistrationId < B.Sample.RegistrationId;
		});

	const int32 Count = FMath::Clamp(MaxSlots, 0, Ranked.Num());
	TArray<FWoTDynamicLightSample> Selected;
	Selected.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Selected.Add(Ranked[Index].Sample);
	}
	return Selected;
}

FWoTDynamicLightSlotValues UWoTDynamicLightSubsystem::BuildSlotValues(
	const TArray<FWoTDynamicLightSample>& Records,
	const FVector& ViewLocation, bool bEnabled, int32 MaxSlots)
{
	const int32 SlotCount = FMath::Max(MaxSlots, 0);
	FWoTDynamicLightSlotValues Slots;
	// FLinearColor::Black carries alpha 1 — cleared slots must be all-zero
	// (PosR.w is the radius, Color.a the shape: 0 NonIncidence, 1 Incidence,
	// 2 Shell).
	Slots.PosR.Init(FLinearColor::Transparent, SlotCount);
	Slots.Color.Init(FLinearColor::Transparent, SlotCount);
	if (!bEnabled)
	{
		return Slots;
	}

	const TArray<FWoTDynamicLightSample> Selected =
		SelectDynamicLights(Records, ViewLocation, SlotCount);
	for (int32 Index = 0; Index < Selected.Num(); ++Index)
	{
		const FWoTDynamicLightSample& Light = Selected[Index];
		Slots.PosR[Index] = FLinearColor(
			Light.Position.X, Light.Position.Y, Light.Position.Z, Light.Radius);
		Slots.Color[Index] = FLinearColor(
			Light.Color.R, Light.Color.G, Light.Color.B,
			static_cast<float>(static_cast<uint8>(Light.Shape)));
	}
	return Slots;
}

bool UWoTDynamicLightSubsystem::AreDynamicActorLightsEnabled()
{
	return CVarWoTDynamicActorLights.GetValueOnGameThread() != 0;
}

TArray<FWoTDynamicLightSample> UWoTDynamicLightSubsystem::Gather(
	float TimeSeconds, float LevelBrightness)
{
	TArray<FWoTDynamicLightSample> Live;
	for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
	{
		FRecord& Record = Records[Index];
		UPointLightComponent* Comp = Record.Component.Get();
		if (!IsValid(Comp))
		{
			Records.RemoveAt(Index);
			continue;
		}
		AActor* Owner = Comp->GetOwner();
		if (!Comp->IsRegistered() || !Comp->IsVisible()
			|| (IsValid(Owner) && Owner->IsHidden()))
		{
			continue;
		}

		const float LiveScale = Record.BaseIntensity > SMALL_NUMBER
			? Comp->Intensity / Record.BaseIntensity : 0.0f;
		const float Waveform = EvaluateWaveform(
			Record.Type, TimeSeconds, Record.Period, Record.Phase,
			Record.RegistrationId, GatherFrame);
		const float Scale =
			static_cast<float>(Record.Brightness) / 255.0f
			* LevelBrightness * LiveScale * Waveform;
		FWoTDynamicLightSample& Sample = Live.AddDefaulted_GetRef();
		Sample.Component = Comp;
		Sample.Position = Comp->GetComponentLocation();
		Sample.Radius = Comp->AttenuationRadius;
		Sample.Color = Record.HSVColor * Scale;
		Sample.Color.A = 1.0f;
		Sample.LiveScale = LiveScale;
		Sample.Brightness = Record.Brightness;
		Sample.Hue = Record.Hue;
		Sample.Saturation = Record.Saturation;
		Sample.Shape = Record.Shape;
		Sample.Type = Record.Type;
		Sample.RegistrationId = Record.RegistrationId;
	}
	++GatherFrame;
	return Live;
}

float UWoTDynamicLightSubsystem::ResolveLevelBrightness()
{
	if (!bMapLightTableResolved)
	{
		MapLightTable = UWoTMeshLight::FindMapLightTable(GetWorld());
		bMapLightTableResolved = true;
	}
	return IsValid(MapLightTable) ? MapLightTable->LevelBrightness : 1.0f;
}

FVector UWoTDynamicLightSubsystem::ResolveViewLocation() const
{
	UWorld* World = GetWorld();
	if (!IsValid(World))
	{
		return FVector::ZeroVector;
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator();
		It; ++It)
	{
		const APlayerController* Controller = It->Get();
		if (IsValid(Controller) && Controller->IsLocalController()
			&& IsValid(Controller->PlayerCameraManager))
		{
			return Controller->PlayerCameraManager->GetCameraLocation();
		}
	}
	return FVector::ZeroVector;
}

void UWoTDynamicLightSubsystem::WriteSlots(
	const FWoTDynamicLightSlotValues& Slots)
{
	if (!DynamicLightCollection)
	{
		DynamicLightCollection = LoadObject<UMaterialParameterCollection>(
			nullptr, DynamicLightCollectionPath);
	}
	if (!DynamicLightCollection)
	{
		return;
	}
	for (int32 Index = 0; Index < MaxDynamicLights; ++Index)
	{
		const FLinearColor PosR = Slots.PosR.IsValidIndex(Index)
			? Slots.PosR[Index] : FLinearColor::Black;
		const FLinearColor Color = Slots.Color.IsValidIndex(Index)
			? Slots.Color[Index] : FLinearColor::Black;
		UKismetMaterialLibrary::SetVectorParameterValue(
			this, DynamicLightCollection,
			*FString::Printf(TEXT("DL%d_PosR"), Index), PosR);
		UKismetMaterialLibrary::SetVectorParameterValue(
			this, DynamicLightCollection,
			*FString::Printf(TEXT("DL%d_Color"), Index), Color);
	}
}

void UWoTDynamicLightSubsystem::TickSubsystem(float)
{
	if (IsRunningDedicatedServer() || GUsingNullRHI)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!IsValid(World))
	{
		return;
	}
	const TArray<FWoTDynamicLightSample> Live =
		Gather(World->GetTimeSeconds(), ResolveLevelBrightness());
	WriteSlots(BuildSlotValues(
		Live, ResolveViewLocation(), AreDynamicActorLightsEnabled()));
}

TArray<FWoTDynamicLightSample>
UWoTDynamicLightSubsystem::GatherForTesting(
	float TimeSeconds, float LevelBrightness)
{
	return Gather(TimeSeconds, LevelBrightness);
}

TArray<FWoTDynamicLightSample>
UWoTDynamicLightSubsystem::GatherForTesting(float TimeSeconds)
{
	return Gather(TimeSeconds, ResolveLevelBrightness());
}

void UWoTDynamicLightSubsystem::SetMapLightTableForTesting(
	UWoTMapLightTable* Table)
{
	MapLightTable = Table;
	bMapLightTableResolved = true;
}
