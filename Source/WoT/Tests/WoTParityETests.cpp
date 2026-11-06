#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTSeal.h"
#include "../Projectiles/WoTProjectile.h"
#include "../Traps/WoTAlarm.h"
#include "../WoTBounceableDecoration.h"
#include "../WoTCarcass.h"
#include "../WoTCharacter.h"
#include "../WoTDecal.h"
#include "../WoTFrontEnd.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTSoundTable.h"
#include "../WoTTeleporter.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTZoneVolume.h"

#include "Components/BoxComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	constexpr EAutomationTestFlags ParityFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FActorSpawnParameters ParityE_AlwaysSpawn()
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return P;
	}

	AActor* SpawnFloor(UWorld& World)
	{
		AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(
			FVector(0,0,-50), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
		UBoxComponent* Box = NewObject<UBoxComponent>(Floor, TEXT("ParityEFloor"));
		Box->SetupAttachment(Floor->GetRootComponent());
		Box->SetBoxExtent(FVector(4000,4000,50));
		Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Box->SetCollisionObjectType(ECC_WorldStatic);
		Box->SetCollisionResponseToAllChannels(ECR_Block);
		Box->RegisterComponent();
		return Floor;
	}

	struct FLocalPlayer
	{
		AWoTPlayerController* Controller = nullptr;
		AWoTCharacter* Pawn = nullptr;
	};

	FLocalPlayer SpawnLocalPlayer(UWorld& World, const FVector& Location = FVector(0,0,100),
		bool bHUD = false)
	{
		FLocalPlayer Result;
		Result.Controller = World.SpawnActor<AWoTPlayerController>(ParityE_AlwaysSpawn());
		Result.Pawn = World.SpawnActor<AWoTCharacter>(Location,
			FRotator::ZeroRotator, ParityE_AlwaysSpawn());
		if (Result.Controller && Result.Pawn)
		{
			Result.Controller->SetAsLocalPlayerController();
			Result.Controller->Possess(Result.Pawn);
			if (bHUD) Result.Controller->ClientSetHUD(AWoTHUD::StaticClass());
		}
		return Result;
	}

	void ParityE_RunCarcassVisibilityCones(FAutomationTestBase& Test)
	{
		UWorld* W = WoTTest::CreateSpecWorld();
		SpawnFloor(*W);
		FLocalPlayer Player = SpawnLocalPlayer(*W, FVector(0,0,100));
		Player.Controller->SetControlRotation(FRotator::ZeroRotator);
		AWoTCarcass* Corpse = W->SpawnActor<AWoTCarcass>(
			FVector(500,0,100), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
		WoTTest::TickWorld(W, 28.2f, 0.1f);
		Test.TestTrue(TEXT("front-cone corpse survives its cleanup timer"), IsValid(Corpse));
		Player.Controller->SetControlRotation(FRotator(0,180,0));
		WoTTest::TickWorld(W, 2.2f, 0.1f);
		Test.TestTrue(TEXT("retail squared-dot rear cone also keeps the corpse"), IsValid(Corpse));
		Player.Controller->SetControlRotation(FRotator(0,90,0));
		WoTTest::TickWorld(W, 2.2f, 0.1f);
		Test.TestFalse(TEXT("a corpse outside both 60-degree cones is removed"), IsValid(Corpse));
		WoTTest::DestroySpecWorld(W);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityEDecalManagerSpec,
	"WoT.Parity.E.DecalManager", ParityFlags)
bool FParityEDecalManagerSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	SpawnFloor(*W);
	AWoTDecalManager* Manager = W->SpawnActor<AWoTDecalManager>(ParityE_AlwaysSpawn());
	Manager->MaxDecals = 1; Manager->ShrinkTime = 1.0f; Manager->DecalExpireTime = 1.5f;
	AWoTDecal* First = WoTDecalFX::SpawnAligned(*W, TEXT("ScorchMark"),
		FVector(0,0,5), FVector::UpVector, false, 1.0f);
	AWoTDecal* Second = WoTDecalFX::SpawnAligned(*W, TEXT("ScorchMark"),
		FVector(100,0,5), FVector::UpVector, false, 1.0f);
	int32 Live = 0;
	for (TActorIterator<AWoTDecal> It(W); It; ++It) if (IsValid(*It)) ++Live;
	TestEqual(TEXT("map manager enforces its one-decal cap"), Live, 1);
	AWoTDecal* Survivor = IsValid(Second) ? Second : First;
	WoTTest::TickWorld(W, 1.0f);
	TestTrue(TEXT("decal shrinks during the manager window"), IsValid(Survivor)
		&& Survivor->GetDecalMesh()->GetRelativeScale3D().X < 0.75f);
	WoTTest::TickWorld(W, 0.7f);
	TestFalse(TEXT("manager expiry removes even a rendered decal"), IsValid(Survivor));
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityEBounceableSpec,
	"WoT.Parity.E.BounceableDecoration", ParityFlags)
bool FParityEBounceableSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	AWoTBounceableDecoration* Prop = W->SpawnActor<AWoTBounceableDecoration>(
		FVector(0,0,100), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	FWoTDamageEvent Hit; Hit.Damage = 20; Hit.UE1Momentum = FVector(1000,0,0);
	Prop->TakeDamage(20, Hit, nullptr, nullptr);
	TestTrue(TEXT("real damage enters Animate bounce"), Prop->IsBouncing());
	TestTrue(TEXT("retail damage transform launches upward"),
		Prop->GetBounceVelocity().Z > 0.0f);
	const FVector Before = Prop->GetActorLocation();
	WoTTest::TickWorld(W, 0.1f);
	TestNotEqual(TEXT("authority bounce moves the placed prop"),
		Prop->GetActorLocation(), Before);

	FLocalPlayer Caster = SpawnLocalPlayer(*W, FVector(-80,0,100));
	Caster.Controller->SetControlRotation(FRotator::ZeroRotator);
	Caster.Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	AWoTBounceableDecoration* AirTarget = W->SpawnActor<AWoTBounceableDecoration>(
		FVector(0,0,164), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	AirTarget->ConfigureRuntimeCollision(false, false, false, 12.0f, 4.0f);
	AWoTAngrealInvAirBurst* Air = W->SpawnActor<AWoTAngrealInvAirBurst>(ParityE_AlwaysSpawn());
	Air->GiveTo(Caster.Pawn);
	WoTTest::TickWorld(W, 0.1f);
	TestTrue(TEXT("Air Burst classifies the retail prop as affectable"),
		Air->IsAffectable(AirTarget));
	const FVector AirStart = Caster.Pawn->GetActorLocation()
		+ FVector(0,0,Caster.Pawn->BaseEyeHeight);
	const FVector AirEnd = AirStart + Caster.Pawn->GetViewRotation().Vector() * Air->RangeX;
	AirTarget->SetActorLocation(AirStart
		+ Caster.Pawn->GetViewRotation().Vector() * 80.0f);
	FHitResult AirProbeHit;
	TestTrue(TEXT("Air Burst trace crosses the disabled-collision retail cylinder"),
		AirTarget->Collision->ExactLineTrace(AirStart, AirEnd, AirProbeHit));
	Air->CastArtifact();
	AddInfo(FString::Printf(TEXT("AIR_BURST_BOUNCE owner=%s uses=%d start=%s end=%s target=%s velocity=%s"),
		Air->GetOwner() ? *Air->GetOwner()->GetName() : TEXT("none"),
		Air->CastUseCount, *AirStart.ToString(), *AirEnd.ToString(),
		*AirTarget->GetActorLocation().ToString(), *AirTarget->GetBounceVelocity().ToString()));
	TestTrue(TEXT("a real Air Burst cast pushes a bounceable decoration"),
		AirTarget->IsBouncing() && AirTarget->GetBounceVelocity().X > 0.0f);
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityEPlayerGibSpec,
	"WoT.Parity.E.PlayerKillingBlowGib", ParityFlags)
bool FParityEPlayerGibSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	FLocalPlayer Player = SpawnLocalPlayer(*W);
	Player.Pawn->Health = 20;
	FWoTDamageEvent Hit; Hit.Damage = 200;
	Player.Pawn->TakeDamage(200, Hit, nullptr, nullptr);
	TestTrue(TEXT("mass-sized killing blow forces the player gib path"),
		Player.Pawn->WasGibbedOnKillingBlow());
	TestEqual(TEXT("overkill health is the negative actual damage"),
		Player.Pawn->Health, -200);
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityENPCGibSpec,
	"WoT.Parity.E.PawnKillingBlowGib", ParityFlags)
bool FParityENPCGibSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	AWoTNPCCharacter* NPC = W->SpawnActor<AWoTNPCCharacter>(
		FVector(0,0,100), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	NPC->Health = 20;
	TWeakObjectPtr<AWoTNPCCharacter> WeakNPC(NPC);
	FWoTDamageEvent Hit; Hit.Damage = 200;
	NPC->TakeDamage(200, Hit, nullptr, nullptr);
	TestFalse(TEXT("gibbed non-player pawn is destroyed immediately"), WeakNPC.IsValid());
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityEAutoSwitchSpec,
	"WoT.Parity.E.EmptySelectionAutoSwitch", ParityFlags)
bool FParityEAutoSwitchSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	FLocalPlayer Player = SpawnLocalPlayer(*W, FVector(0,0,100), true);
	AWoTAngrealInvFireball* Empty = W->SpawnActor<AWoTAngrealInvFireball>(ParityE_AlwaysSpawn());
	AWoTAngrealInvSeeker* Next = W->SpawnActor<AWoTAngrealInvSeeker>(ParityE_AlwaysSpawn());
	Empty->GiveTo(Player.Pawn); Next->GiveTo(Player.Pawn);
	Player.Pawn->InventoryComponent->SelectItem(Empty);
	Player.Pawn->ReconcileHandSet();
	Player.Pawn->InventoryComponent->DeleteInventory(Empty);
	Empty->Destroy();
	Player.Pawn->UseAngreal();
	TestTrue(TEXT("next Fire selects another held artifact without casting"),
		Player.Pawn->InventoryComponent->SelectedItem == Next);
	AWoTHUD* HUD = Cast<AWoTHUD>(Player.Controller->GetHUD());
	TestTrue(TEXT("auto-switch posts the selected-item hand message"),
		HUD && HUD->HandMessage.Message.Contains(Next->Title));
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityESealFireSpec,
	"WoT.Parity.E.SealFireDrops", ParityFlags)
bool FParityESealFireSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	FLocalPlayer Player = SpawnLocalPlayer(*W);
	AWoTSeal* Seal = W->SpawnActor<AWoTSeal>(ParityE_AlwaysSpawn());
	Seal->GiveTo(Player.Pawn);
	Player.Pawn->InventoryComponent->SelectItem(Seal);
	Player.Pawn->ReconcileHandSet();
	Player.Pawn->UseAngreal();
	TestFalse(TEXT("Fire removes the selected seal from carried inventory"),
		Player.Pawn->InventoryComponent->Items.Contains(Seal));
	TestFalse(TEXT("the seal leaves its held lifecycle"), Seal->bHeldItem);
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityECarcassCleanupSpec,
	"WoT.Parity.E.CarcassVisibleCleanup", ParityFlags)
bool FParityECarcassCleanupSpec::RunTest(const FString&)
{
	ParityE_RunCarcassVisibilityCones(*this);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityFix1CarcassVisibilityConesSpec,
	"WoT.Parity.Fix1.CarcassVisibilityCones", ParityFlags)
bool FParityFix1CarcassVisibilityConesSpec::RunTest(const FString&)
{
	ParityE_RunCarcassVisibilityCones(*this);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityECarcassLandedSpec,
	"WoT.Parity.E.CarcassLandedSound", ParityFlags)
bool FParityECarcassLandedSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	SpawnFloor(*W);
	SpawnLocalPlayer(*W, FVector(0,0,100));
	AWoTCarcass* Corpse = W->SpawnActor<AWoTCarcass>(
		FVector(0,0,100), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	Corpse->AddUE1Velocity(FVector(0,0,-50));
	WoTTest::TickWorld(W, 1.0f, 0.02f);
	bool bFound = false;
	for (TActorIterator<AWoTSoundBurst> It(W); It; ++It)
		bFound |= It->SoundSubPath == FName(TEXT("WOT/Landed1"));
	TestTrue(TEXT("real carcass fall emits WOT.Landed1"), bFound);
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityETeleportFeedbackSpec,
	"WoT.Parity.E.TeleportBlockedFeedback", ParityFlags)
bool FParityETeleportFeedbackSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	FLocalPlayer Player = SpawnLocalPlayer(*W, FVector(0,0,100), true);
	AWoTTeleporter* Source = W->SpawnActor<AWoTTeleporter>(
		FVector(0,0,100), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	AWoTTeleporter* Dest = W->SpawnActor<AWoTTeleporter>(
		FVector(1000,0,100), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	Source->URL = TEXT("Exit"); Dest->TeleTag = TEXT("Exit");
	Player.Controller->bTeleportingDisabled = true;
	WoTTest::TickWorld(W, 4.1f);
	WoTTest::PlaceWithoutOverlapEvents(Player.Pawn, Source->GetActorLocation());
	Source->NotifyActorBeginOverlap(Player.Pawn);
	AWoTHUD* HUD = Cast<AWoTHUD>(Player.Controller->GetHUD());
	TestTrue(TEXT("real teleporter touch explains the battle gate"), HUD
		&& !HUD->CenterMessages[0].Message.IsEmpty()
		&& HUD->CenterMessages[0].Message.Contains(TEXT("Teleporting is prohibited")));
	TestEqual(TEXT("blocked player did not move to the destination"),
		Player.Pawn->GetActorLocation(), Source->GetActorLocation());
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityEBattleCycleSpec,
	"WoT.Parity.E.MultiCitadelRestart", ParityFlags)
bool FParityEBattleCycleSpec::RunTest(const FString&)
{
	FWoTHostRequest Four = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Battle4_01"));
	const FString FourURL = FWoTFrontEndModel::BuildHostTravelURL(
		Four, EWoTPlayerFaction::AesSedai);
	TestTrue(TEXT("four-citadel host disables level cycling"), FourURL.Contains(TEXT("?Cycle=0")));
	FWoTHostRequest Two = FWoTFrontEndModel::DefaultHostRequest(
		TEXT("/Game/WoT/Maps/Battle2_01"));
	TestTrue(TEXT("two-citadel host retains normal cycling"),
		FWoTFrontEndModel::BuildHostTravelURL(Two,
			EWoTPlayerFaction::AesSedai).Contains(TEXT("?Cycle=1")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityEWaterViewFlashSpec,
	"WoT.Parity.E.WaterZoneViewFlash", ParityFlags)
bool FParityEWaterViewFlashSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	FLocalPlayer Player = SpawnLocalPlayer(*W, FVector(0,0,100));
	AWoTZoneVolume* Water = W->SpawnActor<AWoTZoneVolume>(
		Player.Pawn->GetActorLocation(), FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	Water->bWaterZone = true;
	Water->ViewFlash = FVector(-0.078f);
	Water->ViewFog = FVector(0.0f, 0.2f, 0.35f);
	Water->Collision->SetBoxExtent(FVector(500));
	WoTTest::TickWorld(W, 1.0f);
	TestEqual(TEXT("underwater scale settles to WaterZone's 0.922"),
		Player.Controller->CurrentViewFlashScale, 0.922f, 0.002f);
	TestEqual(TEXT("ViewFog is retained as additive color offset"),
		Player.Controller->CurrentViewFlashFog, Water->ViewFog, 0.002f);
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParityEAlarmRadiusSpec,
	"WoT.Parity.E.AlarmSoundRadius", ParityFlags)
bool FParityEAlarmRadiusSpec::RunTest(const FString&)
{
	UWorld* W = WoTTest::CreateSpecWorld();
	SpawnLocalPlayer(*W);
	AWoTAlarm* Alarm = W->SpawnActor<AWoTAlarm>(
		FVector::ZeroVector, FRotator::ZeroRotator, ParityE_AlwaysSpawn());
	Alarm->ActivateAlarm(nullptr);
	AWoTSoundBurst* Gong = nullptr;
	for (TActorIterator<AWoTSoundBurst> It(W); It; ++It)
		if (It->SoundSubPath == FName(TEXT("WoTTraps/ActivateAM"))) Gong = *It;
	TestNotNull(TEXT("production alarm activation emitted its gong"), Gong);
	if (Gong) TestEqual(TEXT("no-radius PlaySound uses 1600uu"), Gong->Radius, 1600.0f);
	WoTTest::DestroySpecWorld(W); return !HasAnyErrors();
}

#endif
