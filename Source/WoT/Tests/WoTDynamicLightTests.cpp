// Headless specs for G45 runtime actor-light transport.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTDynamicLightSubsystem.h"
#include "../WoTMapLightTable.h"
#include "../WoTMeshLight.h"
#include "../WoTVertexAnimComponent.h"
#include "../Projectiles/WoTDartProjectile.h"

#include "Components/PointLightComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
FActorSpawnParameters DynamicLightSpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

UPointLightComponent* MakeRegisteredLight(UWorld* World, AActor*& OutOwner,
	uint8 Brightness = 64, uint8 Hue = 20, uint8 Saturation = 32)
{
	OutOwner = World ? World->SpawnActor<AActor>(
		AActor::StaticClass(), FVector(9000.0f, 0.0f, 500.0f),
		FRotator::ZeroRotator, DynamicLightSpawnParams()) : nullptr;
	if (!OutOwner)
	{
		return nullptr;
	}
	UPointLightComponent* Light =
		NewObject<UPointLightComponent>(OutOwner, TEXT("DynamicLightFixture"));
	if (!Light)
	{
		return nullptr;
	}
	OutOwner->AddInstanceComponent(Light);
	Light->SetMobility(EComponentMobility::Movable);
	Light->SetAttenuationRadius(500.0f);
	Light->SetIntensity(static_cast<float>(Brightness) / 64.0f * 5.0f);
	Light->RegisterComponent();
	if (UWoTDynamicLightSubsystem* Subsystem =
		World->GetSubsystem<UWoTDynamicLightSubsystem>())
	{
		Subsystem->Register(Light, Brightness, Hue, Saturation);
	}
	return Light;
}

void TestColorNear(FAutomationTestBase& Test, const TCHAR* Label,
	const FLinearColor& Actual, const FLinearColor& Expected,
	float Tolerance = 1.0e-4f)
{
	Test.TestTrue(Label,
		FMath::IsNearlyEqual(Actual.R, Expected.R, Tolerance)
		&& FMath::IsNearlyEqual(Actual.G, Expected.G, Tolerance)
		&& FMath::IsNearlyEqual(Actual.B, Expected.B, Tolerance));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightRegistrationSpec,
	"WoT.DynamicLight.RegistrationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightRegistrationSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("fixture world"), World))
	{
		return false;
	}
	UWoTDynamicLightSubsystem* Subsystem =
		World->GetSubsystem<UWoTDynamicLightSubsystem>();
	AActor* Owner = nullptr;
	UPointLightComponent* Light = MakeRegisteredLight(World, Owner);
	if (!TestNotNull(TEXT("subsystem"), Subsystem)
		|| !TestNotNull(TEXT("owner spawned with AlwaysSpawn"), Owner)
		|| !TestNotNull(TEXT("point light"), Light))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	TArray<FWoTDynamicLightSample> Gathered =
		Subsystem->GatherForTesting(0.0f, 1.0f);
	TestEqual(TEXT("register appears in gather"), Gathered.Num(), 1);
	Subsystem->Register(Light, 192, 24, 128,
		EWoTDynLightShape::Incidence, EWoTDynLightType::Pulse, 64, 7);
	Gathered = Subsystem->GatherForTesting(0.0f, 1.0f);
	if (TestEqual(TEXT("idempotent re-register keeps one record"),
		Gathered.Num(), 1))
	{
		TestEqual(TEXT("re-register updates brightness"),
			Gathered[0].Brightness, static_cast<uint8>(192));
		TestEqual(TEXT("re-register updates hue"),
			Gathered[0].Hue, static_cast<uint8>(24));
		TestEqual(TEXT("re-register updates saturation"),
			Gathered[0].Saturation, static_cast<uint8>(128));
		TestEqual(TEXT("re-register updates shape"),
			Gathered[0].Shape, EWoTDynLightShape::Incidence);
		TestEqual(TEXT("re-register updates type"),
			Gathered[0].Type, EWoTDynLightType::Pulse);
	}

	Light->DestroyComponent();
	Gathered = Subsystem->GatherForTesting(0.1f, 1.0f);
	TestEqual(TEXT("destroyed component is pruned"), Gathered.Num(), 0);
	TestEqual(TEXT("dead weak record removed"),
		Subsystem->GetRegisteredCountForTesting(), 0);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightLiveScaleSpec,
	"WoT.DynamicLight.LiveScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightLiveScaleSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTDynamicLightSubsystem* Subsystem = World
		? World->GetSubsystem<UWoTDynamicLightSubsystem>() : nullptr;
	AActor* Owner = nullptr;
	UPointLightComponent* Light = MakeRegisteredLight(World, Owner, 64, 20, 32);
	if (!TestNotNull(TEXT("fixture world"), World)
		|| !TestNotNull(TEXT("subsystem"), Subsystem)
		|| !TestNotNull(TEXT("owner spawned with AlwaysSpawn"), Owner)
		|| !TestNotNull(TEXT("point light"), Light))
	{
		if (World) { WoTTest::DestroySpecWorld(World); }
		return false;
	}
	const TArray<FWoTDynamicLightSample> Full =
		Subsystem->GatherForTesting(0.0f, 1.0f);
	Light->SetIntensity(2.5f);
	const TArray<FWoTDynamicLightSample> Half =
		Subsystem->GatherForTesting(0.1f, 1.0f);
	if (TestEqual(TEXT("full gather count"), Full.Num(), 1)
		&& TestEqual(TEXT("half gather count"), Half.Num(), 1))
	{
		TestTrue(TEXT("LiveScale is one half"),
			FMath::IsNearlyEqual(Half[0].LiveScale, 0.5f));
		TestColorNear(*this, TEXT("live color halves"), Half[0].Color,
			Full[0].Color * 0.5f);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightWaveformSpec,
	"WoT.DynamicLight.Waveforms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightWaveformSpec::RunTest(const FString&)
{
	TestTrue(TEXT("pulse t=0"),
		FMath::IsNearlyEqual(UWoTDynamicLightSubsystem::EvaluateWaveform(
			EWoTDynLightType::Pulse, 0.0f, 64, 0, 1, 0), 0.6f, 1.0e-5f));
	TestTrue(TEXT("subtle pulse t=0"),
		FMath::IsNearlyEqual(UWoTDynamicLightSubsystem::EvaluateWaveform(
			EWoTDynLightType::SubtlePulse, 0.0f, 64, 0, 1, 0),
			0.9f, 1.0e-5f));

	// Retail global_Blink: parity of the integer angle
	// units, divisor Period+1. One angle unit at Period=0 lasts
	// 1/(35*65536) s; parity flips each unit.
	const float BlinkUnitSeconds = 1.0f / (35.0f * 65536.0f);
	const float BlinkA = UWoTDynamicLightSubsystem::EvaluateWaveform(
		EWoTDynLightType::Blink, BlinkUnitSeconds * 0.5f, 0, 0, 1, 0);
	const float BlinkB = UWoTDynamicLightSubsystem::EvaluateWaveform(
		EWoTDynLightType::Blink, BlinkUnitSeconds * 1.5f, 0, 0, 1, 0);
	const float BlinkC = UWoTDynamicLightSubsystem::EvaluateWaveform(
		EWoTDynLightType::Blink, BlinkUnitSeconds * 2.5f, 0, 0, 1, 0);
	TestEqual(TEXT("blink on at even angle-unit parity"), BlinkA, 1.0f);
	TestEqual(TEXT("blink off at odd angle-unit parity"), BlinkB, 0.0f);
	TestEqual(TEXT("blink parity alternates per unit"), BlinkC, 1.0f);
	// Phase<<8 lands on even parity and must not flip the phase-0 result.
	TestEqual(TEXT("blink phase shifts by whole even units"),
		UWoTDynamicLightSubsystem::EvaluateWaveform(
			EWoTDynLightType::Blink, BlinkUnitSeconds * 0.5f, 0, 1, 1, 0),
		1.0f);

	const float FlickerA = UWoTDynamicLightSubsystem::EvaluateWaveform(
		EWoTDynLightType::Flicker, 12.0f, 0, 0, 987, 42);
	const float FlickerB = UWoTDynamicLightSubsystem::EvaluateWaveform(
		EWoTDynLightType::Flicker, 99.0f, 255, 255, 987, 42);
	TestEqual(TEXT("flicker is deterministic for record+frame"),
		FlickerA, FlickerB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightHSVSpec,
	"WoT.DynamicLight.FGetHSV",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightHSVSpec::RunTest(const FString&)
{
	struct FPin
	{
		uint8 Hue;
		uint8 Saturation;
		FLinearColor Expected;
	};
	const FPin Pins[] = {
		{20, 32, FLinearColor(0.652311f, 0.272065f, 0.103066f)},
		{24, 32, FLinearColor(0.618512f, 0.305864f, 0.103066f)},
		{204, 204, FLinearColor(0.722753f, 0.657048f, 0.756778f)},
		{30, 224, FLinearColor(0.786070f, 0.756704f, 0.721464f)},
		{0, 255, FLinearColor(0.821310f, 0.821310f, 0.821310f)},
		{96, 128, FLinearColor(0.412265f, 0.768375f, 0.465201f)},
		{12, 0, FLinearColor(0.705360f, 0.115950f, 0.000000f)},
	};
	for (const FPin& Pin : Pins)
	{
		TestColorNear(*this,
			*FString::Printf(TEXT("H=%u S=%u"), Pin.Hue, Pin.Saturation),
			UWoTDynamicLightSubsystem::FGetHSV(Pin.Hue, Pin.Saturation),
			Pin.Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightSelectionSpec,
	"WoT.DynamicLight.SelectionAndCVar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightSelectionSpec::RunTest(const FString&)
{
	TArray<FWoTDynamicLightSample> Records;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		FWoTDynamicLightSample& Sample = Records.AddDefaulted_GetRef();
		Sample.Position = Index == 9
			? FVector(6000.0f, 0.0f, 0.0f) : FVector::ZeroVector;
		Sample.Radius = Index == 9 ? 100.0f : 5000.0f;
		Sample.Brightness = Index == 0 ? 10 : 100;
		Sample.LiveScale = 1.0f;
		Sample.Color = FLinearColor::White;
		Sample.RegistrationId = static_cast<uint64>(Index + 1);
	}
	const TArray<FWoTDynamicLightSample> Selected =
		UWoTDynamicLightSubsystem::SelectDynamicLights(
			Records, FVector::ZeroVector, 8);
	if (TestEqual(TEXT("top-K is eight"), Selected.Num(), 8))
	{
		for (int32 Index = 0; Index < 8; ++Index)
		{
			TestEqual(TEXT("equal-priority ties retain registration order"),
				Selected[Index].RegistrationId,
				static_cast<uint64>(Index + 2));
		}
	}

	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(
		TEXT("wot.DynamicActorLights"));
	if (TestNotNull(TEXT("dynamic-light cvar exists"), CVar))
	{
		const int32 Original = CVar->GetInt();
		CVar->Set(0, ECVF_SetByCode);
		const FWoTDynamicLightSlotValues Slots =
			UWoTDynamicLightSubsystem::BuildSlotValues(
				Records, FVector::ZeroVector,
				UWoTDynamicLightSubsystem::AreDynamicActorLightsEnabled());
		TestEqual(TEXT("disabled slot count"), Slots.Color.Num(), 8);
		// All FOUR channels must clear: PosR.w is the radius and Color.a the
		// shape flag (FLinearColor::Black would leave alpha 1).
		for (int32 Index = 0; Index < Slots.Color.Num(); ++Index)
		{
			TestTrue(TEXT("cvar zero clears every MPC color slot"),
				Slots.Color[Index].Equals(FLinearColor::Transparent));
			TestTrue(TEXT("cvar zero clears every MPC pos/radius slot"),
				Slots.PosR[Index].Equals(FLinearColor::Transparent));
		}
		CVar->Set(Original, ECVF_SetByCode);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightShapeCodeSpec,
	"WoT.DynamicLight.ShapeCodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightShapeCodeSpec::RunTest(const FString&)
{
	// The materials select the spatial function by Color.a: 0 NonIncidence,
	// 1 Incidence, 2 Shell (dynamic_light_material.py). Shell used to travel
	// as NonIncidence, so AMAPearl lit its whole radius instead of its shell.
	const EWoTDynLightShape Shapes[] = {EWoTDynLightShape::NonIncidence,
		EWoTDynLightShape::Incidence, EWoTDynLightShape::Shell};
	TArray<FWoTDynamicLightSample> Records;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FWoTDynamicLightSample& Sample = Records.AddDefaulted_GetRef();
		Sample.Radius = 500.0f;
		Sample.Brightness = 100;
		Sample.LiveScale = 1.0f;
		Sample.Color = FLinearColor::White;
		Sample.Shape = Shapes[Index];
		Sample.RegistrationId = static_cast<uint64>(Index + 1);
	}
	const FWoTDynamicLightSlotValues Slots =
		UWoTDynamicLightSubsystem::BuildSlotValues(
			Records, FVector::ZeroVector, true);
	TArray<float> Codes;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Codes.Add(Slots.Color[Index].A);
	}
	Codes.Sort();
	TestEqual(TEXT("NonIncidence travels as alpha 0"), Codes[0], 0.0f);
	TestEqual(TEXT("Incidence travels as alpha 1"), Codes[1], 1.0f);
	TestEqual(TEXT("Shell travels as alpha 2"), Codes[2], 2.0f);
	TestEqual(TEXT("unused slots stay clear"), Slots.Color[3],
		FLinearColor::Transparent);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightLevelBrightnessSpec,
	"WoT.DynamicLight.LevelBrightness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightLevelBrightnessSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	UWoTDynamicLightSubsystem* Subsystem = World
		? World->GetSubsystem<UWoTDynamicLightSubsystem>() : nullptr;
	AActor* Owner = nullptr;
	UPointLightComponent* Light = MakeRegisteredLight(World, Owner);
	if (!TestNotNull(TEXT("fixture world"), World)
		|| !TestNotNull(TEXT("subsystem"), Subsystem)
		|| !TestNotNull(TEXT("owner spawned with AlwaysSpawn"), Owner)
		|| !TestNotNull(TEXT("point light"), Light))
	{
		if (World) { WoTTest::DestroySpecWorld(World); }
		return false;
	}
	const TArray<FWoTDynamicLightSample> Base =
		Subsystem->GatherForTesting(0.0f, 1.0f);
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	Table->LevelBrightness = 1.1f;
	Subsystem->SetMapLightTableForTesting(Table);
	const TArray<FWoTDynamicLightSample> Bright =
		Subsystem->GatherForTesting(0.1f);
	if (TestEqual(TEXT("base gather count"), Base.Num(), 1)
		&& TestEqual(TEXT("table gather count"), Bright.Num(), 1))
	{
		TestColorNear(*this, TEXT("table 1.1 scales dynamic FloatColor"),
			Bright[0].Color, Base[0].Color * 1.1f);
	}
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTDynamicLightUnlitGateSpec,
	"WoT.DynamicLight.UnlitMeshGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTDynamicLightUnlitGateSpec::RunTest(const FString&)
{
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(
		nullptr, TEXT("/Game/WoT/Materials/M_WoT_MeshLit.M_WoT_MeshLit"));
	USkeletalMesh* TestMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/WoT/Meshes/Trolloc/Trolloc/SkeletalMeshes/Trolloc.Trolloc"));
	if (!TestNotNull(TEXT("MeshLit master"), Master)
		|| !TestNotNull(TEXT("test skeletal mesh"), TestMesh))
	{
		return false;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTDartProjectile* Dart = World ? World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(12000.0f, 0.0f, 1000.0f),
		FRotator::ZeroRotator, DynamicLightSpawnParams()) : nullptr;
	if (!TestNotNull(TEXT("fixture world"), World)
		|| !TestNotNull(TEXT("unlit dart spawned with AlwaysSpawn"), Dart))
	{
		if (World) { WoTTest::DestroySpecWorld(World); }
		return false;
	}
	Dart->bStyleTranslucent = false;
	Dart->DisplayMesh->SetSkeletalMeshAsset(TestMesh);
	Dart->DisplayMesh->SetMaterial(
		0, UMaterialInstanceDynamic::Create(Master, Dart));
	UWoTMapLightTable* Table = NewObject<UWoTMapLightTable>(World);
	UWoTMeshLight* MeshLight = UWoTMeshLight::AttachRuntime(Dart, Table);
	UMaterialInterface* Result = Dart->DisplayMesh->GetMaterial(0);
	float DynamicEnable = 1.0f;
	const bool bHasGate = Result && Result->GetScalarParameterValue(
		FHashedMaterialParameterInfo(TEXT("MLDynEnable")), DynamicEnable);
	TestNotNull(TEXT("runtime mesh-light bake"), MeshLight);
	TestTrue(TEXT("baked MID exposes MLDynEnable"), bHasGate);
	TestEqual(TEXT("bUnlit bake disables dynamic actor lights"),
		DynamicEnable, 0.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
