// Headless specs for the G45 emitter-side UE1 light registry.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"
#include "WoTRetailFamiliesRestoreScope.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTDynamicLightSubsystem.h"
#include "../WoTUE1LightRegistry.h"
#include "../Projectiles/WoTAMAPearl.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTEarthTremorFX.h"
#include "../Projectiles/WoTExpWardProjectile.h"
#include "../Projectiles/WoTFireballProjectile.h"

#include "Components/PointLightComponent.h"
#include "Engine/World.h"

namespace
{
struct FUE1LightRegistryTests_InstalledFixture
{
	FUE1LightRegistryTests_InstalledFixture()
	{
		FWoTUE1LightRegistryBuilder::Reset();
		FWoTUE1LightSet Fixture;
		auto Add = [&Fixture](const TCHAR* Name, uint8 Brightness, uint8 Hue,
			uint8 Saturation, float Radius, EWoTUE1LightShape Shape)
		{
			FWoTUE1LightRow& Row = Fixture.Rows.Add(Name);
			Row.Brightness = Brightness;
			Row.Hue = Hue;
			Row.Saturation = Saturation;
			Row.Radius = Radius;
			Row.Shape = Shape;
		};
		Add(TEXT("AngrealFireballProjectile"), 80, 15, 30, 4.0f,
			EWoTUE1LightShape::NonIncidence);
		Add(TEXT("AngrealExpWardProjectile"), 90, 25, 0, 5.0f,
			EWoTUE1LightShape::NonIncidence);
		Add(TEXT("ETSoundProxy"), 100, 35, 0, 6.0f,
			EWoTUE1LightShape::NonIncidence);
		// Hue 154 is the Blue variant the spec casts (Dart.uc LightHue).
		Add(TEXT("AngrealDartProjectile"), 110, 154, 50, 7.0f,
			EWoTUE1LightShape::Shell);
		Add(TEXT("AMAPearl"), 120, 55, 60, 8.0f,
			EWoTUE1LightShape::Shell);
		FWoTUE1LightRegistryBuilder::Install(MoveTemp(Fixture));
	}
	FWoTRetailFamiliesRestoreScope RestoreFamilies;
};

FActorSpawnParameters UE1LightRegistryTests_SpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

void UE1LightRegistryTests_TestComponent(FAutomationTestBase& Test,
	const TCHAR* Label, const UPointLightComponent* Light,
	const FWoTUE1LightRow& Row)
{
	if (!Test.TestNotNull(*FString::Printf(TEXT("%s point light"), Label), Light))
	{
		return;
	}
	Test.TestEqual(*FString::Printf(TEXT("%s attenuation"), Label),
		Light->AttenuationRadius, 25.0f * (Row.Radius + 1.0f));
	Test.TestEqual(*FString::Printf(TEXT("%s intensity"), Label),
		Light->Intensity, static_cast<float>(Row.Brightness) / 64.0f * 5.0f);
	const FLinearColor Converted = FLinearColor::MakeFromHSV8(
		Row.Hue, static_cast<uint8>(255 - Row.Saturation), 255);
	const FLinearColor Quantized(Converted.ToFColor(true));
	const FLinearColor Actual = Light->GetLightColor();
	Test.TestTrue(*FString::Printf(TEXT("%s color"), Label),
		Actual.Equals(Quantized, 1.0e-4f));
}

const FWoTDynamicLightSample* UE1LightRegistryTests_FindSample(
	const TArray<FWoTDynamicLightSample>& Samples,
	const UPointLightComponent* Light)
{
	return Samples.FindByPredicate([Light](const FWoTDynamicLightSample& Sample)
	{
		return Sample.Component.Get() == Light;
	});
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUE1LightRegistryEmitterSpec,
	"WoT.DynamicLight.UE1Registry.RepresentativeEmitters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTUE1LightRegistryEmitterSpec::RunTest(const FString&)
{
	FUE1LightRegistryTests_InstalledFixture InstalledFixture;
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("fixture world"), World)) { return false; }
	const FActorSpawnParameters Params = UE1LightRegistryTests_SpawnParams();
	AWoTFireballProjectile* Fireball = World->SpawnActor<AWoTFireballProjectile>(
		AWoTFireballProjectile::StaticClass(), FVector(1000, 0, 100),
		FRotator::ZeroRotator, Params);
	AWoTExpWardProjectile* Ward = World->SpawnActor<AWoTExpWardProjectile>(
		AWoTExpWardProjectile::StaticClass(), FVector(2000, 0, 100),
		FRotator::ZeroRotator, Params);
	AWoTETSoundProxy* EarthTremor = World->SpawnActor<AWoTETSoundProxy>(
		AWoTETSoundProxy::StaticClass(), FVector(3000, 0, 100),
		FRotator::ZeroRotator, Params);
	AWoTDartProjectile* Dart = World->SpawnActor<AWoTDartProjectile>(
		AWoTDartProjectile::StaticClass(), FVector(4000, 0, 100),
		FRotator::ZeroRotator, Params);
	AWoTAMAPearl* Pearl = World->SpawnActor<AWoTAMAPearl>(
		AWoTAMAPearl::StaticClass(), FVector(5000, 0, 100),
		FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("fireball"), Fireball)
		|| !TestNotNull(TEXT("exp ward"), Ward)
		|| !TestNotNull(TEXT("earth tremor proxy"), EarthTremor)
		|| !TestNotNull(TEXT("dart"), Dart)
		|| !TestNotNull(TEXT("AMA pearl"), Pearl))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	Ward->Activate();
	Dart->VariantColor = TEXT("Blue");
	Dart->OnRep_VariantColor();
	const FWoTUE1LightRow* FireballRow =
		WoTUE1LightRegistry::Find(TEXT("AngrealFireballProjectile"));
	const FWoTUE1LightRow* WardRow =
		WoTUE1LightRegistry::Find(TEXT("AngrealExpWardProjectile"));
	const FWoTUE1LightRow* EarthTremorRow =
		WoTUE1LightRegistry::Find(TEXT("ETSoundProxy"));
	const FWoTUE1LightRow* DartRow =
		WoTUE1LightRegistry::Find(TEXT("AngrealDartProjectile"));
	const FWoTUE1LightRow* PearlRow =
		WoTUE1LightRegistry::Find(TEXT("AMAPearl"));
	check(FireballRow && WardRow && EarthTremorRow && DartRow && PearlRow);
	UE1LightRegistryTests_TestComponent(*this, TEXT("Fireball"),
		Fireball->Light, *FireballRow);
	UE1LightRegistryTests_TestComponent(*this, TEXT("ExpWard"),
		Ward->Light, *WardRow);
	UE1LightRegistryTests_TestComponent(*this, TEXT("ETSoundProxy"),
		EarthTremor->Light, *EarthTremorRow);
	UE1LightRegistryTests_TestComponent(*this, TEXT("Dart"),
		Dart->Light, *DartRow);
	UE1LightRegistryTests_TestComponent(*this, TEXT("AMAPearl"),
		Pearl->DomeLight, *PearlRow);

	UWoTDynamicLightSubsystem* Subsystem =
		World->GetSubsystem<UWoTDynamicLightSubsystem>();
	if (TestNotNull(TEXT("dynamic light subsystem"), Subsystem))
	{
		const TArray<FWoTDynamicLightSample> Samples =
			Subsystem->GatherForTesting(0.0f, 1.0f);
		const FWoTDynamicLightSample* WardSample =
			UE1LightRegistryTests_FindSample(Samples, Ward->Light);
		const FWoTDynamicLightSample* EarthTremorSample =
			UE1LightRegistryTests_FindSample(Samples, EarthTremor->Light);
		const FWoTDynamicLightSample* PearlSample =
			UE1LightRegistryTests_FindSample(Samples, Pearl->DomeLight);
		if (TestNotNull(TEXT("AMAPearl registered sample"), PearlSample))
		{
			// Regression: LE_Shell used to register as NonIncidence.
			TestEqual(TEXT("AMAPearl registers LE_Shell as the Shell receiver"),
				PearlSample->Shape, EWoTDynLightShape::Shell);
		}
		if (TestNotNull(TEXT("ExpWard registered sample"), WardSample))
		{
			// Regression: the old site supplied saturation 255 (white).
			TestEqual(TEXT("ExpWard pure-red saturation byte"),
				WardSample->Saturation, static_cast<uint8>(0));
		}
		if (TestNotNull(TEXT("ETSoundProxy registered sample"), EarthTremorSample))
		{
			// Regression: the old site supplied saturation 255 (white).
			TestEqual(TEXT("ETSoundProxy pure-hue saturation byte"),
				EarthTremorSample->Saturation, static_cast<uint8>(0));
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
