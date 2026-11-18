// Headless specs for the P04 Light Sphere port: AngrealInvLightGlobe (cast
// toggle, color mapping, dead-owner/drop teardown, pickup star), the
// AWoTLightGlobe follow actor + Pixie jar chain, the five AngrealInvLG*
// self-replacing variants, and the registry rows.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvLightGlobe.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTActorRotator.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTLightGlobe.h"
#include "../WoTPlayerController.h"
#include "../WoTMeshFX.h"
#include "../WoTParticleSprayer.h"
#include "../WoTUE1LightRegistry.h"
#include "../WoTWeaveFX.h"

#include "Components/PointLightComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightGlobeCastToggleTest,
	"WoT.LightGlobe.CastToggleAndFollow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightGlobeCastToggleTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UWoTInventoryComponent* Inv = Pawn->InventoryComponent;

	AWoTAngrealInvLightGlobe* Item = World->SpawnActor<AWoTAngrealInvLightGlobe>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);

	TestEqual(TEXT("999/999 seeding (standalone SP path)"), Item->CurCharges, 999);
	TestTrue(TEXT("pickup grabbed and held"),
		Inv->SelectedItem.Get() == Item && Item->GetOwner() == Pawn);
	// GiveTo folded the non-faction pawn through the seam's NPC fallback.
	TestTrue(TEXT("plain pawn owner maps to the Green globe"),
		Item->LightType == AWoTLightGlobeGreen::StaticClass());

	// First activation: the globe spawns following the caster.
	Inv->UseAngreal();
	AWoTLightGlobe* Globe = Item->Light;
	TestNotNull(TEXT("cast spawned the light globe"), Globe);
	if (!Globe)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestTrue(TEXT("globe carries the color class identity"),
		Globe->GetClass() == AWoTLightGlobeGreen::StaticClass());
	TestTrue(TEXT("globe follows the caster"), Globe->FollowActor == Pawn);
	TestEqual(TEXT("cost-0 charge use leaves the count"), Item->CurCharges, 999);

	// The local cosmetic chain (standalone machine).
	TestNotNull(TEXT("jar rotator spawned"), Globe->GetJarRotatorForTest());
	TestNotNull(TEXT("glass jar spawned"), Globe->GetJarForTest());
	TestNotNull(TEXT("pixie rotator spawned"), Globe->GetPixieForTest());
	AWoTParticleSprayer* Sprayer = Globe->GetSprayerForTest();
	TestNotNull(TEXT("color sprayer spawned"), Sprayer);
	if (AWoTActorRotator* JarRotator = Globe->GetJarRotatorForTest())
	{
		TestEqual(TEXT("jar orbit radius bound"), JarRotator->MaxRadius, 20.0f);
		TestEqual(TEXT("jar orbit tumbles (PHYS_Rotating yaw)"),
			JarRotator->PhysRotationRateUnits.Yaw, 6000.0);
	}
	if (Sprayer && Sprayer->Light)
	{
		// LGLightRadius default -1 keeps the authored 25 -> (25+1)*25 UU.
		TestEqual(TEXT("sprayer light keeps the authored radius"),
			Sprayer->Light->AttenuationRadius, 650.0f);
	}

	// The smoothed follow converges on the caster-relative offset.
	Pawn->SetActorLocation(FVector(400, 0, 100));
	WoTTest::TickWorld(World, 3.0f);
	const FVector Desired = Pawn->GetActorLocation()
		+ Pawn->GetActorRotation().RotateVector(FVector(30, 15, 70));
	TestTrue(TEXT("globe eased to the follow offset"),
		Globe->GetActorLocation().Equals(Desired, 8.0));

	// Second activation unravels it.
	WoTTest::TickWorld(World, 0.05f);
	Inv->UseAngreal();
	TestNull(TEXT("toggle-off cleared the handle"), Item->Light.Get());
	TestFalse(TEXT("globe actor destroyed"), IsValid(Globe));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightGlobeColorMapTest,
	"WoT.LightGlobe.SetColorMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightGlobeColorMapTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvLightGlobe* Item = World->SpawnActor<AWoTAngrealInvLightGlobe>(
		FVector(0, 0, 5000), FRotator::ZeroRotator);

	TestTrue(TEXT("CDO default is the base globe"),
		Item->GetClass()->GetDefaultObject<AWoTAngrealInvLightGlobe>()->LightType
			== AWoTLightGlobe::StaticClass());

	struct { const TCHAR* Color; UClass* Expected; } Cases[] = {
		{TEXT("Red"), AWoTLightGlobeRed::StaticClass()},
		{TEXT("PC_Red"), AWoTLightGlobeRed::StaticClass()},
		{TEXT("Yellow"), AWoTLightGlobeYellow::StaticClass()},
		{TEXT("PC_Gold"), AWoTLightGlobeYellow::StaticClass()},
		{TEXT("Gold"), AWoTLightGlobeYellow::StaticClass()},
		{TEXT("Green"), AWoTLightGlobeGreen::StaticClass()},
		{TEXT("PC_Green"), AWoTLightGlobeGreen::StaticClass()},
		{TEXT("Purple"), AWoTLightGlobePurple::StaticClass()},
		{TEXT("PC_Purple"), AWoTLightGlobePurple::StaticClass()},
		{TEXT("Blue"), AWoTLightGlobeBlue::StaticClass()},
		{TEXT("PC_Blue"), AWoTLightGlobeBlue::StaticClass()},
		// The retail default case: anything unmapped (incl. White) is Blue.
		{TEXT("White"), AWoTLightGlobeBlue::StaticClass()},
	};
	for (const auto& Case : Cases)
	{
		Item->SetColor(Case.Color);
		TestTrue(FString::Printf(TEXT("SetColor(%s)"), Case.Color),
			Item->LightType == Case.Expected);
	}

	// The color leaves' own Color defaults (LightGlobeBlue.uc is empty — the
	// base default already reads Blue).
	TestEqual(TEXT("base Color default"),
		AWoTLightGlobe::StaticClass()->GetDefaultObject<AWoTLightGlobe>()->Color,
		FName(TEXT("Blue")));
	TestEqual(TEXT("yellow leaf Color"),
		AWoTLightGlobeYellow::StaticClass()->GetDefaultObject<AWoTLightGlobe>()->Color,
		FName(TEXT("Yellow")));

	// AI: the WaysZone legs are SP-scope; MP-reachable behavior is -MaxInt.
	TestEqual(TEXT("GetPriority is the retail don't-cast"),
		Item->GetPriority(), -static_cast<float>(MAX_int32));

	// The GiveTo color path itself (retro-review: SetColor-direct doesn't pin
	// it): an unpossessed faction pawn maps through the mesh-name fallback...
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		APawn* Forsaken = World->SpawnActor<AWoTForsaken>(
			FVector(0, 0, 6000), FRotator::ZeroRotator, Params);
		AWoTAngrealInvLightGlobe* ForsakenGlobe =
			World->SpawnActor<AWoTAngrealInvLightGlobe>(
				FVector(0, 0, 6000), FRotator::ZeroRotator, Params);
		if (Forsaken && ForsakenGlobe)
		{
			ForsakenGlobe->GiveTo(Forsaken);
			TestTrue(TEXT("GiveTo maps a Forsaken carrier to Red"),
				ForsakenGlobe->LightType == AWoTLightGlobeRed::StaticClass());
		}
		// ...and a player-controlled faction pawn reads its class PlayerColor.
		APawn* AesSedai = World->SpawnActor<AWoTAesSedai>(
			FVector(0, 0, 7000), FRotator::ZeroRotator, Params);
		AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>(Params);
		AWoTAngrealInvLightGlobe* AesGlobe =
			World->SpawnActor<AWoTAngrealInvLightGlobe>(
				FVector(0, 0, 7000), FRotator::ZeroRotator, Params);
		if (AesSedai && PC && AesGlobe)
		{
			PC->Possess(AesSedai);
			AesGlobe->GiveTo(AesSedai);
			TestTrue(TEXT("GiveTo maps a player AesSedai (PC_Blue) to Blue"),
				AesGlobe->LightType == AWoTLightGlobeBlue::StaticClass());
		}
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightGlobeTeardownTest,
	"WoT.LightGlobe.DeadOwnerDropAndTriggerTeardown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightGlobeTeardownTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTAngrealInvLightGlobe* Item = World->SpawnActor<AWoTAngrealInvLightGlobe>(
		FVector(0, 0, 100), FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("held"), Item->GetOwner() == Pawn);

	// UnCast with a LIVE owner keeps the light (artifact switching survives).
	Item->CastArtifact();
	TestNotNull(TEXT("lit"), Item->Light.Get());
	Item->UnCastArtifact();
	TestNotNull(TEXT("live-owner UnCast keeps the light"), Item->Light.Get());

	// The SP tutorial Trigger author unravels a lit globe.
	Item->WoTTrigger(nullptr, nullptr);
	TestNull(TEXT("trigger toggled the light off"), Item->Light.Get());

	// Dead owner: UnCast turns it off through the toggle.
	Item->CastArtifact();
	TestNotNull(TEXT("re-lit"), Item->Light.Get());
	Pawn->Health = 0;
	Item->UnCastArtifact();
	TestNull(TEXT("dead-owner UnCast unraveled the light"), Item->Light.Get());

	// Drop tears the light down outright.
	Pawn->Health = 100;
	Item->CastArtifact();
	AWoTLightGlobe* Globe = Item->Light;
	TestNotNull(TEXT("lit before drop"), Globe);
	Item->DropFrom(Pawn->GetActorLocation());
	TestNull(TEXT("drop cleared the handle"), Item->Light.Get());
	TestFalse(TEXT("drop destroyed the globe"), IsValid(Globe));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightGlobePickupStarTest,
	"WoT.LightGlobe.PickupStar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightGlobePickupStarTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	const FVector Spot(0, 0, 100);
	AWoTAngrealInvLightGlobe* Item = World->SpawnActor<AWoTAngrealInvLightGlobe>(
		Spot, FRotator::ZeroRotator);
	WoTTest::TickWorld(World, 0.1f);

	AWoTWeaveSpriteEffect* Star = Item->GetGlobeStarForTest();
	TestNotNull(TEXT("visible pickup spawned the star"), Star);
	if (!Star)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestFalse(TEXT("star visible"), Star->IsHidden());
	// The rotated GlowOffset (Y=7,Z=3) — the spinning pickup orbits it.
	const FVector Offset = Star->GetActorLocation() - Item->GetActorLocation();
	TestTrue(TEXT("star rides at the rotated glow offset"),
		FMath::IsNearlyEqual(Offset.Size2D(), 7.0, 0.05)
			&& FMath::IsNearlyEqual(Offset.Z, 3.0, 0.05));

	// Taking the item hides the star.
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		Spot, FRotator::ZeroRotator);
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	WoTTest::TickWorld(World, 0.1f);
	TestTrue(TEXT("taken"), Item->GetOwner() == Pawn);
	TestTrue(TEXT("held item hides the star"), Star->IsHidden());

	// Dropping shows it again at the new spot.
	Item->DropFrom(Pawn->GetActorLocation() + FVector(50, 0, 0));
	WoTTest::TickWorld(World, 0.05f);
	TestFalse(TEXT("dropped pickup shows the star"), Star->IsHidden());

	// Destroying the pickup destroys the star.
	Item->Destroy();
	TestFalse(TEXT("star died with the pickup"), IsValid(Star));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightGlobeVariantTest,
	"WoT.LightGlobe.VariantSelfReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightGlobeVariantTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();

	// A map-placed AngrealInvLGRed with instance charge overrides.
	const FTransform Where(FRotator::ZeroRotator, FVector(0, 0, 100));
	AWoTAngrealInvLGRed* Variant = World->SpawnActorDeferred<AWoTAngrealInvLGRed>(
		AWoTAngrealInvLGRed::StaticClass(), Where, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	TestTrue(TEXT("variant CDO carries its color LightType"),
		Variant->LightType == AWoTLightGlobeRed::StaticClass());
	Variant->MinInitialCharges = 3;
	Variant->MaxInitialCharges = 3;
	Variant->MaxCharges = 5;
	Variant->FinishSpawning(Where);

	TestFalse(TEXT("variant replaced itself"), IsValid(Variant));
	AWoTAngrealInvLightGlobe* Replacement = nullptr;
	for (TActorIterator<AWoTAngrealInvLightGlobe> It(World); It; ++It)
	{
		if (IsValid(*It))
		{
			Replacement = *It;
			break;
		}
	}
	TestNotNull(TEXT("base replacement exists"), Replacement);
	if (Replacement)
	{
		TestTrue(TEXT("replacement is the BASE class"),
			Replacement->GetClass() == AWoTAngrealInvLightGlobe::StaticClass());
		TestTrue(TEXT("replacement got the variant color"),
			Replacement->LightType == AWoTLightGlobeRed::StaticClass());
		TestEqual(TEXT("charge bounds carried"), Replacement->MaxCharges, 5);
		TestEqual(TEXT("min initial carried"), Replacement->MinInitialCharges, 3);
		// The retail PreBeginPlay order: the replacement seeded charges from
		// ITS OWN defaults before the bounds copy.
		TestEqual(TEXT("seeded from replacement defaults (retail order)"),
			Replacement->CurCharges, 999);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightGlobeRegistryTest,
	"WoT.LightGlobe.RegistryRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightGlobeRegistryTest::RunTest(const FString&)
{
	// PixieSprayer color rows: the .uc chain resolution with the base-template
	// inheritance rule and the authored Spark04/Spark03 slot swap.
	struct { const TCHAR* Name; float Hue; const TCHAR* Color; } Rows[] = {
		{TEXT("PixieSprayerBlue"), 140.0f, TEXT("Blue")},
		{TEXT("PixieSprayerGreen"), 96.0f, TEXT("Green")},
		{TEXT("PixieSprayerPurple"), 200.0f, TEXT("Purple")},
		{TEXT("PixieSprayerRed"), 0.0f, TEXT("Red")},
		{TEXT("PixieSprayerYellow"), 50.0f, TEXT("Yellow")},
	};
	for (const auto& Row : Rows)
	{
		const FWoTSprayerBundleRow* R = WoTWeaveFX::Find(Row.Name);
		TestNotNull(Row.Name, R);
		if (!R)
		{
			continue;
		}
		TestEqual(TEXT("volume"), R->Volume, 20.0f);
		TestEqual(TEXT("gravity"), R->Gravity.Z, -100.0);
		TestEqual(TEXT("templates"), R->Templates.Num(), 4);
		TestTrue(TEXT("LT_Steady light"), R->bHasLight);
		TestEqual(TEXT("registry-backed light"), R->LightRegistryKey,
			FName(TEXT("PixieSprayer")));
		TestTrue(TEXT("leaf overrides hue"), R->bOverrideLightHue);
		TestEqual(TEXT("light hue"), R->LightHue, Row.Hue);
		const FWoTUE1LightRow* LightRow =
			WoTUE1LightRegistry::Find(R->LightRegistryKey);
		if (TestNotNull(TEXT("PixieSprayer registry row"), LightRow))
		{
			TestEqual(TEXT("light brightness"), LightRow->Brightness,
				static_cast<uint8>(100));
			TestEqual(TEXT("light saturation"), LightRow->Saturation,
				static_cast<uint8>(64));
			TestEqual(TEXT("light radius"), LightRow->Radius, 25.0f);
		}
		if (R->Templates.Num() == 4)
		{
			// T0 lists no LifeSpan in the .uc — it inherits the BASE
			// ParticleSprayer element's 1.0 (binary-CDO rule), not zero.
			TestEqual(TEXT("T0 inherited LifeSpan"),
				R->Templates[0].Template.LifeSpan, 1.0f);
			// T1 lists no MaxDrawScale — inherits the base 1.0.
			TestEqual(TEXT("T1 inherited MaxDrawScale"),
				R->Templates[1].Template.MaxDrawScale, 1.0f);
			TestEqual(TEXT("T3 weight"), R->Templates[3].Template.Weight, 15.0f);
			TestTrue(TEXT("slot 2 is Spark04 (authored swap)"),
				R->Templates[2].MaterialSlug.ToString().Contains(
					FString::Printf(TEXT("Spark04%s"), Row.Color)));
			TestTrue(TEXT("slot 3 is Spark03 (authored swap)"),
				R->Templates[3].MaterialSlug.ToString().Contains(
					FString::Printf(TEXT("Spark03%s"), Row.Color)));
		}
	}
	// No shipped spawner exists for the base PixieSprayer — no speculative row.
	TestNull(TEXT("no base PixieSprayer row"),
		WoTWeaveFX::Find(TEXT("PixieSprayer")));

	const FWoTMeshEffectRow* Jar = WoTMeshFX::Find(TEXT("PixieJar"));
	TestNotNull(TEXT("PixieJar row"), Jar);
	if (Jar)
	{
		TestEqual(TEXT("jar mesh"), Jar->MeshName, FName(TEXT("AMAPearl")));
		TestEqual(TEXT("jar skin"), Jar->SkinNames[0], FName(TEXT("Glass")));
		TestEqual(TEXT("jar scale"), Jar->DrawScale, 0.2f);
		TestEqual(TEXT("jar glow"), Jar->ScaleGlow, 2.0f);
		TestEqual(TEXT("jar persistent"), Jar->LifeSpan, 0.0f);
		TestTrue(TEXT("jar translucent"),
			Jar->Style == EWoTMeshEffectStyle::Translucent);
	}

	const FWoTSpriteEffectRow* Star = WoTMeshFX::FindSprite(TEXT("LightGlobeStar"));
	TestNotNull(TEXT("LightGlobeStar row"), Star);
	if (Star)
	{
		TestEqual(TEXT("star frame"), Star->Frames[0],
			FName(TEXT("LightGlobeGlow1")));
		TestEqual(TEXT("star scale"), Star->DrawScale, 0.12f);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
