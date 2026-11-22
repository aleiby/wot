#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTHUDStreamFixture.h"
#include "WoTLocalPlayerHooksTestFixture.h"
#include "WoTTestWorld.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCanvasScale.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameMode.h"
#include "../WoTHUD.h"
#include "../WoTLocalPlayerHooks.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "RHIFeatureLevel.h"
#include "UnrealClient.h"

// The flat HUD's canvas output, item by item, for representative states. The
// streams were recorded from the HUD before its update, snapshot, layout and
// draw were separated (H5); with no consumer attached they must not change.
// The harness, the states and the goldens are in WoTHUDStreamFixture.cpp,
// shared with the optional VR plugin's specs.
namespace WoTHUDHookTest
{
constexpr EAutomationTestFlags Flags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookHUDInertStreamSpec,
	"WoT.VR.Hooks.H5.InertFlatDrawStream", WoTHUDHookTest::Flags)

bool FWoTHookHUDInertStreamSpec::RunTest(const FString&)
{
	// The ten recorded states, with nothing attached.
	WoTHUDHookTest::RunRecordedStates(*this);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookHUDUpdateOnceSpec,
	"WoT.VR.Hooks.H5.UpdateOncePerFrame", WoTHUDHookTest::Flags)

bool FWoTHookHUDUpdateOnceSpec::RunTest(const FString&)
{
	using WoTHUDHookTest::FScene;
	using WoTHUDHookTest::DrawHUDOnce;
	FScene S;
	if (!S.Init(*this)) return false;
	S.AddMessages();
	S.Draw();
	const FWoTHUDSnapshot& Shown = S.HUD->GetSnapshot();
	const uint64 FirstSerial = Shown.Serial;
	const float FirstLife = S.HUD->LeftMessages[0].LifeSpan;
	TestTrue(TEXT("the first draw updates the HUD"), FirstSerial > 0);
	TestTrue(TEXT("the latest HUD is the HUD's own snapshot"),
		FWoTLocalPlayerHooks::LatestHUD(*S.F.Player) == &Shown);

	// A second draw in the same frame (a second destination, a repeated draw)
	// reads the same update.
	DrawHUDOnce(*S.HUD, 640, 480);
	TestTrue(TEXT("a second draw in a frame does not update again"), Shown.Serial == FirstSerial);
	TestEqual(TEXT("a second draw in a frame does not age the messages again"),
		S.HUD->LeftMessages[0].LifeSpan, FirstLife);

	// Two readers, an observer and the destination: one update per frame each.
	int32 Observed = 0;
	int32 Presented = 0;
	uint64 ObservedSerial = 0;
	uint64 PresentedSerial = 0;
	FWoTLocalPlayerConsumer Observer;
	Observer.HUDObserver = [&](const FWoTHUDSnapshot& Snapshot)
	{
		++Observed;
		ObservedSerial = Snapshot.Serial;
	};
	FWoTLocalPlayerConsumer Destination;
	Destination.HUDDestination = [&](const FWoTHUDSnapshot& Snapshot)
	{
		++Presented;
		PresentedSerial = Snapshot.Serial;
	};
	if (!TestTrue(TEXT("an observer attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, TEXT("HUDObserver"), Observer))) return false;
	if (!TestTrue(TEXT("a destination attaches"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, TEXT("HUDDestination"), Destination))) return false;
	const float StartTime = static_cast<float>(S.F.World->GetTimeSeconds());
	for (int32 Frame = 1; Frame <= 5; ++Frame)
	{
		S.Wait(4);
		const TArray<FString> Flat = S.DrawFrame();
		DrawHUDOnce(*S.HUD, 640, 480);
		TestEqual(TEXT("the observer reads one update per frame"), Observed, Frame);
		TestEqual(TEXT("the destination reads one update per frame"), Presented, Frame);
		TestTrue(TEXT("both read this frame's update"), ObservedSerial == Shown.Serial);
		TestTrue(TEXT("the destination and the observer read the same update"),
			PresentedSerial == ObservedSerial);
		TestTrue(TEXT("the serial counts frames"), Shown.Serial == FirstSerial + Frame);
		TestTrue(TEXT("the flat Canvas HUD is not drawn while the destination is owned"),
			Flat.IsEmpty());
	}
	// Message time ran once per frame: exactly the world time since the first
	// draw (the four ticks before each frame plus the frame's own).
	const float Elapsed = static_cast<float>(S.F.World->GetTimeSeconds()) - StartTime;
	TestEqual(TEXT("messages age by the world time, once"),
		S.HUD->LeftMessages[0].LifeSpan, FirstLife - Elapsed, 1.0e-4f);
	FWoTLocalPlayerHooks::Detach(*S.F.Player, TEXT("HUDDestination"));
	FWoTLocalPlayerHooks::Detach(*S.F.Player, TEXT("HUDObserver"));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookHUDPausedRulesSpec,
	"WoT.VR.Hooks.H5.PausedMessagesAndVisibility", WoTHUDHookTest::Flags)

bool FWoTHookHUDPausedRulesSpec::RunTest(const FString&)
{
	using WoTHUDHookTest::FScene;
	// The same frames with the flat HUD and with a destination presenting:
	// the update's rules do not depend on where the HUD is drawn.
	TArray<float> Lives[2];
	for (const bool bDestination : {false, true})
	{
		const FString Label = bDestination ? TEXT("destination") : TEXT("flat");
		TArray<float>& Life = Lives[bDestination ? 1 : 0];
		FScene S;
		if (!S.Init(*this)) return false;
		if (bDestination && !TestTrue(Label + TEXT(": attach"), FWoTLocalPlayerHooks::Attach(
			*S.F.Player, TEXT("HUDDestination"), [] {
				FWoTLocalPlayerConsumer Consumer;
				Consumer.HUDDestination = [](const FWoTHUDSnapshot&) {};
				return Consumer;
			}()))) return false;
		S.AddMessages();
		S.Draw();
		Life.Add(S.HUD->LeftMessages[0].LifeSpan);
		// bHideMessages pauses message time; the HUD's own time keeps running.
		S.HUD->bHideMessages = true;
		S.Wait(30);
		S.Draw();
		Life.Add(S.HUD->LeftMessages[0].LifeSpan);
		TestEqual(Label + TEXT(": hidden messages do not age"), Life.Last(), Life[0]);
		TestNull(Label + TEXT(": hidden messages leave the snapshot"),
			S.HUD->GetSnapshot().FindSection(EWoTHUDSection::Messages));
		S.HUD->bHideMessages = false;
		S.Draw();
		Life.Add(S.HUD->LeftMessages[0].LifeSpan);
		TestEqual(Label + TEXT(": paused time is not caught up"), Life.Last(),
			Life[1] - 1.0f / 60.0f, 1.0e-4f);
		// The item window hides the ordinary messages the same way, and keeps
		// the groups and progress.
		S.F.PC->ShowInventoryInfo();
		S.Wait(30);
		S.Draw();
		Life.Add(S.HUD->LeftMessages[0].LifeSpan);
		const FWoTHUDSnapshot& Shown = S.HUD->GetSnapshot();
		TestEqual(Label + TEXT(": the item window pauses message time"), Life.Last(), Life[2]);
		TestNull(Label + TEXT(": the item window hides health"), Shown.FindSection(EWoTHUDSection::Health));
		TestNotNull(Label + TEXT(": the item window keeps the groups"), Shown.FindSection(EWoTHUDSection::Hands));
		TestNotNull(Label + TEXT(": the item window keeps progress"), Shown.FindSection(EWoTHUDSection::Progress));
		S.F.PC->ShowInventoryInfo();
		// The in-game menu hides the whole HUD: nothing shows, and the time
		// it was up is caught up on the first visible frame.
		S.F.PC->ToggleInGameMenu();
		S.Wait(30);
		S.Draw();
		Life.Add(S.HUD->LeftMessages[0].LifeSpan);
		TestFalse(Label + TEXT(": the menu hides the HUD"), Shown.bVisible);
		TestEqual(Label + TEXT(": a hidden HUD shows nothing"), Shown.Sections.Num(), 0);
		TestTrue(Label + TEXT(": a hidden HUD exposes no messages"), Shown.Messages.IsEmpty());
		TestFalse(Label + TEXT(": a hidden HUD exposes no health"), Shown.bHealth);
		TestEqual(Label + TEXT(": the menu stops message time"), Life.Last(), Life[3]);
		S.F.PC->ResumeFromInGameMenu();
		S.Draw();
		Life.Add(S.HUD->LeftMessages[0].LifeSpan);
		TestTrue(Label + TEXT(": the HUD shows again"), Shown.bVisible);
		TestTrue(Label + TEXT(": the first visible frame catches the hidden time up"),
			Life.Last() < Life[4] - 0.5f);
	}
	TestTrue(TEXT("message time runs the same with a destination"), Lives[1] == Lives[0]);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookHUDSnapshotContentsSpec,
	"WoT.VR.Hooks.H5.SnapshotMatchesFlatDraw", WoTHUDHookTest::Flags)

bool FWoTHookHUDSnapshotContentsSpec::RunTest(const FString&)
{
	using WoTHUDHookTest::FScene;
	using WoTHUDHookTest::DrawSectionsOnce;
	using WoTHUDHookTest::SectionTexts;
	{
		FScene S;
		if (!S.Init(*this)) return false;
		S.AddMessages();
		S.HUD->ClientWarn();
		S.HUD->SetFlashTexture(TEXT("Turtle"), 2.0f);
		for (const FIntPoint Size : {FIntPoint(640, 480), FIntPoint(1280, 800), FIntPoint(320, 240)})
		{
			const FString Label = FString::Printf(TEXT("%dx%d"), Size.X, Size.Y);
			const TArray<FString> Flat = S.DrawFrame(Size.X, Size.Y);
			const FWoTHUDSnapshot& Shown = S.HUD->GetSnapshot();
			// The snapshot's sections, drawn on a canvas of the caller's own,
			// are exactly what the flat HUD drew.
			TestEqual(Label + TEXT(": the snapshot draws what the flat HUD drew"),
				DrawSectionsOnce(S.F.World, Shown.Sections, Size.X, Size.Y), Flat);
			TestFalse(Label + TEXT(": the flat HUD drew"), Flat.IsEmpty());
		}
		const FWoTHUDSnapshot& Shown = S.HUD->GetSnapshot();
		TestTrue(TEXT("health"), Shown.bHealth && Shown.Health == 73);
		TestTrue(TEXT("the selected artifact"), Shown.Artifact == FName(TEXT("AngrealInvFireball")));
		TestEqual(TEXT("its charges"), Shown.Charges, 12);
		TestEqual(TEXT("both status icons"), Shown.Status.Num(), 2);
		TestTrue(TEXT("every message queue"), Shown.Messages.ContainsByPredicate(
			[](const FWoTHUDMessage& Message)
			{
				return Message.Kind == EWoTHUDMessageKind::Hand && Message.Text == TEXT("Hand message");
			}) && Shown.Messages.ContainsByPredicate([](const FWoTHUDMessage& Message)
			{
				return Message.Kind == EWoTHUDMessageKind::Generic && Message.Text == TEXT("Generic left");
			}));
		TestEqual(TEXT("both progress lines"), Shown.Progress.Num(), 2);
		TestTrue(TEXT("the warning"), Shown.bWarning);
		TestTrue(TEXT("the flash texture"), Shown.FlashTexture == FName(TEXT("Turtle")));
		TArray<EWoTHUDSection> Order;
		for (const FWoTHUDSection& Section : Shown.Sections)
		{
			Order.Add(Section.Section);
		}
		TestTrue(TEXT("the sections follow the flat draw order"), Order == TArray<EWoTHUDSection>({
			EWoTHUDSection::Health, EWoTHUDSection::StatusIcons, EWoTHUDSection::Reticle,
			EWoTHUDSection::CurrentHand, EWoTHUDSection::Hands, EWoTHUDSection::Progress,
			EWoTHUDSection::Messages, EWoTHUDSection::Flash, EWoTHUDSection::Warning}));
		// Nothing debug-only: the developer readout never reaches the snapshot.
		IConsoleVariable* Debug = IConsoleManager::Get().FindConsoleVariable(TEXT("wot.HUDDebug"));
		const int32 DebugBefore = Debug ? Debug->GetInt() : 0;
		if (Debug)
		{
			Debug->Set(1, ECVF_SetByCode);
		}
		S.Draw();
		for (const FWoTHUDSection& Section : S.HUD->GetSnapshot().Sections)
		{
			for (const FWoTHUDDrawCmd& Command : Section.Commands)
			{
				TestFalse(TEXT("no developer readout in the snapshot"),
					Command.Text.StartsWith(TEXT("Health: ")) || Command.Text.StartsWith(TEXT("Score: ")));
			}
		}
		if (Debug)
		{
			Debug->Set(DebugBefore, ECVF_SetByCode);
		}
	}
	// Sixteen players: every row in the flat board, and every row reachable a
	// page at a time on a short reading panel.
	{
		FScene S;
		if (!S.Init(*this)) return false;
		TArray<FString> Names;
		for (int32 Index = 1; Index < 16; ++Index)
		{
			AWoTPlayerState* Other = S.F.World->SpawnActor<AWoTPlayerState>(WoTHUDHookTest::SpawnParams());
			if (!TestNotNull(TEXT("scoreboard player"), Other)) return false;
			Other->SetPlayerName(FString::Printf(TEXT("Player %02d"), Index));
			Other->Kills = (Index * 7) % 11;
			Names.Add(Other->GetPlayerName());
		}
		AWoTPlayerState* Own = S.F.PC->GetPlayerState<AWoTPlayerState>();
		if (!TestNotNull(TEXT("own player state"), Own)) return false;
		Own->SetPlayerName(TEXT("Local Player"));
		Names.Add(Own->GetPlayerName());
		S.F.PC->bShowScores = true;
		S.Draw();
		const FWoTHUDSnapshot& Shown = S.HUD->GetSnapshot();
		TestTrue(TEXT("the scoreboard shows"), Shown.bScoreboard);
		TestEqual(TEXT("the snapshot carries all sixteen players"), Shown.Scoreboard.Players.Num(), 16);
		for (const FString& Name : Names)
		{
			TestEqual(FString::Printf(TEXT("the flat board draws %s"), *Name),
				SectionTexts(Shown, EWoTHUDSection::Scoreboard, Name), 1);
		}
		FWoTHUDLayoutOptions Options;
		Options.Layout = EWoTHUDLayout::Reading;
		Options.Size = FIntPoint(640, 360);
		const FWoTHUDLayoutResult First = FWoTHUDLayout::Arrange(Shown, Options);
		TestTrue(TEXT("a short reading panel pages the board"), First.PageCount > 1);
		TSet<FString> Reached;
		for (int32 Page = 0; Page < First.PageCount; ++Page)
		{
			Options.Page = Page;
			for (const FWoTHUDSection& Section : FWoTHUDLayout::Arrange(Shown, Options).Sections)
			{
				for (const FWoTHUDDrawCmd& Command : Section.Commands)
				{
					if (Names.Contains(Command.Text))
					{
						TestFalse(TEXT("each row shows on one page"), Reached.Contains(Command.Text));
						Reached.Add(Command.Text);
					}
				}
			}
		}
		TestEqual(TEXT("every row is reachable"), Reached.Num(), 16);
		Options.Size = FIntPoint(1280, 1440);
		Options.Page = 0;
		TestEqual(TEXT("a tall reading panel holds every row on one page"),
			FWoTHUDLayout::Arrange(Shown, Options).PageCount, 1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTHookHUDDestinationSpec,
	"WoT.VR.Hooks.H5.DestinationOwnership", WoTHUDHookTest::Flags)

bool FWoTHookHUDDestinationSpec::RunTest(const FString&)
{
	using WoTHUDHookTest::FScene;
	using WoTHUDHookTest::DrawSectionsOnce;
	FScene S;
	if (!S.Init(*this)) return false;
	S.AddMessages();
	S.HUD->ClientWarn();
	TestFalse(TEXT("the flat HUD draws"), S.DrawFrame().IsEmpty());

	const float LifeBefore = S.HUD->LeftMessages[0].LifeSpan;
	FWoTHUDSnapshot Presented;
	FWoTLocalPlayerConsumer Destination;
	Destination.HUDDestination = [&Presented](const FWoTHUDSnapshot& Snapshot)
	{
		Presented = Snapshot;
	};
	if (!TestTrue(TEXT("the destination attaches in a match"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, TEXT("Destination"), Destination))) return false;
	AddExpectedError(TEXT("exclusive local-player slot"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("a second destination is rejected"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, TEXT("SecondDestination"), Destination));
	FWoTLocalPlayerConsumer Observer;
	Observer.HUDObserver = [](const FWoTHUDSnapshot&) {};
	TestTrue(TEXT("an observer coexists with the destination"), FWoTLocalPlayerHooks::Attach(
		*S.F.Player, TEXT("Observer"), Observer));
	TestTrue(TEXT("owning the destination suppresses the flat Canvas HUD"), S.DrawFrame().IsEmpty());
	TestTrue(TEXT("the destination receives what shows"),
		Presented.FindSection(EWoTHUDSection::Hands) && Presented.FindSection(EWoTHUDSection::Messages));

	// The destination arranges and draws on its own canvas.
	for (const EWoTHUDLayout Layout : {EWoTHUDLayout::Compact, EWoTHUDLayout::Reading})
	{
		FWoTHUDLayoutOptions Options;
		Options.Layout = Layout;
		Options.Size = FIntPoint(1280, 1280);
		const FWoTHUDLayoutResult Arranged = FWoTHUDLayout::Arrange(Presented, Options);
		TestFalse(TEXT("an arranged layout has sections"), Arranged.Sections.IsEmpty());
		TestTrue(TEXT("an arranged layout fits its destination"),
			Arranged.Extent.X <= Options.Size.X && Arranged.Extent.Y <= Options.Size.Y);
		TestFalse(TEXT("the destination draws on its own canvas"),
			DrawSectionsOnce(S.F.World, Arranged.Sections, Options.Size.X, Options.Size.Y).IsEmpty());
		for (const FWoTHUDSection& Section : Arranged.Sections)
		{
			TestTrue(TEXT("no reticle in a panel"), Section.Section != EWoTHUDSection::Reticle);
		}
	}
	FWoTHUDLayoutOptions Flat;
	TestEqual(TEXT("the flat layout is the flat HUD's own"),
		FWoTHUDLayout::Arrange(Presented, Flat).Sections.Num(), Presented.Sections.Num());

	// Mid-match, while dead and back: detaching restores the flat HUD, which
	// draws exactly its snapshot.
	const float LifeOwned = S.HUD->LeftMessages[0].LifeSpan;
	FWoTLocalPlayerHooks::Detach(*S.F.Player, TEXT("Destination"));
	const TArray<FString> Restored = S.DrawFrame();
	TestFalse(TEXT("detaching restores the flat HUD"), Restored.IsEmpty());
	TestEqual(TEXT("the restored flat HUD draws its snapshot"),
		DrawSectionsOnce(S.F.World, S.HUD->GetSnapshot().Sections, 640, 480), Restored);
	TestTrue(TEXT("message time kept running while the destination owned the HUD"),
		LifeOwned < LifeBefore);
	S.F.PC->UnPossess();
	TestTrue(TEXT("the destination attaches while the player has no body"),
		FWoTLocalPlayerHooks::Attach(*S.F.Player, TEXT("Destination"), Destination));
	TestTrue(TEXT("no body, a destination: nothing flat"), S.DrawFrame().IsEmpty());
	FWoTLocalPlayerHooks::Detach(*S.F.Player, TEXT("Destination"));
	TestFalse(TEXT("no body, detached: the flat HUD (crosshair, messages) draws"), S.DrawFrame().IsEmpty());
	FWoTLocalPlayerHooks::Detach(*S.F.Player, TEXT("Observer"));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
