// Headless gates for the pure Arena scoreboard and its thin HUD/input adapters.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTInventory.h"
#include "../WoTHUD.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTCitadelTeamOverview.h"
#include "../WoTGameMode.h"
#include "../WoTGameState.h"
#include "../WoTGameInstance.h"
#include "../WoTFactionCharacters.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTScoreboard.h"
#include "../WoTHUDTextures.h"
#include "../World/WoTMapDisplay.h"
#include "../World/WoTMaterializedWorldHost.h"
#include "../World/WoTWorldMaterializer.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTPayloadSubsystem.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "WoTTestBattleClasses.h"
#include "WoTWorldMaterializerTestData.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/WorldSettings.h"
#if WITH_EDITOR
#include "Tests/NetTestHelpers.h"
#endif
#include "InputCoreTypes.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
	UWorld* ScoreboardTests_CreateWorld(bool bBattleMode = false)
	{
		WoTTest::FSpecWorldOptions Options;
		if (bBattleMode)
		{
			Options.GameModeClass = AWoTBattleGameMode::StaticClass();
		}
		Options.BeforeInitializeActorsForPlay = [bBattleMode](UWorld* World)
		{
			if (bBattleMode)
			{
				AWoTBattleInfo* Battle = World->SpawnActorDeferred<AWoTBattleInfo>(
					AWoTBattleInfo::StaticClass(), FTransform::Identity);
				Battle->NumTeams = 2;
				Battle->TeamClass[0] = AWoTTestTeamAlpha::StaticClass();
				Battle->TeamClass[1] = AWoTTestTeamBravo::StaticClass();
				Battle->MaxTeamSize = 3;
				Battle->bTeleportersDisabled = true;
				Battle->FinishSpawning(FTransform::Identity);
			}
		};
		return WoTTest::CreateSpecWorld(Options);
	}

	FActorSpawnParameters ScoreboardTests_AlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	const FWoTHUDDrawCmd* ScoreboardTests_FindText(
		const TArray<FWoTHUDDrawCmd>& Commands, const FString& Text)
	{
		return Commands.FindByPredicate([&Text](const FWoTHUDDrawCmd& Command)
		{
			return Command.Text == Text;
		});
	}

	FWoTScoreboardSnapshot ScoreboardTests_OnePlayerSnapshot()
	{
		FWoTScoreboardSnapshot Snapshot;
		Snapshot.MapName = TEXT("Mashadar");
		Snapshot.ElapsedTime = 3661;
		FWoTScoreboardPlayer& Player = Snapshot.Players.AddDefaulted_GetRef();
		Player.Name = TEXT("Rand");
		Player.Ping = 42;
		Player.Kills = 7;
		Player.Deaths = 3;
		Player.Suicides = 1;
		Player.Score = 6;
		Player.TimeOnServer = 3661;
		Player.bHighlight = true;
		return Snapshot;
	}

	AWoTBattleInfo* ScoreboardTests_FindBattle(UWorld* World)
	{
		for (TActorIterator<AWoTBattleInfo> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				return *It;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardJustificationSpec,
	"WoT.HUD.Scoreboard.Justification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardJustificationSpec::RunTest(const FString&)
{
	const FWoTScoreboardDisplayInfo Columns[] = {
		{ EWoTScoreboardJustification::LeftThird, 0, true },
		{ EWoTScoreboardJustification::LeftQuarter, -72, true },
		{ EWoTScoreboardJustification::LeftQuarter, -48, true },
		{ EWoTScoreboardJustification::Center, -48, true },
		{ EWoTScoreboardJustification::RightThird, -56, true },
		{ EWoTScoreboardJustification::RightQuarter, 32, true }
	};
	const int32 Expected640[] = { 213, 88, 112, 272, 370, 512 };
	const int32 Expected801[] = { 267, 128, 152, 352, 478, 632 };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Columns); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("640 column %d"), Index),
			FWoTArenaScoreboard::JustifiedX(Columns[Index], 640), Expected640[Index]);
		TestEqual(*FString::Printf(TEXT("odd column %d"), Index),
			FWoTArenaScoreboard::JustifiedX(Columns[Index], 801), Expected801[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardCoordinatesSpec,
	"WoT.HUD.Scoreboard.Coordinates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardCoordinatesSpec::RunTest(const FString&)
{
	FWoTScoreboardSnapshot Snapshot;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		FWoTScoreboardPlayer& Player = Snapshot.Players.AddDefaulted_GetRef();
		Player.Name = FString::Printf(TEXT("P%02d"), Index);
		Player.TimeOnServer = Index * 60;
	}
	FWoTArenaScoreboard Scoreboard;
	Scoreboard.SortOrder.Reset();
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, true);
	const FWoTHUDDrawCmd* Legend = ScoreboardTests_FindText(Commands, TEXT("Time"));
	const FWoTHUDDrawCmd* First = ScoreboardTests_FindText(Commands, TEXT("P00"));
	const FWoTHUDDrawCmd* Last = ScoreboardTests_FindText(Commands, TEXT("P15"));
	TestTrue(TEXT("legend at shipped anchor"), Legend &&
		Legend->X == 213.0f && Legend->Y == 120.0f);
	TestTrue(TEXT("first row starts one slot below legend"), First &&
		First->X == 112.0f && First->Y == 140.0f);
	TestTrue(TEXT("sixteenth row ends at y=440"), Last && Last->Y == 440.0f);
	TestNotNull(TEXT("row time uses HH:MM"),
		ScoreboardTests_FindText(Commands, TEXT("00:15")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardIconLayersSpec,
	"WoT.HUD.Scoreboard.IconLayers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardIconLayersSpec::RunTest(const FString&)
{
	FWoTScoreboardSnapshot Snapshot = ScoreboardTests_OnePlayerSnapshot();
	Snapshot.Players.AddDefaulted_GetRef().Name = TEXT("Egwene");
	FWoTArenaScoreboard Scoreboard;
	Scoreboard.SortOrder.Reset();
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, true);
	const TArray<FWoTHUDDrawCmd> Icons = Commands.FilterByPredicate(
		[](const FWoTHUDDrawCmd& Command)
		{
			return !Command.TexId.IsNone();
		});

	TestEqual(TEXT("two texture layers are emitted per row"), Icons.Num(), 4);
	for (int32 Row = 0; Row < 2 && Icons.Num() >= (Row + 1) * 2; ++Row)
	{
		const FWoTHUDDrawCmd& Base = Icons[Row * 2];
		const FWoTHUDDrawCmd& Head = Icons[Row * 2 + 1];
		TestEqual(*FString::Printf(TEXT("row %d base is drawn first"), Row),
			Base.TexId, FName(TEXT("HeadsBase")));
		TestEqual(*FString::Printf(TEXT("row %d defaults to Aes Sedai"), Row),
			Head.TexId, FName(TEXT("HeadsAesSedai")));
		TestTrue(*FString::Printf(TEXT("row %d base is opaque"), Row),
			Base.Style == EWoTHUDStyle::Normal);
		TestTrue(*FString::Printf(TEXT("row %d head is masked"), Row),
			Head.Style == EWoTHUDStyle::Masked);
		TestTrue(*FString::Printf(TEXT("row %d layers share icon position"), Row),
			Base.X == 88.0f && Head.X == Base.X && Head.Y == Base.Y);
		TestTrue(*FString::Printf(TEXT("row %d layers draw at native size"), Row),
			Base.Scale == 1.0f && Head.Scale == 1.0f);
		// FLIPPED 2026-07-13 (previously asserted "icons are not row-tinted", enshrining
		// the divergence): ArenaScoreBoard.uc:145-159 sets Canvas.DrawColor per row
		// (255 highlight / DisplayBrightness=120 otherwise) BEFORE DrawPlayerElements,
		// and DrawIcon draws through it — retail dims the non-highlighted row's
		// swatch + head like the row's text. Row 0 = Rand (bHighlight), row 1 = Egwene.
		const FColor ExpectedTint =
			Row == 0 ? FColor::White : FColor(120, 120, 120);
		TestTrue(*FString::Printf(TEXT("row %d icons carry the row tint"), Row),
			Base.Tint.ToFColor(true) == ExpectedTint &&
			Head.Tint.ToFColor(true) == ExpectedTint);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardIconMeasuredYSpec,
	"WoT.HUD.Scoreboard.IconMeasuredY",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardIconMeasuredYSpec::RunTest(const FString&)
{
	const FWoTScoreboardSnapshot Snapshot = ScoreboardTests_OnePlayerSnapshot();
	const FWoTArenaScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 13.5f, false, true);
	const FWoTHUDDrawCmd* Base = Commands.FindByPredicate(
		[](const FWoTHUDDrawCmd& Command)
		{
			return Command.TexId == FName(TEXT("HeadsBase"));
		});
	const FWoTHUDDrawCmd* Head = Commands.FindByPredicate(
		[](const FWoTHUDDrawCmd& Command)
		{
			return Command.TexId == FName(TEXT("HeadsAesSedai"));
		});
	const float ExpectedY = 140.0f - (16.0f - 13.5f);
	TestTrue(TEXT("base icon Y uses the measured font height"),
		Base && FMath::IsNearlyEqual(Base->Y, ExpectedY));
	TestTrue(TEXT("head icon Y uses the measured font height"),
		Head && FMath::IsNearlyEqual(Head->Y, ExpectedY));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardIconDegradeBlankSpec,
	"WoT.HUD.Scoreboard.IconDegradeBlank",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardIconDegradeBlankSpec::RunTest(const FString&)
{
	const FWoTScoreboardSnapshot Snapshot = ScoreboardTests_OnePlayerSnapshot();
	const FWoTArenaScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, false);
	TestFalse(TEXT("unavailable icons leave the icon cell blank"),
		Commands.ContainsByPredicate([](const FWoTHUDDrawCmd& Command)
		{
			return !Command.TexId.IsNone() || !Command.Texture.IsNull();
		}));
	TestNotNull(TEXT("the text table still renders when icons are unavailable"),
		ScoreboardTests_FindText(Commands, TEXT("Rand")));
	TestNotNull(TEXT("the row score still renders when icons are unavailable"),
		ScoreboardTests_FindText(Commands, TEXT("6")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardIconRegistrySpec,
	"WoT.HUD.Scoreboard.IconRegistry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardIconRegistrySpec::RunTest(const FString&)
{
	const FName IconIds[] = {
		TEXT("HeadsBase"), TEXT("HeadsBlack"), TEXT("HeadsBlue"),
		TEXT("HeadsRed"), TEXT("HeadsGreen"), TEXT("HeadsAesSedai"),
		TEXT("HeadsForsaken"), TEXT("HeadsHound"), TEXT("HeadsWC")
	};
	const TArray<FName> RequiredIds = FWoTArenaScoreboard::AllIconTextureIds();
	TestEqual(TEXT("HUD adapter requires all nine scoreboard textures"),
		RequiredIds.Num(), static_cast<int32>(UE_ARRAY_COUNT(IconIds)));
	for (int32 Index = 0;
		Index < RequiredIds.Num() && Index < UE_ARRAY_COUNT(IconIds); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("required texture %d"), Index),
			RequiredIds[Index], IconIds[Index]);
	}
	UWoTHUDTextures* Registry = LoadObject<UWoTHUDTextures>(nullptr,
		TEXT("/Game/WoT/UI/DA_WoTHUDTextures.DA_WoTHUDTextures"));
	if (!TestNotNull(TEXT("HUD texture registry is imported"), Registry))
	{
		return false;
	}
	for (const FName IconId : IconIds)
	{
		UTexture2D* Texture = Registry->Resolve(IconId);
		TestNotNull(*FString::Printf(TEXT("%s resolves"), *IconId.ToString()), Texture);
		if (Texture)
		{
			// GetImportedSize() reads the import metadata (build-independent); GetSizeX()
			// reads platform data, which is 0 for a freshly-imported texture under -nullrhi.
			const FIntPoint Size = Texture->GetImportedSize();
			TestEqual(*FString::Printf(TEXT("%s width"), *IconId.ToString()), Size.X, 16);
			TestEqual(*FString::Printf(TEXT("%s height"), *IconId.ToString()), Size.Y, 16);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardHeaderSpec,
	"WoT.HUD.Scoreboard.Header",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardHeaderSpec::RunTest(const FString&)
{
	FWoTScoreboardSnapshot Snapshot;
	Snapshot.MapName = TEXT("Shadar Logoth");
	Snapshot.bDisplayRemaining = true;
	Snapshot.RemainingTime = 3661;
	Snapshot.VictoryMessage = TEXT("Victory");
	const FWoTArenaScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, false);
	const FWoTHUDDrawCmd* Map =
		ScoreboardTests_FindText(Commands, TEXT("Shadar Logoth"));
	const FWoTHUDDrawCmd* Clock =
		ScoreboardTests_FindText(Commands, TEXT("Remaining Time: 01:01:01"));
	const FWoTHUDDrawCmd* Victory =
		ScoreboardTests_FindText(Commands, TEXT("Victory"));
	TestTrue(TEXT("map is first centered header line"), Map &&
		Map->X == 320.0f && Map->Y == 64.0f &&
		Map->TextAlignment == EWoTTextAlignment::Center);
	TestTrue(TEXT("header clock is zero-padded HH:MM:SS"), Clock && Clock->Y == 80.0f);
	TestTrue(TEXT("victory message is rendered when supplied"), Victory && Victory->Y == 96.0f);

	const TArray<FWoTHUDDrawCmd> Clipped =
		Scoreboard.Layout(Snapshot, 640, 480, 20.0f, false, false);
	TestNotNull(TEXT("map fits before strict header limit"),
		ScoreboardTests_FindText(Clipped, TEXT("Shadar Logoth")));
	TestNull(TEXT("clock clips at Y == limit"),
		ScoreboardTests_FindText(Clipped, TEXT("Remaining Time: 01:01:01")));
	TestEqual(TEXT("standalone formatter HH:MM:SS"),
		FWoTArenaScoreboard::FormatTime(59), FString(TEXT("00:00:59")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardStandaloneSpec,
	"WoT.HUD.Scoreboard.Standalone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardStandaloneSpec::RunTest(const FString&)
{
	const FWoTScoreboardSnapshot Snapshot = ScoreboardTests_OnePlayerSnapshot();
	const FWoTArenaScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Networked =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, false);
	TestNotNull(TEXT("elapsed header uses HH:MM:SS"),
		ScoreboardTests_FindText(Networked, TEXT("Elapsed Time: 01:01:01")));
	TestNotNull(TEXT("unknown packet loss degrades to dashes"),
		ScoreboardTests_FindText(Networked, TEXT("(42/--)")));
	const TArray<FWoTHUDDrawCmd> Standalone =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, true, false);
	TestNull(TEXT("standalone omits player net stats"),
		ScoreboardTests_FindText(Standalone, TEXT("(42/--)")));
	TestNotNull(TEXT("standalone retains Ping/PL legend"),
		ScoreboardTests_FindText(Standalone, TEXT("Ping/PL")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardScoreAndColorSpec,
	"WoT.HUD.Scoreboard.ScoreAndColor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardScoreAndColorSpec::RunTest(const FString&)
{
	FWoTScoreboardSnapshot Snapshot = ScoreboardTests_OnePlayerSnapshot();
	FWoTScoreboardPlayer& Remote = Snapshot.Players.AddDefaulted_GetRef();
	Remote.Name = TEXT("Ishamael");
	Remote.Score = 99;
	const FWoTArenaScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, false);
	const FWoTHUDDrawCmd* LocalName = ScoreboardTests_FindText(Commands, TEXT("Rand"));
	const FWoTHUDDrawCmd* RemoteName =
		ScoreboardTests_FindText(Commands, TEXT("Ishamael"));
	const FWoTHUDDrawCmd* Score = ScoreboardTests_FindText(Commands, TEXT("99"));
	TestTrue(TEXT("local row is white"), LocalName && LocalName->Tint == FLinearColor::White);
	TestTrue(TEXT("remote row is RGB 120"), RemoteName &&
		RemoteName->Tint.ToFColor(true) == FColor(120, 120, 120));
	TestTrue(TEXT("score uses anchor+24 with right alignment"), Score &&
		Score->X == 536.0f && Score->TextAlignment == EWoTTextAlignment::Right);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardPlayerCapSpec,
	"WoT.HUD.Scoreboard.PlayerCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardPlayerCapSpec::RunTest(const FString&)
{
	FWoTScoreboardSnapshot Snapshot;
	for (int32 Index = 0; Index < 17; ++Index)
	{
		Snapshot.Players.AddDefaulted_GetRef().Name =
			FString::Printf(TEXT("Cap%02d"), Index);
	}
	FWoTArenaScoreboard Scoreboard;
	Scoreboard.SortOrder.Reset();
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, false);
	TestNotNull(TEXT("sixteenth player retained"),
		ScoreboardTests_FindText(Commands, TEXT("Cap15")));
	TestNull(TEXT("seventeenth player omitted"),
		ScoreboardTests_FindText(Commands, TEXT("Cap16")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardDefaultSortSpec,
	"WoT.HUD.Scoreboard.DefaultSort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardDefaultSortSpec::RunTest(const FString&)
{
	FWoTArenaScoreboard Scoreboard;
	auto Add = [&Scoreboard](const TCHAR* Name, int32 Score, int32 Time,
		int32 Loss, int32 Ping)
	{
		FWoTScoreboardPlayer& Player = Scoreboard.Players.AddDefaulted_GetRef();
		Player.Name = Name;
		Player.Score = Score;
		Player.TimeOnServer = Time;
		Player.PacketLoss = Loss;
		Player.Ping = Ping;
	};
	Add(TEXT("OriginalA"), 10, 70, 5, 9);
	Add(TEXT("OriginalB"), 10, 70, 5, 9);
	Add(TEXT("PingOnly"), 10, 70, 5, 10);
	Add(TEXT("Loss"), 10, 80, 6, 0);
	Add(TEXT("Early"), 10, 10, 0, 0);
	Add(TEXT("Winner"), 20, 999, 0, 0);
	Scoreboard.Sort();
	const TCHAR* Expected[] = {
		TEXT("Winner"), TEXT("Early"), TEXT("Loss"), TEXT("PingOnly"),
		TEXT("OriginalA"), TEXT("OriginalB")
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Expected); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("effective priority row %d"), Index),
			Scoreboard.Players[Index].Name, FString(Expected[Index]));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardComparatorsSpec,
	"WoT.HUD.Scoreboard.Comparators",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardComparatorsSpec::RunTest(const FString&)
{
	FWoTArenaScoreboard Scoreboard;
	FWoTScoreboardPlayer& High = Scoreboard.Players.AddDefaulted_GetRef();
	High.Name = TEXT("B");
	High.Ping = High.PacketLoss = High.Kills = High.Deaths = High.Suicides =
		High.Score = 2;
	High.TimeOnServer = 120;
	High.Color = High.PlayerType = 2;
	FWoTScoreboardPlayer& Low = Scoreboard.Players.AddDefaulted_GetRef();
	Low.Name = TEXT("A");
	Low.Ping = Low.PacketLoss = Low.Kills = Low.Deaths = Low.Suicides =
		Low.Score = 1;
	Low.TimeOnServer = 60;
	Low.Color = Low.PlayerType = 1;
	for (int32 Field = 1; Field <= 10; ++Field)
	{
		TestTrue(*FString::Printf(TEXT("positive field %d"), Field),
			Scoreboard.IsGreater(0, 1, Field));
		TestTrue(*FString::Printf(TEXT("negative field %d"), Field),
			Scoreboard.IsGreater(1, 0, -Field));
	}
	TestFalse(TEXT("same index is never greater"), Scoreboard.IsGreater(0, 0, 7));
	Scoreboard.Players[0].Score = Scoreboard.Players[1].Score;
	TestFalse(TEXT("equal values are never greater"), Scoreboard.IsGreater(0, 1, 7));
	TestFalse(TEXT("unknown sort type degrades safely"),
		Scoreboard.HandleSortTypeException(0, 1, 99));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardSortHistorySpec,
	"WoT.HUD.Scoreboard.SortHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardSortHistorySpec::RunTest(const FString&)
{
	FWoTArenaScoreboard Scoreboard;
	TestEqual(TEXT("default history has ten physical slots"),
		Scoreboard.SortOrder.Num(), 10);
	TestTrue(TEXT("default oldest-to-newest filters are Ping, PL, -Time, Score"),
		Scoreboard.SortOrder[0] == 2 && Scoreboard.SortOrder[1] == 3 &&
		Scoreboard.SortOrder[2] == -8 && Scoreboard.SortOrder[3] == 7 &&
		Scoreboard.SortOrder[4] == 0);
	Scoreboard.SortBy(4);
	TestTrue(TEXT("open slot appends without evicting the oldest filter"),
		Scoreboard.SortOrder[0] == 2 && Scoreboard.SortOrder[4] == 4);
	Scoreboard.SortOrder = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	Scoreboard.SortBy(-5);
	TestEqual(TEXT("duplicate field keeps ten slots"), Scoreboard.SortOrder.Num(), 10);
	TestEqual(TEXT("duplicate is removed then appended with new direction"),
		Scoreboard.SortOrder.Last(), -5);
	TestEqual(TEXT("only one absolute field 5 remains"),
		Scoreboard.SortOrder.FilterByPredicate([](int32 Value)
		{
			return FMath::Abs(Value) == 5;
		}).Num(), 1);
	Scoreboard.SortBy(11);
	TestEqual(TEXT("full history evicts oldest"), Scoreboard.SortOrder[0], 2);
	TestEqual(TEXT("new field occupies newest slot"), Scoreboard.SortOrder.Last(), 11);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardGatherSpec,
	"WoT.HUD.Scoreboard.Gather",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardGatherSpec::RunTest(const FString&)
{
	UWorld* World = ScoreboardTests_CreateWorld();
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!HUD || !PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	HUD->PlayerOwner = PC;
	// The controller auto-creates its PlayerState on spawn in a begun-play world;
	// calling InitPlayerState again orphans a second, duplicate PlayerState in
	// GameState->PlayerArray (which Gather would count). Use the existing one.
	AWoTPlayerState* PS = PC->GetPlayerState<AWoTPlayerState>();
	if (!PS)
	{
		PC->InitPlayerState();
		PS = PC->GetPlayerState<AWoTPlayerState>();
	}
	AWoTGameState* GS = World->GetGameState<AWoTGameState>();
	if (!PS || !GS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PS->SetPlayerName(TEXT("Nynaeve"));
	PS->Kills = 8;
	PS->Deaths = 4;
	PS->Suicides = 2;
	AWoTPlayerState::SetPacketLossSamplerForTesting(
		[](const UNetConnection*) { return 7.0f; });
	PS->Tick(1.0f);
	AWoTPlayerState::SetPacketLossSamplerForTesting({});
	PS->Color = 3;
	PS->SetStartTime(5);
	GS->ElapsedTime = 125.0f;
	GS->RemainingTime = 75.0f;
	GS->bDisplayRemaining = true;

	const FWoTScoreboardSnapshot Snapshot = HUD->GatherArenaScoreboard();
	TestFalse(TEXT("cleaned map package name is present"), Snapshot.MapName.IsEmpty());
	TestTrue(TEXT("clock mode mapped"), Snapshot.bDisplayRemaining);
	TestEqual(TEXT("remaining clock truncates to seconds"), Snapshot.RemainingTime, 75);
	TestEqual(TEXT("one WoT player gathered"), Snapshot.Players.Num(), 1);
	if (Snapshot.Players.Num() == 1)
	{
		const FWoTScoreboardPlayer& Player = Snapshot.Players[0];
		TestEqual(TEXT("name mapped"), Player.Name, FString(TEXT("Nynaeve")));
		TestEqual(TEXT("kills mapped"), Player.Kills, 8);
		TestEqual(TEXT("deaths mapped"), Player.Deaths, 4);
		TestEqual(TEXT("suicides mapped"), Player.Suicides, 2);
		TestEqual(TEXT("score uses CalcScore"), Player.Score, 6);
		TestEqual(TEXT("time on server uses elapsed minus join time"),
			Player.TimeOnServer, 120);
		TestEqual(TEXT("replicated UE connection packet loss is gathered"),
			Player.PacketLoss, 7);
		TestEqual(TEXT("SB-D1 replicated color layer is gathered"),
			Player.Color, uint8(3));
		TestEqual(TEXT("SB-D1 player type defaults to Aes Sedai"),
			Player.PlayerType, uint8(0));
		TestTrue(TEXT("owning player highlighted"), Player.bHighlight);
	}
	for (int32 Index = 0; Index < 16; ++Index)
	{
		AWoTPlayerState* Extra = World->SpawnActor<AWoTPlayerState>();
		if (Extra)
		{
			Extra->SetPlayerName(FString::Printf(TEXT("Extra%02d"), Index));
			GS->AddPlayerState(Extra);
		}
	}
	TestEqual(TEXT("gather caps PlayerArray at sixteen WoT players"),
		HUD->GatherArenaScoreboard().Players.Num(), 16);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardPacketLossNetSpec,
	"WoT.HUD.Scoreboard.PacketLossAuthorityReplication",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardPacketLossNetSpec::RunTest(const FString&)
{
#if WITH_EDITOR
	using namespace UE::Net;
	FTestWorlds Worlds(TEXT("/Engine/Maps/Entry"),
		TEXT("/Script/WoT.WoTGameMode"));
	Worlds.SetDescription(TEXT("scoreboard packet-loss authority sampling"));
	if (!TestTrue(TEXT("packet-loss server loads"), Worlds.Server.IsLoaded())
		|| !TestTrue(TEXT("packet-loss client connects"),
			Worlds.CreateAndConnectClient()))
	{
		return false;
	}
	AWoTPlayerState::SetPacketLossSamplerForTesting(
		[](const UNetConnection*) { return 7.0f; });
	ON_SCOPE_EXIT { AWoTPlayerState::SetPacketLossSamplerForTesting({}); };
	AWoTPlayerController* ServerPC = Cast<AWoTPlayerController>(
		Worlds.GetServerPlayerControllerOfClient(0));
	AWoTPlayerState* ServerPS = ServerPC
		? ServerPC->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!TestNotNull(TEXT("authority player state"), ServerPS)) return false;
	ServerPS->Tick(1.0f);
	ServerPS->ForceNetUpdate();
	AWoTPlayerState* ClientPS = nullptr;
	TestTrue(TEXT("sampled packet loss replicates to scoreboard client"),
		Worlds.TickUntil([&Worlds, &ClientPS]()
		{
			for (TActorIterator<AWoTPlayerState> It(Worlds.Clients[0].GetWorld()); It; ++It)
			{
				if (It->PacketLoss == 7) { ClientPS = *It; return true; }
			}
			return false;
		}, 120));
	TestNotNull(TEXT("client observes replicated sampled state"), ClientPS);
#endif
	return !HasAnyErrors();
}

// Guarded worlds are neutral shells (/WoTNeutralShells/WoTMapShellNN): the
// header showed "WoTMapShell12" in every packaged frame (T6-2 open list). The
// header must print the retail Level.Title (ArenaScoreBoard.uc:315-318) that
// the materialized world record carries, on the scoreboard AND the citadel
// overview, and print nothing for an empty Title (the retail branch).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardMaterializedMapTitleSpec,
	"WoT.HUD.Scoreboard.MaterializedMapTitle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardMaterializedMapTitleSpec::RunTest(const FString&)
{
	ResetWoTPayloadActivationForTesting();
	FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, false);
	const FString CacheRoot = FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()),
		TEXT("ScoreboardMaterialized-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits),
		TEXT("ContentCache"));
	IFileManager::Get().MakeDirectory(*CacheRoot, true);
	UWorld* ActivationWorld = nullptr;
	UWorld* World = nullptr;
	ON_SCOPE_EXIT
	{
		if (World && !World->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			World->DestroyWorld(false);
		WoTTest::DestroySpecWorld(ActivationWorld);
		FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
		ResetWoTPayloadActivationForTesting();
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		IFileManager::Get().DeleteDirectory(
			*FPaths::GetPath(CacheRoot), false, true);
	};

	TArray<FWoTWorldDescriptor> Descriptors;
	FString Error;
	if (!TestTrue(TEXT("authenticated production world fixture publishes"),
		WoTWorldMaterializerTest_PublishWorldSet(
			CacheRoot, Descriptors, Error)))
	{
		AddError(Error);
		return false;
	}
	const FWoTWorldDescriptor Descriptor = Descriptors[11];
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bStartPlay = false;
	Options.bActivatePayload = false;
	ActivationWorld = WoTTest::CreateSpecWorld(Options);
	UWoTPayloadSubsystem* Payload = ActivationWorld
		&& ActivationWorld->GetGameInstance()
		? ActivationWorld->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>()
		: nullptr;
	if (!TestNotNull(TEXT("production activation payload subsystem"), Payload))
	{
		return false;
	}
	UPackage* BootstrapPackage = CreatePackage(TEXT("/Game/WoT/Maps/WoTBootstrap"));
	BootstrapPackage->SetFlags(RF_Transient);
	UWorld* BootstrapWorld = UWorld::CreateWorld(EWorldType::Game, false,
		MakeUniqueObjectName(BootstrapPackage, UWorld::StaticClass(), TEXT("World")),
		BootstrapPackage, true, ERHIFeatureLevel::Num, nullptr, true);
	const bool bBootstrapSpent = TestNotNull(
		TEXT("production bootstrap candidate exists"), BootstrapWorld)
		&& TestTrue(TEXT("production bootstrap world is admitted before world authority"),
			WoTGuardedStartup_MaterializeWorld(BootstrapWorld,
				EWoTWorldStartPath::NormalInitialLoad, &Error));
	if (BootstrapWorld)
	{
		BootstrapWorld->DestroyWorld(false);
		BootstrapWorld->MarkAsGarbage();
	}
	if (!bBootstrapSpent)
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("authenticated world set activates and authorizes"),
			WoTWorldMaterializerTest_ActivateAndAuthorize(
				Payload, CacheRoot, MoveTemp(Descriptors), Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTRuntimeWorldResource AuthenticatedWorld;
	const FWoTBootstrapStatus LoadedWorld = AuthenticatedWorld.LoadFirstUse(
		FWoTNeutralResourceId{Descriptor.WorldLogicalId,
			EWoTNeutralResourceKind::World});
	if (!TestFalse(TEXT("authenticated production world record loads"),
		LoadedWorld.IsError()))
	{
		AddError(LoadedWorld.GetError().ToString());
		return false;
	}
	FString AuthenticatedTitle;
	for (const FWoTRuntimeWorldActor& Actor : AuthenticatedWorld.Actors())
	{
		if (!Actor.ClassIdentity.Equals(TEXT("Engine.LevelInfo"),
			ESearchCase::IgnoreCase)) continue;
		if (const FWoTRuntimeWorldProperty* Title =
			WoTRuntimeWorldFindProperty(Actor, TEXT("Title")))
		{
			WoTRuntimeWorldPropertyUE1String(*Title, AuthenticatedTitle);
		}
		break;
	}
	TestEqual(TEXT("authenticated production record carries Level.Title"),
		AuthenticatedTitle, FString(TEXT("Materialized Scoreboard Title")));
	UPackage* Package = CreatePackage(TEXT("/WoTNeutralShells/WoTMapShell12"));
	Package->SetFlags(RF_Transient);
	World = UWorld::CreateWorld(EWorldType::Game, false,
		MakeUniqueObjectName(Package, UWorld::StaticClass(), TEXT("World")),
		Package, true, ERHIFeatureLevel::Num, nullptr, true);
	if (!TestTrue(TEXT("real production materializer produces the world"),
		WoTMaterializeAuthenticatedWorld(
			World, EWoTWorldStartPath::NormalInitialLoad, &Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("production exposure follows materialization"),
		WoTCommitAuthenticatedWorldExposure(World, &Error)))
	{
		AddError(Error);
		return false;
	}
	World->InitWorld(UWorld::InitializationValues()
		.AllowAudioPlayback(false)
		.RequiresHitProxies(false)
		.CreateNavigation(false)
		.CreateAISystem(false)
		.CreateFXSystem(false));
	World->UpdateWorldComponents(false, false);
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	if (!TestNotNull(TEXT("scoreboard HUD in materialized world"), HUD))
		return false;
	const FString ExpectedTitle = TEXT("Materialized Scoreboard Title");
	TestTrue(TEXT("materialized worlds carry the retail Title"),
		WoTWorldHasRetailLevelTitle(World));
	TestEqual(TEXT("identity comes from the world record, not the package"),
		WoTWorldMapIdentity(World), Descriptor.MapIdentity);
	TestEqual(TEXT("scoreboard header prints Level.Title"),
		HUD->GatherArenaScoreboard().MapName, ExpectedTitle);
	TestEqual(TEXT("citadel overview header prints Level.Title"),
		HUD->GatherCitadelTeamOverview().MapName, ExpectedTitle);

	FWoTArenaScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Commands = Scoreboard.Layout(
		HUD->GatherArenaScoreboard(), 640, 480, 16.0f, false, false);
	TestTrue(TEXT("the drawn header line is the title"),
		Commands.ContainsByPredicate([](const FWoTHUDDrawCmd& Command)
		{
			return Command.Text == TEXT("Materialized Scoreboard Title");
		}));
	TestFalse(TEXT("no shell or package name is drawn"),
		Commands.ContainsByPredicate([](const FWoTHUDDrawCmd& Command)
		{
			return Command.Text.Contains(TEXT("WoTMapShell"));
		}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardPlayerTypeIconSpec,
	"WoT.HUD.Scoreboard.PlayerTypeIcon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardPlayerTypeIconSpec::RunTest(const FString&)
{
	UWorld* World = ScoreboardTests_CreateWorld();
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	AWoTPlayerState* PS = PC ? PC->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!HUD || !PC || !PS)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	HUD->PlayerOwner = PC;
	PS->SetPlayerName(TEXT("Galad"));
	PS->SetFactionClass(AWoTWhitecloak::StaticClass());

	const FWoTScoreboardSnapshot Snapshot = HUD->GatherArenaScoreboard();
	TestEqual(TEXT("one faction player gathered"), Snapshot.Players.Num(), 1);
	if (Snapshot.Players.Num() != 1)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	TestEqual(TEXT("unported Skin axis leaves the base color layer"),
		Snapshot.Players[0].Color, uint8(0));
	TestEqual(TEXT("replicated PlayerType selects the head layer"),
		Snapshot.Players[0].PlayerType, uint8(3));

	FWoTArenaScoreboard Scoreboard;
	Scoreboard.SortOrder.Reset();
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, true);
	const TArray<FWoTHUDDrawCmd> Icons = Commands.FilterByPredicate(
		[](const FWoTHUDDrawCmd& Command)
		{
			return !Command.TexId.IsNone();
		});
	TestEqual(TEXT("scoreboard emits both icon layers"), Icons.Num(), 2);
	if (Icons.Num() == 2)
	{
		TestEqual(TEXT("unported Skin color draw command"),
			Icons[0].TexId, FName(TEXT("HeadsBase")));
		TestEqual(TEXT("PlayerType 3 head draw command"),
			Icons[1].TexId, FName(TEXT("HeadsWC")));
		TestTrue(TEXT("head remains the masked overlay"),
			Icons[1].Style == EWoTHUDStyle::Masked);
	}

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTeamOverviewRouteSpec,
	"WoT.HUD.Scoreboard.CitadelOverview.Route",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTCitadelTeamOverviewRouteSpec::RunTest(const FString&)
{
	// MenuHUD: bShowScores wins; otherwise Battle's bShowOverview instantiates
	// CitadelTeamOverview rather than the Arena board.
	UWorld* BattleWorld = ScoreboardTests_CreateWorld(true);
	AWoTHUD* BattleHUD = BattleWorld->SpawnActor<AWoTHUD>();
	AWoTPlayerController* BattlePC =
		BattleWorld->SpawnActor<AWoTPlayerController>();
	if (!IsValid(BattleHUD) || !IsValid(BattlePC))
	{
		WoTTest::DestroySpecWorld(BattleWorld);
		return false;
	}
	BattlePC->SetAsLocalPlayerController();
	BattleHUD->PlayerOwner = BattlePC;
	BattlePC->bShowOverview = true;
	TestTrue(TEXT("Battle overview gate is active"),
		BattleHUD->ShouldDrawCitadelTeamOverview());
	TestFalse(TEXT("Battle overview does not route to Arena"),
		BattleHUD->ShouldDrawArenaScoreboard());

	const FWoTCitadelTeamOverview Overview;
	const TArray<FWoTHUDDrawCmd> OverviewCommands = Overview.Layout(
		BattleHUD->GatherCitadelTeamOverview(), 640, 480, 16.0f);
	// The legend text is whatever the HUD currently carries: the retail labels
	// once FWoTRetailText has loaded them in BeginPlay, else the neutral defaults.
	TestNotNull(TEXT("Battle overview emits team-status legend"),
		ScoreboardTests_FindText(OverviewCommands,
			*BattleHUD->CitadelOverviewTeamStr.ToString()));
	TestNotNull(TEXT("Battle overview emits players legend"),
		ScoreboardTests_FindText(OverviewCommands,
			*BattleHUD->CitadelOverviewPlayersStr.ToString()));
	TestNull(TEXT("Battle overview does not emit Arena name column"),
		ScoreboardTests_FindText(OverviewCommands, TEXT("Name")));

	BattlePC->bShowScores = true;
	TestTrue(TEXT("manual scores retains MenuHUD precedence"),
		BattleHUD->ShouldDrawArenaScoreboard());
	TestFalse(TEXT("manual scores suppresses overview branch"),
		BattleHUD->ShouldDrawCitadelTeamOverview());

	// BattleInfo is a separate replicated actor channel. MenuHUD.uc draws nothing
	// for bShowOverview until that model exists; it does not fall through to Arena.
	BattlePC->bShowScores = false;
	if (AWoTBattleInfo* DelayedBattleInfo = ScoreboardTests_FindBattle(BattleWorld))
	{
		DelayedBattleInfo->Destroy();
	}
	TestTrue(TEXT("replication-gap fixture retains bShowOverview"),
		BattlePC->bShowOverview);
	TestFalse(TEXT("no BattleInfo draws no Citadel overview"),
		BattleHUD->ShouldDrawCitadelTeamOverview());
	TestFalse(TEXT("no BattleInfo does not fall back to Arena"),
		BattleHUD->ShouldDrawArenaScoreboard());
	WoTTest::DestroySpecWorld(BattleWorld);

	UWorld* ArenaWorld = ScoreboardTests_CreateWorld();
	AWoTHUD* ArenaHUD = ArenaWorld->SpawnActor<AWoTHUD>();
	AWoTPlayerController* ArenaPC = ArenaWorld->SpawnActor<AWoTPlayerController>();
	if (!IsValid(ArenaHUD) || !IsValid(ArenaPC))
	{
		WoTTest::DestroySpecWorld(ArenaWorld);
		return false;
	}
	ArenaPC->SetAsLocalPlayerController();
	ArenaHUD->PlayerOwner = ArenaPC;
	ArenaPC->bShowScores = true;
	TestTrue(TEXT("Arena bShowScores remains on Arena board"),
		ArenaHUD->ShouldDrawArenaScoreboard());
	TestFalse(TEXT("Arena never invents a Citadel overview"),
		ArenaHUD->ShouldDrawCitadelTeamOverview());
	WoTTest::DestroySpecWorld(ArenaWorld);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelTeamOverviewContentSpec,
	"WoT.HUD.Scoreboard.CitadelOverview.Content",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTCitadelTeamOverviewContentSpec::RunTest(const FString&)
{
	UWorld* World = ScoreboardTests_CreateWorld(true);
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	AWoTBattleInfo* Battle = ScoreboardTests_FindBattle(World);
	if (!IsValid(HUD) || !IsValid(PC) || !IsValid(Battle))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	HUD->PlayerOwner = PC;
	AWoTPlayerState* LocalState = PC->GetPlayerState<AWoTPlayerState>();
	if (!IsValid(LocalState))
	{
		PC->InitPlayerState();
		LocalState = PC->GetPlayerState<AWoTPlayerState>();
	}
	if (!IsValid(LocalState))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	LocalState->Team = 1;
	Battle->NumPlayers[0] = 2;
	Battle->NumPlayers[1] = 1;
	Battle->bEditingComplete[0] = 1;
	Battle->bEditingComplete[1] = 0;

	FWoTCitadelTeamOverviewSnapshot Snapshot = HUD->GatherCitadelTeamOverview();
	TestEqual(TEXT("replicated team count becomes row count"),
		Snapshot.Teams.Num(), 2);
	if (Snapshot.Teams.Num() == 2)
	{
		TestEqual(TEXT("team 0 description comes from TeamClass CDO"),
			Snapshot.Teams[0].Description, FString(TEXT("Alpha")));
		TestEqual(TEXT("team 0 player count mapped"),
			Snapshot.Teams[0].NumPlayers, 2);
		TestEqual(TEXT("team cap mapped"), Snapshot.Teams[0].MaxTeamSize, 3);
		TestEqual(TEXT("ready team status mapped"),
			Snapshot.Teams[0].Status, Battle->ReadyStr);
		TestEqual(TEXT("waiting team status mapped"),
			Snapshot.Teams[1].Status, Battle->WaitingStr);
		TestFalse(TEXT("other team is dimmed"), Snapshot.Teams[0].bHighlight);
		TestTrue(TEXT("local team is highlighted"), Snapshot.Teams[1].bHighlight);
	}
	TestFalse(TEXT("one waiting team withholds begin prompt"),
		Snapshot.bAllTeamsReady);

	const FWoTCitadelTeamOverview Overview;
	TArray<FWoTHUDDrawCmd> Commands = Overview.Layout(
		Snapshot, 640, 480, 16.0f);
	TestNotNull(TEXT("team number rendered"),
		ScoreboardTests_FindText(Commands, TEXT("1)")));
	TestNotNull(TEXT("team description rendered"),
		ScoreboardTests_FindText(Commands, TEXT("Alpha")));
	const FWoTHUDDrawCmd* Alpha = ScoreboardTests_FindText(Commands, TEXT("Alpha"));
	const FWoTHUDDrawCmd* Bravo = ScoreboardTests_FindText(Commands, TEXT("Bravo"));
	TestTrue(TEXT("other team row uses shipped dim tint"), Alpha &&
		Alpha->Tint == FLinearColor::FromSRGBColor(FColor(120, 120, 120)));
	TestTrue(TEXT("local team row remains white"), Bravo &&
		Bravo->Tint == FLinearColor::White);
	TestNotNull(TEXT("count and maximum rendered"),
		ScoreboardTests_FindText(Commands, TEXT("2 (3)")));
	TestNotNull(TEXT("ready status rendered"),
		ScoreboardTests_FindText(Commands, Battle->ReadyStr));
	TestNotNull(TEXT("waiting status rendered"),
		ScoreboardTests_FindText(Commands, Battle->WaitingStr));
	TestNull(TEXT("begin prompt absent until every team is ready"),
		ScoreboardTests_FindText(Commands,
			HUD->CitadelOverviewBeginBattleStr.ToString()));

	Battle->bEditingComplete[1] = 1;
	Snapshot = HUD->GatherCitadelTeamOverview();
	Commands = Overview.Layout(Snapshot, 640, 480, 16.0f);
	const FWoTHUDDrawCmd* Begin = ScoreboardTests_FindText(
		Commands, HUD->CitadelOverviewBeginBattleStr.ToString());
	TestTrue(TEXT("all-ready prompt is centered below the rows"), Begin &&
		Begin->TextAlignment == EWoTTextAlignment::Center &&
		Begin->X == 320.0f && Begin->Y == 176.0f);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardVisibilitySpec,
	"WoT.HUD.Scoreboard.Visibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardVisibilitySpec::RunTest(const FString&)
{
	UWorld* World = ScoreboardTests_CreateWorld();
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!HUD || !PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->InitPlayerState();
	HUD->PlayerOwner = PC;
	TestFalse(TEXT("hidden by default"), HUD->ShouldDrawArenaScoreboard());
	PC->ShowScores();
	TestTrue(TEXT("first input press shows"), HUD->ShouldDrawArenaScoreboard());
	PC->ShowScores();
	TestFalse(TEXT("second input press hides"), HUD->ShouldDrawArenaScoreboard());
	// FLIPPED 2026-07-13 (previously asserted match end forces visibility over the
	// toggle, enshrining the divergence): retail GameEnded.BeginState sets bShowScores
	// ONCE and the ShowScores exec still toggles afterward — GameEnded overrides
	// neither ShowScores nor the draw gate (Engine.PlayerPawn GameEnded.BeginState
	// + the global exec ShowScores).
	PC->NotifyMatchEnded();
	TestTrue(TEXT("match end seeds the toggle once (GameEnded.BeginState)"),
		PC->bShowScores);
	TestTrue(TEXT("match end shows through the ordinary toggle"),
		HUD->ShouldDrawArenaScoreboard());
	PC->ShowScores();
	TestFalse(TEXT("ShowScores still toggles the board off after match end"),
		HUD->ShouldDrawArenaScoreboard());
	PC->ShowScores();
	TestTrue(TEXT("and toggles it back on"), HUD->ShouldDrawArenaScoreboard());
	HUD->bShowHUD = false;
	TestFalse(TEXT("ordinary HUD visibility gate retained"),
		HUD->ShouldDrawArenaScoreboard());
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardDyingDelaySpec,
	"WoT.HUD.Scoreboard.Lifecycle.DyingDelay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardDyingDelaySpec::RunTest(const FString&)
{
	UWorld* World = ScoreboardTests_CreateWorld();
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!HUD || !PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	HUD->PlayerOwner = PC;

	PC->ArmRespawnFire(nullptr, 10.0f);
	TestEqual(TEXT("Playing enters Dying"),
		static_cast<int32>(PC->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Dying));
	TestFalse(TEXT("entering Dying does not show immediately"),
		HUD->ShouldDrawArenaScoreboard());
	TestFalse(TEXT("death auto-show is not active while its timer is armed"),
		PC->IsDeathScoreboardAutoShowActive());

	WoTTest::TickWorld(World, 119);
	TestFalse(TEXT("scoreboard remains hidden before two seconds"),
		HUD->ShouldDrawArenaScoreboard());
	WoTTest::TickWorld(World, 2);
	TestTrue(TEXT("still-Dying controller auto-shows after two seconds"),
		HUD->ShouldDrawArenaScoreboard());
	TestTrue(TEXT("death auto-show source is active after the timer"),
		PC->IsDeathScoreboardAutoShowActive());
	TestTrue(TEXT("retail Dying timer sets bShowScores"), PC->bShowScores);
	PC->ShowScores();
	TestFalse(TEXT("manual press hides an auto-shown scoreboard"),
		HUD->ShouldDrawArenaScoreboard());
	PC->ShowScores();
	TestTrue(TEXT("next manual press shows again"),
		HUD->ShouldDrawArenaScoreboard());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardRespawnClearSpec,
	"WoT.HUD.Scoreboard.Lifecycle.RespawnClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardRespawnClearSpec::RunTest(const FString&)
{
	UWorld* World = ScoreboardTests_CreateWorld();
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* InitialPawn = World->SpawnActor<AWoTCharacter>(
		FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator,
		ScoreboardTests_AlwaysSpawnParams());
	AWoTCharacter* RespawnedPawn = World->SpawnActor<AWoTCharacter>(
		FVector(200.0, 0.0, 100.0), FRotator::ZeroRotator,
		ScoreboardTests_AlwaysSpawnParams());
	if (!HUD || !PC || !InitialPawn || !RespawnedPawn)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	PC->Possess(InitialPawn);
	HUD->PlayerOwner = PC;
	PC->ArmRespawnFire(InitialPawn, 10.0f);
	WoTTest::TickWorld(World, 60);
	PC->ShowScores();
	TestTrue(TEXT("manual toggle precondition shows during Dying"),
		HUD->ShouldDrawArenaScoreboard());

	PC->Possess(RespawnedPawn);
	TestEqual(TEXT("possession returns the FSM to Playing"),
		static_cast<int32>(PC->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::Playing));
	TestFalse(TEXT("leaving Dying clears bShowScores"), PC->bShowScores);
	TestFalse(TEXT("leaving Dying clears the death-auto source"),
		PC->IsDeathScoreboardAutoShowActive());
	TestFalse(TEXT("respawn hides the scoreboard"),
		HUD->ShouldDrawArenaScoreboard());

	WoTTest::TickWorld(World, 121);
	TestFalse(TEXT("cancelled Dying timer cannot re-show after respawn"),
		HUD->ShouldDrawArenaScoreboard());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardGameEndedSpec,
	"WoT.HUD.Scoreboard.Lifecycle.GameEnded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardGameEndedSpec::RunTest(const FString&)
{
	UWorld* World = ScoreboardTests_CreateWorld();
	AWoTHUD* HUD = World->SpawnActor<AWoTHUD>();
	AWoTPlayerController* PC = World->SpawnActor<AWoTPlayerController>();
	if (!HUD || !PC)
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	PC->SetAsLocalPlayerController();
	HUD->PlayerOwner = PC;
	PC->ArmRespawnFire(nullptr, 10.0f);

	PC->NotifyMatchEnded();
	TestEqual(TEXT("match end enters GameEnded"),
		static_cast<int32>(PC->GetPlayerStateMachineState()),
		static_cast<int32>(EWoTPlayerStateMachineState::GameEnded));
	TestTrue(TEXT("GameEnded transition shows immediately"), PC->bShowScores);
	TestTrue(TEXT("GameEnded draws immediately"), HUD->ShouldDrawArenaScoreboard());
	TestFalse(TEXT("GameEnded clears the Dying auto-show source"),
		PC->IsDeathScoreboardAutoShowActive());

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardInputMapSpec,
	"WoT.HUD.Scoreboard.InputMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardInputMapSpec::RunTest(const FString&)
{
	const UInputSettings* Settings = GetDefault<UInputSettings>();
	TArray<FInputActionKeyMapping> Mappings;
	Settings->GetActionMappingByName(FName(TEXT("ShowScores")), Mappings);
	TestEqual(TEXT("two ShowScores mappings"), Mappings.Num(), 2);
	TestTrue(TEXT("F4 is mapped"), Mappings.ContainsByPredicate(
		[](const FInputActionKeyMapping& Mapping) { return Mapping.Key == EKeys::F4; }));
	TestTrue(TEXT("middle mouse is mapped"), Mappings.ContainsByPredicate(
		[](const FInputActionKeyMapping& Mapping)
		{
			return Mapping.Key == EKeys::MiddleMouseButton;
		}));
	return true;
}

namespace
{
	void CitadelTests_AddPlayer(FWoTScoreboardSnapshot& Snapshot, const TCHAR* Name,
		uint8 Team, int32 TeamSealCount, int32 Score)
	{
		// CitadelScoreBoard.InitializePlayer (uc:121-134): per-row team economy,
		// TeamScore = TeamSealCount * 10 (CalcTeamScore, uc:131-134).
		FWoTScoreboardPlayer& Player = Snapshot.Players.AddDefaulted_GetRef();
		Player.Name = Name;
		Player.Team = Team;
		Player.TeamSealCount = TeamSealCount;
		Player.TeamScore = TeamSealCount * 10;
		Player.Score = Score;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardCitadelTeamsSpec,
	"WoT.HUD.Scoreboard.CitadelTeams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardCitadelTeamsSpec::RunTest(const FString&)
{
	// SB-D3: team-grouped rows ordered by team score (SortOrder(4)=SORT_TEAM,
	// (5)=SORT_TEAM_SCORE — newest dominant), per-block centered team header +
	// legend, the top-of-board legend suppressed, and the seal-goal header line.
	FWoTScoreboardSnapshot Snapshot;
	Snapshot.SealGoal = 4;
	Snapshot.TeamDescriptions.Add(0, TEXT("Children of the Light"));
	Snapshot.TeamDescriptions.Add(1, TEXT("Aes Sedai"));
	CitadelTests_AddPlayer(Snapshot, TEXT("Galad"), 0, 1, 5);
	CitadelTests_AddPlayer(Snapshot, TEXT("Moiraine"), 1, 2, 3);
	CitadelTests_AddPlayer(Snapshot, TEXT("Egwene"), 1, 2, 7);

	const FWoTCitadelScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 480, 16.0f, false, false);

	TestNotNull(TEXT("seal-goal header (uc:187-203, shipped NumSealsText)"),
		ScoreboardTests_FindText(Commands, TEXT("Number of seals to win: 4")));
	const FWoTHUDDrawCmd* AesHeader = ScoreboardTests_FindText(Commands,
		TEXT("Aes Sedai    Seals: 2    Score: 20"));
	const FWoTHUDDrawCmd* WCHeader = ScoreboardTests_FindText(Commands,
		TEXT("Children of the Light    Seals: 1    Score: 10"));
	if (TestNotNull(TEXT("winning team header"), AesHeader)
		&& TestNotNull(TEXT("trailing team header"), WCHeader))
	{
		TestTrue(TEXT("higher team score groups first"), AesHeader->Y < WCHeader->Y);
		TestEqual(TEXT("first team title follows stride then one title-space line"),
			AesHeader->Y, 122.0f);
		TestEqual(TEXT("next team title retains the retail header advances"),
			WCHeader->Y, 214.0f);
	}
	const FWoTHUDDrawCmd* Egwene = ScoreboardTests_FindText(Commands, TEXT("Egwene"));
	const FWoTHUDDrawCmd* Moiraine =
		ScoreboardTests_FindText(Commands, TEXT("Moiraine"));
	const FWoTHUDDrawCmd* Galad = ScoreboardTests_FindText(Commands, TEXT("Galad"));
	if (Egwene && Moiraine && Galad)
	{
		TestTrue(TEXT("arena Score key still orders rows inside the team block"),
			Egwene->Y < Moiraine->Y && Moiraine->Y < Galad->Y);
		TestEqual(TEXT("citadel rows use SlotSpacing 14"),
			Moiraine->Y - Egwene->Y, 14.0f);
		TestEqual(TEXT("first team row follows space/title/space/legend/advance"),
			Egwene->Y, 170.0f);
	}
	const int32 LegendCount = Commands.FilterByPredicate(
		[](const FWoTHUDDrawCmd& Command) { return Command.Text == TEXT("Time"); })
		.Num();
	TestEqual(TEXT("one legend per team block, none at the top (uc:107-111)"),
		LegendCount, 2);
	TestNull(TEXT("no victory banner below the goal"),
		ScoreboardTests_FindText(Commands, TEXT("[[ Victory goes to Aes Sedai ]]")));

	// PrevTeam initializes to 255. DrawLegend first subtracts 2*SlotSpacing, then
	// ShowScores advances 14 per row; a leading neutral block triggers no header.
	FWoTScoreboardSnapshot NeutralSnapshot;
	NeutralSnapshot.SealGoal = 4;
	CitadelTests_AddPlayer(NeutralSnapshot, TEXT("Neutral A"), 255, 0, 2);
	CitadelTests_AddPlayer(NeutralSnapshot, TEXT("Neutral B"), 255, 0, 1);
	const TArray<FWoTHUDDrawCmd> NeutralCommands =
		Scoreboard.Layout(NeutralSnapshot, 640, 480, 16.0f, false, false);
	const FWoTHUDDrawCmd* NeutralA =
		ScoreboardTests_FindText(NeutralCommands, TEXT("Neutral A"));
	const FWoTHUDDrawCmd* NeutralB =
		ScoreboardTests_FindText(NeutralCommands, TEXT("Neutral B"));
	if (TestNotNull(TEXT("first neutral row"), NeutralA)
		&& TestNotNull(TEXT("second neutral row"), NeutralB))
	{
		TestEqual(TEXT("citadel DrawLegend starts rows at quarter-height minus 28"),
			NeutralA->Y, 106.0f);
		TestEqual(TEXT("neutral rows retain the 14-UU stride"),
			NeutralB->Y - NeutralA->Y, 14.0f);
	}
	const int32 NeutralLegendCount = NeutralCommands.FilterByPredicate(
		[](const FWoTHUDDrawCmd& Command) { return Command.Text == TEXT("Time"); })
		.Num();
	TestEqual(TEXT("leading neutral block has no forced header or legend"),
		NeutralLegendCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTScoreboardCitadelVictorySpec,
	"WoT.HUD.Scoreboard.CitadelVictory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTScoreboardCitadelVictorySpec::RunTest(const FString&)
{
	// CitadelScoreBoard.uc:29-36: slot 0 at the goal produces the shipped
	// "[[ Victory goes to <Desc> ]]" banner; an unnamed team falls back to
	// "Team #N+1" (GetTeamDescription, uc:90-104).
	FWoTScoreboardSnapshot Snapshot;
	Snapshot.MapName = TEXT("Battle2_03");
	Snapshot.bDisplayRemaining = true;
	Snapshot.RemainingTime = 59;
	Snapshot.SealGoal = 4;
	CitadelTests_AddPlayer(Snapshot, TEXT("Moiraine"), 1, 4, 3);
	CitadelTests_AddPlayer(Snapshot, TEXT("Galad"), 0, 1, 5);

	const FWoTCitadelScoreboard Scoreboard;
	const TArray<FWoTHUDDrawCmd> Commands =
		Scoreboard.Layout(Snapshot, 640, 640, 16.0f, false, false);
	const FWoTHUDDrawCmd* Map =
		ScoreboardTests_FindText(Commands, TEXT("Battle2_03"));
	const FWoTHUDDrawCmd* SealGoal = ScoreboardTests_FindText(
		Commands, TEXT("Number of seals to win: 4"));
	const FWoTHUDDrawCmd* Clock = ScoreboardTests_FindText(
		Commands, TEXT("Remaining Time: 00:00:59"));
	const FWoTHUDDrawCmd* Victory = ScoreboardTests_FindText(
		Commands, TEXT("[[ Victory goes to Team #2 ]]"));
	TestTrue(TEXT("citadel header order is map then seal goal then time"),
		Map && SealGoal && Clock
		&& Map->Y == 64.0f && SealGoal->Y == 80.0f && Clock->Y == 96.0f);
	TestTrue(TEXT("working victory banner occupies retail's post-clock header slot"),
		Victory && Victory->Y == 112.0f);
	TestNotNull(TEXT("fallback team header"),
		ScoreboardTests_FindText(Commands, TEXT("Team #2    Seals: 4    Score: 40")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
