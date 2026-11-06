#include "WoTNetTestActions.h"
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES

#include "UnrealClient.h"                 // FScreenshotRequest
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Controller.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Components/BoxComponent.h"
#include "../WoTCharacter.h"
#include "../WoTGameMode.h"
#include "../WoTBattleGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTBattleInfo.h"
#include "../WoTFrontEnd.h"
#include "../WoTFrontEndPlayerController.h"
#include "../WoTFrontEndSlate.h"
#include "../WoTGameInstance.h"
#include "../WoTPlayerState.h"
#include "../WoTSessionSubsystem.h"
#include "../World/WoTWorldMaterializer.h"
#include "../WoTZoneVolume.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTInventoryComponent.h"
#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// Resolve an angreal class from a bare native class name, a "Pkg.Class", or a
	// full object path. Native classes are already loaded, so FindObject hits;
	// LoadClass covers any (future) content-path form.
	UClass* ResolveArtifactClass(const FString& NameOrPath)
	{
		FString Path = NameOrPath;
		if (!Path.Contains(TEXT("/")) && !Path.Contains(TEXT(".")))
		{
			Path = FString::Printf(TEXT("/Script/WoT.%s"), *NameOrPath);
		}
		UClass* Found = FindObject<UClass>(nullptr, *Path);
		if (!Found)
		{
			Found = LoadClass<AWoTAngrealInventory>(nullptr, *Path);
		}
		return (Found && Found->IsChildOf(AWoTAngrealInventory::StaticClass())) ? Found : nullptr;
	}
}

APlayerController* WoTNetTest::LocalPlayerController(UWorld* World, int32 PlayerIndex)
{
	if (!IsValid(World) || PlayerIndex < 0) return nullptr;
	UGameInstance* GameInstance = World->GetGameInstance();
	ULocalPlayer* LocalPlayer = GameInstance
		? GameInstance->GetLocalPlayerByIndex(PlayerIndex) : nullptr;
	if (LocalPlayer) return LocalPlayer->PlayerController;
#if WITH_AUTOMATION_TESTS
	// The existing headless action specs create a bare local-marked controller
	// in a synthetic world with no GameInstance or ULocalPlayer. Keep that test
	// seam for p0 only; packaged harness builds always use the indexed local
	// player collection above.
	if (PlayerIndex == 0 && IsValid(World)) return World->GetFirstPlayerController();
#endif
	return nullptr;
}

AWoTCharacter* WoTNetTest::LocalPlayerPawn(UWorld* World, int32 PlayerIndex)
{
	APlayerController* Controller = LocalPlayerController(World, PlayerIndex);
	return Controller ? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;
}

void WoTNetTest::ApplyMoveTick(UWorld* World, int32 PlayerIndex, const FMoveState& Move)
{
	AWoTCharacter* Pawn = LocalPlayerPawn(World, PlayerIndex);
	if (!IsValid(Pawn) || Move.Axis.IsNearlyZero())
	{
		return;
	}
	const FRotator Yaw(0.f, Pawn->GetControlRotation().Yaw, 0.f);
	const FRotationMatrix Basis(Yaw);
	Pawn->AddMovementInput(Basis.GetScaledAxis(EAxis::X), Move.Axis.X);
	Pawn->AddMovementInput(Basis.GetScaledAxis(EAxis::Y), Move.Axis.Y);
}

bool WoTNetTest::CastPress(UWorld* World, int32 PlayerIndex)
{
	AWoTPlayerController* Controller = Cast<AWoTPlayerController>(
		LocalPlayerController(World, PlayerIndex));
	if (!IsValid(Controller)) { return false; }
	Controller->Fire();          // Playing cast / Dying respawn / GameEnded restart RPC
	return true;
}

bool WoTNetTest::CastRelease(UWorld* World, int32 PlayerIndex)
{
	AWoTCharacter* Pawn = LocalPlayerPawn(World, PlayerIndex);
	if (!IsValid(Pawn)) { return false; }
	Pawn->CeaseUsingAngreal();   // exec -> ServerCeaseUsingAngreal RPC
	return true;
}

bool WoTNetTest::NextArtifact(UWorld* World, int32 PlayerIndex)
{
	AWoTCharacter* Pawn = LocalPlayerPawn(World, PlayerIndex);
	if (!IsValid(Pawn)) { return false; }
	Pawn->NextInventory();       // exec -> ServerNextInventory RPC
	return true;
}

bool WoTNetTest::Jump(UWorld* World, int32 PlayerIndex)
{
	AWoTPlayerController* Controller = Cast<AWoTPlayerController>(
		LocalPlayerController(World, PlayerIndex));
	if (!IsValid(Controller)) return false;
	Controller->Jump();
	return true;
}

bool WoTNetTest::StopJump(UWorld* World, int32 PlayerIndex)
{
	AWoTPlayerController* Controller = Cast<AWoTPlayerController>(
		LocalPlayerController(World, PlayerIndex));
	if (!IsValid(Controller)) return false;
	Controller->StopJumping();
	return true;
}

bool WoTNetTest::SelectArtifact(UWorld* World, const FString& ClassPath,
	int32 PlayerIndex)
{
	AWoTCharacter* Pawn = LocalPlayerPawn(World, PlayerIndex);
	UClass* Class = ResolveArtifactClass(ClassPath);
	UWoTInventoryComponent* Inventory = IsValid(Pawn)
		? Pawn->FindComponentByClass<UWoTInventoryComponent>() : nullptr;
	AWoTInventory* Item = Inventory && Class
		? Inventory->FindInventoryType(Class) : nullptr;
	if (!Item) return false;
	Inventory->SelectItem(Item);
	return true;
}

bool WoTNetTest::DamagePawn(UWorld* World, const FString& PawnName, float Amount)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client)
	{
		return false;
	}
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (It->GetName() == PawnName)
		{
			FWoTDamageEvent Ev;
			Ev.WoTDamageType = TEXT("hurt");
			Ev.UE1Momentum = FVector::ZeroVector;   // test action: no retail shove
			Ev.HitInfo.ImpactPoint = It->GetActorLocation();
			It->TakeDamage(Amount, Ev, nullptr, nullptr);
			return true;
		}
	}
	return false;
}

bool WoTNetTest::GiveArtifact(UWorld* World, const FString& PawnName,
                             const FString& ClassPath, int32 Charges)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client)
	{
		return false;
	}
	AWoTGameMode* GameMode = World->GetAuthGameMode<AWoTGameMode>();
	if (!GameMode)
	{
		return false;
	}
	UClass* Cls = ResolveArtifactClass(ClassPath);
	if (!Cls)
	{
		return false;
	}
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (It->GetName() == PawnName)
		{
			GameMode->GivePawnAngreal(*It, Cls, Charges);
			// Spawn selection sticks to the first-selected item (Dart), and
			// AddInventory only auto-selects when nothing is selected — so select
			// the just-granted artifact here, making a following castpress fire it.
			if (UWoTInventoryComponent* Inv = It->FindComponentByClass<UWoTInventoryComponent>())
			{
				if (AWoTInventory* Granted = Inv->FindInventoryType(Cls))
				{
					Inv->SelectItem(Granted);
				}
			}
			return true;
		}
	}
	return false;
}

bool WoTNetTest::GiveSeal(UWorld* World, const FString& PawnName)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client) return false;
	const AWoTBattleGameMode* BattleMode =
		World->GetAuthGameMode<AWoTBattleGameMode>();
	const AWoTBattleInfo* Battle = BattleMode ? BattleMode->Battle : nullptr;
	if (!Battle)
	{
		return false;
	}
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (It->GetName() != PawnName) continue;
		AWoTPlayerState* PlayerState = It->GetPlayerState<AWoTPlayerState>();
		UWoTInventoryComponent* Inventory =
			It->FindComponentByClass<UWoTInventoryComponent>();
		if (It->bDead || !PlayerState || PlayerState->Team >= Battle->NumTeams
			|| PlayerState->Team >= 4 || !Battle->TeamClass[PlayerState->Team]
			|| !Inventory
			|| Inventory->FindInventoryType(AWoTSeal::StaticClass()))
		{
			return false;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AWoTSeal* Seal = World->SpawnActor<AWoTSeal>(AWoTSeal::StaticClass(),
			It->GetActorLocation() + FVector(0.0, 0.0, 500.0),
			FRotator::ZeroRotator, Params);
		if (!Seal)
		{
			return false;
		}

		// This guarded command establishes the state that production altar
		// hand-back establishes: a real, team-authored seal directly given to its
		// carrier. TryPickupBy routes through giMPBattle.PickupQuery and is
		// intentionally refused during the edit/pre-play phase this command sets up.
		Seal->Team = PlayerState->Team;
		Seal->GiveTo(*It);
		const bool bCarried = Seal->GetOwner() == *It
			&& Inventory->Items.Contains(Seal)
			&& Seal->GetItemState() == EWoTInventoryState::Held;
		if (!bCarried)
		{
			Seal->Destroy();
		}
		return bCarried;
	}
	return false;
}

TSharedPtr<FJsonObject> WoTNetTest::BuildStatus(UWorld* World)
{
	TSharedPtr<FJsonObject> Status = MakeShared<FJsonObject>();
	if (!IsValid(World))
	{
		Status->SetBoolField(TEXT("front_end_ready"), false);
		Status->SetStringField(TEXT("world"), TEXT(""));
		Status->SetStringField(TEXT("net_mode"), TEXT("invalid"));
		Status->SetStringField(TEXT("battle_phase"), TEXT("none"));
		Status->SetNumberField(TEXT("team"), -1);
		TArray<TSharedPtr<FJsonValue>> EmptySeals;
		Status->SetArrayField(TEXT("seals"), EmptySeals);
		return Status;
	}
	UGameInstance* GameInstance = World->GetGameInstance();
	Status->SetBoolField(TEXT("front_end_ready"),
		FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance));
	FString WorldIdentity = World->GetMapName().Replace(TEXT("UEDPIE_0_"), TEXT(""));
	if (const TOptional<FString> GuardedIdentity =
		WoTMapIdentityForGuardedShellPackage(World->GetOutermost()->GetName());
		GuardedIdentity.IsSet())
	{
		WorldIdentity = GuardedIdentity.GetValue();
	}
	Status->SetStringField(TEXT("world"), WorldIdentity);
	const ENetMode NetMode = World->GetNetMode();
	const TCHAR* NetModeName = NetMode == NM_Client ? TEXT("client")
		: NetMode == NM_ListenServer ? TEXT("listen")
		: NetMode == NM_DedicatedServer ? TEXT("dedicated") : TEXT("standalone");
	Status->SetStringField(TEXT("net_mode"), NetModeName);

	int32 LocalTeam = -1;
	if (const APlayerController* Controller = World->GetFirstPlayerController())
	{
		if (const AWoTPlayerState* PlayerState =
			Controller->GetPlayerState<AWoTPlayerState>())
		{
			LocalTeam = PlayerState->Team;
		}
	}
	Status->SetNumberField(TEXT("team"), LocalTeam);
	TArray<TSharedPtr<FJsonValue>> Seals;
	FString BattlePhase(TEXT("none"));
	if (TActorIterator<AWoTBattleInfo> It(World); It)
	{
		const AWoTBattleInfo* Battle = *It;
		bool bAnyEditing = false;
		bool bAllReady = Battle->NumTeams > 0;
		for (int32 TeamIndex = 0; TeamIndex < 4; ++TeamIndex)
		{
			Seals.Add(MakeShared<FJsonValueNumber>(Battle->NumSeals[TeamIndex]));
			if (TeamIndex < Battle->NumTeams)
			{
				bAnyEditing = bAnyEditing || Battle->bEditing[TeamIndex] != 0;
				bAllReady = bAllReady && Battle->bEditingComplete[TeamIndex] != 0;
			}
		}
		BattlePhase = !Battle->bTeleportersDisabled ? TEXT("playing")
			: bAnyEditing ? TEXT("editing")
			: bAllReady ? TEXT("ready") : TEXT("waiting");
	}
	Status->SetStringField(TEXT("battle_phase"), BattlePhase);
	Status->SetArrayField(TEXT("seals"), Seals);
	Status->SetObjectField(TEXT("browser"), BuildBrowserStatus(World));
	return Status;
}

namespace
{
	TSharedPtr<SWoTFrontEndRoot> FrontEndBrowser(UWorld* World)
	{
		if (!IsValid(World))
		{
			return nullptr;
		}
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const AWoTFrontEndPlayerController* FrontEnd =
				Cast<AWoTFrontEndPlayerController>(It->Get());
			if (FrontEnd && FrontEnd->IsLocalController())
			{
				return FrontEnd->GetFrontEndRootForHarness();
			}
		}
		return nullptr;
	}

	const TCHAR* BrowserOperationName(EWoTFrontEndAsyncOperation Operation)
	{
		switch (Operation)
		{
		case EWoTFrontEndAsyncOperation::Hosting: return TEXT("hosting");
		case EWoTFrontEndAsyncOperation::Searching: return TEXT("searching");
		case EWoTFrontEndAsyncOperation::Joining: return TEXT("joining");
		case EWoTFrontEndAsyncOperation::Connecting: return TEXT("connecting");
		default: return TEXT("none");
		}
	}
}

bool WoTNetTest::BrowserFind(UWorld* World, bool bLan)
{
	const TSharedPtr<SWoTFrontEndRoot> Browser = FrontEndBrowser(World);
	return Browser.IsValid()
		&& FWoTFrontEndModel::CanConstructNormalFrontEnd(World->GetGameInstance())
		&& Browser->HarnessRefreshBrowser(bLan);
}

bool WoTNetTest::BrowserSelectHostMapKind(UWorld* World, bool bBattle, double& OutScrollable)
{
	OutScrollable = -1.0;
	const TSharedPtr<SWoTFrontEndRoot> Browser = FrontEndBrowser(World);
	if (!Browser.IsValid() || !Browser->HarnessSelectHostMapKind(bBattle))
	{
		return false;
	}
	OutScrollable = Browser->HarnessHostPanelScrollable();
	return true;
}

bool WoTNetTest::BrowserJoin(UWorld* World, int32 Row)
{
	const TSharedPtr<SWoTFrontEndRoot> Browser = FrontEndBrowser(World);
	return Browser.IsValid()
		&& FWoTFrontEndModel::CanConstructNormalFrontEnd(World->GetGameInstance())
		&& Browser->HarnessJoinBrowserRow(Row);
}

TSharedPtr<FJsonObject> WoTNetTest::BuildBrowserStatus(UWorld* World)
{
	TSharedPtr<FJsonObject> Browser = MakeShared<FJsonObject>();
	const TSharedPtr<SWoTFrontEndRoot> Root = FrontEndBrowser(World);
	Browser->SetBoolField(TEXT("available"), Root.IsValid());
	TArray<TSharedPtr<FJsonValue>> Rows;
	if (Root.IsValid())
	{
		Browser->SetStringField(TEXT("operation"),
			BrowserOperationName(Root->HarnessBrowserOperation()));
		Browser->SetStringField(TEXT("status"), Root->HarnessBrowserStatus());
		int32 RowIndex = 0;
		for (const FWoTSessionListEntry& Entry : Root->HarnessBrowserRows())
		{
			const TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("row"), RowIndex++);
			Row->SetStringField(TEXT("server"), Entry.ServerName);
			Row->SetStringField(TEXT("map"), Entry.MapName);
			Row->SetNumberField(TEXT("players"), Entry.CurrentPlayers);
			Row->SetNumberField(TEXT("max_players"), Entry.MaxPlayers);
			Row->SetNumberField(TEXT("ping"), Entry.PingMs);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}
	else
	{
		Browser->SetStringField(TEXT("operation"), TEXT("none"));
		Browser->SetStringField(TEXT("status"), TEXT(""));
	}
	Browser->SetArrayField(TEXT("rows"), Rows);
	const UGameInstance* GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
	const UWoTSessionSubsystem* Sessions = GameInstance
		? GameInstance->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	Browser->SetStringField(TEXT("session"),
		Sessions ? Sessions->HostDiagnostics() : FString(TEXT("none")));
	return Browser;
}

bool WoTNetTest::Host(UWorld* World, const FString& Map, const FString& Game,
	int32 FragLimit, int32 TimeLimit, bool bLan, int32 SealGoal,
	int32 SealBudget)
{
	UGameInstance* GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
	UWoTSessionSubsystem* Sessions = GameInstance
		? GameInstance->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	if (!Sessions || !FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance))
	{
		return false;
	}
	FString MapPackage = Map;
	if (!MapPackage.StartsWith(TEXT("/Game/")))
	{
		MapPackage = TEXT("/Game/WoT/Maps/") + MapPackage;
	}
	if (!Game.IsEmpty())
	{
		const bool bBattleRequested = Game.Equals(TEXT("battle"), ESearchCase::IgnoreCase)
			|| Game.Equals(TEXT("/Script/WoT.WoTBattleGameMode"),
				ESearchCase::IgnoreCase);
		const bool bArenaRequested = Game.Equals(TEXT("arena"), ESearchCase::IgnoreCase)
			|| Game.Equals(TEXT("/Script/WoT.WoTGameMode"), ESearchCase::IgnoreCase);
		if ((!bBattleRequested && !bArenaRequested)
			|| (bBattleRequested != FWoTFrontEndModel::IsBattleMap(MapPackage)))
		{
			return false;
		}
	}
	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(MapPackage);
	Request.bLAN = bLan;
	if (FragLimit >= 0)
	{
		Request.FragLimit = FragLimit;
	}
	if (TimeLimit >= 0)
	{
		Request.TimeLimit = TimeLimit;
	}
	if (SealGoal >= 0)
	{
		Request.SealGoal = SealGoal;
	}
	if (SealBudget >= 0)
	{
		Request.SealBudget = SealBudget;
	}
	return Sessions->Host(Request);
}

bool WoTNetTest::BuildTravelURL(const FString& Map, const FString& Game,
	FString& OutURL)
{
	OutURL.Reset();
	FString MapPackage = Map;
	MapPackage.TrimStartAndEndInline();
	if (MapPackage.IsEmpty() || MapPackage.Contains(TEXT("?"))
		|| MapPackage.Contains(TEXT("#")) || MapPackage.Contains(TEXT("\\")))
	{
		return false;
	}
	if (!MapPackage.StartsWith(TEXT("/Game/")))
	{
		if (MapPackage.Contains(TEXT("/"))) return false;
		for (const TCHAR Character : MapPackage)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_')) return false;
		}
		MapPackage = TEXT("/Game/WoT/Maps/") + MapPackage;
	}
	const FString Prefix(TEXT("/Game/WoT/Maps/"));
	if (!MapPackage.StartsWith(Prefix, ESearchCase::CaseSensitive)) return false;
	const FString Leaf = MapPackage.Mid(Prefix.Len());
	if (Leaf.IsEmpty() || Leaf.Contains(TEXT("/"))) return false;
	for (const TCHAR Character : Leaf)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_')) return false;
	}
	const bool bBattleRequested = Game.Equals(TEXT("battle"), ESearchCase::IgnoreCase)
		|| Game.Equals(TEXT("/Script/WoT.WoTBattleGameMode"),
			ESearchCase::IgnoreCase);
	const bool bArenaRequested = Game.Equals(TEXT("arena"), ESearchCase::IgnoreCase)
		|| Game.Equals(TEXT("/Script/WoT.WoTGameMode"), ESearchCase::IgnoreCase);
	if ((!bBattleRequested && !bArenaRequested)
		|| !FWoTFrontEndModel::IsPlayableMap(MapPackage)
		|| bBattleRequested != FWoTFrontEndModel::IsBattleMap(MapPackage))
	{
		return false;
	}
	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(MapPackage);
	Request.bLAN = true;
	OutURL = FWoTFrontEndModel::BuildHostTravelURL(
		Request, EWoTPlayerFaction::AesSedai);
	return !OutURL.IsEmpty();
}

bool WoTNetTest::Travel(UWorld* World, const FString& Map, const FString& Game)
{
	if (!IsValid(World) || World->GetNetMode() != NM_ListenServer) return false;
	FString URL;
	if (!BuildTravelURL(Map, Game, URL)) return false;
	UWoTGameInstance* GameInstance = Cast<UWoTGameInstance>(World->GetGameInstance());
	if (!GameInstance) return false;
	// The URL must carry player zero's current in-memory faction. The helper's
	// AesSedai default is only useful to pure parser tests.
	int32 OptionIndex = INDEX_NONE;
	URL.FindChar(TEXT('?'), OptionIndex);
	const FString MapPackage = OptionIndex == INDEX_NONE ? URL : URL.Left(OptionIndex);
	FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(MapPackage);
	Request.bLAN = true;
	// The play-through must survive hundreds of deliberately lethal casts.
	// Zero disables both retail match-ending rules for this guarded travel.
	Request.FragLimit = 0;
	Request.TimeLimit = 0;
	URL = FWoTFrontEndModel::BuildHostTravelURL(
		Request, GameInstance->GetSelectedFaction());
	if (URL.IsEmpty()) return false;
	// Guarded worlds load only through their neutral shells, as the host path
	// (UWoTSessionSubsystem) and map rotation (AWoTGameMode::TravelURLForMap)
	// already route. A direct /Game/WoT/Maps travel fails to load and returns
	// the host to the front end. Unguarded contexts keep the raw URL.
	if (const TOptional<FString> Shell = WoTGuardedShellPackageForMapIdentity(
			FWoTFrontEndModel::ShortMapName(MapPackage));
		Shell.IsSet())
	{
		int32 ShellOptionIndex = INDEX_NONE;
		URL.FindChar(TEXT('?'), ShellOptionIndex);
		URL = ShellOptionIndex == INDEX_NONE
			? Shell.GetValue() : Shell.GetValue() + URL.Mid(ShellOptionIndex);
	}
	while (GameInstance->GetNumLocalPlayers() > 1)
	{
		ULocalPlayer* Extra = GameInstance->GetLocalPlayerByIndex(
			GameInstance->GetNumLocalPlayers() - 1);
		if (!Extra || !GameInstance->RemoveLocalPlayer(Extra)) return false;
	}
	return World->ServerTravel(URL, /*bAbsolute*/ false);
}

bool WoTNetTest::Join(UWorld* World, const FString& Address)
{
	UGameInstance* GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
	UWoTSessionSubsystem* Sessions = GameInstance
		? GameInstance->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
	return Sessions && FWoTFrontEndModel::CanConstructNormalFrontEnd(GameInstance)
		&& Sessions->ConnectDirect(Address);
}

bool WoTNetTest::SetFaction(UWorld* World, const FString& FactionName, int32 PlayerIndex)
{
	UWoTGameInstance* WoTGameInstance = IsValid(World)
		? Cast<UWoTGameInstance>(World->GetGameInstance()) : nullptr;
	if (!WoTGameInstance)
	{
		return false;
	}
	struct FFactionByName
	{
		const TCHAR* Name;
		EWoTPlayerFaction Faction;
	};
	static const FFactionByName Factions[] = {
		{ TEXT("AesSedai"), EWoTPlayerFaction::AesSedai },
		{ TEXT("Forsaken"), EWoTPlayerFaction::Forsaken },
		{ TEXT("Hound"), EWoTPlayerFaction::Hound },
		{ TEXT("Whitecloak"), EWoTPlayerFaction::Whitecloak },
	};
	for (const FFactionByName& Entry : Factions)
	{
		if (FactionName.Equals(Entry.Name, ESearchCase::IgnoreCase))
		{
			if (PlayerIndex == 0)
			{
				WoTGameInstance->SetSelectedFaction(Entry.Faction, /*bSave*/ false);
				return WoTGameInstance->GetSelectedFaction() == Entry.Faction;
			}
			APlayerController* Controller = LocalPlayerController(World, PlayerIndex);
			AWoTGameMode* GameMode = World->GetAuthGameMode<AWoTGameMode>();
			AWoTPlayerState* State = Controller
				? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
			if (!Controller || !GameMode || !State) return false;
			const FString Options = FString::Printf(TEXT("?Class=WOTPawns.%s"), Entry.Name);
			State->SetFactionClass(AWoTGameMode::ResolvePlayerClassOption(Options));
			if (APawn* OldPawn = Controller->GetPawn())
			{
				Controller->UnPossess();
				OldPawn->Destroy();
			}
			GameMode->RestartPlayer(Controller);
			return IsValid(Controller->GetPawn())
				&& Controller->GetPawn()->GetClass() == State->FactionClass;
		}
	}
	return false;
}

bool WoTNetTest::CreatePuppets(UWorld* World, int32 Count,
	const TArray<FString>& Names, const TArray<FString>& Factions,
	TArray<TSharedPtr<FJsonValue>>& OutPuppets)
{
	OutPuppets.Reset();
	UGameInstance* GameInstance = IsValid(World) ? World->GetGameInstance() : nullptr;
	if (!GameInstance || World->GetNetMode() == NM_Client || Count < 0) return false;
	if (UGameViewportClient* Viewport = GameInstance->GetGameViewportClient())
	{
		Viewport->SetForceDisableSplitscreen(true);
		// UGameInstance::CreateLocalPlayer refuses players past MaxSplitscreenPlayers
		// (4 by default; GameInstance.cpp:869). Puppets are local players, so the
		// spell reels' hero + three Light Sphere carriers filled every slot and the
		// late joiner was refused. Real players join over the network and
		// never hit this cap; only the harness raises it.
		Viewport->MaxSplitscreenPlayers = FMath::Max(Viewport->MaxSplitscreenPlayers, Count + 1);
	}
	const int32 ExistingPuppets = GameInstance->GetNumLocalPlayers() - 1;
	while (GameInstance->GetNumLocalPlayers() < Count + 1)
	{
		FString Error;
		const int32 PlayerIndex = GameInstance->GetNumLocalPlayers();
		if (!GameInstance->CreateLocalPlayer(PlayerIndex, Error, true)) return false;
	}
	if (GameInstance->GetNumLocalPlayers() != Count + 1) return false;

	for (int32 PlayerIndex = 1; PlayerIndex <= Count; ++PlayerIndex)
	{
		APlayerController* Controller = LocalPlayerController(World, PlayerIndex);
		if (!Controller) return false;
		if (PlayerIndex > ExistingPuppets)
		{
			const FString Faction = Factions.IsValidIndex(PlayerIndex - 1)
				? Factions[PlayerIndex - 1] : TEXT("AesSedai");
			if (!SetFaction(World, Faction, PlayerIndex)) return false;
			Controller = LocalPlayerController(World, PlayerIndex);
			if (!Controller) return false;
			const FString Name = Names.IsValidIndex(PlayerIndex - 1)
				? Names[PlayerIndex - 1]
				: FString::Printf(TEXT("Puppet%d"), PlayerIndex);
			if (APlayerState* State = Controller->PlayerState)
			{
				State->SetPlayerName(Name);
			}
		}
		if (AHUD* HUD = Controller->GetHUD()) HUD->bShowHUD = false;
		APawn* Pawn = Controller->GetPawn();
		if (!Pawn) return false;
		TSharedRef<FJsonObject> Puppet = MakeShared<FJsonObject>();
		Puppet->SetNumberField(TEXT("p"), PlayerIndex);
		Puppet->SetStringField(TEXT("pawn"), Pawn->GetName());
		Puppet->SetStringField(TEXT("class"), Pawn->GetClass()->GetName());
		OutPuppets.Add(MakeShared<FJsonValueObject>(Puppet));
	}
	return true;
}

bool WoTNetTest::FacePoint(UWorld* World, const FVector& Point, int32 PlayerIndex)
{
	AWoTCharacter* Pawn = LocalPlayerPawn(World, PlayerIndex);
	if (!IsValid(Pawn))
	{
		return false;
	}
	AController* Controller = Pawn->GetController();
	if (!Controller)
	{
		return false;
	}
	const FVector Dir = Point - Pawn->GetActorLocation();
	if (Dir.IsNearlyZero())
	{
		return false;
	}
	// Yaw only — the pawn uses bUseControllerRotationYaw, so this turns it toward the
	// point; move x=1 then walks toward it (harness.walkto). Pitch/roll preserved.
	FRotator Ctrl = Controller->GetControlRotation();
	Ctrl.Yaw = Dir.Rotation().Yaw;
	Controller->SetControlRotation(Ctrl);
	return true;
}

bool WoTNetTest::FaceTarget(UWorld* World, const FString& PawnName, int32 PlayerIndex)
{
	if (!IsValid(World))
	{
		return false;
	}
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (It->GetName() == PawnName)
		{
			return FacePoint(World, It->GetActorLocation(), PlayerIndex);
		}
	}
	return false;
}

bool WoTNetTest::AimPoint(UWorld* World, const FVector& Point, int32 PlayerIndex)
{
	AWoTCharacter* Pawn = LocalPlayerPawn(World, PlayerIndex);
	if (!IsValid(Pawn))
	{
		return false;
	}
	AController* Controller = Pawn->GetController();
	if (!Controller)
	{
		return false;
	}
	// Aim FROM the eye (the weave's launch height) so the pitch accounts for the muzzle
	// sitting above the pawn centre: a yaw-only shot leaves pitch=0 and flies horizontally
	// over a same-height target's capsule. Yaw + Pitch; roll preserved. Movement is
	// yaw-driven (bUseControllerRotationYaw), so the added pitch does not affect walking.
	const FVector Dir = Point - Pawn->GetPawnViewLocation();
	if (Dir.IsNearlyZero())
	{
		return false;
	}
	const FRotator Look = Dir.Rotation();
	FRotator Ctrl = Controller->GetControlRotation();
	Ctrl.Yaw = Look.Yaw;
	Ctrl.Pitch = Look.Pitch;
	Controller->SetControlRotation(Ctrl);
	return true;
}

bool WoTNetTest::AimTarget(UWorld* World, const FString& PawnName, int32 PlayerIndex)
{
	if (!IsValid(World))
	{
		return false;
	}
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (It->GetName() == PawnName)
		{
			// Aim at the target's centre (actor origin = capsule centre).
			return AimPoint(World, It->GetActorLocation(), PlayerIndex);
		}
	}
	return false;
}

bool WoTNetTest::SpawnZone(UWorld* World, const FVector& At, const FVector& ZoneVelocity)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client)
	{
		return false;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTZoneVolume* Zone = World->SpawnActor<AWoTZoneVolume>(At, FRotator::ZeroRotator, Params);
	if (!Zone)
	{
		return false;
	}
	Zone->ZoneVelocity = ZoneVelocity;
	// A known extent so a walked-in pawn overlaps cleanly (half-extents 500 -> a
	// 1000uu box). Phase 2 zones get their extent from the compiled BSP.
	if (Zone->Collision)
	{
		Zone->Collision->SetBoxExtent(FVector(500.0f));
	}
	return true;
}

bool WoTNetTest::Teleport(UWorld* World, const FString& PawnName, const FVector& To)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client)
	{
		return false;
	}
	for (TActorIterator<AWoTCharacter> It(World); It; ++It)
	{
		if (It->GetName() == PawnName)
		{
			// Authority teleport: the autonomous client is corrected to the server
			// position on its next move. TeleportPhysics so the CMC doesn't interpolate.
			if (It->SetActorLocation(To, /*bSweep=*/false, nullptr,
				ETeleportType::TeleportPhysics))
			{
				UWoTZoneIndex::RefreshAfterRelocation(*It);
			}
			return true;
		}
	}
	return false;
}

int32 WoTNetTest::ClearProjectiles(UWorld* World)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client)
	{
		return -1;
	}
	TArray<AWoTProjectile*> Doomed;
	for (TActorIterator<AWoTProjectile> It(World); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed())
		{
			Doomed.Add(*It);
		}
	}
	for (AWoTProjectile* Projectile : Doomed)
	{
		Projectile->Destroy();
	}
	return Doomed.Num();
}

bool WoTNetTest::EndMatch(UWorld* World)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client)
	{
		return false;
	}
	AWoTGameMode* GameMode = World->GetAuthGameMode<AWoTGameMode>();
	if (!GameMode)
	{
		return false;
	}
	GameMode->EndGame(TEXT("harness"));
	return true;
}

bool WoTNetTest::ChangeTeam(UWorld* World, int32 Team, int32 PlayerIndex)
{
	APlayerController* PC = LocalPlayerController(World, PlayerIndex);
	AWoTPlayerController* WPC = Cast<AWoTPlayerController>(PC);
	if (!WPC)
	{
		return false;
	}
	// Server RPC — ships to the authority from a client, runs directly standalone.
	WPC->ServerChangeTeam(Team);
	return true;
}

bool WoTNetTest::OpenBattle(UWorld* World, const TArray<int32>& Teams)
{
	if (!IsValid(World) || World->GetNetMode() == NM_Client || Teams.IsEmpty())
	{
		return false;
	}
	AWoTBattleGameMode* GameMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!GameMode || !IsValid(GameMode->Battle))
	{
		return false;
	}
	for (int32 PlayerIndex = 0; PlayerIndex < Teams.Num(); ++PlayerIndex)
	{
		// ChangeTeam reports no refusal (range, full team, editing), so read
		// the resulting team back.
		if (!ChangeTeam(World, Teams[PlayerIndex], PlayerIndex))
		{
			return false;
		}
		const APlayerController* Controller = LocalPlayerController(World, PlayerIndex);
		const AWoTPlayerState* PlayerState = Controller
			? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
		if (!PlayerState || PlayerState->Team != Teams[PlayerIndex])
		{
			return false;
		}
	}
	if (GameMode->Battle->bTeleportersDisabled)
	{
		GameMode->EnableTeleporters();
	}
	return !GameMode->Battle->bTeleportersDisabled;
}

bool WoTNetTest::Screenshot(UWorld* World)
{
	if (!IsValid(World) || !World->GetGameViewport())
	{
		return false;   // no viewport (dedicated server) — nothing to grab
	}
	// bShowUI=true captures the final backbuffer including the HUD + Slate; the PNG lands in
	// Saved/Screenshots after the next frame renders (the caller sleeps before fetching it).
	FScreenshotRequest::RequestScreenshot(/*bShowUI=*/true);
	return true;
}

#endif
