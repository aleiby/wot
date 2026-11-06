#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../AI/WoTBrainComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "WoTTestSoundDispatcher.h"

#include "Engine/Engine.h"
#include "Engine/DamageEvents.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Slate/SceneViewport.h"
#include "UObject/UnrealType.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
constexpr EAutomationTestFlags Fix12_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FKeyEvent Fix12_KeyEvent(const FKey& Key)
{
	return FKeyEvent(Key, FModifierKeysState(), 0,
		/*bInIsRepeat*/ false, 0, 0);
}

FPointerEvent Fix12_MouseEvent(const FVector2D& Position, bool bPressed)
{
	TSet<FKey> PressedButtons;
	if (bPressed)
	{
		PressedButtons.Add(EKeys::LeftMouseButton);
	}
	return FPointerEvent(0, FSlateApplicationBase::CursorPointerIndex,
		Position, Position, PressedButtons,
		EKeys::LeftMouseButton, 0.0f, FModifierKeysState());
}

AWoTTestSoundDispatcherEventSink* Fix12_SpawnSink(
	UWorld& World, const FName Tag)
{
	AWoTTestSoundDispatcherEventSink* Sink =
		World.SpawnActorDeferred<AWoTTestSoundDispatcherEventSink>(
			AWoTTestSoundDispatcherEventSink::StaticClass(),
			FTransform::Identity);
	if (Sink)
	{
		Sink->TagName = Tag;
		Sink->FinishSpawning(FTransform::Identity);
	}
	return Sink;
}

bool Fix12_SetNameProperty(AWoTNPCCharacter& NPC, const FName PropertyName,
	const FName Value, FAutomationTestBase& Test)
{
	FNameProperty* Property = FindFProperty<FNameProperty>(
		NPC.GetClass(), PropertyName);
	if (!Test.TestNotNull(FString::Printf(TEXT("%s is an authored NPC property"),
		*PropertyName.ToString()), Property))
	{
		return false;
	}
	Test.TestTrue(FString::Printf(TEXT("%s is editor-authored"),
		*PropertyName.ToString()), Property->HasAnyPropertyFlags(CPF_Edit));
	Property->SetPropertyValue_InContainer(&NPC, Value);
	return true;
}

bool Fix12_SetBoolProperty(AWoTNPCCharacter& NPC, const FName PropertyName,
	const bool Value, FAutomationTestBase& Test)
{
	FBoolProperty* Property = FindFProperty<FBoolProperty>(
		NPC.GetClass(), PropertyName);
	if (!Test.TestNotNull(FString::Printf(TEXT("%s is an authored NPC property"),
		*PropertyName.ToString()), Property))
	{
		return false;
	}
	Test.TestTrue(FString::Printf(TEXT("%s is editor-authored"),
		*PropertyName.ToString()), Property->HasAnyPropertyFlags(CPF_Edit));
	Property->SetPropertyValue_InContainer(&NPC, Value);
	return true;
}

#if WITH_EDITOR
struct FFix12_ChatFixture
{
	UWorld* World = nullptr;
	FWorldContext* Context = nullptr;
	UGameInstance* GameInstance = nullptr;
	UGameViewportClient* ViewportClient = nullptr;
	TSharedPtr<SViewport> ViewportWidget;
	TSharedPtr<FSceneViewport> SceneViewport;
	TSharedPtr<SWindow> Window;
	UGameViewportClient* PreviousEngineViewport = nullptr;
	AWoTGameMode* Game = nullptr;
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Pawn = nullptr;
	AWoTCharacter* Victim = nullptr;

	~FFix12_ChatFixture()
	{
		if (FSlateApplication::IsInitialized()
			&& FSlateApplication::Get().GetPressedMouseButtons().Contains(
				EKeys::LeftMouseButton))
		{
			FSlateApplication::Get().ProcessMouseButtonUpEvent(
				Fix12_MouseEvent(FVector2D(5000.0f, 5000.0f), false));
		}
		if (GEngine)
		{
			GEngine->GameViewport = PreviousEngineViewport;
		}
		if (Window.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
			FSlateApplication::Get().Tick();
		}
		if (ViewportClient && SceneViewport)
		{
			ViewportClient->RemoveAssociation(*SceneViewport);
		}
		SceneViewport.Reset();
		ViewportWidget.Reset();
		if (Context)
		{
			Context->GameViewport = nullptr;
		}
		WoTTest::DestroySpecWorld(World);
	}

	bool Initialize(FAutomationTestBase& Test, const FKey& FireKey)
	{
		WoTTest::FSpecWorldOptions Options;
		Options.GameModeClass = AWoTGameMode::StaticClass();
		Options.BeforeInitializeActorsForPlay = [](UWorld* InWorld)
		{
			InWorld->WorldType = EWorldType::PIE;
			InWorld->SetPlayInEditorInitialNetMode(NM_ListenServer);
		};
		World = WoTTest::CreateSpecWorld(Options);
		Context = World ? GEngine->GetWorldContextFromWorld(World) : nullptr;
		GameInstance = World ? World->GetGameInstance() : nullptr;
		ViewportClient = Context && GameInstance
			? NewObject<UGameViewportClient>(
				GEngine, GEngine->GameViewportClientClass)
			: nullptr;
		PreviousEngineViewport = GEngine ? GEngine->GameViewport.Get() : nullptr;
		if (!Test.TestNotNull(TEXT("listen world"), World)
			|| !Test.TestNotNull(TEXT("world context"), Context)
			|| !Test.TestNotNull(TEXT("game instance"), GameInstance)
			|| !Test.TestNotNull(TEXT("game viewport client"), ViewportClient)
			|| !Test.TestTrue(TEXT("Slate application is initialized"),
				FSlateApplication::IsInitialized()))
		{
			return false;
		}

		ViewportClient->Init(*Context, GameInstance);
		Context->GameViewport = ViewportClient;
		GEngine->GameViewport = ViewportClient;
		ViewportWidget = SNew(SViewport);
		SceneViewport = ViewportClient->CreateViewport(ViewportWidget);
		SceneViewport->SetInitialSize(FIntPoint(640, 480));
		ViewportClient->AddAssociation(*SceneViewport);
		ViewportClient->SetMouseCaptureMode(
			EMouseCaptureMode::CaptureDuringMouseDown);
		Window = SNew(SWindow)
			.ScreenPosition(FVector2D(100.0f, 100.0f))
			.ClientSize(FVector2D(640.0f, 480.0f))
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.CreateTitleBar(false)
			[ViewportWidget.ToSharedRef()];
		FSlateApplication& Slate = FSlateApplication::Get();
		Slate.AddWindow(Window.ToSharedRef(), false);
		Slate.Tick();
		Window->SlatePrepass(1.0f);
		Slate.SetKeyboardFocus(ViewportWidget, EFocusCause::SetDirectly);
		if (!Test.TestTrue(TEXT("real scene viewport owns keyboard focus"),
			Slate.GetKeyboardFocusedWidget() == ViewportWidget))
		{
			return false;
		}

		FString PlayerError;
		ULocalPlayer* LocalPlayer = GameInstance->CreateLocalPlayer(
			0, PlayerError, /*bSpawnPlayerController*/ false);
		Game = World->GetAuthGameMode<AWoTGameMode>();
		Controller = World->SpawnActor<AWoTPlayerController>();
		Pawn = World->SpawnActor<AWoTCharacter>(
			FVector(0.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
		Victim = World->SpawnActor<AWoTCharacter>(
			FVector(300.0f, 0.0f, 542.0f), FRotator::ZeroRotator);
		if (!Test.TestNotNull(TEXT("local player"), LocalPlayer)
			|| !Test.TestNotNull(TEXT("listen rules"), Game)
			|| !Test.TestNotNull(TEXT("local controller"), Controller)
			|| !Test.TestNotNull(TEXT("caster"), Pawn)
			|| !Test.TestNotNull(TEXT("balefire victim"), Victim))
		{
			return false;
		}
		Controller->SetPlayer(LocalPlayer);
		Controller->Possess(Pawn);
		Controller->EnableChatWidgetForTesting();
		Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Victim->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Victim->Health = 1000;
		Controller->GetPlayerState<AWoTPlayerState>()->bAdmin = true;
		Controller->PlayerInput->AddActionMapping(
			FInputActionKeyMapping(TEXT("Fire"), FireKey));
		Controller->PlayerInput->AddAxisMapping(
			FInputAxisKeyMapping(TEXT("MoveForward"), EKeys::W, 1.0f));

		AWoTAngrealInvBalefire* Balefire =
			World->SpawnActor<AWoTAngrealInvBalefire>(
				FVector(0.0f, 0.0f, 500.0f), FRotator::ZeroRotator);
		if (!Test.TestNotNull(TEXT("balefire inventory"), Balefire))
		{
			return false;
		}
		Balefire->GiveTo(Pawn);
		Pawn->InventoryComponent->SelectItem(Balefire);
		WoTTest::TickWorld(World, 0.1f);
		return true;
	}
};
#endif
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix12PausedChatExternalUnpauseSpec,
	"WoT.Parity.Fix12.PausedChatExternalUnpause", Fix12_Flags)

bool FFix12PausedChatExternalUnpauseSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless (-nullrhi) Slate focus on an editable chat widget has no native
	// NSWindow, so the macOS text-input-method system logs an Error that a real
	// game window never produces. Ignore it here; the assertions are unaffected.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FFix12_ChatFixture Fixture;
	if (!Fixture.Initialize(*this, EKeys::F))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.ProcessKeyDownEvent(Fix12_KeyEvent(EKeys::F));
	Slate.ProcessKeyDownEvent(Fix12_KeyEvent(EKeys::W));
	WoTTest::TickWorld(Fixture.World, 2.5f);
	TestEqual(TEXT("held balefire remains charged before pause"),
		Fixture.Victim->Health, 1000);
	TestTrue(TEXT("real listen pause succeeds"),
		Fixture.Game->TrySetPlayerPause(Fixture.Controller, NM_ListenServer));
	Slate.ProcessKeyDownEvent(Fix12_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("Talk opens the actual chat overlay while paused"),
		Fixture.Controller->IsChatTyping());
	TestTrue(TEXT("held Fire remains in the gameplay input state"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	TestTrue(TEXT("held movement remains in the gameplay input state"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::W));

	Slate.ProcessKeyUpEvent(Fix12_KeyEvent(EKeys::F));
	Slate.ProcessKeyUpEvent(Fix12_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("Slate forwards Fire release before external unpause"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	TestEqual(TEXT("Fire release ends the cast while paused"),
		Fixture.Victim->Health, 600);
	Fixture.Game->ClearPause();
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("chat remains open when another admin unpauses"),
		Fixture.Controller->IsChatTyping());
	TestEqual(TEXT("external unpause adds no second Fire transition"),
		Fixture.Victim->Health, 600);
	TestTrue(TEXT("a key still held through chat stays held"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::W));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix12PausedChatMouseReleaseSpec,
	"WoT.Parity.Fix12.PausedChatMouseRelease", Fix12_Flags)

bool FFix12PausedChatMouseReleaseSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	// Headless (-nullrhi) Slate focus on an editable chat widget has no native
	// NSWindow, so the macOS text-input-method system logs an Error that a real
	// game window never produces. Ignore it here; the assertions are unaffected.
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, /*IsRegex*/ false);
#endif
	FFix12_ChatFixture Fixture;
	if (!Fixture.Initialize(*this, EKeys::LeftMouseButton))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	TestFalse(TEXT("mouse starts uncaptured in the commandlet fixture"),
		Fixture.ViewportWidget->HasMouseCapture());
	FWidgetPath ViewportPath;
	if (!TestTrue(TEXT("Slate builds a path to the real viewport"),
		Slate.GeneratePathToWidgetUnchecked(
			Fixture.ViewportWidget.ToSharedRef(), ViewportPath)))
	{
		return false;
	}
	const FGeometry ViewportGeometry = ViewportPath.Widgets.Last().Geometry;
	const FVector2D Inside = ViewportGeometry.LocalToAbsolute(
		ViewportGeometry.GetLocalSize() * 0.5f);
	TestTrue(TEXT("mouse-down point is inside the real viewport"),
		ViewportGeometry.IsUnderLocation(Inside));
	TArray<FWidgetAndPointer> PointerWidgets;
	PointerWidgets.Reserve(ViewportPath.Widgets.Num());
	for (int32 Index = 0; Index < ViewportPath.Widgets.Num(); ++Index)
	{
		PointerWidgets.Emplace(ViewportPath.Widgets[Index],
			FVirtualPointerPosition(Inside, Inside));
	}
	FWidgetPath PointerPath(MakeArrayView(PointerWidgets));
	if (!TestTrue(TEXT("pointer path reaches the real viewport"),
		PointerPath.ContainsWidget(Fixture.ViewportWidget.Get())))
	{
		return false;
	}
	const FPointerEvent MouseDown = Fix12_MouseEvent(Inside, true);
	Slate.RoutePointerDownEvent(PointerPath, MouseDown);
	WoTTest::TickWorld(Fixture.World, 2.5f);
	TestTrue(TEXT("real mouse path starts gameplay Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	TestEqual(TEXT("mouse-held balefire remains charged before pause"),
		Fixture.Victim->Health, 1000);
	TestTrue(TEXT("real listen pause succeeds"),
		Fixture.Game->TrySetPlayerPause(Fixture.Controller, NM_ListenServer));
	Slate.ProcessKeyDownEvent(Fix12_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("Talk opens the actual chat overlay while paused"),
		Fixture.Controller->IsChatTyping());
	TestTrue(TEXT("paused chat retains capture for held gameplay mouse input"),
		Fixture.ViewportWidget->HasMouseCapture());
	TestEqual(TEXT("chat uses explicit capture rather than mouse-down mode"),
		Fixture.ViewportClient->GetMouseCaptureMode(),
		EMouseCaptureMode::NoCapture);

	Slate.ProcessMouseButtonUpEvent(
		Fix12_MouseEvent(FVector2D(5000.0f, 5000.0f), false));
	Slate.ProcessKeyUpEvent(Fix12_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("outside mouse release reaches gameplay while paused"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	TestEqual(TEXT("outside mouse release ends the cast while paused"),
		Fixture.Victim->Health, 600);
	Slate.ProcessKeyDownEvent(Fix12_KeyEvent(EKeys::Escape));
	TestFalse(TEXT("real Escape path closes chat"),
		Fixture.Controller->IsChatTyping());
	Fixture.Game->ClearPause();
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestEqual(TEXT("unpause adds no second mouse Fire transition"),
		Fixture.Victim->Health, 600);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix12InitThreatGoalEventSpec,
	"WoT.Parity.Fix12.InitThreatGoalEvent", Fix12_Flags)

bool FFix12InitThreatGoalEventSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	const FName EventTag(TEXT("Fix12InitThreat"));
	AWoTTestSoundDispatcherEventSink* Sink = World
		? Fix12_SpawnSink(*World, EventTag) : nullptr;
	AWoTNPCCharacter* NPC = World
		? World->SpawnActor<AWoTNPCCharacter>() : nullptr;
	AActor* Target = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("AI world"), World)
		|| !TestNotNull(TEXT("event sink"), Sink)
		|| !TestNotNull(TEXT("NPC"), NPC)
		|| !TestNotNull(TEXT("threat target"), Target)
		|| !TestNotNull(TEXT("NPC brain"), NPC ? NPC->Brain.Get() : nullptr))
	{
		return false;
	}
	Fix12_SetNameProperty(*NPC, TEXT("InitThreatGoalEvent"), EventTag, *this);
	NPC->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	NPC->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Target);
	TestEqual(TEXT("every threat initialization fires, including same target"),
		Sink->TriggerCount, 2);
	TestTrue(TEXT("threat event Other is the grunt"),
		Sink->LastOther.Get() == NPC);
	TestTrue(TEXT("threat event instigator is the grunt"),
		Sink->LastInstigator.Get() == NPC);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix12TakeDamageEventSpec,
	"WoT.Parity.Fix12.TakeDamageEvent", Fix12_Flags)

bool FFix12TakeDamageEventSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	const FName RepeatTag(TEXT("Fix12DamageRepeat"));
	const FName OnceTag(TEXT("Fix12DamageOnce"));
	AWoTTestSoundDispatcherEventSink* RepeatSink = World
		? Fix12_SpawnSink(*World, RepeatTag) : nullptr;
	AWoTTestSoundDispatcherEventSink* OnceSink = World
		? Fix12_SpawnSink(*World, OnceTag) : nullptr;
	AWoTNPCCharacter* RepeatNPC = World
		? World->SpawnActor<AWoTNPCCharacter>() : nullptr;
	AWoTNPCCharacter* OnceNPC = World
		? World->SpawnActor<AWoTNPCCharacter>() : nullptr;
	if (!TestNotNull(TEXT("AI world"), World)
		|| !TestNotNull(TEXT("repeat sink"), RepeatSink)
		|| !TestNotNull(TEXT("once sink"), OnceSink)
		|| !TestNotNull(TEXT("repeat NPC"), RepeatNPC)
		|| !TestNotNull(TEXT("once NPC"), OnceNPC))
	{
		return false;
	}
	Fix12_SetNameProperty(*RepeatNPC, TEXT("TakeDamageEvent"), RepeatTag, *this);
	Fix12_SetNameProperty(*OnceNPC, TEXT("TakeDamageEvent"), OnceTag, *this);
	Fix12_SetBoolProperty(*OnceNPC, TEXT("bTakeDamageEventOnce"), true, *this);
	const FDamageEvent DamageEvent;
	RepeatNPC->TakeDamage(1.0f, DamageEvent, nullptr, nullptr);
	RepeatNPC->TakeDamage(1.0f, DamageEvent, nullptr, nullptr);
	OnceNPC->TakeDamage(1.0f, DamageEvent, nullptr, nullptr);
	OnceNPC->TakeDamage(1.0f, DamageEvent, nullptr, nullptr);
	TestEqual(TEXT("ordinary damage event fires for every hit"),
		RepeatSink->TriggerCount, 2);
	TestEqual(TEXT("once damage event fires only for the first hit"),
		OnceSink->TriggerCount, 1);
	TestTrue(TEXT("damage event Other is the grunt"),
		OnceSink->LastOther.Get() == OnceNPC);
	TestTrue(TEXT("damage event instigator is the grunt"),
		OnceSink->LastInstigator.Get() == OnceNPC);
	return !HasAnyErrors();
}

#endif
