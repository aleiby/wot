#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvBalefire.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../AI/WoTBrainComponent.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTWorldProjector.h"
#include "WoTRetailReader/Generated/WorldProperties.generated.h"
#include "WoTTestSoundDispatcher.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace
{
constexpr EAutomationTestFlags Fix13_Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FKeyEvent Fix13_KeyEvent(const FKey& Key)
{
	return FKeyEvent(Key, FModifierKeysState(), 0,
		/*bInIsRepeat*/ false, 0, 0);
}

FPointerEvent Fix13_PointerEvent(const FVector2D& Position,
	const FVector2D& LastPosition, const FKey& EffectingButton,
	TSet<FKey> PressedButtons = {})
{
	return FPointerEvent(0, FSlateApplicationBase::CursorPointerIndex,
		Position, LastPosition, MoveTemp(PressedButtons), EffectingButton,
		0.0f, FModifierKeysState());
}

bool Fix13_RegistryHas(FStringView ClassIdentity, FStringView PropertyName,
	uint8 TagType)
{
	for (const WoT::Retail::Generated::FWorldProperty& Property :
		WoT::Retail::Generated::WorldProperties)
	{
		if (ClassIdentity.Equals(Property.ActorClass, ESearchCase::IgnoreCase)
			&& PropertyName.Equals(
				Property.PropertyName, ESearchCase::IgnoreCase))
		{
			return Property.TagType == TagType;
		}
	}
	return false;
}

bool Fix13_ParseT3DActor(FStringView MapIdentity, FStringView T3D,
	FWoTRuntimeWorldActor& Out)
{
	TArray<FString> Lines;
	FString(T3D).ParseIntoArrayLines(Lines, true);
	for (FString Line : Lines)
	{
		Line.TrimStartAndEndInline();
		if (Line.StartsWith(TEXT("Begin Actor Class=")))
		{
			FString ClassAndName = Line.Mid(18);
			FString ClassLeaf;
			if (!ClassAndName.Split(TEXT(" Name="), &ClassLeaf, &Out.Identity)
				|| ClassLeaf.IsEmpty() || Out.Identity.IsEmpty())
			{
				return false;
			}
			Out.ClassIdentity = TEXT("WoTPawns.") + ClassLeaf;
			Out.Identity = FString(MapIdentity) + TEXT(".") + Out.Identity;
			continue;
		}
		if (Line.IsEmpty() || Line == TEXT("End Actor"))
		{
			continue;
		}
		FString Name;
		FString Value;
		if (!Line.Split(TEXT("="), &Name, &Value))
		{
			return false;
		}
		FWoTRuntimeWorldProperty& Property = Out.Properties.AddDefaulted_GetRef();
		Property.Name = MoveTemp(Name);
		if (Property.Name.Equals(TEXT("bTakeDamageEventOnce"),
			ESearchCase::IgnoreCase))
		{
			Property.Type = EWoTRuntimeWorldPropertyType::Boolean;
			Property.Value.Add(Value.Equals(TEXT("True"),
				ESearchCase::IgnoreCase) ? 1 : 0);
		}
		else
		{
			Property.Type = EWoTRuntimeWorldPropertyType::Name;
			Property.NameValue = MoveTemp(Value);
		}
	}
	return !Out.Identity.IsEmpty() && !Out.ClassIdentity.IsEmpty();
}

AWoTTestSoundDispatcherEventSink* Fix13_SpawnSink(
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

#if WITH_EDITOR
struct FFix13_ChatFixture
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
	FVector2D Inside = FVector2D::ZeroVector;

	~FFix13_ChatFixture()
	{
		if (Controller && Controller->IsChatTyping())
		{
			Controller->CancelChatInput();
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& Slate = FSlateApplication::Get();
			const TSet<FKey> Pressed = Slate.GetPressedMouseButtons();
			for (const FKey& Key : Pressed)
			{
				Slate.ProcessMouseButtonUpEvent(Fix13_PointerEvent(
					Inside, Inside, Key));
			}
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
		FWidgetPath ViewportPath;
		if (!Test.TestTrue(TEXT("real scene viewport owns keyboard focus"),
			Slate.GetKeyboardFocusedWidget() == ViewportWidget)
			|| !Test.TestTrue(TEXT("Slate builds a path to the real viewport"),
				Slate.GeneratePathToWidgetUnchecked(
					ViewportWidget.ToSharedRef(), ViewportPath)))
		{
			return false;
		}
		const FGeometry ViewportGeometry = ViewportPath.Widgets.Last().Geometry;
		Inside = ViewportGeometry.LocalToAbsolute(
			ViewportGeometry.GetLocalSize() * 0.5f);

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
		Controller->PlayerInput->AddActionMapping(
			FInputActionKeyMapping(TEXT("Fire"), EKeys::G));
		Controller->PlayerInput->AddAxisMapping(
			FInputAxisKeyMapping(TEXT("MoveForward"), EKeys::W, 1.0f));
		Controller->PlayerInput->AddAxisMapping(
			FInputAxisKeyMapping(TEXT("MouseX"), EKeys::MouseX, 1.0f));

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

	void TypeCharacter(const TCHAR Character) const
	{
		FSlateApplication::Get().ProcessKeyCharEvent(FCharacterEvent(
			Character, FModifierKeysState(), 0, false));
	}

	bool PressMouseButton(const FKey& Key, TSet<FKey> PressedButtons) const
	{
		FWidgetPath ViewportPath;
		if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(
			ViewportWidget.ToSharedRef(), ViewportPath))
		{
			return false;
		}
		TArray<FWidgetAndPointer> PointerWidgets;
		PointerWidgets.Reserve(ViewportPath.Widgets.Num());
		for (int32 Index = 0; Index < ViewportPath.Widgets.Num(); ++Index)
		{
			PointerWidgets.Emplace(ViewportPath.Widgets[Index],
				FVirtualPointerPosition(Inside, Inside));
		}
		return FSlateApplication::Get().RoutePointerDownEvent(
			FWidgetPath(MakeArrayView(PointerWidgets)),
			Fix13_PointerEvent(Inside, Inside, Key,
				MoveTemp(PressedButtons))).IsEventHandled();
	}

	bool ReleaseMouseButton(const FKey& Key, TSet<FKey> PressedButtons,
		const FVector2D& Position) const
	{
		return FSlateApplication::Get().ProcessMouseButtonUpEvent(
			Fix13_PointerEvent(Position, Position, Key, MoveTemp(PressedButtons)));
	}
};
#endif
} // namespace

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix13LiveChatHeldFireSpec,
	"WoT.Parity.Fix13.LiveChatHeldFire", Fix13_Flags)

bool FFix13LiveChatHeldFireSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
#endif
	FFix13_ChatFixture Fixture;
	if (!Fixture.Initialize(*this, EKeys::F))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::F));
	WoTTest::TickWorld(Fixture.World, 2.5f);
	TestTrue(TEXT("Fire is held before chat"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	TestEqual(TEXT("the charged cast has not released before chat"),
		Fixture.Victim->Health, 1000);

	Slate.ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::T));
	Fixture.TypeCharacter(TEXT('x'));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("Talk opens the focused chat widget"),
		Fixture.Controller->IsChatTyping());
	TestTrue(TEXT("opening and typing preserve held Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	TestEqual(TEXT("the cast remains charged while typing"),
		Fixture.Victim->Health, 1000);
	Slate.ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::G));
	TestFalse(TEXT("a new gameplay press is not added while typing"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::G));

	Slate.ProcessKeyUpEvent(Fix13_KeyEvent(EKeys::F));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("the held gameplay release reaches PlayerInput"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	TestEqual(TEXT("the release ends the charged cast while chat stays open"),
		Fixture.Victim->Health, 600);
	TestTrue(TEXT("chat remains open after the gameplay release"),
		Fixture.Controller->IsChatTyping());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix13PausedChatHeldFireSpec,
	"WoT.Parity.Fix13.PausedChatHeldFire", Fix13_Flags)

bool FFix13PausedChatHeldFireSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
#endif
	FFix13_ChatFixture Fixture;
	if (!Fixture.Initialize(*this, EKeys::F))
	{
		return false;
	}
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::F));
	Slate.ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::W));
	WoTTest::TickWorld(Fixture.World, 2.5f);
	TestTrue(TEXT("real listen pause succeeds"),
		Fixture.Game->TrySetPlayerPause(Fixture.Controller, NM_ListenServer));
	Slate.ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::T));
	Fixture.TypeCharacter(TEXT('x'));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("Talk opens the focused chat widget while paused"),
		Fixture.Controller->IsChatTyping());
	TestTrue(TEXT("paused chat preserves held Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	TestTrue(TEXT("paused chat preserves a separately held movement key"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::W));
	Slate.ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::G));
	TestFalse(TEXT("paused typing does not add a new gameplay press"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::G));

	Slate.ProcessKeyUpEvent(Fix13_KeyEvent(EKeys::F));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("the paused gameplay release reaches PlayerInput immediately"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	Fixture.Game->ClearPause();
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("external unpause leaves chat open"),
		Fixture.Controller->IsChatTyping());
	TestFalse(TEXT("external unpause does not restore released Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::F));
	TestFalse(TEXT("external unpause does not add the swallowed press"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::G));
	TestTrue(TEXT("external unpause does not lose the held movement key"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::W));
	TestEqual(TEXT("the released charged cast resolves after unpause"),
		Fixture.Victim->Health, 600);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix13ChatMouseCaptureSpec,
	"WoT.Parity.Fix13.ChatMouseCapture", Fix13_Flags)

bool FFix13ChatMouseCaptureSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
#endif
	FFix13_ChatFixture Fixture;
	if (!Fixture.Initialize(*this, EKeys::LeftMouseButton))
	{
		return false;
	}
	Fixture.PressMouseButton(EKeys::LeftMouseButton,
		{EKeys::LeftMouseButton});
	WoTTest::TickWorld(Fixture.World, 2.5f);
	TestTrue(TEXT("real mouse path starts gameplay Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	FSlateApplication::Get().ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("chat preserves the held mouse Fire"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	TestTrue(TEXT("chat explicitly retains viewport capture for held Fire"),
		Fixture.ViewportWidget->HasMouseCapture());

	Fixture.PressMouseButton(EKeys::MiddleMouseButton,
		{EKeys::LeftMouseButton, EKeys::MiddleMouseButton});
	Fixture.ReleaseMouseButton(EKeys::MiddleMouseButton,
		{EKeys::LeftMouseButton}, Fixture.Inside);
	TestTrue(TEXT("an unrelated mouse click does not drop held-Fire capture"),
		Fixture.ViewportWidget->HasMouseCapture());
	Fixture.ReleaseMouseButton(EKeys::LeftMouseButton, {},
		FVector2D(5000.0f, 5000.0f));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestFalse(TEXT("the outside release reaches PlayerInput"),
		Fixture.Controller->PlayerInput->IsPressed(EKeys::LeftMouseButton));
	TestEqual(TEXT("the outside release ends the charged cast"),
		Fixture.Victim->Health, 600);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix13ChatMouseLookSpec,
	"WoT.Parity.Fix13.ChatMouseLook", Fix13_Flags)

bool FFix13ChatMouseLookSpec::RunTest(const FString&)
{
#if PLATFORM_MAC
	AddExpectedError(TEXT("context failed when its window couldn't be found"),
		EAutomationExpectedErrorFlags::Contains, -1, false);
#endif
	FFix13_ChatFixture Fixture;
	if (!Fixture.Initialize(*this, EKeys::F))
	{
		return false;
	}
	FSlateApplication::Get().ProcessKeyDownEvent(Fix13_KeyEvent(EKeys::T));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	const float YawBefore = Fixture.Controller->GetControlRotation().Yaw;
	FSlateApplication::Get().ProcessMouseMoveEvent(Fix13_PointerEvent(
		Fixture.Inside + FVector2D(48.0f, 0.0f), Fixture.Inside,
		EKeys::Invalid));
	WoTTest::TickWorld(Fixture.World, 1.0f / 60.0f);
	TestTrue(TEXT("mouse-look axis still turns the player while chat is focused"),
		!FMath::IsNearlyEqual(
			Fixture.Controller->GetControlRotation().Yaw, YawBefore));
	TestTrue(TEXT("mouse-look leaves chat open"),
		Fixture.Controller->IsChatTyping());
	return !HasAnyErrors();
}
#endif

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix13NPCAuthoredEventMaterializationSpec,
	"WoT.Parity.Fix13.NPCAuthoredEventMaterialization", Fix13_Flags)

bool FFix13NPCAuthoredEventMaterializationSpec::RunTest(const FString&)
{
	TestTrue(TEXT("Mission_03 HoundBot threat event is in UE1 reflection"),
		Fix13_RegistryHas(TEXT("WoTPawns.HoundBot"),
			TEXT("InitThreatGoalEvent"), 6));
	TestTrue(TEXT("Mission_16 TrollocMed damage event is in UE1 reflection"),
		Fix13_RegistryHas(TEXT("WoTPawns.TrollocMed"),
			TEXT("TakeDamageEvent"), 6));
	TestTrue(TEXT("Mission_16 TrollocMed once flag is in UE1 reflection"),
		Fix13_RegistryHas(TEXT("WoTPawns.TrollocMed"),
			TEXT("bTakeDamageEventOnce"), 3));

	TArray<FWoTRuntimeWorldActor> Records;
	FWoTRuntimeWorldActor& Hound = Records.AddDefaulted_GetRef();
	FWoTRuntimeWorldActor& Trolloc = Records.AddDefaulted_GetRef();
	if (!TestTrue(TEXT("Mission_03-shaped T3D actor parses"),
		Fix13_ParseT3DActor(TEXT("Mission_03"), TEXT(
			"Begin Actor Class=HoundBot Name=HoundBot2\n"
			"    InitThreatGoalEvent=DestroyHoundLaughter\n"
			"End Actor\n"), Hound))
		|| !TestTrue(TEXT("Mission_16-shaped T3D actor parses"),
			Fix13_ParseT3DActor(TEXT("Mission_16"), TEXT(
				"Begin Actor Class=TrollocMed Name=TrollocMed0\n"
				"    TakeDamageEvent=GetHer\n"
				"    bTakeDamageEventOnce=True\n"
				"End Actor\n"), Trolloc)))
	{
		return false;
	}

	TArray<FWoTRuntimeClassDefaultRecord> Classes;
	auto AddClass = [&Classes](const TCHAR* Identity, const TCHAR* Super)
	{
		FWoTRuntimeClassDefaultRecord& Record = Classes.AddDefaulted_GetRef();
		Record.Identity = Identity;
		Record.SuperclassIdentity = Super;
	};
	AddClass(TEXT("Engine.Actor"), TEXT(""));
	AddClass(TEXT("Engine.Pawn"), TEXT("Engine.Actor"));
	AddClass(TEXT("WOT.WOTPawn"), TEXT("Engine.Pawn"));
	AddClass(TEXT("WOT.Grunt"), TEXT("WOT.WOTPawn"));
	AddClass(TEXT("WoTPawns.HoundBot"), TEXT("WOT.Grunt"));
	AddClass(TEXT("WoTPawns.Trolloc"), TEXT("WOT.Grunt"));
	AddClass(TEXT("WoTPawns.TrollocMed"), TEXT("WoTPawns.Trolloc"));
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("NPC class-default fixture loads"),
		Defaults.LoadFixture(MoveTemp(Classes), Error)))
	{
		AddError(Error);
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	FWoTWorldProjectionSource Source;
	Source.MapIdentity = TEXT("Fix13NPCEvents");
	Source.Records = Records;
	FWoTWorldProjectionResult Projection;
	const bool bProjected = FWoTWorldProjector::ProjectDeferred(
		*World, Source, Defaults, Projection, Error);
	TestTrue(TEXT("parsed NPC actors project"), bProjected);
	if (!bProjected)
	{
		AddError(Error);
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	AWoTNPCCharacter* ProjectedHound = nullptr;
	AWoTNPCCharacter* ProjectedTrolloc = nullptr;
	for (AActor* Actor : Projection.DeferredActors)
	{
		if (Actor && Actor->GetName().Contains(TEXT("NPC")))
		{
			if (!ProjectedHound) ProjectedHound = Cast<AWoTNPCCharacter>(Actor);
			else if (!ProjectedTrolloc) ProjectedTrolloc = Cast<AWoTNPCCharacter>(Actor);
		}
	}
	if (TestNotNull(TEXT("Mission_03 HoundBot materializes as an NPC"),
			ProjectedHound))
	{
		TestEqual(TEXT("InitThreatGoalEvent reaches the materialized NPC"),
			ProjectedHound->InitThreatGoalEvent,
			FName(TEXT("DestroyHoundLaughter")));
	}
	if (TestNotNull(TEXT("Mission_16 TrollocMed materializes as an NPC"),
			ProjectedTrolloc))
	{
		TestEqual(TEXT("TakeDamageEvent reaches the materialized NPC"),
			ProjectedTrolloc->TakeDamageEvent, FName(TEXT("GetHer")));
		TestTrue(TEXT("bTakeDamageEventOnce reaches the materialized NPC"),
			ProjectedTrolloc->bTakeDamageEventOnce);
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFix13CaptainThreatGoalInitializedSpec,
	"WoT.Parity.Fix13.CaptainThreatGoalInitialized", Fix13_Flags)

bool FFix13CaptainThreatGoalInitializedSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	const FName EventTag(TEXT("Fix13CaptainThreat"));
	AWoTTestSoundDispatcherEventSink* Sink = World
		? Fix13_SpawnSink(*World, EventTag) : nullptr;
	AWoTNPCCharacter* Grunt = World
		? World->SpawnActor<AWoTNPCCharacter>() : nullptr;
	AWoTNPCCharacter* Captain = World
		? World->SpawnActor<AWoTNPCCharacter>() : nullptr;
	AActor* Threat = World ? World->SpawnActor<AActor>() : nullptr;
	if (!TestNotNull(TEXT("AI world"), World)
		|| !TestNotNull(TEXT("event sink"), Sink)
		|| !TestNotNull(TEXT("grunt"), Grunt)
		|| !TestNotNull(TEXT("captain"), Captain)
		|| !TestNotNull(TEXT("captain's threat"), Threat)
		|| !TestNotNull(TEXT("grunt brain"), Grunt ? Grunt->Brain.Get() : nullptr)
		|| !TestNotNull(TEXT("captain brain"),
			Captain ? Captain->Brain.Get() : nullptr))
	{
		return false;
	}

	Captain->bIsCaptain = true;
	Grunt->InitThreatGoalEvent = EventTag;
	Captain->Brain->InitGoalWithActor(EWoTGoalIndex::Threat, Threat);
	FWoTGoalCacheItem& CaptainCache = Captain->Brain
		->GetGoal(EWoTGoalIndex::Threat).Cache.Get(
			EWoTGoalCacheItem::ReachableOK);
	CaptainCache.bValid = true;
	CaptainCache.GoalLocation = FVector(101.0, 202.0, 303.0);
	CaptainCache.ObserverLocation = FVector(404.0, 505.0, 606.0);

	Grunt->Brain->AssignGoalFrom(EWoTGoalIndex::Threat, Captain->Brain,
		EWoTGoalIndex::Threat);
	const FWoTGoal& Adopted = Grunt->Brain->GetGoal(EWoTGoalIndex::Threat);
	const FWoTGoalCacheItem& AdoptedCache = Adopted.Cache.Get(
		EWoTGoalCacheItem::ReachableOK);
	TestTrue(TEXT("the grunt adopts the captain's threat actor"),
		Adopted.GoalActor.Get() == Threat);
	TestEqual(TEXT("captain threat adoption initializes the grunt's goal"),
		Sink->TriggerCount, 1);
	TestTrue(TEXT("the copied cache remains valid"), AdoptedCache.bValid);
	TestEqual(TEXT("the copied cache preserves the goal stamp"),
		AdoptedCache.GoalLocation, CaptainCache.GoalLocation);
	TestEqual(TEXT("the copied cache preserves the observer stamp"),
		AdoptedCache.ObserverLocation, CaptainCache.ObserverLocation);
	return !HasAnyErrors();
}

#endif
