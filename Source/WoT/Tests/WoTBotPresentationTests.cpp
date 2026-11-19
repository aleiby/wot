#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
#include "WoTTestWorld.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTScoreboard.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "../WoTFrontEnd.h"
#include "../WoTFrontEndViewModel.h"
#include "../WoTSessionSubsystem.h"
#include "../WoTGameInstance.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemUtils.h"
#include "Engine/Engine.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardBotTagSpec,
	"WoT.HUD.Scoreboard.BotTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardBotTagSpec::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	if (!TestNotNull(TEXT("world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerState* State = Controller ? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("HUD"), HUD) || !TestNotNull(TEXT("state"), State)) return false;
	State->SetPlayerName(TEXT("Participant"));
	auto Draw = [&]() { return FWoTArenaScoreboard().Layout(
		HUD->GatherArenaScoreboard(), 640, 480, 16.0f, false, false); };
	auto HasTag = [](const TArray<FWoTHUDDrawCmd>& Commands)
	{
		return Commands.ContainsByPredicate([](const FWoTHUDDrawCmd& Command)
		{ return Command.Text == TEXT("bot"); });
	};
	const auto Human = Draw();
	TestFalse(TEXT("human row has no bot tag"), HasTag(Human));
	State->SetIsABot(true);
	const auto Bot = Draw();
	TestTrue(TEXT("bot state produces a bot draw command"), HasTag(Bot));
	TestEqual(TEXT("tag adds one command"), Bot.Num(), Human.Num() + 1);
	State->SetIsABot(false);
	const auto HumanAgain = Draw();
	TestEqual(TEXT("human command count remains unchanged"), HumanAgain.Num(), Human.Num());
	for (int32 Index = 0; Index < Human.Num() && Index < HumanAgain.Num(); ++Index)
	{
		TestTrue(TEXT("human draw commands remain unchanged"),
			Human[Index].Text == HumanAgain[Index].Text && Human[Index].X == HumanAgain[Index].X
			&& Human[Index].Y == HumanAgain[Index].Y && Human[Index].Scale == HumanAgain[Index].Scale
			&& Human[Index].TexId == HumanAgain[Index].TexId && Human[Index].FontId == HumanAgain[Index].FontId
			&& Human[Index].Tint == HumanAgain[Index].Tint && Human[Index].Style == HumanAgain[Index].Style
			&& Human[Index].TextAlignment == HumanAgain[Index].TextAlignment);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndBotHostOptionsSpec,
	"WoT.FrontEnd.BotHostOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFrontEndBotHostOptionsSpec::RunTest(const FString&)
{
	const FName Owner(TEXT("BotHostSpec"));
	TArray<FWoTHostOptionRow> SavedRows = FWoTFrontEndModel::HostOptions();
	for (const FWoTHostOptionRow& Saved : SavedRows)
	{
		FWoTFrontEndModel::UnregisterHostOptions(Saved.Owner);
	}
	ON_SCOPE_EXIT
	{
		FWoTFrontEndModel::UnregisterHostOptions(Owner);
		for (FWoTHostOptionRow& Saved : SavedRows)
		{
			FWoTFrontEndModel::RegisterHostOption(MoveTemp(Saved));
		}
	};
	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(TEXT("/Game/WoT/Maps/Arena_01"));
	const FString Baseline = FWoTFrontEndModel::BuildHostTravelURL(Request, EWoTPlayerFaction::Hound);
	Request.ExtraValues.Add(TEXT("BotFill"), 9);
	TestEqual(TEXT("unregistered values cannot affect human host URL"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, EWoTPlayerFaction::Hound), Baseline);
	FWoTHostOptionRow Row;
	Row.Owner = Owner;
	Row.URLKey = TEXT("BotFill");
	Row.Label = []() { return FText::FromString(TEXT("Fill with bots to")); };
	Row.Min = 0;
	Row.Max = [](const FWoTHostRequest& Current) { return Current.MaxPlayers; };
	Row.DefaultValue = 0;
	TestTrue(TEXT("plugin can register a row"), FWoTFrontEndModel::RegisterHostOption(MoveTemp(Row)));
	TestTrue(TEXT("URL value clamps to current human capacity"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, EWoTPlayerFaction::Hound).Contains(TEXT("?BotFill=6")));
	Request.ExtraValues.Reset();
	TestTrue(TEXT("registered default travels"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, EWoTPlayerFaction::Hound).Contains(TEXT("?BotFill=0")));
	FWoTFrontEndViewModel View;
	View.SetMaxPlayers(12);
	View.SetHostOption(TEXT("BotFill"), 10);
	View.SetMaxPlayers(4);
	TestEqual(TEXT("lowering Max Players reclamps stored plugin values"),
		View.GetHostRequest().ExtraValues.FindRef(TEXT("BotFill")), 4);
	View.SetHostOption(TEXT("BotFill"), -10);
	TestEqual(TEXT("minimum bounds apply"), View.GetHostRequest().ExtraValues.FindRef(TEXT("BotFill")), 0);
	FWoTFrontEndModel::UnregisterHostOptions(Owner);
	TestEqual(TEXT("unregister restores original URL"),
		FWoTFrontEndModel::BuildHostTravelURL(Request, EWoTPlayerFaction::Hound), Baseline);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndBotBrowserCountSpec,
	"WoT.FrontEnd.BotBrowserCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFrontEndBotBrowserCountSpec::RunTest(const FString&)
{
	UGameInstance* Instance = NewObject<UGameInstance>();
	UWoTSessionSubsystem* Sessions = NewObject<UWoTSessionSubsystem>(Instance);
	FOnlineSessionSearchResult Raw;
	Raw.Session.SessionSettings.NumPublicConnections = 16;
	Raw.Session.NumOpenPublicConnections = 13;
	auto Read = [&]() { return Sessions->InjectSearchResultForTesting(nullptr, false, Raw); };
	TestEqual(TEXT("old hosts keep the human count"), Read().PlayerCountText(), FString(TEXT("3/16")));
	Raw.Session.SessionSettings.Set(FName(TEXT("WOTBOTS")), 3,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	const FWoTSessionListEntry WithBots = Read();
	TestEqual(TEXT("bot suffix is separate"), WithBots.PlayerCountText(), FString(TEXT("3/16 + 3 bots")));
	TestEqual(TEXT("human count unchanged"), WithBots.CurrentPlayers, 3);
	Raw.Session.SessionSettings.Set(FName(TEXT("WOTBOTS")), 1,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	TestEqual(TEXT("one bot is singular"), Read().PlayerCountText(), FString(TEXT("3/16 + 1 bot")));
	Raw.Session.SessionSettings.Set(FName(TEXT("WOTBOTS")), -2,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	TestEqual(TEXT("negative count has no suffix"), Read().PlayerCountText(), FString(TEXT("3/16")));
	Raw.Session.SessionSettings.Set(FName(TEXT("WOTBOTS")), 0,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	TestEqual(TEXT("zero count has no suffix"), Read().PlayerCountText(), FString(TEXT("3/16")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndBotAdvertisedIntsSpec,
	"WoT.FrontEnd.BotAdvertisedInts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTFrontEndBotAdvertisedIntsSpec::RunTest(const FString&)
{
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	// Exercise the advertisement seam before optional world services publish.
	Options.bBeginPlay = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (!TestNotNull(TEXT("world"), World)) return false;
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	UGameInstance* Instance = World->GetGameInstance();
	UWoTSessionSubsystem* Sessions = Instance->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("session subsystem"), Sessions)) return false;
	const IOnlineSubsystem* OSS = Online::GetSubsystem(World, FName(TEXT("NULL")));
	if (!TestNotNull(TEXT("Null provider"), OSS)) return false;
	const IOnlineSessionPtr Provider = OSS->GetSessionInterface();
	if (!TestTrue(TEXT("session provider"), Provider.IsValid())) return false;
	int32 Updates = 0;
	FDelegateHandle Handle = Provider->AddOnUpdateSessionCompleteDelegate_Handle(
		FOnUpdateSessionCompleteDelegate::CreateLambda([&Updates](FName Name, bool bOk)
		{ if (Name == NAME_GameSession && bOk) ++Updates; }));
	ON_SCOPE_EXIT
	{
		Provider->ClearOnUpdateSessionCompleteDelegate_Handle(Handle);
		if (Provider->GetNamedSession(NAME_GameSession))
		{
			Provider->DestroySession(NAME_GameSession);
		}
	};
	FWoTHostRequest Request;
	Request.MapPackage = TEXT("/Game/WoT/Maps/Arena_01");
	Request.MapTitle = TEXT("Arena");
	int32 Count = -1;
	TestFalse(TEXT("no plugin means no advertised bot key"),
		Sessions->BuildSessionSettingsForTesting(Request).Get(FName(TEXT("WOTBOTS")), Count));
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 3);
	TestEqual(TEXT("no named session means no update"), Updates, 0);
	for (bool bLAN : {true, false})
	{
		Request.bLAN = bLAN;
		const FOnlineSessionSettings Settings = Sessions->BuildSessionSettingsForTesting(Request);
		TestTrue(TEXT("both providers include the integer"), Settings.Get(FName(TEXT("WOTBOTS")), Count));
		TestEqual(TEXT("bot value"), Count, 3);
		const FOnlineSessionSetting* Setting = Settings.Settings.Find(FName(TEXT("WOTBOTS")));
		TestTrue(TEXT("available to service and LAN ping"), Setting
			&& Setting->AdvertisementType == EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	}
	Request.bLAN = true;
	Sessions->ActivateForAuthorizedWorld();
	FOnlineSessionSettings LiveSettings = Sessions->BuildSessionSettingsForTesting(Request);
	LiveSettings.bShouldAdvertise = false;
	if (!TestTrue(TEXT("public provider lifecycle creates session"),
		Provider->CreateSession(0, NAME_GameSession, LiveSettings))
		|| !TestNotNull(TEXT("named session is ready"), Provider->GetNamedSession(NAME_GameSession))) return false;
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 3);
	TestEqual(TEXT("unchanged value causes no update"), Updates, 0);
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 4);
	TestEqual(TEXT("changed value updates existing provider"), Updates, 1);
	TestTrue(TEXT("live setting updated"), Provider->GetNamedSession(NAME_GameSession)
		->SessionSettings.Get(FName(TEXT("WOTBOTS")), Count));
	TestEqual(TEXT("live count"), Count, 4);
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 0);
	TestEqual(TEXT("final zero is published"), Updates, 2);
	Sessions->DeactivateForUnauthorizedWorld();
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 2);
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 2);
	TestEqual(TEXT("readiness loss defers publication"), Updates, 2);
	TestTrue(TEXT("world authority reactivates"), Sessions->ActivateForAuthorizedWorld());
	TestEqual(TEXT("reactivation publishes deferred value"), Updates, 3);
	Provider->GetNamedSession(NAME_GameSession)->SessionSettings.Get(FName(TEXT("WOTBOTS")), Count);
	TestEqual(TEXT("deferred live value"), Count, 2);
	TArray<TFunction<bool()>> DeferredUpdates;
	Sessions->SetUpdateSessionDispatchHookForTesting([&DeferredUpdates](TFunction<bool()>&& Dispatch)
	{
		DeferredUpdates.Add(MoveTemp(Dispatch));
		return true;
	});
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 7);
	Sessions->DeactivateForUnauthorizedWorld();
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 8);
	TestTrue(TEXT("authority reactivates while prior provider update is outstanding"),
		Sessions->ActivateForAuthorizedWorld());
	TestEqual(TEXT("reactivation waits for the outstanding provider completion"), DeferredUpdates.Num(), 1);
	Provider->OnUpdateSessionCompleteDelegates.Broadcast(FName(TEXT("UnrelatedSession")), true);
	TestEqual(TEXT("unrelated session completion cannot flush pending generation"), DeferredUpdates.Num(), 1);
	if (DeferredUpdates.Num() != 1) return false;
	TFunction<bool()> FirstUpdate = MoveTemp(DeferredUpdates[0]);
	TestTrue(TEXT("first deferred provider update completes"), FirstUpdate());
	TestEqual(TEXT("completion flushes newest pending generation"), DeferredUpdates.Num(), 2);
	Provider->GetNamedSession(NAME_GameSession)->SessionSettings.Get(FName(TEXT("WOTBOTS")), Count);
	TestEqual(TEXT("first completion contains its own snapshot"), Count, 7);
	if (DeferredUpdates.Num() != 2) return false;
	Provider->OnUpdateSessionCompleteDelegates.Broadcast(NAME_GameSession, false);
	TestEqual(TEXT("failed newest generation is not immediately retried"), DeferredUpdates.Num(), 2);
	Sessions->ActivateForAuthorizedWorld();
	TestEqual(TEXT("reactivation retries failed newest generation"), DeferredUpdates.Num(), 3);
	if (DeferredUpdates.Num() != 3) return false;
	TFunction<bool()> LatestUpdate = MoveTemp(DeferredUpdates[2]);
	TestTrue(TEXT("retried newest provider update completes"), LatestUpdate());
	Provider->GetNamedSession(NAME_GameSession)->SessionSettings.Get(FName(TEXT("WOTBOTS")), Count);
	TestEqual(TEXT("newest generation reaches live settings"), Count, 8);
	Sessions->SetUpdateSessionDispatchHookForTesting({});
	const int32 SuccessfulUpdates = Updates;
	Sessions->DeactivateForUnauthorizedWorld();
	Sessions->ActivateForAuthorizedWorld();
	TestEqual(TEXT("successful current generation stays clean on reactivation"), Updates, SuccessfulUpdates);
	int32 RejectedUpdates = 0;
	Sessions->SetUpdateSessionDispatchHookForTesting([&RejectedUpdates](TFunction<bool()>&&)
	{
		++RejectedUpdates;
		return false;
	});
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 9);
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 9);
	TestEqual(TEXT("unchanged setter never retries a failed generation"), RejectedUpdates, 1);
	Sessions->SetUpdateSessionDispatchHookForTesting({});
	Sessions->DeactivateForUnauthorizedWorld();
	Sessions->ActivateForAuthorizedWorld();
	Provider->GetNamedSession(NAME_GameSession)->SessionSettings.Get(FName(TEXT("WOTBOTS")), Count);
	TestEqual(TEXT("reactivation recovers failed publication"), Count, 9);
	TFunction<bool()> FailedCompletionDispatch;
	int32 FailedCompletionAttempts = 0;
	Sessions->SetUpdateSessionDispatchHookForTesting(
		[&FailedCompletionDispatch, &FailedCompletionAttempts](TFunction<bool()>&& Dispatch)
		{
			++FailedCompletionAttempts;
			FailedCompletionDispatch = MoveTemp(Dispatch);
			return true;
		});
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 10);
	Provider->OnUpdateSessionCompleteDelegates.Broadcast(NAME_GameSession, false);
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 10);
	TestEqual(TEXT("failed completion has no immediate or unchanged-value retry"), FailedCompletionAttempts, 1);
	Sessions->SetUpdateSessionDispatchHookForTesting({});
	Sessions->DeactivateForUnauthorizedWorld();
	Sessions->ActivateForAuthorizedWorld();
	Provider->GetNamedSession(NAME_GameSession)->SessionSettings.Get(FName(TEXT("WOTBOTS")), Count);
	TestEqual(TEXT("reactivation recovers failed async completion"), Count, 10);
	Sessions->SetAdvertisedInt(FName(TEXT("WOTBOTS")), 0);
	FWorldContext* Context = GEngine->GetWorldContextFromWorld(World);
	UWorld* Replacement = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("replacement world"), Replacement) || !TestNotNull(TEXT("context"), Context)) return false;
	Replacement->SetGameInstance(Instance);
	Context->SetCurrentWorld(Replacement);
	TestTrue(TEXT("subsystem now reads replacement world"), Sessions->GetWorld() == Replacement);
	TestTrue(TEXT("advertised value survives world change"),
		Sessions->BuildSessionSettingsForTesting(Request).Get(FName(TEXT("WOTBOTS")), Count));
	TestEqual(TEXT("latest value survived"), Count, 0);
	Context->SetCurrentWorld(World);
	Replacement->DestroyWorld(false);
	return !HasAnyErrors();
}

#endif
