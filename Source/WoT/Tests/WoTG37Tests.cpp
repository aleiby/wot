// G37 headless specs: Distant Eye's two-phase placement/view contract, the
// priority-ordered Use/Cease reflector seam, eye-owned camera/FOV presentation,
// dart stream, exit paths, and shot-only charge economy. Retail citations are
// pinned beside the native implementation; this file exercises the observable
// composite rather than re-implementing the UnrealScript.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAMAReflectors.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvDistantEye.h"
#include "../Inventory/WoTDistantEyeCastReflector.h"
#include "../Inventory/WoTDistantEyeLeech.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTDistantEyeProjectile.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTSoundTable.h"
#include "../WoTRetailText.h"

#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Camera/PlayerCameraManager.h"

namespace
{
	struct FG37Fixture
	{
		UWorld* World = nullptr;
		AWoTPlayerController* Controller = nullptr;
		AWoTCharacter* Pawn = nullptr;
		AWoTAngrealInvDistantEye* Item = nullptr;
	};

	FG37Fixture MakeG37Fixture(const FVector& Location = FVector::ZeroVector)
	{
		FG37Fixture F;
		F.World = WoTTest::CreateSpecWorld();
		F.Controller = F.World->SpawnActor<AWoTPlayerController>();
		F.Pawn = F.World->SpawnActor<AWoTCharacter>(Location,
			FRotator::ZeroRotator);
		if (!F.Controller || !F.Pawn)
		{
			return F;
		}
		F.Controller->SetAsLocalPlayerController();
		F.Controller->InitPlayerState();
		F.Controller->Possess(F.Pawn);
		F.Pawn->Health = 100;
		F.Item = F.World->SpawnActor<AWoTAngrealInvDistantEye>();
		if (F.Item)
		{
			F.Item->CurCharges = 5;
			F.Item->GiveTo(F.Pawn);
		}
		return F;
	}

	int32 CountG37Sound(UWorld* World, FName SubPath)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTSoundBurst> It(World); It; ++It)
		{
			Count += IsValid(*It) && It->SoundSubPath == SubPath ? 1 : 0;
		}
		return Count;
	}

	int32 CountEyeDarts(UWorld* World, const AWoTAngrealInventory* Source)
	{
		int32 Count = 0;
		for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
		{
			Count += IsValid(*It) && It->SourceAngreal == Source ? 1 : 0;
		}
		return Count;
	}

	AWoTDartProjectile* FindEyeDart(UWorld* World,
		const AWoTAngrealInventory* Source)
	{
		for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
		{
			if (IsValid(*It) && It->SourceAngreal == Source)
			{
				return *It;
			}
		}
		return nullptr;
	}

	AActor* SpawnG37Blocker(UWorld* World, const FVector& Location)
	{
		AActor* Blocker = World->SpawnActor<AActor>();
		UBoxComponent* Box = NewObject<UBoxComponent>(Blocker, TEXT("G37Blocker"));
		Box->SetBoxExtent(FVector(20.0f));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Blocker->SetRootComponent(Box);
		Box->RegisterComponent();
		Blocker->SetActorLocation(Location);
		return Blocker;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG37PlacementAndViewSpec,
	"WoT.Angreal.DistantEye.PlacementAndView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG37PlacementAndViewSpec::RunTest(const FString&)
{
	FG37Fixture F = MakeG37Fixture(FVector(100.0f, 200.0f, 300.0f));
	if (!TestNotNull(TEXT("fixture item"), F.Item))
	{
		WoTTest::DestroySpecWorld(F.World);
		return true;
	}

	F.Controller->SetControlRotation(FRotator(10.0f, 90.0f, 0.0f));
	const FVector ExpectedLocation = F.Pawn->GetActorLocation()
		+ F.Controller->GetControlRotation().RotateVector(FVector(48, 0, 96));
	F.Item->CastArtifact();
	AWoTDistantEyeProjectile* Eye = F.Item->GetEye();
	TestNotNull(TEXT("first cast fixes an eye"), Eye);
	if (Eye)
	{
		TestTrue(TEXT("SpawnOffset is view-rotated from the owner"),
			Eye->GetActorLocation().Equals(ExpectedLocation, 0.1f));
		TestTrue(TEXT("eye inherits the view rotation"),
			Eye->GetActorRotation().Equals(F.Controller->GetControlRotation(), 0.1f));
	}
	TestEqual(TEXT("placement has the replicated DropDE marker"),
		CountG37Sound(F.World, TEXT("Angreal/DropDE")), 1);
	TestEqual(TEXT("placement is free"), F.Item->CurCharges, 5);

	if (Eye)
	{
		Eye->SetActorRotation(FRotator(-12.0f, 27.0f, 0.0f));
	}
	F.Controller->SetControlRotation(FRotator::ZeroRotator);
	F.Item->CastArtifact();
	TestTrue(TEXT("second cast enters eye view"), F.Item->IsViewing());
	TestEqual(TEXT("eye is the view target"), F.Controller->GetViewTarget(),
		static_cast<AActor*>(Eye));
	if (Eye)
	{
		TestTrue(TEXT("controller rotation snaps to the eye"),
			F.Controller->GetControlRotation().Equals(Eye->GetActorRotation(), 0.1f));
		FMinimalViewInfo View;
		Eye->CalcCamera(0.0f, View);
		TestTrue(TEXT("eye camera originates at the eye"),
			View.Location.Equals(Eye->GetActorLocation(), 0.1f));
		TestTrue(TEXT("eye camera follows owner control rotation"),
			View.Rotation.Equals(F.Controller->GetControlRotation(), 0.1f));
	}
	TestEqual(TEXT("view activation has the ActivateDE marker"),
		CountG37Sound(F.World, TEXT("Angreal/ActivateDE")), 1);
	TestEqual(TEXT("view activation is free"), F.Item->CurCharges, 5);

	// G32 owns the higher camera state: an eye that was already external becomes
	// the end-camera anchor, and the controller no longer delegates to Eye::CalcCamera.
	F.Controller->NotifyMatchEnded();
	FMinimalViewInfo EndView;
	F.Controller->CalcCamera(0.0f, EndView);
	TestFalse(TEXT("match-end camera takes over from exact eye location"),
		Eye && EndView.Location.Equals(Eye->GetActorLocation(), 0.1f));

	// Review regression (G32 overwrite): an eye destroyed AFTER match end must
	// not stomp the end-camera view target back to the pawn — ExitViewing only
	// restores when the eye still owns the view.
	AActor* EndTarget = F.Controller->GetViewTarget();
	if (Eye)
	{
		Eye->Destroy();
	}
	TestTrue(TEXT("post-match eye destruction leaves the end camera target"),
		F.Controller->GetViewTarget() == EndTarget
		&& F.Controller->GetViewTarget() != F.Pawn);

	WoTTest::DestroySpecWorld(F.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG37UnconditionalPlacementAndDefaultsSpec,
	"WoT.Angreal.DistantEye.UnconditionalPlacementAndDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG37UnconditionalPlacementAndDefaultsSpec::RunTest(const FString&)
{
	// Retail placement is UNCONDITIONAL (the eye ships bCollideWorld=False, no
	// bCollideWhenPlacing — UE1 SpawnActor never encroach-tests it): geometry at
	// the offset must NOT stop the drop; the eye may legally sit inside a wall.
	FG37Fixture F = MakeG37Fixture();
	F.Controller->SetControlRotation(FRotator::ZeroRotator);
	SpawnG37Blocker(F.World, FVector(48, 0, 96));
	F.Item->CastArtifact();
	AWoTDistantEyeProjectile* Eye = F.Item->GetEye();
	if (TestNotNull(TEXT("geometry does not stop the drop (AlwaysSpawn)"), Eye))
	{
		TestTrue(TEXT("the eye sits at the authored offset, wall or not"),
			Eye->GetActorLocation().Equals(FVector(48, 0, 96), 1.0f));
	}
	TestFalse(TEXT("placement alone does not enter viewing"), F.Item->IsViewing());
	TestEqual(TEXT("placement plays DropDE"),
		CountG37Sound(F.World, TEXT("Angreal/DropDE")), 1);
	TestEqual(TEXT("placement spends no charge"), F.Item->CurCharges, 5);

	const AWoTAngrealInvDistantEye* CDO =
		GetDefault<AWoTAngrealInvDistantEye>();
	TestTrue(TEXT("common Air/Earth combat artifact"), CDO->bCommon
		&& CDO->bElementAir && CDO->bElementEarth && CDO->bCombat);
	TestFalse(TEXT("retail-dead bInfo remains false"), CDO->bInfo);
	TestEqual(TEXT("initial charges min"), CDO->MinInitialCharges, 3);
	TestEqual(TEXT("initial charges max"), CDO->MaxInitialCharges, 7);
	TestEqual(TEXT("charge cap"), CDO->MaxCharges, 10);
	TestEqual(TEXT("inventory group"), CDO->InventoryGroup, uint8(58));
	TestEqual(TEXT("pickup scale"), CDO->PickupViewScale, 0.7f);
	TestEqual(TEXT("draw scale"), CDO->DrawScale, 0.7f);
	// Title/PickupMessage are retail localized text: Angreal.int [AngrealInvDistantEye]
	// at runtime (FWoTRetailText), never compiled into the port.
	if (FWoTRetailText::EnsureLoaded())
	{
		const FString* Title = FWoTRetailText::FindLocalized(
			AWoTAngrealInvDistantEye::StaticClass(), TEXT("Title"));
		const FString* Pickup = FWoTRetailText::FindLocalized(
			AWoTAngrealInvDistantEye::StaticClass(), TEXT("PickupMessage"));
		TestTrue(TEXT("Distant Eye title is the retail Angreal.int text"),
			Title && !Title->IsEmpty() && CDO->Title == *Title);
		TestTrue(TEXT("retail pickup message is the Angreal.int text"),
			Pickup && !Pickup->IsEmpty() && CDO->PickupMessage == *Pickup);
	}
	else
	{
		AddWarning(TEXT("no retail .int source: Distant Eye text not checked"));
	}
	TestEqual(TEXT("ActivateDE soft path"),
		CDO->ActivateSound.ToSoftObjectPath().ToString(),
		FString(TEXT("/Game/WoT/Sounds/Angreal/ActivateDE.ActivateDE")));
	TestEqual(TEXT("DeactivateDE soft path"),
		CDO->DeactivateSound.ToSoftObjectPath().ToString(),
		FString(TEXT("/Game/WoT/Sounds/Angreal/DeactivateDE.DeactivateDE")));
	TestEqual(TEXT("DropDE soft path"),
		CDO->DropSound.ToSoftObjectPath().ToString(),
		FString(TEXT("/Game/WoT/Sounds/Angreal/DropDE.DropDE")));
	TestEqual(TEXT("I_DistantEye status icon"),
		CDO->StatusIcon.ToSoftObjectPath().ToString(),
		FString(TEXT("/Game/WoT/Textures/Angreal/I_DistantEye.I_DistantEye")));
	TestEqual(TEXT("M_DistantEye status frame"),
		CDO->StatusIconFrame.ToSoftObjectPath().ToString(),
		FString(TEXT("/Game/WoT/Textures/Angreal/M_DistantEye.M_DistantEye")));
	const AWoTDistantEyeProjectile* EyeCDO =
		GetDefault<AWoTDistantEyeProjectile>();
	TestEqual(TEXT("eye health"), EyeCDO->Health, 50);
	TestTrue(TEXT("eye is permanent and always relevant"),
		EyeCDO->InitialLifeSpan == 0.0f && EyeCDO->bAlwaysRelevant);
	TestTrue(TEXT("eye is shootable at radius/height 8"),
		EyeCDO->bProjTarget
		&& FMath::IsNearlyEqual(EyeCDO->Collision->GetUnscaledSphereRadius(), 8.0f));
	TestEqual(TEXT("eye ignores world collision"),
		EyeCDO->Collision->GetCollisionResponseToChannel(ECC_WorldStatic),
		ECR_Ignore);
	TestEqual(TEXT("eye overlaps the projectile channel"),
		EyeCDO->Collision->GetCollisionResponseToChannel(ECC_GameTraceChannel1),
		ECR_Overlap);
	TestEqual(TEXT("seven skin frames"), EyeCDO->AnimSkins.Num(), 7);
	TestEqual(TEXT("skin frame time"), EyeCDO->FrameTime, 0.2f);
	TestEqual(TEXT("dart stream RPM"), EyeCDO->EyeRoundsPerMinute, 320.0f);
	TestEqual(TEXT("reflector priority"),
		GetDefault<UWoTDistantEyeCastReflector>()->Priority, uint8(128));
	TestFalse(TEXT("reflector cannot be cleansed"),
		GetDefault<UWoTDistantEyeCastReflector>()->bRemovable);
	TestEqual(TEXT("reflector initial duration is default eye health"),
		GetDefault<UWoTDistantEyeCastReflector>()->GetInitialDuration(), 50.0f);

	WoTTest::DestroySpecWorld(F.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG37CastReflectorSpec,
	"WoT.Angreal.DistantEye.CastReflector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG37CastReflectorSpec::RunTest(const FString&)
{
	FG37Fixture F = MakeG37Fixture();
	F.Item->CastArtifact();
	F.Item->CastArtifact();
	AWoTDistantEyeProjectile* Eye = F.Item->GetEye();
	if (!TestNotNull(TEXT("open eye"), Eye))
	{
		WoTTest::DestroySpecWorld(F.World);
		return true;
	}

	AWoTAngrealInvDart* Held = F.World->SpawnActor<AWoTAngrealInvDart>();
	Held->CurCharges = 5;
	Held->GiveTo(F.Pawn);
	F.Pawn->InventoryComponent->SelectItem(Held);
	WoTTest::TickWorld(F.World, 1);
	F.Pawn->InventoryComponent->UseAngreal();
	TestEqual(TEXT("viewing Use fires one eye dart immediately"),
		CountEyeDarts(F.World, F.Item), 1);
	TestEqual(TEXT("held artifact does not cast through interception"),
		CountEyeDarts(F.World, Held), 0);
	TestEqual(TEXT("placement + view + dart spend no eye charge"),
		F.Item->CurCharges, 5);
	TestEqual(TEXT("free actions never call UseCharge"),
		F.Item->CastUseCount, 0);
	F.Pawn->InventoryComponent->UseAngreal();
	TestEqual(TEXT("same-tick Use respects eye RPM cadence"),
		CountEyeDarts(F.World, F.Item), 1);
	if (AWoTDartProjectile* Dart = FindEyeDart(F.World, F.Item))
	{
		TestEqual(TEXT("eye dart uses the folded green variant"),
			Dart->VariantColor, FName(TEXT("Green")));
		TestTrue(TEXT("eye dart originates at eye location"),
			Dart->GetActorLocation().Equals(Eye->GetActorLocation(), 0.1f));
		TestTrue(TEXT("eye dart uses eye rotation"),
			Dart->GetActorRotation().Equals(Eye->GetActorRotation(), 0.1f));
	}
	F.Pawn->InventoryComponent->CeaseUsingAngreal();
	TestFalse(TEXT("Cease interception stops the eye stream"), Eye->IsFiring());

	UWoTNoCastReflector* NoCast = NewObject<UWoTNoCastReflector>(F.Pawn);
	NoCast->Install(F.Pawn);
	F.Pawn->InventoryComponent->UseAngreal();
	TestFalse(TEXT("priority-255 NoCast wins over eye priority 128"),
		Eye->IsFiring());
	TestEqual(TEXT("NoCast prevents another dart"),
		CountEyeDarts(F.World, F.Item), 1);

	WoTTest::DestroySpecWorld(F.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG37ExitAndChargeSpec,
	"WoT.Angreal.DistantEye.ExitsAndChargeEconomy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG37ExitAndChargeSpec::RunTest(const FString&)
{
	// Moving more than ten units exits view but keeps the eye and its charge.
	FG37Fixture Move = MakeG37Fixture();
	Move.Item->CastArtifact();
	Move.Item->CastArtifact();
	Move.Pawn->SetActorLocation(FVector(10.1f, 0, 0));
	Move.Item->Tick(0.0f);
	TestFalse(TEXT(">10 owner movement exits eye view"), Move.Item->IsViewing());
	TestEqual(TEXT("movement exit restores pawn view target"),
		Move.Controller->GetViewTarget(), static_cast<AActor*>(Move.Pawn));
	TestNotNull(TEXT("movement exit keeps placed eye"), Move.Item->GetEye());
	TestNull(TEXT("movement exit removes cast reflector"),
		Move.Pawn->FindComponentByClass<UWoTDistantEyeCastReflector>());
	TestNull(TEXT("movement exit removes view leech"),
		Move.Pawn->FindComponentByClass<UWoTDistantEyeLeech>());
	TestEqual(TEXT("movement exit is free"), Move.Item->CurCharges, 5);
	TestEqual(TEXT("movement exit has DeactivateDE marker"),
		CountG37Sound(Move.World, TEXT("Angreal/DeactivateDE")), 1);
	WoTTest::DestroySpecWorld(Move.World);

	// Shooting the eye is the only path that spends a charge, exactly once.
	FG37Fixture Shot = MakeG37Fixture();
	Shot.Item->CastArtifact();
	Shot.Item->CastArtifact();
	AWoTDistantEyeProjectile* ShotEye = Shot.Item->GetEye();
	if (TestNotNull(TEXT("shot-path eye"), ShotEye))
	{
		FDamageEvent Damage;
		ShotEye->TakeDamage(50.0f, Damage, nullptr, nullptr);
	}
	TestNull(TEXT("fatal eye damage clears item pointer"), Shot.Item->GetEye());
	TestFalse(TEXT("fatal eye damage restores owner view"), Shot.Item->IsViewing());
	TestEqual(TEXT("fatal eye damage restores pawn view target"),
		Shot.Controller->GetViewTarget(), static_cast<AActor*>(Shot.Pawn));
	TestEqual(TEXT("fatal eye damage has DeactivateDE marker"),
		CountG37Sound(Shot.World, TEXT("Angreal/DeactivateDE")), 1);
	TestEqual(TEXT("fatal eye damage spends exactly one charge"),
		Shot.Item->CurCharges, 4);
	TestEqual(TEXT("fatal eye damage records one UseCharge call"),
		Shot.Item->CastUseCount, 1);
	TestEqual(TEXT("destroyed eye emits DropDE impact marker"),
		CountG37Sound(Shot.World, TEXT("Angreal/DropDE")), 2);
	WoTTest::DestroySpecWorld(Shot.World);

	// Inventory drop and owner death destroy the eye and therefore take that
	// same one destruction charge; neither path may double-spend it.
	FG37Fixture Drop = MakeG37Fixture(FVector(200, 0, 0));
	Drop.Item->CastArtifact();
	Drop.Item->DropFrom(FVector(600, 0, 100));
	TestNull(TEXT("dropping destroys the eye"), Drop.Item->GetEye());
	TestEqual(TEXT("drop-triggered eye destruction charges once"),
		Drop.Item->CurCharges, 4);
	TestEqual(TEXT("drop-triggered destruction records one UseCharge"),
		Drop.Item->CastUseCount, 1);
	WoTTest::DestroySpecWorld(Drop.World);

	FG37Fixture Death = MakeG37Fixture();
	Death.Item->CastArtifact();
	Death.Pawn->TakeDamage(1000.0f, FDamageEvent(), nullptr, nullptr);
	TestNull(TEXT("owner death destroys the eye"), Death.Item->GetEye());
	TestEqual(TEXT("death-triggered eye destruction charges once"),
		Death.Item->CurCharges, 4);
	TestEqual(TEXT("death-triggered destruction records one UseCharge"),
		Death.Item->CastUseCount, 1);
	WoTTest::DestroySpecWorld(Death.World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTG37LeechSpec,
	"WoT.Angreal.DistantEye.OwnerViewLeech",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTG37LeechSpec::RunTest(const FString&)
{
	FG37Fixture F = MakeG37Fixture();
	const float OldFOV = F.Controller->GetCameraFOVForTest();
	UWoTDistantEyeLeech* Leech = NewObject<UWoTDistantEyeLeech>(F.Pawn);
	Leech->AttachTo(F.Pawn);
	TestEqual(TEXT("attach widens owner FOV"),
		F.Controller->GetCameraFOVForTest(), 120.0f);
	TestEqual(TEXT("attach retains retail zero glow scale"),
		F.Controller->ConstantGlowScale, 0.0f);
	TestTrue(TEXT("attach applies retail glow fog through .001 fold"),
		F.Controller->ConstantGlowFog.Equals(
			FVector(0.28125f, 0.421875f, 0.140625f), KINDA_SMALL_NUMBER));
	Leech->Unattach();
	TestEqual(TEXT("unattach restores exact owner FOV"),
		F.Controller->GetCameraFOVForTest(), OldFOV);
	TestTrue(TEXT("unattach cancels owner glow fog"),
		F.Controller->ConstantGlowFog.IsNearlyZero());

	WoTTest::DestroySpecWorld(F.World);
	return true;
}

#endif
