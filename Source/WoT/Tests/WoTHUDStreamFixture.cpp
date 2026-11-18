#include "WoTHUDStreamFixture.h"

#if WITH_AUTOMATION_TESTS

#include "WoTTestWorld.h"
#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCanvasScale.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTGameMode.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../Inventory/WoTAngrealInvAirBurst.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvFireball.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "RHIFeatureLevel.h"
#include "UnrealClient.h"

namespace WoTHUDHookTest
{
FActorSpawnParameters SpawnParams()
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return Params;
}

namespace
{
/** A render target with only a size: the deferred canvas is never flushed. */
class FSizeOnlyTarget final : public FRenderTarget
{
public:
	explicit FSizeOnlyTarget(FIntPoint InSize) : Size(InSize) {}
	virtual FIntPoint GetSizeXY() const override { return Size; }

private:
	FIntPoint Size;
};

/** A spec world's title is its transient package name (Untitled_N), which
 * counts the worlds the process made before it: the scoreboards' centred
 * title line keeps its place in the stream, without that text or the x its
 * width centres it at. */
FString NormalizeWorldTitle(const FString& Line)
{
	const int32 Start = Line.Find(TEXT("\"Untitled_"), ESearchCase::CaseSensitive);
	if (Start == INDEX_NONE)
	{
		return Line;
	}
	const int32 Position = Line.Find(TEXT("\" pos=("), ESearchCase::CaseSensitive,
		ESearchDir::FromStart, Start + 1);
	const int32 Comma = Position == INDEX_NONE ? INDEX_NONE : Line.Find(TEXT(","),
		ESearchCase::CaseSensitive, ESearchDir::FromStart, Position);
	if (Comma == INDEX_NONE || Position <= Start + 10)
	{
		return Line;
	}
	for (int32 Index = Start + 10; Index < Position; ++Index)
	{
		if (!FChar::IsDigit(Line[Index]))
		{
			return Line;
		}
	}
	return Line.Left(Start) + TEXT("\"<map>\" pos=(<x>") + Line.Mid(Comma);
}

FString StreamDigest(const TArray<FString>& RawLines)
{
	TArray<FString> Lines;
	for (const FString& Line : RawLines)
	{
		Lines.Add(NormalizeWorldTitle(Line));
	}
	const FString Joined = FString::Join(Lines, TEXT("\n"));
	const FTCHARToUTF8 Utf8(*Joined);
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
	return BytesToHex(Hash, FSHA1::DigestSize).ToLower();
}

struct FGoldenStream
{
	const TCHAR* State;
	int32 Items;
	const TCHAR* Digest;
};

/** Recorded from the HUD before H5 separated its update, snapshot, layout
 * and draw (the same harness, states and frames). */
const FGoldenStream GoldenStreams[] =
{
	{TEXT("Arena640"), 149, TEXT("14772c07ee95bd592cedf6f77baebb502e8f279c")},
	{TEXT("Arena1280x800"), 70, TEXT("4e060bfa098a125df0fe4a8db52b8f206e55deef")},
	{TEXT("Arena1920x1080"), 68, TEXT("4b2241bdfcaf382886491f1dd99082442a8a0b11")},
	{TEXT("Arena320x240"), 74, TEXT("0767f14c0f236ee65bad9e9bb4f33deffdf86dda")},
	{TEXT("Levels"), 120, TEXT("630bc9ad8f5adcc26233e15ff47bbc911593c6b2")},
	{TEXT("Scoreboard"), 385, TEXT("00a5b4334ca66ef29558b52d8a490b6083b5a593")},
	{TEXT("FlashAndTurtle"), 95, TEXT("57b5a8b8ab96721f41ea3268282eda5ccc821858")},
	{TEXT("MenuAndHidden"), 133, TEXT("c2aa948a3bbd375ade62a99ab90cb29f5e105ce0")},
	{TEXT("PlayerID"), 91, TEXT("71de5cd7b2fbbb270bb4722d7a2e5b3d98696287")},
	{TEXT("Battle"), 190, TEXT("4e61f98ff6664531d76283a18d34bf391250e38d")},
};
}

TArray<FString> DrawHUDOnce(AWoTHUD& HUD, int32 Width, int32 Height)
{
	TArray<FString> Lines;
	FSizeOnlyTarget Target(FIntPoint(Width, Height));
	FCanvas Canvas(&Target, nullptr, HUD.GetWorld(), GMaxRHIFeatureLevel,
		FCanvas::CDM_DeferDrawing, 1.0f);
	UCanvas* CanvasObject = NewObject<UCanvas>(GetTransientPackage());
	CanvasObject->Init(Width, Height, nullptr, &Canvas);
	FWoTCanvasScale::SubmissionObserverForTesting() = [&Lines](const FString& Line)
	{
		Lines.Add(Line);
	};
	HUD.SetCanvas(CanvasObject, CanvasObject);
	HUD.DrawHUD();
	HUD.SetCanvas(nullptr, nullptr);
	FWoTCanvasScale::SubmissionObserverForTesting() = nullptr;
	CanvasObject->Canvas = nullptr;
	return Lines;
}

TArray<FString> DrawSectionsOnce(UWorld* World, TConstArrayView<FWoTHUDSection> Sections,
	int32 Width, int32 Height)
{
	TArray<FString> Lines;
	FSizeOnlyTarget Target(FIntPoint(Width, Height));
	FCanvas Canvas(&Target, nullptr, World, GMaxRHIFeatureLevel,
		FCanvas::CDM_DeferDrawing, 1.0f);
	UCanvas* CanvasObject = NewObject<UCanvas>(GetTransientPackage());
	CanvasObject->Init(Width, Height, nullptr, &Canvas);
	FWoTCanvasScale::SubmissionObserverForTesting() = [&Lines](const FString& Line)
	{
		Lines.Add(Line);
	};
	FWoTHUDLayout::Draw(*CanvasObject, Sections);
	FWoTCanvasScale::SubmissionObserverForTesting() = nullptr;
	CanvasObject->Canvas = nullptr;
	return Lines;
}

int32 SectionTexts(const FWoTHUDSnapshot& Snapshot, EWoTHUDSection Part, const FString& Text)
{
	const FWoTHUDSection* Section = Snapshot.FindSection(Part);
	return Section ? Section->Commands.FilterByPredicate([&Text](const FWoTHUDDrawCmd& Command)
	{
		return Command.Text == Text;
	}).Num() : 0;
}

bool LandedOnCanvas(const FString& Line, int32 Width, int32 Height)
{
	return Line.Contains(FString::Printf(TEXT("canvas=(%d,%d,"), Width, Height));
}

bool FScene::Init(FAutomationTestBase& Test, TSubclassOf<AGameModeBase> Mode)
{
	if (!F.Init(Test, FVector(0, 0, 500), MOVE_Flying,
		Mode ? Mode : TSubclassOf<AGameModeBase>(AWoTGameMode::StaticClass())))
	{
		return false;
	}
	Body = F.World->SpawnActor<AWoTAesSedai>(FVector(0, 0, 500), FRotator::ZeroRotator, SpawnParams());
	if (!Test.TestNotNull(TEXT("HUD scene body"), Body))
	{
		return false;
	}
	AWoTCharacter* Spare = F.Pawn;
	F.PC->Possess(Body);
	F.Pawn = Body;
	if (Spare)
	{
		Spare->Destroy();
	}
	Body->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Body->Health = 73;
	F.PC->ClientSetHUD(AWoTHUD::StaticClass());
	HUD = Cast<AWoTHUD>(F.PC->GetHUD());
	if (!Test.TestNotNull(TEXT("HUD scene HUD"), HUD))
	{
		return false;
	}
	// No notify sounds: the streams are about pixels.
	HUD->LeftMessageName.Reset();
	HUD->RightMessageName.Reset();
	Body->EnsureAngrealHands();
	AWoTAngrealInvDart* Dart = F.World->SpawnActor<AWoTAngrealInvDart>(SpawnParams());
	AWoTAngrealInvAirBurst* Air = F.World->SpawnActor<AWoTAngrealInvAirBurst>(SpawnParams());
	AWoTAngrealInvFireball* Fire = F.World->SpawnActor<AWoTAngrealInvFireball>(SpawnParams());
	if (!Test.TestNotNull(TEXT("HUD scene artifacts"), Dart) || !Air || !Fire)
	{
		return false;
	}
	Dart->GiveTo(Body);
	Dart->CurCharges = 37;
	Air->GiveTo(Body);
	Air->CurCharges = 4;
	Fire->GiveTo(Body);
	Fire->CurCharges = 12;
	Body->InventoryComponent->SelectItem(Fire);
	Body->EnsureAngrealHands();
	FWoTStatusIconEntry& Good = Body->StatusIcons.AddDefaulted_GetRef();
	Good.IconID = TEXT("SceneGood");
	Good.IconTexture = TEXT("M_Levitation");
	Good.bGood = true;
	Good.InitialDuration = 20.0f;
	Good.RemainingDuration = 15.0f;
	FWoTStatusIconEntry& Bad = Body->StatusIcons.AddDefaulted_GetRef();
	Bad.IconID = TEXT("SceneBad");
	Bad.IconTexture = TEXT("M_Freeze");
	Bad.bGood = false;
	Bad.InitialDuration = 10.0f;
	Bad.RemainingDuration = 2.5f;
	F.Frames(2);
	return true;
}

void FScene::AddMessages()
{
	HUD->AddHandMessage(TEXT("Hand message"));
	HUD->AddLeftMessage(TEXT("Left one"));
	HUD->AddLeftMessage(TEXT("Left two"), 3.0f);
	HUD->AddLeftMessage(TEXT("Left three"));
	HUD->AddRightMessage(TEXT("Right one"));
	HUD->AddRightMessage(TEXT("Right two"), 2.0f);
	HUD->AddCenterMessage(TEXT("Centre older"), 3.0f);
	HUD->AddCenterMessage(TEXT("Centre newer"));
	HUD->AddSubtitleMessage(TEXT("A long subtitle that wraps across the middle of the screen ")
		TEXT("because it is wider than eight tenths of it, then more words follow.\n")
		TEXT("And a forced second line."));
	HUD->AddGenericMessage(TEXT("Generic centred"), 100, 200, true, 0);
	HUD->AddGenericMessage(TEXT("Generic left"), 40, 300, false, 128, nullptr, 3.0f);
	F.PC->SetProgressMessage(TEXT("Progress heading"), 0);
	F.PC->SetProgressMessage(TEXT("Progress detail"), 2);
	F.PC->SetProgressColor(FLinearColor(1.0f, 0.25f, 0.25f, 1.0f), 2);
	F.PC->SetProgressTime(6.0f);
}

void FScene::ShowWarningAndFlash()
{
	HUD->ClientWarn();
	HUD->SetFlashTexture(TEXT("Turtle"), 2.0f);
}

bool FScene::AddScoreboardPlayers(FAutomationTestBase& Test, int32 Count)
{
	for (int32 Index = 1; Index <= Count; ++Index)
	{
		AWoTPlayerState* Other = F.World->SpawnActor<AWoTPlayerState>(SpawnParams());
		if (!Test.TestNotNull(TEXT("scoreboard player"), Other))
		{
			return false;
		}
		Other->SetPlayerName(FString::Printf(TEXT("Player %02d"), Index));
		Other->Kills = (Index * 7) % 11;
		Other->Deaths = (Index * 5) % 9;
	}
	if (AWoTPlayerState* Own = F.PC->GetPlayerState<AWoTPlayerState>())
	{
		Own->SetPlayerName(TEXT("Local Player"));
	}
	return true;
}

void FScene::SetScoreboardShown(bool bShown)
{
	F.PC->bShowScores = bShown;
}

void FScene::Draw(int32 Width, int32 Height)
{
	DrawFrame(Width, Height);
}

TArray<FString> FScene::DrawFrame(int32 Width, int32 Height)
{
	F.Frames(1);
	if (BeforeDraw)
	{
		BeforeDraw(*this, Width, Height);
	}
	TArray<FString> Lines = DrawHUDOnce(*HUD, Width, Height);
	Stream.Add(FString::Printf(TEXT("frame %d %dx%d"), FrameIndex++, Width, Height));
	Stream.Append(Lines);
	return Lines;
}

void FScene::Wait(int32 Count)
{
	F.Frames(Count);
}

FScene::~FScene()
{
	if (OnTeardown)
	{
		OnTeardown();
	}
}

void CheckStream(FAutomationTestBase& Test, const FString& State, const TArray<FString>& Lines)
{
	const FString Digest = StreamDigest(Lines);
	Test.AddInfo(FString::Printf(TEXT("flat draw stream %s: %d lines, sha1 %s"),
		*State, Lines.Num(), *Digest));
	const FString DumpDirectory = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_HUD_STREAM_DIR"));
	if (!DumpDirectory.IsEmpty())
	{
		FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"),
			*FPaths::Combine(DumpDirectory, State + TEXT(".txt")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	const FGoldenStream* Golden = nullptr;
	for (const FGoldenStream& Entry : GoldenStreams)
	{
		if (Entry.State && State == Entry.State)
		{
			Golden = &Entry;
		}
	}
	if (!Golden)
	{
		Test.AddError(FString::Printf(TEXT("no recorded stream for %s"), *State));
		return;
	}
	const bool bSame = Golden->Items == Lines.Num() && Digest == Golden->Digest;
	if (!bSame)
	{
		const FString Saved = FPaths::Combine(FPaths::AutomationDir(),
			TEXT("H5FlatDrawStreams"), State + TEXT(".txt"));
		FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *Saved,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		Test.AddError(FString::Printf(
			TEXT("%s: the flat HUD drew %d lines (sha1 %s), recorded %d (sha1 %s); this stream is in %s"),
			*State, Lines.Num(), *Digest, Golden->Items, Golden->Digest, *Saved));
	}
}

bool RunRecordedStates(FAutomationTestBase& Test, TFunction<bool(FScene&)> Prepare)
{
	auto Ready = [&Test, &Prepare](FScene& S, TSubclassOf<AGameModeBase> Mode = nullptr)
	{
		return S.Init(Test, Mode) && (!Prepare || Prepare(S));
	};
	// Messages, warning, hands and status at the retail 640x480, then their
	// fades as time passes.
	{
		FScene S;
		if (!Ready(S)) return false;
		S.AddMessages();
		S.HUD->ClientWarn();
		S.Draw();
		S.Wait(30);
		S.Draw();
		S.Wait(30);
		S.Draw();
		S.Wait(120);
		S.Draw();
		S.Wait(480);
		S.Draw();
		CheckStream(Test, TEXT("Arena640"), S.Stream);
	}
	// The doubled text of a 1280x800 HUD, the integer upscale of 1080p and the
	// sub-640 factors.
	for (const FIntPoint Size : {FIntPoint(1280, 800), FIntPoint(1920, 1080), FIntPoint(320, 240)})
	{
		FScene S;
		if (!Ready(S)) return false;
		S.AddMessages();
		S.HUD->ClientWarn();
		S.Draw(Size.X, Size.Y);
		S.Wait(30);
		S.Draw(Size.X, Size.Y);
		CheckStream(Test, FString::Printf(TEXT("Arena%dx%d"), Size.X, Size.Y), S.Stream);
	}
	// Interface levels, the current-hand fan and the item window's hidden
	// surfaces.
	{
		FScene S;
		if (!Ready(S)) return false;
		S.HUD->SetInterfaceLevel(EWoTInterfaceLevel::Minimized, false);
		S.HUD->NotifyHandSelection();
		S.Draw();
		S.Wait(30);
		S.Draw();
		S.AddMessages();
		S.F.PC->ShowInventoryInfo();
		S.Draw();
		S.Wait(30);
		S.Draw();
		S.F.PC->ShowInventoryInfo();
		S.HUD->SetInterfaceLevel(EWoTInterfaceLevel::UltraMinimized, false);
		S.HUD->NotifyHandSelection();
		S.Draw();
		S.Wait(60);
		S.Draw();
		S.HUD->SetInterfaceLevel(EWoTInterfaceLevel::Normal, false);
		CheckStream(Test, TEXT("Levels"), S.Stream);
	}
	// A full sixteen-player scoreboard on the three canvas classes.
	{
		FScene S;
		if (!Ready(S)) return false;
		for (int32 Index = 1; Index < 16; ++Index)
		{
			AWoTPlayerState* Other = S.F.World->SpawnActor<AWoTPlayerState>(SpawnParams());
			if (!Test.TestNotNull(TEXT("scoreboard player"), Other)) return false;
			Other->SetPlayerName(FString::Printf(TEXT("Player %02d"), Index));
			Other->Kills = (Index * 7) % 11;
			Other->Deaths = (Index * 5) % 9;
			Other->Color = static_cast<uint8>(Index % 5);
		}
		if (AWoTPlayerState* Own = S.F.PC->GetPlayerState<AWoTPlayerState>())
		{
			Own->SetPlayerName(TEXT("Local Player"));
			Own->Kills = 9;
		}
		S.F.PC->bShowScores = true;
		S.Draw();
		S.Draw(1280, 800);
		S.Draw(1920, 1080);
		S.F.PC->bShowScores = false;
		S.Draw();
		CheckStream(Test, TEXT("Scoreboard"), S.Stream);
	}
	// The flash texture and the slow-frame turtle.
	{
		FScene S;
		if (!Ready(S)) return false;
		S.HUD->BringBackTurtle();
		S.HUD->SetFlashTexture(TEXT("Turtle"), 2.0f);
		S.Draw();
		S.Wait(6);
		S.Draw();
		S.Wait(6);
		S.Draw();
		S.HUD->GetRidOfTurtle();
		S.Wait(60);
		S.Draw();
		S.Wait(60);
		S.Draw();
		CheckStream(Test, TEXT("FlashAndTurtle"), S.Stream);
	}
	// The in-game menu hides the HUD and pauses message time; closing it
	// catches the time up. A hidden HUD draws nothing.
	{
		FScene S;
		if (!Ready(S)) return false;
		S.AddMessages();
		S.Draw();
		S.F.PC->ToggleInGameMenu();
		S.Wait(30);
		S.Draw();
		S.Wait(30);
		S.Draw();
		S.F.PC->ResumeFromInGameMenu();
		S.Draw();
		S.Wait(30);
		S.Draw();
		S.HUD->bShowHUD = false;
		S.Draw();
		S.Wait(30);
		S.Draw();
		S.HUD->bShowHUD = true;
		S.Draw();
		CheckStream(Test, TEXT("MenuAndHidden"), S.Stream);
	}
	// The player ID of a named player in the aim, fading after it leaves.
	{
		FScene S;
		if (!Ready(S)) return false;
		AWoTAesSedai* Target = S.F.World->SpawnActor<AWoTAesSedai>(
			FVector(400, 0, 500), FRotator(0, 180, 0), SpawnParams());
		AWoTPlayerController* Owner = S.F.World->SpawnActor<AWoTPlayerController>();
		if (!Test.TestNotNull(TEXT("player ID target"), Target) || !Test.TestNotNull(TEXT("target owner"), Owner)) return false;
		Owner->Possess(Target);
		Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		if (APlayerState* TargetState = Target->GetPlayerState())
		{
			TargetState->SetPlayerName(TEXT("Target Player"));
		}
		S.F.PC->SetControlRotation(FRotator::ZeroRotator);
		S.Draw();
		S.Wait(30);
		S.Draw();
		Target->SetActorLocation(FVector(400, 3000, 500));
		S.Wait(30);
		S.Draw();
		S.Wait(60);
		S.Draw();
		S.Wait(240);
		S.Draw();
		CheckStream(Test, TEXT("PlayerID"), S.Stream);
	}
	// A Battle map: the team prompt, the team overview and the citadel board.
	{
		FScene S;
		if (!Ready(S, AWoTBattleGameMode::StaticClass())) return false;
		AWoTBattleGameMode* Battle = S.F.World->GetAuthGameMode<AWoTBattleGameMode>();
		AWoTBattleInfo* Info = S.F.World->SpawnActorDeferred<AWoTBattleInfo>(
			AWoTBattleInfo::StaticClass(), FTransform::Identity);
		if (!Test.TestNotNull(TEXT("battle rules"), Battle) || !Test.TestNotNull(TEXT("battle info"), Info)) return false;
		Info->NumTeams = 2;
		Info->TeamClass[0] = AWoTCharacter::StaticClass();
		Info->TeamClass[1] = AWoTCharacter::StaticClass();
		Info->FinishSpawning(FTransform::Identity);
		Battle->Battle = Info;
		AWoTPlayerState* Own = S.F.PC->GetPlayerState<AWoTPlayerState>();
		if (!Test.TestNotNull(TEXT("battle player state"), Own)) return false;
		Own->SetPlayerName(TEXT("Local Player"));
		Own->Team = 255;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			AWoTPlayerState* Other = S.F.World->SpawnActor<AWoTPlayerState>(SpawnParams());
			if (!Test.TestNotNull(TEXT("battle player"), Other)) return false;
			Other->SetPlayerName(FString::Printf(TEXT("Side Player %d"), Index));
			Other->Team = static_cast<uint8>(Index % 2);
			Other->Kills = Index;
		}
		S.Draw();
		S.F.PC->bShowOverview = true;
		S.Draw();
		S.F.PC->bShowScores = true;
		S.Draw();
		S.Draw(1280, 800);
		S.F.PC->bShowScores = false;
		S.F.PC->bShowOverview = false;
		Own->Team = 0;
		S.Draw();
		CheckStream(Test, TEXT("Battle"), S.Stream);
	}
	return true;
}
}

#endif // WITH_AUTOMATION_TESTS
