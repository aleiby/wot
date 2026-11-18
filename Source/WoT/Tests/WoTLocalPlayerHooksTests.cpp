#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTLocalPlayerHooksTestFixture.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTDartProjectile.h"
#include "Engine/LocalPlayer.h"
#include "EngineUtils.h"
#include <limits>

namespace
{
constexpr auto HookFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
const FName HookOwner(TEXT("HookSpec"));

int32 HookEventCount(const TArray<EWoTLocalPlayerEvent>& Events, EWoTLocalPlayerEvent Event)
{
	int32 Count = 0;
	for (const EWoTLocalPlayerEvent Each : Events)
	{
		Count += Each == Event ? 1 : 0;
	}
	return Count;
}

TSet<AWoTDartProjectile*> HookDarts(UWorld* World)
{
	TSet<AWoTDartProjectile*> Darts;
	for (TActorIterator<AWoTDartProjectile> It(World); It; ++It)
	{
		Darts.Add(*It);
	}
	return Darts;
}

/** The darts launched since Before, in no particular order. */
TArray<AWoTDartProjectile*> HookNewDarts(UWorld* World, const TSet<AWoTDartProjectile*>& Before)
{
	TArray<AWoTDartProjectile*> New;
	for (AWoTDartProjectile* Dart : HookDarts(World))
	{
		if (!Before.Contains(Dart))
		{
			New.Add(Dart);
		}
	}
	return New;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookRegistrySpec, "WoT.VR.Hooks.H1.LazyOwnershipAndInput", HookFlags)
bool FWoTHookRegistrySpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	TestFalse(TEXT("play creates no registration"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	TArray<EWoTLocalPlayerEvent> Events;
	int32 InputCalls = 0;
	FWoTLocalPlayerConsumer C;
	C.Lifecycle = [&](EWoTLocalPlayerEvent Event, const FWoTLocalPlayerContext& Context)
	{
		TestTrue(TEXT("events name the attached local player"), Context.Player.Get() == F.Player);
		Events.Add(Event);
	};
	C.RegisterInput = [&](ULocalPlayer&, UInputComponent* Input)
	{
		++InputCalls;
		TestTrue(TEXT("a late attachment registers on the live input component"),
			Input == F.PC->InputComponent);
	};
	TestTrue(TEXT("attach in a match"), FWoTLocalPlayerHooks::Attach(*F.Player, HookOwner, C));
	TestTrue(TEXT("Attached is the first event"), Events.Num() > 0 && Events[0] == EWoTLocalPlayerEvent::Attached);
	TestTrue(TEXT("attaching while possessed reports the current pawn"),
		Events.Contains(EWoTLocalPlayerEvent::Possessed));
	TestEqual(TEXT("a late attachment registers input once"), InputCalls, 1);
	Events.Reset();
	F.PC->UnPossess();
	F.PC->Possess(F.Pawn);
	TestTrue(TEXT("unpossession"), Events.Contains(EWoTLocalPlayerEvent::Unpossessed));
	TestTrue(TEXT("possession"), Events.Contains(EWoTLocalPlayerEvent::Possessed));
	F.Pawn->TeleportTo(FVector(1500, 0, 500), FRotator::ZeroRotator, false, true);
	TestEqual(TEXT("a teleport is reported after the whole relocation, not inside it"),
		HookEventCount(Events, EWoTLocalPlayerEvent::Teleported), 0);
	F.Frames(1);
	TestEqual(TEXT("the next player tick reports the teleport"),
		HookEventCount(Events, EWoTLocalPlayerEvent::Teleported), 1);
	F.Frames(3);
	TestEqual(TEXT("one teleport is reported once"),
		HookEventCount(Events, EWoTLocalPlayerEvent::Teleported), 1);
	// A relocation shorter than the teleport distance carries the view like any
	// other movement.
	const FVector Near = F.Pawn->GetActorLocation()
		+ FVector(FWoTLocalPlayerHooks::TeleportDistance * 0.5, 0, 0);
	F.Pawn->TeleportTo(Near, FRotator::ZeroRotator, false, true);
	F.Frames(2);
	TestEqual(TEXT("a relocation within the teleport distance is not a teleport"),
		HookEventCount(Events, EWoTLocalPlayerEvent::Teleported), 1);
	FWoTLocalPlayerConsumer Observer;
	TestTrue(TEXT("consumers without exclusive slots coexist"),
		FWoTLocalPlayerHooks::Attach(*F.Player, TEXT("Observer"), Observer));
	FWoTLocalPlayerHooks::Detach(*F.Player, HookOwner);
	TestTrue(TEXT("Detached is the last event"), Events.Num() > 0 && Events.Last() == EWoTLocalPlayerEvent::Detached);
	const int32 DetachedCount = Events.Num();
	F.Frames(2);
	F.PC->UnPossess();
	TestEqual(TEXT("a detached consumer hears nothing more"), Events.Num(), DetachedCount);
	FWoTLocalPlayerHooks::Detach(*F.Player, TEXT("Observer"));
	TestFalse(TEXT("the last detach removes the registration"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookAimSpec, "WoT.VR.Hooks.H2.HeldAimAndValidation", HookFlags)
bool FWoTHookAimSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	AWoTAngrealInvDart* Item = F.World->SpawnActor<AWoTAngrealInvDart>();
	if (!TestNotNull(TEXT("repeating artifact"), Item)) return false;
	Item->GiveTo(F.Pawn);
	Item->CurCharges = 50;
	F.Pawn->InventoryComponent->SelectItem(Item);
	F.PC->SetControlRotation(FRotator(5, 10, 0));
	F.Frames(2);
	TestTrue(TEXT("without a provider the flat aim stands"),
		F.PC->GetControlRotation().Equals(FRotator(5, 10, 0), 0.01));

	// A mouse player's cast: the Fire key's own handler sends the control
	// rotation through the cast RPC (its validation, then the launch).
	const FRotator MouseAim(-10, 70, 0);
	F.PC->SetControlRotation(MouseAim);
	TSet<AWoTDartProjectile*> Before = HookDarts(F.World);
	TestTrue(TEXT("the Fire binding dispatches"), F.Binding(TEXT("Fire")));
	TArray<AWoTDartProjectile*> Launched = HookNewDarts(F.World, Before);
	TestTrue(TEXT("a mouse rotation passes the cast RPC's validation and launches along it"),
		Launched.Num() == 1 && Launched[0]->GetActorForwardVector().Equals(MouseAim.Vector(), 0.001));
	F.Binding(TEXT("Fire"), IE_Released);
	F.Frames(30);

	FRotator Aim(25, 40, 0);
	FWoTLocalPlayerConsumer C;
	C.AimRotation = [&](FRotator& Out) { Out = Aim; return true; };
	TestTrue(TEXT("aim attached"), FWoTLocalPlayerHooks::Attach(*F.Player, HookOwner, C));
	AddExpectedError(TEXT("exclusive local-player slot"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("a second aim provider is rejected"), FWoTLocalPlayerHooks::Attach(*F.Player, TEXT("Conflict"), C));
	F.Frames(1);
	TestTrue(TEXT("a still provider pose becomes the aim"), F.PC->GetControlRotation().Equals(Aim, 0.01));
	F.PC->AddYawInput(30.0f);
	F.PC->AddPitchInput(10.0f);
	F.Frames(1);
	TestTrue(TEXT("mouse look cannot move provider-owned aim"), F.PC->GetControlRotation().Equals(Aim, 0.01));
	Aim = FRotator(10, 20, 35);
	F.Frames(1);
	TestEqual(TEXT("provider roll never reaches the control rotation"),
		static_cast<float>(F.PC->GetControlRotation().Roll), 0.0f, 0.01f);

	// The same cast with the provider's rotation, changed since the last player
	// tick: the press samples it, and the same RPC validation passes it.
	Aim = MouseAim;
	F.Frames(1);
	Aim = FRotator(-15, 65, 0);
	Before = HookDarts(F.World);
	TestTrue(TEXT("the Fire binding dispatches with a provider"), F.Binding(TEXT("Fire")));
	TestTrue(TEXT("Fire samples the provider"), F.PC->GetControlRotation().Equals(Aim, 0.01));
	Launched = HookNewDarts(F.World, Before);
	TestTrue(TEXT("a provider rotation passes the same cast RPC validation and launches along it"),
		Launched.Num() == 1 && Launched[0]->GetActorForwardVector().Equals(Aim.Vector(), 0.001));

	// A held, repeating cast keeps launching on the launcher's own ticks. With the
	// player's tick held back, only the launch itself can read the new pose.
	F.PC->SetActorTickEnabled(false);
	Aim = FRotator(-20, 100, 0);
	Before = HookDarts(F.World);
	for (int32 Frame = 0; Frame < 30 && HookNewDarts(F.World, Before).Num() == 0; ++Frame)
	{
		F.Frames(1);
	}
	Launched = HookNewDarts(F.World, Before);
	TestEqual(TEXT("the held cast launches again between player ticks"), Launched.Num(), 1);
	TestTrue(TEXT("a launcher tick launches along the provider's current aim"),
		Launched.Num() == 1 && Launched[0]->GetActorForwardVector().Equals(Aim.Vector(), 0.001));
	F.PC->SetActorTickEnabled(true);
	F.Binding(TEXT("Fire"), IE_Released);

	// A malformed pose never reaches the cast: the aim holds.
	const FRotator Held = F.PC->GetControlRotation();
	Aim = FRotator(std::numeric_limits<double>::quiet_NaN(), 0, 0);
	F.Frames(30);
	Before = HookDarts(F.World);
	TestTrue(TEXT("the Fire binding dispatches with a malformed pose"), F.Binding(TEXT("Fire")));
	Launched = HookNewDarts(F.World, Before);
	TestTrue(TEXT("a nonfinite provider pose holds the aim, and the cast launches along it"),
		Launched.Num() == 1 && Launched[0]->GetActorForwardVector().Equals(Held.Vector(), 0.001));
	F.Binding(TEXT("Fire"), IE_Released);
	Aim = FRotator(0, 50, 0);
	F.Frames(1);

	// Gameplay pitch constraints still bound a provider's aim: an impaled body may
	// not rest inside the forbidden band, and the aim shows the bound honestly.
	F.Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::Impaled);
	Aim = FRotator(30, 50, 0);
	F.Frames(3);
	TestEqual(TEXT("an impaled body bounds the provider's raised pitch"),
		static_cast<float>(FRotator::NormalizeAxis(F.PC->GetControlRotation().Pitch)),
		3000.0f * 360.0f / 65536.0f, 0.01f);
	F.Pawn->SetPlayerMovementConstraint(EWoTPlayerMovementConstraint::None);
	F.Frames(1);
	TestEqual(TEXT("the provider's pitch returns once the constraint ends"),
		static_cast<float>(FRotator::NormalizeAxis(F.PC->GetControlRotation().Pitch)), 30.0f, 0.01f);
	Aim = FRotator(0, 50, 0);
	F.Frames(1);

	FWoTLocalPlayerHooks::Detach(*F.Player, HookOwner);
	TestTrue(TEXT("detach continues from the current aim"),
		F.PC->GetControlRotation().Equals(FRotator(0, 50, 0), 0.5));
	F.PC->SetControlRotation(FRotator(5, 15, 0));
	F.Frames(2);
	TestTrue(TEXT("flat aim is restored"), F.PC->GetControlRotation().Equals(FRotator(5, 15, 0), 0.01));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookCommandsSpec, "WoT.VR.Hooks.H8.CommandsAndRelease", HookFlags)
bool FWoTHookCommandsSpec::RunTest(const FString&)
{
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	TestFalse(TEXT("an unregistered command is inert"),
		FWoTLocalPlayerHooks::Command(*F.Player, HookOwner, EWoTLocalPlayerCommand::Jump));
	FWoTLocalPlayerConsumer C;
	FWoTLocalPlayerHooks::Attach(*F.Player, HookOwner, C);
	TestTrue(TEXT("jump dispatched"), FWoTLocalPlayerHooks::Command(*F.Player, HookOwner, EWoTLocalPlayerCommand::Jump));
	TestTrue(TEXT("jump held"), F.Pawn->bPressedJump);
	FWoTLocalPlayerHooks::Command(*F.Player, HookOwner, EWoTLocalPlayerCommand::ShowScores);
	TestTrue(TEXT("scores shown"), F.PC->bShowScores);
	FWoTLocalPlayerHooks::Detach(*F.Player, HookOwner);
	TestFalse(TEXT("jump released on detach"), F.Pawn->bPressedJump);
	TestFalse(TEXT("scores restored on detach"), F.PC->bShowScores);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookGameInputBlockedSpec, "WoT.VR.Hooks.H3.GameInputBlockedQuery", HookFlags)
bool FWoTHookGameInputBlockedSpec::RunTest(const FString&)
{
	// A read-only query of the gates the stick already obeys; it needs no
	// consumer and changes nothing.
	WoTLocalPlayerHooksTest::FPlayerWorld F;
	if (!F.Init(*this)) return false;
	TestFalse(TEXT("no controller is never blocked"), FWoTLocalPlayerHooks::IsGameInputBlocked(nullptr));
	TestFalse(TEXT("ordinary play takes gameplay input"), FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));
	TestFalse(TEXT("the query attaches nothing"), FWoTLocalPlayerHooks::HasConsumers(F.Player));

	F.PC->Talk();
	if (TestTrue(TEXT("Talk enters typing"), F.PC->IsChatTyping()))
	{
		TestTrue(TEXT("typing chat blocks gameplay input"), FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));
		F.PC->CancelChatInput();
	}
	TestFalse(TEXT("leaving chat restores gameplay input"), FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));

	F.PC->ToggleInGameMenu();
	if (TestTrue(TEXT("the in-game menu opens"), F.PC->IsInGameMenuOpen()))
	{
		TestTrue(TEXT("the in-game menu blocks gameplay input"), FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));
		F.PC->ResumeFromInGameMenu();
		F.Frames(1);
	}
	TestFalse(TEXT("closing the menu restores gameplay input"), FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));

	if (F.SetOverlayInput(true))
	{
		TestTrue(TEXT("a window holding UI-only input blocks gameplay input"),
			FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));
		F.SetOverlayInput(false);
		TestFalse(TEXT("releasing the window restores gameplay input"), FWoTLocalPlayerHooks::IsGameInputBlocked(F.PC));
	}
	TestFalse(TEXT("still nothing attached"), FWoTLocalPlayerHooks::HasConsumers(F.Player));
	return !HasAnyErrors();
}
#endif
