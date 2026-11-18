#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTLocalPlayerHooksTestFixture.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTBudgetInfo.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerStart.h"
#include "../WoTPlayerState.h"
#include "../WoTRotatingMover.h"
#include "../WoTTeleporter.h"
#include "../WoTZoneVolume.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Inventory/WoTWhirlwindLeech.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "../Projectiles/WoTSwapPlacesProjectile.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/DamageEvents.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
constexpr auto GameplayYawFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName GameplayYawOwner(TEXT("GameplayYawSpec"));

FActorSpawnParameters GameplayYawSpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

double GameplayYawError(double A, double B)
{
	return FMath::Abs(FMath::FindDeltaAngleDegrees(A, B));
}

/** Stands in for a headset consumer: a still head under the consumer's own
 * heading (the artificial yaw it adds to the head). It records each lifecycle
 * event with the aim and the gameplay yaw at that moment, and can take the
 * gameplay yaw into its heading when an event arrives. */
struct FGameplayYawProvider
{
	ULocalPlayer* Player = nullptr;
	double Heading = 0.0;
	double Pitch = 0.0;
	TOptional<EWoTLocalPlayerEvent> TakeOn;

	struct FRecord
	{
		EWoTLocalPlayerEvent Event;
		double AimYaw = 0.0;
		double GameplayYaw = 0.0;
	};
	TArray<FRecord> Events;

	FWoTLocalPlayerConsumer Consumer()
	{
		FWoTLocalPlayerConsumer Result;
		Result.AimRotation = [this](FRotator& Out)
		{
			Out = FRotator(Pitch, Heading, 0.0);
			return true;
		};
		Result.Lifecycle = [this](EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext& Context)
		{
			FRecord Record{Event};
			if (const APlayerController* Controller = Context.Controller.Get())
			{
				Record.AimYaw = Controller->GetControlRotation().Yaw;
			}
			Record.GameplayYaw = Player ? FWoTLocalPlayerHooks::PeekGameplayYaw(*Player) : 0.0;
			Events.Add(Record);
			if (TakeOn.IsSet() && *TakeOn == Event)
			{
				Take();
			}
		};
		return Result;
	}

	bool Attach()
	{
		return Player && FWoTLocalPlayerHooks::Attach(*Player, GameplayYawOwner, Consumer());
	}

	/** Re-align: the gameplay yaw joins the consumer's own heading. */
	double Take()
	{
		const double Taken = Player ? FWoTLocalPlayerHooks::TakeGameplayYaw(*Player, GameplayYawOwner) : 0.0;
		Heading += Taken;
		return Taken;
	}

	double Peek() const
	{
		return Player ? FWoTLocalPlayerHooks::PeekGameplayYaw(*Player) : 0.0;
	}

	int32 Count(EWoTLocalPlayerEvent Event) const
	{
		int32 Result = 0;
		for (const FRecord& Record : Events)
		{
			Result += Record.Event == Event ? 1 : 0;
		}
		return Result;
	}

	const FRecord* Last(EWoTLocalPlayerEvent Event) const
	{
		for (int32 Index = Events.Num() - 1; Index >= 0; --Index)
		{
			if (Events[Index].Event == Event)
			{
				return &Events[Index];
			}
		}
		return nullptr;
	}
};

/** After a re-alignment the aim sits on the consumer's heading with no gameplay
 * yaw left. Limits applied inside every rotation update (view yaw and pitch
 * limits that clamp the consumer's heading and raised pitch) never read as
 * gameplay yaw, however long they hold, and the aim returns to the heading once
 * they lift. */
void GameplayYawExpectAligned(FAutomationTestBase& Test, WoTLocalPlayerHooksTest::FPlayerWorld& F,
	FGameplayYawProvider& Provider, const FString& Label)
{
	F.Frames(1);
	Test.TestTrue(Label + TEXT(": after taking the gameplay yaw the aim stays on the consumer's heading"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, Provider.Heading) < 0.01);
	Test.TestTrue(Label + TEXT(": aim and view are back in line"), FMath::Abs(Provider.Peek()) < 0.01);
	APlayerCameraManager* Camera = F.PC->PlayerCameraManager;
	if (!Test.TestNotNull(Label + TEXT(": camera manager"), Camera))
	{
		return;
	}
	const float OldYawMin = Camera->ViewYawMin;
	const float OldYawMax = Camera->ViewYawMax;
	const float OldPitchMax = Camera->ViewPitchMax;
	const double OldPitch = Provider.Pitch;
	const double Limit = FRotator::ClampAxis(Provider.Heading - 10.0);
	Camera->ViewYawMin = static_cast<float>(FRotator::ClampAxis(Provider.Heading - 20.0));
	Camera->ViewYawMax = static_cast<float>(Limit);
	Camera->ViewPitchMax = 10.0f;
	Provider.Pitch = 30.0;
	double WorstAim = 0.0;
	double WorstOffset = 0.0;
	double WorstPitch = 0.0;
	for (int32 Frame = 0; Frame < 120; ++Frame)
	{
		F.Frames(1);
		WorstAim = FMath::Max(WorstAim, GameplayYawError(F.PC->GetControlRotation().Yaw, Limit));
		WorstPitch = FMath::Max(WorstPitch, GameplayYawError(F.PC->GetControlRotation().Pitch, 10.0));
		WorstOffset = FMath::Max(WorstOffset, FMath::Abs(Provider.Peek()));
	}
	Test.TestTrue(Label + TEXT(": a view yaw limit holds the aim at the limit every frame"), WorstAim < 0.01);
	Test.TestTrue(Label + TEXT(": a view pitch limit holds the raised pitch every frame"), WorstPitch < 0.01);
	Test.TestTrue(FString::Printf(TEXT("%s: 120 limited frames add no gameplay yaw (worst %.4f)"),
		*Label, WorstOffset), WorstOffset < 0.01);
	Camera->ViewYawMin = OldYawMin;
	Camera->ViewYawMax = OldYawMax;
	Camera->ViewPitchMax = OldPitchMax;
	Provider.Pitch = OldPitch;
	F.Frames(1);
	Test.TestTrue(Label + TEXT(": once the limit lifts the aim is the consumer's heading again"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, Provider.Heading) < 0.01);
	Test.TestTrue(Label + TEXT(": still no gameplay yaw"), FMath::Abs(Provider.Peek()) < 0.01);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookForcedTurnSpec,
	"WoT.VR.Hooks.H2.GameplayYaw.ForcedTurn", GameplayYawFlags)
bool FWoTHookForcedTurnSpec::RunTest(const FString&)
{
	// Declared before the world: the world's teardown detaches it while it lives.
	FGameplayYawProvider Provider;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	Provider.Player = F.Player;
	Provider.Heading = 40.0;
	// The consumer keeps its head unrotated through the spin and re-aligns when
	// it ends.
	Provider.TakeOn = EWoTLocalPlayerEvent::ForcedTurnEnded;
	if (!TestTrue(TEXT("aim attached"), Provider.Attach())) return false;
	F.Frames(2);
	TestTrue(TEXT("the provider's heading is the aim"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 40.0) < 0.01);
	TestFalse(TEXT("no forced turn yet"), FWoTLocalPlayerHooks::IsForcedTurn(*F.Player));

	// The spinning artifact lifts and spins its victim through the real leech.
	UWoTWhirlwindLeech* Leech = NewObject<UWoTWhirlwindLeech>(F.Pawn);
	Leech->AttachTo(F.Pawn);
	TestEqual(TEXT("the forced turn starts once"), Provider.Count(EWoTLocalPlayerEvent::ForcedTurnStarted), 1);
	TestTrue(TEXT("the consumer can tell a forced turn is in progress"),
		FWoTLocalPlayerHooks::IsForcedTurn(*F.Player));
	const FVector Start = F.Pawn->GetActorLocation();
	Leech->SetDesiredLocation(Start + FVector(0, 0, 300));
	const int32 AppliedBefore = F.PC->GetForcedViewApplyCountForTest();
	double WorstFollow = 0.0;
	for (int32 Frame = 0; Frame < 60; ++Frame)
	{
		F.Frames(1);
		// The aim is the consumer's heading plus the gameplay yaw, every frame.
		WorstFollow = FMath::Max(WorstFollow,
			GameplayYawError(F.PC->GetControlRotation().Yaw, Provider.Heading + Provider.Peek()));
	}
	const int32 Applied = F.PC->GetForcedViewApplyCountForTest() - AppliedBefore;
	const double Spin = Applied * (120000.0f * 360.0f / 65536.0f * (1.0f / 60.0f));
	TestTrue(TEXT("the spin turned the aim"), Applied > 50);
	TestTrue(FString::Printf(TEXT("the aim follows the forced turn every frame (worst %.4f)"), WorstFollow),
		WorstFollow < 0.01);
	TestTrue(TEXT("the whole turn reached the aim"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 40.0 + Spin) < 0.05);
	TestTrue(TEXT("the consumer reads the turn as gameplay yaw"),
		GameplayYawError(Provider.Peek(), Spin) < 0.05);
	TestTrue(TEXT("the consumer's head stays unrotated during the spin"),
		GameplayYawError(Provider.Heading, 40.0) < 1e-9);
	TestTrue(TEXT("the lift raised the body"), F.Pawn->GetActorLocation().Z > Start.Z + 50.0);
	TestEqual(TEXT("a lift relocated every tick is not a teleport"),
		Provider.Count(EWoTLocalPlayerEvent::Teleported), 0);

	const double AimAtEnd = F.PC->GetControlRotation().Yaw;
	Leech->Unattach();
	TestEqual(TEXT("the forced turn ends once"), Provider.Count(EWoTLocalPlayerEvent::ForcedTurnEnded), 1);
	TestFalse(TEXT("the consumer can tell the forced turn ended"),
		FWoTLocalPlayerHooks::IsForcedTurn(*F.Player));
	const FGameplayYawProvider::FRecord* Ended = Provider.Last(EWoTLocalPlayerEvent::ForcedTurnEnded);
	TestTrue(TEXT("at the end the gameplay yaw holds the whole turn"),
		Ended && GameplayYawError(Ended->GameplayYaw, Spin) < 0.05);
	TestTrue(TEXT("taking it brings the consumer's heading to the aim"),
		GameplayYawError(Provider.Heading, AimAtEnd) < 0.01);
	GameplayYawExpectAligned(*this, F, Provider, TEXT("forced turn"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookTeleporterHeadingSpec,
	"WoT.VR.Hooks.H2.GameplayYaw.TeleporterExitHeading", GameplayYawFlags)
bool FWoTHookTeleporterHeadingSpec::RunTest(const FString&)
{
	// Declared before the world: the world's teardown detaches it while it lives.
	FGameplayYawProvider Provider;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	AWoTAngrealInvDart* Item = F.World->SpawnActor<AWoTAngrealInvDart>(GameplayYawSpawnParams());
	if (!TestNotNull(TEXT("artifact"), Item)) return false;
	Item->GiveTo(F.Pawn);
	Item->CurCharges = 50;
	F.Pawn->InventoryComponent->SelectItem(Item);
	Provider.Player = F.Player;
	Provider.Heading = 30.0;
	if (!TestTrue(TEXT("aim attached"), Provider.Attach())) return false;
	F.Frames(2);

	AWoTTeleporter* Exit = F.World->SpawnActor<AWoTTeleporter>(
		F.Pawn->GetActorLocation() + FVector(1500, 0, 0), FRotator(0, 135, 0), GameplayYawSpawnParams());
	if (!TestNotNull(TEXT("real teleporter"), Exit)) return false;
	Exit->bChangesYaw = true;
	TSet<AWoTDartProjectile*> Before;
	for (TActorIterator<AWoTDartProjectile> It(F.World); It; ++It) Before.Add(*It);
	TestTrue(TEXT("the teleporter accepts the pawn"), Exit->Accept(F.Pawn));
	TestEqual(TEXT("the relocation is not reported inside it"),
		Provider.Count(EWoTLocalPlayerEvent::Teleported), 0);
	// A cast between the accept and the next player tick re-samples the
	// provider; the relocation reaches the consumer first.
	TestTrue(TEXT("the Fire binding dispatches"), F.Binding(TEXT("Fire")));
	TestEqual(TEXT("the teleport reaches consumers before the cast re-samples the aim"),
		Provider.Count(EWoTLocalPlayerEvent::Teleported), 1);
	const FGameplayYawProvider::FRecord* Arrived = Provider.Last(EWoTLocalPlayerEvent::Teleported);
	TestTrue(TEXT("at Teleported the aim is the exit heading"),
		Arrived && GameplayYawError(Arrived->AimYaw, 135.0) < 0.01);
	TestTrue(TEXT("at Teleported the consumer reads the turn as gameplay yaw"),
		Arrived && GameplayYawError(Arrived->GameplayYaw, 105.0) < 0.01);
	TestTrue(TEXT("the cast's re-sample keeps the exit heading"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 135.0) < 0.01);
	bool bLaunchedAtExit = false;
	for (TActorIterator<AWoTDartProjectile> It(F.World); It; ++It)
	{
		bLaunchedAtExit |= !Before.Contains(*It)
			&& It->GetActorForwardVector().Equals(FRotator(0, 135, 0).Vector(), 0.001);
	}
	TestTrue(TEXT("the cast launches along the exit heading"), bLaunchedAtExit);
	F.Binding(TEXT("Fire"), IE_Released);
	F.Frames(3);
	TestEqual(TEXT("one teleport is reported once"), Provider.Count(EWoTLocalPlayerEvent::Teleported), 1);
	TestTrue(TEXT("the aim keeps the exit heading"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 135.0) < 0.01);
	TestTrue(TEXT("the consumer can read the exit turn"), GameplayYawError(Provider.Peek(), 105.0) < 0.01);
	TestTrue(TEXT("the consumer takes the exit turn"), GameplayYawError(Provider.Take(), 105.0) < 0.01);
	GameplayYawExpectAligned(*this, F, Provider, TEXT("teleporter"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookRespawnFacingSpec,
	"WoT.VR.Hooks.H2.GameplayYaw.RespawnFacing", GameplayYawFlags)
bool FWoTHookRespawnFacingSpec::RunTest(const FString&)
{
	// Declared before the world: the world's teardown detaches it while it lives.
	FGameplayYawProvider Provider;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this, FVector(0, 0, 500), MOVE_Flying, AWoTGameMode::StaticClass())) return false;
	AWoTGameMode* Game = F.World->GetAuthGameMode<AWoTGameMode>();
	if (!TestNotNull(TEXT("game mode"), Game)
		|| !TestNotNull(TEXT("floor"), F.SpawnBlock(FVector(0, 0, -10), FVector(6000, 6000, 10)))
		|| !TestNotNull(TEXT("respawn start"), F.World->SpawnActor<AWoTPlayerStart>(
			FVector(1000, 0, 48), FRotator(0, 90, 0), GameplayYawSpawnParams())))
	{
		return false;
	}
	Provider.Player = F.Player;
	Provider.Heading = 10.0;
	if (!TestTrue(TEXT("aim attached"), Provider.Attach())) return false;
	F.Frames(2);
	TestTrue(TEXT("the provider's heading is the aim"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 10.0) < 0.01);

	Game->RespawnDelay = 0.0f;
	AWoTCharacter* Dead = F.Pawn;
	Dead->MaxHealth = 100;
	Dead->Health = 25;
	Dead->TakeDamage(26.0f, FDamageEvent(), nullptr, nullptr);
	TestNull(TEXT("death releases possession"), F.PC->GetPawn());
	// A press at least a frame after the death respawns through the Fire key.
	F.Frames(1);
	TestTrue(TEXT("the Fire binding dispatches"), F.Binding(TEXT("Fire")));
	AWoTCharacter* Respawned = Cast<AWoTCharacter>(F.PC->GetPawn());
	if (!TestNotNull(TEXT("the Fire key respawns the player"), Respawned)) return false;
	F.Pawn = Respawned;
	F.Binding(TEXT("Fire"), IE_Released);
	TestEqual(TEXT("one respawn"), Provider.Count(EWoTLocalPlayerEvent::Respawn), 1);
	F.Frames(2);
	TestTrue(TEXT("the aim follows the respawn facing"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 90.0) < 0.01);
	TestTrue(TEXT("the consumer can read the respawn turn"), GameplayYawError(Provider.Peek(), 80.0) < 0.01);
	TestTrue(TEXT("the consumer takes the respawn turn"), GameplayYawError(Provider.Take(), 80.0) < 0.01);
	GameplayYawExpectAligned(*this, F, Provider, TEXT("respawn"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookRotatingBaseSpec,
	"WoT.VR.Hooks.H2.GameplayYaw.RotatingBase", GameplayYawFlags)
bool FWoTHookRotatingBaseSpec::RunTest(const FString&)
{
	// Declared before the world: the world's teardown detaches it while it lives.
	FGameplayYawProvider Provider;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	// A real rotating mover carrying a deck the player stands on.
	AWoTRotatingMover* Mover = F.World->SpawnActorDeferred<AWoTRotatingMover>(
		AWoTRotatingMover::StaticClass(), FTransform::Identity, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("rotating mover"), Mover)) return false;
	Mover->RotateRate = FRotator(0, 90, 0);
	Mover->FinishSpawning(FTransform::Identity);
	UBoxComponent* Deck = NewObject<UBoxComponent>(Mover);
	Deck->SetBoxExtent(FVector(800, 800, 10));
	Deck->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Deck->SetCollisionObjectType(ECC_WorldDynamic);
	Deck->SetCollisionResponseToAllChannels(ECR_Block);
	Deck->SetupAttachment(Mover->GetRootComponent());
	Deck->RegisterComponent();
	const float HalfHeight = F.Pawn->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	F.Pawn->SetActorLocation(FVector(200, 0, 10 + HalfHeight + 2), false, nullptr,
		ETeleportType::TeleportPhysics);
	UCharacterMovementComponent* Move = F.Pawn->GetCharacterMovement();
	Move->StopMovementImmediately();
	Move->SetMovementMode(MOVE_Walking);
	F.Frames(10);
	if (!TestTrue(TEXT("the player stands on the mover's deck"), F.Pawn->GetMovementBaseObject() == Deck))
	{
		return false;
	}

	Provider.Player = F.Player;
	Provider.Heading = 0.0;
	if (!TestTrue(TEXT("aim attached"), Provider.Attach())) return false;
	F.Frames(2);
	TestTrue(TEXT("the provider's heading is the aim"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 0.0) < 0.01);
	const double MoverStart = Mover->GetActorRotation().Yaw;
	Mover->WoTTrigger(nullptr, nullptr);
	double WorstFollow = 0.0;
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		F.Frames(1);
		WorstFollow = FMath::Max(WorstFollow,
			GameplayYawError(F.PC->GetControlRotation().Yaw, Provider.Heading + Provider.Peek()));
	}
	Mover->WoTUnTrigger(nullptr, nullptr);
	F.Frames(3);
	const double Turned = FMath::FindDeltaAngleDegrees(MoverStart, Mover->GetActorRotation().Yaw);
	TestTrue(FString::Printf(TEXT("the base turned (%.2f degrees)"), Turned), FMath::Abs(Turned) > 20.0);
	TestTrue(FString::Printf(TEXT("the aim follows the turning base every frame (worst %.4f)"), WorstFollow),
		WorstFollow < 0.01);
	TestTrue(FString::Printf(TEXT("the base's whole turn reached the aim (aim %.3f, base %.3f)"),
		F.PC->GetControlRotation().Yaw, Turned),
		GameplayYawError(F.PC->GetControlRotation().Yaw, Turned) < 0.1);
	TestTrue(TEXT("the consumer can read the base's turn"), GameplayYawError(Provider.Peek(), Turned) < 0.1);
	TestTrue(TEXT("the consumer takes the base's turn"), GameplayYawError(Provider.Take(), Turned) < 0.1);
	GameplayYawExpectAligned(*this, F, Provider, TEXT("rotating base"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookPlaceSwapSpec,
	"WoT.VR.Hooks.H2.GameplayYaw.PlaceSwap", GameplayYawFlags)
bool FWoTHookPlaceSwapSpec::RunTest(const FString&)
{
	// Declared before the world: the world's teardown detaches it while it lives.
	FGameplayYawProvider Provider;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	Provider.Player = F.Player;
	Provider.Heading = 0.0;
	if (!TestTrue(TEXT("aim attached"), Provider.Attach())) return false;
	F.Frames(2);
	AWoTSwapPlacesProjectile* Swap = F.World->SpawnActor<AWoTSwapPlacesProjectile>(
		FVector(0, 8000, 8000), FRotator::ZeroRotator, GameplayYawSpawnParams());
	AWoTCharacter* Far = F.World->SpawnActor<AWoTCharacter>(
		F.Pawn->GetActorLocation() + FVector(1000, 0, 0), FRotator(0, 120, 0), GameplayYawSpawnParams());
	if (!TestNotNull(TEXT("swap effect"), Swap) || !TestNotNull(TEXT("far target"), Far)) return false;
	Far->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	const FVector FarSpot = Far->GetActorLocation();

	// A place swap with a distant target is a teleport, and its facing turns the aim.
	Swap->PerformSwap(F.Pawn, Far);
	TestTrue(TEXT("the swap moved the player"), F.Pawn->GetActorLocation().Equals(FarSpot, 1.0));
	F.Frames(1);
	TestEqual(TEXT("a distant place swap is reported once"),
		Provider.Count(EWoTLocalPlayerEvent::Teleported), 1);
	const FGameplayYawProvider::FRecord* Swapped = Provider.Last(EWoTLocalPlayerEvent::Teleported);
	TestTrue(TEXT("at Teleported the aim is the swapped facing"),
		Swapped && GameplayYawError(Swapped->AimYaw, 120.0) < 0.01);
	TestTrue(TEXT("the aim follows the swapped facing"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 120.0) < 0.01);
	TestTrue(TEXT("the consumer takes the swap's turn"), GameplayYawError(Provider.Take(), 120.0) < 0.01);

	// A swap with a neighbour inside the teleport distance carries the view like
	// movement; its facing still reaches the aim as gameplay yaw.
	AWoTCharacter* Near = F.World->SpawnActor<AWoTCharacter>(
		F.Pawn->GetActorLocation() + FVector(0, 60, 0), FRotator(0, 200, 0), GameplayYawSpawnParams());
	if (!TestNotNull(TEXT("near target"), Near)) return false;
	Near->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Swap->PerformSwap(F.Pawn, Near);
	F.Frames(2);
	TestEqual(TEXT("a swap within the teleport distance is not a teleport"),
		Provider.Count(EWoTLocalPlayerEvent::Teleported), 1);
	TestTrue(TEXT("the near swap's facing still turns the aim"),
		GameplayYawError(F.PC->GetControlRotation().Yaw, 200.0) < 0.01);
	TestTrue(TEXT("the consumer takes the near swap's turn"), GameplayYawError(Provider.Take(), 80.0) < 0.01);
	GameplayYawExpectAligned(*this, F, Provider, TEXT("place swap"));
	Swap->Destroy();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookCitadelOrbitSpec,
	"WoT.VR.Hooks.H2.CitadelOrbitIgnoresProvider", GameplayYawFlags)
bool FWoTHookCitadelOrbitSpec::RunTest(const FString&)
{
	// Declared before the world: the world's teardown detaches it while it lives.
	FGameplayYawProvider Provider;
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this, FVector(0, 0, 300), MOVE_Flying, AWoTBattleGameMode::StaticClass())) return false;
	AWoTBattleGameMode* Battle = F.World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!TestNotNull(TEXT("battle rules"), Battle)) return false;
	AWoTBattleInfo* Info = F.World->SpawnActorDeferred<AWoTBattleInfo>(
		AWoTBattleInfo::StaticClass(), FTransform::Identity);
	AWoTZoneVolume* Zone = F.World->SpawnActorDeferred<AWoTZoneVolume>(
		AWoTZoneVolume::StaticClass(), FTransform::Identity);
	AWoTBudgetInfo* Budget = F.World->SpawnActor<AWoTBudgetInfo>(GameplayYawSpawnParams());
	AWoTPlayerState* State = F.PC->GetPlayerState<AWoTPlayerState>();
	if (!TestNotNull(TEXT("battle info"), Info) || !TestNotNull(TEXT("edit zone"), Zone)
		|| !TestNotNull(TEXT("budget"), Budget) || !TestNotNull(TEXT("player state"), State))
	{
		return false;
	}
	Info->NumTeams = 2;
	Info->TeamClass[0] = AWoTCharacter::StaticClass();
	Info->TeamClass[1] = AWoTCharacter::StaticClass();
	Info->bTeleportersDisabled = true;
	Info->FinishSpawning(FTransform::Identity);
	Battle->Battle = Info;
	Zone->EditTeam = 1;
	Zone->bAllowEditing = true;
	Zone->Collision->SetBoxExtent(FVector(2000.0f));
	Zone->FinishSpawning(FTransform::Identity);
	Budget->Team = 1;
	State->Team = 1;

	Provider.Player = F.Player;
	Provider.Pitch = 30.0;
	Provider.Heading = 70.0;
	if (!TestTrue(TEXT("aim attached"), Provider.Attach())) return false;
	F.Frames(2);
	// The orbit editor, entered the way its console command enters it.
	F.PC->ServerEditCitadel(1);
	if (!TestEqual(TEXT("orbit editing"), static_cast<int32>(F.PC->GetCitadelEditCameraMode()),
		static_cast<int32>(EWoTCitadelEditCameraMode::Orbit)))
	{
		return false;
	}
	const FRotator Offset = F.PC->GetCitadelOrbitViewOffset();
	const FRotator Held = F.PC->GetControlRotation();
	Provider.Pitch = -20.0;
	Provider.Heading = 150.0;
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		F.Frames(1);
		// Casts and launches re-sample the aim between player ticks.
		F.PC->RefreshLocalAim();
		F.PC->RefreshLocalAim();
	}
	TestTrue(FString::Printf(TEXT("the provider never turns the orbit camera (%s)"),
		*F.PC->GetCitadelOrbitViewOffset().ToString()),
		F.PC->GetCitadelOrbitViewOffset().Equals(Offset, 1e-3));
	TestTrue(TEXT("the provider never moves the aim in the orbit view"),
		F.PC->GetControlRotation().Equals(Held, 0.01));
	F.PC->AddYawInput(10.0f);
	F.Frames(1);
	TestFalse(TEXT("look input still turns the orbit camera"),
		FMath::IsNearlyEqual(F.PC->GetCitadelOrbitViewOffset().Yaw, Offset.Yaw, 0.1));
	F.PC->ServerEditCitadel();
	TestEqual(TEXT("editing ends"), static_cast<int32>(F.PC->GetCitadelEditCameraMode()),
		static_cast<int32>(EWoTCitadelEditCameraMode::Inactive));
	F.Frames(2);
	TestTrue(TEXT("after the orbit view the provider drives the aim again"),
		F.PC->GetControlRotation().Equals(FRotator(-20.0, 150.0 + Provider.Peek(), 0.0), 0.01));
	return !HasAnyErrors();
}
#endif // WITH_AUTOMATION_TESTS
