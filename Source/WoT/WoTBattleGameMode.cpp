#include "WoTBattleGameMode.h"
#include "WoTFrontEnd.h"
#include "WoTRandom.h"

#include "WoTBattleInfo.h"
#include "WoTBudgetInfo.h"
#include "WoTCharacter.h"
#include "WoTCitadelGameState.h"
#include "WoTFrontEnd.h"
#include "WoTGameState.h"
#include "WoTFactionCharacters.h"
#include "WoTPlayerState.h"
#include "WoTPlayerController.h"
#include "WoTRetailText.h"
#include "WoTSealAltar.h"
#include "AI/WoTNPCCharacter.h"
#include "Projectiles/WoTMashadarGuide.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Inventory/WoTSeal.h"
#include "Inventory/WoTAngrealInvFireball.h"
#include "Inventory/WoTAngrealInvLightning.h"
#include "Inventory/WoTAngrealInvSoulBarb.h"
#include "Inventory/WoTAngrealInvShift.h"
#include "Inventory/WoTAngrealInvFork.h"
#include "Inventory/WoTAngrealInvWallOfAir.h"

#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	EWoTPlayerFaction WoTBattleFactionForClass(const UClass* FactionClass)
	{
		if (FactionClass && FactionClass->IsChildOf(AWoTForsaken::StaticClass()))
			return EWoTPlayerFaction::Forsaken;
		if (FactionClass && FactionClass->IsChildOf(AWoTHound::StaticClass()))
			return EWoTPlayerFaction::Hound;
		if (FactionClass && FactionClass->IsChildOf(AWoTWhitecloak::StaticClass()))
			return EWoTPlayerFaction::Whitecloak;
		return EWoTPlayerFaction::AesSedai;
	}

	FString WoTBattleCantChangeTeamsText()
	{
		if (FWoTRetailText::EnsureLoaded())
		{
			if (const FString* Value = FWoTRetailText::FindSectionValue(
				TEXT("WOTPlayer"), TEXT("CantChangeTeamsStr")))
			{
				return *Value;
			}
		}
		return TEXT("Team switching is unavailable during editing.");
	}

	// The team a pawn belongs to, via its PlayerState (giMPBattle reads the pawn's PRI —
	// giMPBattle.uc:449,461,480). A PRI-less pawn evaluates team 0 there (Accessed None
	// -> 0): damageable and pickup-allowed, NOT neutral-immune — so no-PlayerState -> 0.
	// Convention decision point: future creature pawns without a PlayerState read as
	// team 0 by this rule (revisit when NPCs land).
	uint8 TeamOfPawn(APawn* P)
	{
		const AWoTPlayerState* PS = P ? P->GetPlayerState<AWoTPlayerState>() : nullptr;
		return PS ? PS->Team : uint8(0);
	}
}

AWoTBattleGameMode::AWoTBattleGameMode()
{
	// giMPBattle defaultproperties: the citadel GRI + the Battle map family.
	GameStateClass = AWoTCitadelGameState::StaticClass();
	MapPrefix = TEXT("Battle2_");
	// giMPBattle defaultproperties: spawn players in their team area (base flag, default false).
	bSpawnInTeamArea = true;
	// bChangeLevels=True is already the base default. bTeamGame is a member (declared true).
	// The shared native HUD routes Battle's overview; the match-time CitadelScoreBoard
	// remains the distinct scoreboard-ledger item SB-D3.

	// giMPBattle.uc:522-527 AngrealSet1/2 — the per-spawn random Battle loadout.
	AngrealSet1[0] = AWoTAngrealInvFireball::StaticClass();
	AngrealSet1[1] = AWoTAngrealInvLightning::StaticClass();
	AngrealSet1[2] = AWoTAngrealInvSoulBarb::StaticClass();
	AngrealSet2[0] = AWoTAngrealInvShift::StaticClass();
	AngrealSet2[1] = AWoTAngrealInvFork::StaticClass();
	AngrealSet2[2] = AWoTAngrealInvWallOfAir::StaticClass();
}

void AWoTBattleGameMode::InitGame(const FString& MapName, const FString& Options,
	FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	RequestedSealGoal = UGameplayStatics::GetIntOption(
		Options, TEXT("SealGoal"), 4);
	RequestedSealBudget = UGameplayStatics::GetIntOption(
		Options, TEXT("SealBudget"), 3);
	const int32 Citadels = FMath::Clamp(
		FWoTFrontEndModel::BattleCitadelCount(
			TEXT("/Game/WoT/Maps/") + FWoTFrontEndModel::ShortMapName(MapName)),
		2, 4);
	bAutoTeamSelect = UGameplayStatics::GetIntOption(
		Options, TEXT("AutoTeamSelect"), bAutoTeamSelect ? 1 : 0) != 0;
	MaxTeamSize = FMath::Clamp(UGameplayStatics::GetIntOption(
		Options, TEXT("MaxTeamSize"), MaxTeamSize), 1, 16 / Citadels);
	if (bAutoTeamSelect)
	{
		MaxPlayers = Citadels * MaxTeamSize;
	}
	else
	{
		MaxPlayers = FMath::Clamp(MaxPlayers, Citadels, 16);
		MaxTeamSize = FMath::Min(MaxTeamSize, MaxPlayers);
	}
}

void AWoTBattleGameMode::SetPlayerDefaults(APawn* PlayerPawn)
{
	AGameModeBase::SetPlayerDefaults(PlayerPawn);   // engine hook only

	// UE1 keeps one WOTPlayer pawn across death, so LeaveTeam's Style/collision state
	// naturally survives a respawn. UE5 RestartPlayer replaces the pawn: derive the
	// persistent neutral-player state from the possessed controller's PlayerState here.
	// bTeleportingDisabled and bShowOverview already live on that controller and are not
	// rewritten by possession, so only the fresh pawn's presentation/collision needs work.
	if (TeamOfPawn(PlayerPawn) == 255)
	{
		if (AWoTCharacter* WoTPawn = Cast<AWoTCharacter>(PlayerPawn))
		{
			WoTPawn->SetNeutralGhost(true);
			if (UCapsuleComponent* Capsule = WoTPawn->GetCapsuleComponent();
				IsValid(Capsule))
			{
				Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
			}
		}
	}

	// giMPBattle.AddDefaultInventory (giMPBattle.uc:184-195): the editing phase
	// grants NO loadout — EnableTeleporters re-arms everyone when play begins.
	if (Battle && Battle->bTeleportersDisabled)
	{
		return;
	}
	GrantBattleLoadout(PlayerPawn);
}

void AWoTBattleGameMode::GrantBattleLoadout(APawn* PlayerPawn)
{
	// Retail grant order — Dart(25), a Set1 roll, a Set2 roll, then Super up to
	// giWOT's faction default (Air Pulse). Inventory order drives selection
	// cycling, so the base's Dart->AirBurst path is unrolled here to keep it.
	// GivePawnAngreal dedups per class, so re-arming an already-armed pawn is safe.
	if (DefaultAngreal)
	{
		GivePawnAngreal(PlayerPawn, DefaultAngreal, DefaultAngrealCharges);
	}
	// FRand()*(ArrayCount-0.001) — a uniform 0..2 pick.
	if (TSubclassOf<AWoTAngrealInventory> Pick1 = AngrealSet1[WoTRandom::RandHelper(3)])
	{
		GivePawnAngreal(PlayerPawn, Pick1, 0);
	}
	if (TSubclassOf<AWoTAngrealInventory> Pick2 = AngrealSet2[WoTRandom::RandHelper(3)])
	{
		GivePawnAngreal(PlayerPawn, Pick2, 0);
	}
	if (const AWoTCharacter* WoTPawn = Cast<AWoTCharacter>(PlayerPawn))
	{
		if (WoTPawn->DefaultAngrealInventory)
		{
			GivePawnAngreal(PlayerPawn, WoTPawn->DefaultAngrealInventory, 0);
		}
	}
}

void AWoTBattleGameMode::LocateBattle()
{
	// Locate the battle configuration data placed in this level (AllActors class'BattleInfo' ->
	// break: take the first). Push MaxTeamSize onto it so clients replicate the cap.
	if (Battle)
	{
		return;
	}
	if (TActorIterator<AWoTBattleInfo> It(GetWorld()); It)
	{
		Battle = *It;
		Battle->MaxTeamSize = MaxTeamSize;
	}
}

void AWoTBattleGameMode::BeginPlay()
{
	// giMPBattle.PostBeginPlay: Super first (giCombatBase seeds the clock + starts Timer).
	Super::BeginPlay();
	PrepareBattle();
}

void AWoTBattleGameMode::PrepareBattle()
{
	if (bBattlePrepared)
	{
		return;
	}
	bBattlePrepared = true;
	LocateBattle();
	if (!Battle)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("%s: unable to locate a BattleInfo in this level."), *GetName());
	}

	ApplyRequestedSealSettings();
	ResetBattle();
}

int32 AWoTBattleGameMode::ResolveCitadelCount() const
{
	if (IsValid(Battle) && Battle->NumTeams >= 2 && Battle->NumTeams <= 4)
	{
		return Battle->NumTeams;
	}
	TSet<uint8> AltarTeams;
	for (TActorIterator<AWoTSealAltar> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && It->Team < 4)
		{
			AltarTeams.Add(It->Team);
		}
	}
	return FMath::Clamp(AltarTeams.Num(), 2, 4);
}

void AWoTBattleGameMode::ApplyRequestedSealSettings()
{
	FWoTHostRequest Settings;
	Settings.SealGoal = RequestedSealGoal;
	Settings.SealBudget = RequestedSealBudget;
	FWoTFrontEndModel::UpdateSealGoal(
		Settings, Settings.SealGoal, ResolveCitadelCount());
	SealBudget = Settings.SealBudget;
	if (AWoTCitadelGameState* GS = GetGameState<AWoTCitadelGameState>())
	{
		GS->SealGoal = Settings.SealGoal;
	}
}

bool AWoTBattleGameMode::ChangeTeam(AController* Controller, int32 Team)
{
	// WOTPlayer.ServerChangeTeam rejects changes while that controller owns an edit grant
	// and tells the requester why (CantChangeTeamsStr, WOTPlayer.uc:4228-4232 / WoT.int:48).
	// Checked here too: not every ChangeTeam caller routes through the controller RPC.
	if (AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(Controller))
	{
		if (!WoTPC->CanChangeTeamNow())
		{
			WoTPC->ClientCenterMessage(WoTBattleCantChangeTeamsText());
			return true;
		}
	}

	// giMPBattle.ChangeTeam assumes Battle exists; the port warns and reports no
	// change so a misconfigured level does not crash headless. Direct -game boots
	// reach here from PostLogin before BeginPlay has run, so locate on demand first.
	LocateBattle();
	if (!ensure(Battle))
	{
		return true;
	}

	// The controller carries the PlayerState in the WOTPlayer split. Not a WoT player -> the
	// original warns and returns true.
	AWoTPlayerState* PS = Controller ? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!PS)
	{
		return true;
	}

	// Validate a request only when it selects a playable team (giMPBattle.uc:318-339).
	if (Team != 255)
	{
		// out of range -> reject, no change (giMPBattle.uc:321-325). `>= 4` is the same
		// C++-OOB cap on the NumPlayers[4] index that IsBattlefieldClear applies
		// (BattleInfo's NumTeams <= 4 validation is non-fatal).
		if (Team < 0 || Team >= Battle->NumTeams || Team >= 4)
		{
			return true;
		}
		// already on this team -> no change + tell the requester (giMPBattle.uc:328-332,
		// LeftMessage TeamIsYoursStr $ Team).
		if (Team == PS->Team)
		{
			if (AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(Controller))
			{
				WoTPC->ClientLeftMessage(TeamIsYoursStr + FString::FromInt(Team));
			}
			return true;
		}
		// A full destination is the one rejection reported as false
		// (giMPBattle.uc:335-339; TeamIsFilledStr plus the team number).
		if (Battle->NumPlayers[Team] >= MaxTeamSize)
		{
			if (AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(Controller))
			{
				WoTPC->ClientLeftMessage(TeamIsFilledStr + FString::FromInt(Team));
			}
			return false;
		}
	}

	// Debit the old team's population before assigning the new team
	// (giMPBattle.uc:343-347).
	// (`< 4` caps the NumPlayers[4] index — see IsBattlefieldClear.)
	if (PS->Team < Battle->NumTeams && PS->Team < 4)
	{
		Battle->NumPlayers[PS->Team]--;
	}

	// Store the requested team, including the neutral sentinel (giMPBattle.uc:350).
	PS->Team = static_cast<uint8>(Team);

	if (Team != 255)
	{
		Battle->NumPlayers[Team]++;
		JoinTeam(Controller, Team);
		// giMPBattle.uc:357 composes the player's name, PlayerJoinTeamStr, and the
		// chosen team's description. Retail reads Player.TeamDescription, copied from
		// TeamClass[Team]'s CDO — GetTeamDescription(Team) is the same source string.
		BroadcastLeftMessage(PS->GetPlayerName() + PlayerJoinTeamStr +
			Battle->GetTeamDescription(static_cast<uint8>(Team)));
	}
	else
	{
		LeaveTeam(Controller);
		// giMPBattle.uc:362 appends the loaded PlayerLeaveTeamStr to the player's name.
		BroadcastLeftMessage(PS->GetPlayerName() + PlayerLeaveTeamStr);
	}

	return true;
}

void AWoTBattleGameMode::BroadcastLeftMessage(const FString& Message) const
{
	// WOTPlayer.BroadcastLeftMessage (WOTPlayer.uc:1832-1840): every player's left queue.
	for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It))
		{
			It->ClientLeftMessage(Message);
		}
	}
}

int32 AWoTBattleGameMode::GetSmallestTeam() const
{
	// giMPBattle.GetSmallestTeam: fewest players wins; a tie keeps the lower index.
	if (!Battle || Battle->NumTeams <= 0)
	{
		return 0;
	}
	int32 MinTeam = 0;
	int32 MinCount = Battle->NumPlayers[0];
	for (int32 i = 1; i < Battle->NumTeams && i < 4; ++i)
	{
		if (Battle->NumPlayers[i] < MinCount)
		{
			MinTeam = i;
			MinCount = Battle->NumPlayers[i];
		}
	}
	return MinTeam;
}

void AWoTBattleGameMode::PostLogin(APlayerController* NewPlayer)
{
	// giMPBattle.Login: auto-select the smallest team when configured, else join neutral (255,
	// which routes through LeaveTeam). The port assigns team in PostLogin; skip the base's
	// free-for-all ChangeTeam(255) and go straight to the engine PostLogin (which spawns the
	// pawn), then assign — so the pawn exists for JoinTeam's deferred ServerChangeClass.
	// Retail's PostBeginPlay (the BattleInfo, the seal settings and ResetBattle's editing
	// phase) always preceded Login; a listen host's LoadMap logs in before BeginPlay.
	PrepareBattle();
	AGameModeBase::PostLogin(NewPlayer);
	// A BattleInfo that appeared after the setup ran is still found on demand, so
	// GetSmallestTeam reads real team counts.
	LocateBattle();
	const int32 InTeam = bAutoTeamSelect ? GetSmallestTeam() : 255;
	ChangeTeam(NewPlayer, InTeam);
	RecordClientLifecycle(NewPlayer, /*bLeaving*/ false);
}

void AWoTBattleGameMode::Logout(AController* Exiting)
{
	// giMPBattle.Logout: drop the leaving player from their team's headcount. A
	// no-team controller is the port's spectator-equivalent and is excluded.
	// Citadel teardown must run before Super::Logout invalidates controller/PlayerState state.
	if (AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(Exiting))
	{
		WoTPC->EndEditSession();
	}

	// (`< 4` caps the NumPlayers[4] index — see IsBattlefieldClear.)
	AWoTPlayerState* PS = Exiting ? Exiting->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (Battle && PS && PS->Team < Battle->NumTeams && PS->Team < 4)
	{
		Battle->NumPlayers[PS->Team]--;
		PS->Team = 255;
	}

	Super::Logout(Exiting);
}

void AWoTBattleGameMode::RestartGame()
{
	// giMPBattle.RestartGame: reset the battle, then the base rotation/reload.
	// The base uses relative ServerTravel, so the incoming seal options remain
	// in LastURL across ?Cycle map rotation with the other match rules.
	ResetBattle();
	Super::RestartGame();
}

void AWoTBattleGameMode::Timer()
{
	Super::Timer();

	// giMPBattle.Timer (giMPBattle.uc:210-233): once play has begun, count neutral players
	// down once per second and place anyone who reaches zero on the smallest team.
	if (CanTeleport())
	{
		for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
		{
			AWoTPlayerController* WoTController = *It;
			AWoTPlayerState* WoTState = WoTController
				? WoTController->GetPlayerState<AWoTPlayerState>() : nullptr;
			if (!WoTController || !WoTController->GetPawn() || !WoTState ||
				WoTState->Team != 255 || WoTController->JoinTeamTimeout <= 0)
			{
				continue;
			}
			--WoTController->JoinTeamTimeout;
			if (WoTController->JoinTeamTimeout == 0)
			{
				ChangeTeam(WoTController, GetSmallestTeam());
			}
		}
	}

	// giMPBattle.uc:235-250: after editing has changed the field, an empty
	// server resets it for whoever joins next; this is independent of CanTeleport.
	if (!IsBattlefieldClear())
	{
		int32 PlayerCount = 0;
		for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
		{
			if (IsValid(*It))
			{
				++PlayerCount;
			}
		}
		if (PlayerCount == 0)
		{
			UE_LOG(LogTemp, Log,
				TEXT("No players remain after field setup; resetting the match."));
			RestartGame();
		}
	}
}

int32 AWoTBattleGameMode::ReduceDamage(int32 Damage, FName /*DamageType*/,
                                       AWoTCharacter* Injured, APawn* InstigatedBy)
{
	// giMPBattle.ReduceDamage dereferences Battle directly; off a configured battle, no team
	// rules apply (defensive — the original relies on the placed BattleInfo always existing).
	if (!Battle)
	{
		return Damage;
	}

	// During citadel setup, suppress pawn-instigated damage (giMPBattle.uc:441-444).
	if (Battle->bTeleportersDisabled && InstigatedBy != nullptr)
	{
		return 0;
	}

	// A neutral participant cannot deal or receive damage (giMPBattle.uc:447-450).
	const uint8 InjuredTeam = TeamOfPawn(Injured);
	if (InjuredTeam == 255 || (InstigatedBy != nullptr && TeamOfPawn(InstigatedBy) == 255))
	{
		return 0;
	}

	// (giMPBattle.uc:455 neutral-zone immunity deferred — "not implemented" in the original.)

	// Friendly-fire damage scaling: same-team hits are scaled by FriendlyFireScale.
	// giMPBattle.uc:463 returns `float(Damage) * FriendlyFireScale` into an int —
	// UnrealScript float->int coercion truncates toward zero.
	if (InstigatedBy != nullptr && InjuredTeam == TeamOfPawn(InstigatedBy))
	{
		return FMath::TruncToInt(Damage * FriendlyFireScale);
	}

	return Damage;
}

void AWoTBattleGameMode::SetEditing(int32 Team, bool bOn)
{
	// giMPBattle.SetEditing: shadow WOTPlayer.bEditing on the BattleInfo.
	if (HasAuthority() && Battle && Team >= 0 && Team < 4)
	{
		Battle->bEditing[Team] = bOn ? 1 : 0;
	}
}

bool AWoTBattleGameMode::IsEditing(int32 Team) const
{
	return Battle && Team >= 0 && Team < 4 && Battle->bEditing[Team] == 1;
}

void AWoTBattleGameMode::EnableTeamReady(int32 Team)
{
	// giMPBattle.EnableTeamReady: this team has finished editing.
	if (HasAuthority() && Battle && Team >= 0 && Team < 4)
	{
		Battle->bEditingComplete[Team] = 1;
	}
}

bool AWoTBattleGameMode::IsBattlefieldClear() const
{
	// giMPBattle.IsBattlefieldClear: true while no team has readied up (all bEditingComplete 0).
	if (!Battle)
	{
		return true;
	}
	// The `i < 4` bound is a C++-OOB safety cap on the bEditingComplete[4] read — identical to the
	// original's `i < NumTeams` for valid data (BattleInfo validates NumTeams <= 4), but its
	// validation is non-fatal, so it guards the read if invalid map data slips a larger NumTeams in.
	for (int32 i = 0; i < Battle->NumTeams && i < 4; ++i)
	{
		if (Battle->bEditingComplete[i] != 0)
		{
			return false;
		}
	}
	return true;
}

bool AWoTBattleGameMode::CanTeleport() const
{
	// giMPBattle.CanTeleport: teleporters work once the editing phase is over.
	return Battle && !Battle->bTeleportersDisabled;
}

void AWoTBattleGameMode::EnableTeleporters()
{
	// giMPBattle.EnableTeleporters: leave the editing phase — teleporters on + the match clock
	// resumes. JoinTeamTimeout is ported here; the remaining per-pawn re-arm loop
	// (teleport flag, overview off, AddDefaultInventory, begin-battle message/sound)
	// stays deferred with the editor UI + Battle loadout.
	if (!HasAuthority())
	{
		return;
	}
	// A phase edge is also an editor-exit edge. End authority grants first so the
	// owner-only mirror restores palettes/HUDs before normal play is enabled.
	for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It))
		{
			It->EndEditSession();
		}
	}
	if (Battle)
	{
		Battle->bTeleportersDisabled = false;
	}
	if (AWoTGameState* GS = GetGameState<AWoTGameState>())
	{
		GS->bStopCountDown = false;
	}
	for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
	{
		AWoTPlayerController* Player = *It;
		if (!IsValid(Player))
		{
			continue;
		}
		// giMPBattle.uc:161-176 per-player loop: teamed players are un-frozen, drop the
		// overview, and get the battle loadout; everyone gets the join-nag timer re-armed
		// plus the begin-battle message + conch.
		const AWoTPlayerState* PS = Player->GetPlayerState<AWoTPlayerState>();
		if (PS && PS->Team != 255)
		{
			Player->bTeleportingDisabled = false;
			Player->bShowOverview = false;
			if (APawn* Pawn = Player->GetPawn())
			{
				GrantBattleLoadout(Pawn);
			}
		}
		Player->JoinTeamTimeout = MaxSpectatorPeriod;
		Player->ClientCenterMessage(BattleBeginStr);
		// giMPBattle.GetBeginBattleSound: AmbientA.ConchShell2, owner-local.
		Player->ClientPlayUISound(FName(TEXT("AmbientA/ConchShell2")), 1.0f);
	}
}

void AWoTBattleGameMode::EndGame(FName Reason)
{
	// giMPBattle.EndGame: retail walks the PawnList for PlayerPawns with a
	// PlayerReplicationInfo and ClientPlaySounds WOT.EndGameSounds.WinGame to the
	// WinningTeam, LoseGame to everyone else (neutral players included).
	if (Reason == FName(TEXT("AltarFilled")))
	{
		for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
		{
			AWoTPlayerController* Player = *It;
			const AWoTPlayerState* PS = IsValid(Player)
				? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
			if (!PS)
			{
				continue;
			}
			Player->ClientPlayUISound(PS->Team == WinningTeam
				? FName(TEXT("WOT/WinGame")) : FName(TEXT("WOT/LoseGame")), 1.0f);
		}
	}
	Super::EndGame(Reason);
}

void AWoTBattleGameMode::AssessBattlefield()
{
	if (!HasAuthority() || !IsValid(Battle.Get()) || CanTeleport())
	{
		return;
	}
	for (TActorIterator<AWoTBudgetInfo> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && It->SealBudgetRemaining() > 0)
		{
			return;
		}
	}

	const int32 TeamCount = FMath::Min(Battle->NumTeams, 4);
	for (int32 TeamIndex = 0; TeamIndex < TeamCount; ++TeamIndex)
	{
		if (IsEditing(TeamIndex))
		{
			return;
		}
	}
	EnableTeleporters();
}

uint8 AWoTBattleGameMode::NearestAltarTeam(const FVector& Loc) const
{
	return NearestAltarTeamInWorld(GetWorld(), Loc);
}

uint8 AWoTBattleGameMode::NearestAltarTeamInWorld(
	const UWorld* World, const FVector& Loc)
{
	uint8 BestTeam = 255;
	double BestDistanceSq = TNumericLimits<double>::Max();
	if (!World)
	{
		return BestTeam;
	}
	for (TActorIterator<AWoTSealAltar> It(World); It; ++It)
	{
		if (!IsValid(*It))
		{
			continue;
		}
		const double DistanceSq = FVector::DistSquared(Loc, It->GetActorLocation());
		if (DistanceSq < BestDistanceSq)
		{
			BestDistanceSq = DistanceSq;
			BestTeam = It->Team;
		}
	}
	return BestTeam;
}

AWoTBudgetInfo* AWoTBattleGameMode::FindTeamBudget(uint8 Team) const
{
	AWoTBudgetInfo* FoundBudget = nullptr;
	for (TActorIterator<AWoTBudgetInfo> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && It->Team == Team)
		{
			if (FoundBudget)
			{
				// A session must bind to exactly one authoritative budget for its team.
				return nullptr;
			}
			FoundBudget = *It;
		}
	}
	return FoundBudget;
}

AWoTSealAltar* AWoTBattleGameMode::FindTeamAltar(uint8 Team) const
{
	for (TActorIterator<AWoTSealAltar> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && It->Team == Team)
		{
			return *It;
		}
	}
	return nullptr;
}

void AWoTBattleGameMode::RegisterCitadelDeployedTroop(
	AWoTNPCCharacter* Troop, uint8 Team)
{
	if (!HasAuthority() || !IsValid(Troop) || Team >= UE_ARRAY_COUNT(CitadelDeployedTroops))
	{
		return;
	}
	UnregisterCitadelDeployedTroop(Troop);
	CitadelDeployedTroops[Team].Add(Troop);
}

void AWoTBattleGameMode::UnregisterCitadelDeployedTroop(AWoTNPCCharacter* Troop)
{
	if (!HasAuthority() || !Troop)
	{
		return;
	}
	const TWeakObjectPtr<AWoTNPCCharacter> Key(Troop);
	for (TArray<TWeakObjectPtr<AWoTNPCCharacter>>& TeamTroops : CitadelDeployedTroops)
	{
		TeamTroops.RemoveAll([Troop](const TWeakObjectPtr<AWoTNPCCharacter>& Entry)
		{
			return !Entry.IsValid() || Entry.Get() == Troop;
		});
	}
	CitadelTroopMashadarGuides.Remove(Key);
}

bool AWoTBattleGameMode::IsCitadelDeployedTroop(
	const AWoTNPCCharacter* Troop, uint8 ExpectedTeam) const
{
	if (!IsValid(Troop))
	{
		return false;
	}
	const int32 FirstTeam = ExpectedTeam < UE_ARRAY_COUNT(CitadelDeployedTroops)
		? static_cast<int32>(ExpectedTeam) : 0;
	const int32 LastTeam = ExpectedTeam < UE_ARRAY_COUNT(CitadelDeployedTroops)
		? FirstTeam : UE_ARRAY_COUNT(CitadelDeployedTroops) - 1;
	for (int32 TeamIndex = FirstTeam; TeamIndex <= LastTeam; ++TeamIndex)
	{
		for (const TWeakObjectPtr<AWoTNPCCharacter>& Entry
			: CitadelDeployedTroops[TeamIndex])
		{
			if (Entry.Get() == Troop)
			{
				return true;
			}
		}
	}
	return false;
}

void AWoTBattleGameMode::GetCitadelDeployedTroops(
	uint8 Team, TArray<AWoTNPCCharacter*>& OutTroops) const
{
	OutTroops.Reset();
	if (Team >= UE_ARRAY_COUNT(CitadelDeployedTroops))
	{
		return;
	}
	for (const TWeakObjectPtr<AWoTNPCCharacter>& Entry : CitadelDeployedTroops[Team])
	{
		if (AWoTNPCCharacter* Troop = Entry.Get())
		{
			OutTroops.Add(Troop);
		}
	}
}

void AWoTBattleGameMode::SetCitadelTroopMashadarGuide(
	AWoTNPCCharacter* Troop, AWoTMashadarGuide* Guide)
{
	if (!HasAuthority() || !IsValid(Troop)
		|| !IsCitadelDeployedTroop(Troop))
	{
		return;
	}
	const TWeakObjectPtr<AWoTNPCCharacter> Key(Troop);
	if (IsValid(Guide))
	{
		CitadelTroopMashadarGuides.Add(Key, Guide);
	}
	else
	{
		CitadelTroopMashadarGuides.Remove(Key);
	}
}

AWoTMashadarGuide* AWoTBattleGameMode::GetCitadelTroopMashadarGuide(
	const AWoTNPCCharacter* Troop) const
{
	if (!IsValid(Troop))
	{
		return nullptr;
	}
	const TWeakObjectPtr<AWoTMashadarGuide>* Found =
		CitadelTroopMashadarGuides.Find(
			TWeakObjectPtr<AWoTNPCCharacter>(const_cast<AWoTNPCCharacter*>(Troop)));
	return Found ? Found->Get() : nullptr;
}

bool AWoTBattleGameMode::AddMashadarSlot(uint8 Team, int32 MinHealth,
	int32 MaxHealth)
{
	if (!HasAuthority() || Team >= UE_ARRAY_COUNT(MashadarSlots)) return false;
	TArray<FMashadarSlot>& Slots = MashadarSlots[Team];
	FMashadarSlot* CreatedSlot = nullptr;
	if (Slots.Num() < 256)
	{
		CreatedSlot = &Slots.AddDefaulted_GetRef();
	}
	else
	{
		// MashadarManager.AddMashadar recycles a dead inactive entry after the
		// fixed 256-slot array has filled.
		CreatedSlot = Slots.FindByPredicate([](const FMashadarSlot& Slot)
		{
			return !Slot.ActiveGuide.IsValid() && Slot.Health <= 0;
		});
	}
	if (!CreatedSlot) return false;
	// Retail calls int(RandRange(float,float)); truncation reaches the upper bound
	// only on the inclusive 1.0 draw (1 time in 32768, as in retail).
	CreatedSlot->Health = FMath::TruncToInt(WoTRandom::FRandRange(
		static_cast<float>(MinHealth), static_cast<float>(MaxHealth)));
	CreatedSlot->ActiveGuide.Reset();
	return true;
}

bool AWoTBattleGameMode::RequestMashadarActivation(AWoTMashadarGuide* Guide)
{
	if (!HasAuthority() || !IsValid(Guide) || Guide->Team >= 4) return false;
	TArray<FMashadarSlot>& Slots = MashadarSlots[Guide->Team];
	for (FMashadarSlot& Slot : Slots)
	{
		if (!Slot.ActiveGuide.IsValid()) Slot.ActiveGuide.Reset();
		if (Slot.ActiveGuide.Get() == Guide)
		{
			Guide->SetInstigator(FindMashadarFocus(Guide->Team));
			Guide->MashadarHealth = Slot.Health;
			return Slot.Health > 0;
		}
	}
	for (FMashadarSlot& Slot : Slots)
	{
		if (!Slot.ActiveGuide.IsValid() && Slot.Health > 0)
		{
			Slot.ActiveGuide = Guide;
			Guide->SetInstigator(FindMashadarFocus(Guide->Team));
			Guide->MashadarHealth = Slot.Health;
			return true;
		}
	}
	return false;
}

void AWoTBattleGameMode::ReleaseMashadarActivation(AWoTMashadarGuide* Guide)
{
	if (!HasAuthority() || !Guide || Guide->Team >= 4) return;
	for (FMashadarSlot& Slot : MashadarSlots[Guide->Team])
	{
		if (Slot.ActiveGuide.Get() == Guide)
		{
			Slot.ActiveGuide.Reset();
			Guide->SetInstigator(nullptr);
			return;
		}
	}
}

void AWoTBattleGameMode::UpdateMashadarHealth(AWoTMashadarGuide* Guide,
	int32 Health)
{
	if (!HasAuthority() || !Guide || Guide->Team >= 4) return;
	for (FMashadarSlot& Slot : MashadarSlots[Guide->Team])
	{
		if (Slot.ActiveGuide.Get() == Guide)
		{
			Slot.Health = FMath::Max(Health, 0);
			return;
		}
	}
}

AWoTNPCCharacter* AWoTBattleGameMode::FindMashadarFocus(uint8 Team) const
{
	if (!HasAuthority() || Team >= 4) return nullptr;
	AWoTNPCCharacter* Focus = nullptr;
	for (const TWeakObjectPtr<AWoTNPCCharacter>& Entry : CitadelDeployedTroops[Team])
	{
		AWoTNPCCharacter* Candidate = Entry.Get();
		if (!Candidate || Candidate->CreatureRowName != TEXT("MashadarFocusPawn"))
			continue;
		Focus = Candidate;
		break;
	}
	return Focus;
}

void AWoTBattleGameMode::ConsumeMashadarFocus(AWoTMashadarGuide* Guide,
	uint8 Team)
{
	AWoTNPCCharacter* Focus = FindMashadarFocus(Team);
	if (Focus)
	{
		UnregisterCitadelDeployedTroop(Focus);
		Focus->Destroy();
	}
}

bool AWoTBattleGameMode::PickupQuery(APawn* Other, AWoTInventory* Item) const
{
	// giMPBattle.PickupQuery: no pickups for neutral players, an editing player, or anyone before
	// play begins (teleporters disabled). The "is editing" check reads the team shadow
	// Battle->bEditing[team] — WOTPlayer.bEditing (per-player editor input state) isn't ported;
	// PlayerEditLimit=1 makes the shadow track the single editing player per team.
	const uint8 Team = TeamOfPawn(Other);
	if (Team == 255
		|| (Battle && Team < 4 && Battle->bEditing[Team] == 1)
		|| (Battle && Battle->bTeleportersDisabled))
	{
		return false;
	}
	return Super::PickupQuery(Other, Item);
}

void AWoTBattleGameMode::ResetBattle()
{
	// giMPBattle.ResetBattle: reset every altar to the seal goal, every budget to the seal
	// budget, destroy stray seals, then disable teleporters for the editing phase.
	const AWoTCitadelGameState* GS = GetGameState<AWoTCitadelGameState>();
	const int32 Goal = GS ? GS->SealGoal : 4;
	for (TActorIterator<AWoTSealAltar> It(GetWorld()); It; ++It)
	{
		It->Reset(Goal);
	}
	for (TActorIterator<AWoTBudgetInfo> It(GetWorld()); It; ++It)
	{
		It->Reset(SealBudget);
	}
	// Collect first — destroying actors while iterating the level's actor list is unsafe.
	TArray<AWoTSeal*> StraySeals;
	for (TActorIterator<AWoTSeal> It(GetWorld()); It; ++It)
	{
		StraySeals.Add(*It);
	}
	for (AWoTSeal* S : StraySeals)
	{
		if (IsValid(S))
		{
			S->Destroy();
		}
	}

	DisableTeleporters();
}

void AWoTBattleGameMode::DisableTeleporters()
{
	// giMPBattle.DisableTeleporters (giMPBattle.uc:132-146): freeze every player's
	// teleporting, then the mode + clock flags (match clock frozen during editing).
	for (TActorIterator<AWoTPlayerController> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It))
		{
			It->bTeleportingDisabled = true;
		}
	}
	if (Battle)
	{
		Battle->bTeleportersDisabled = true;
	}
	if (AWoTGameState* GS = GetGameState<AWoTGameState>())
	{
		GS->bStopCountDown = true;
	}
}

void AWoTBattleGameMode::JoinTeam(AController* Controller, int32 Team)
{
	// giMPBattle.JoinTeam (giMPBattle.uc:253-278): ServerChangeClass(Battle.TeamClass[Team])
	// — the faction-pawn morph (appearance onto the EXISTING pawn; a later respawn spawns
	// the class itself via GetDefaultPawnClassForController) — then the teleporter flag
	// folded to the current phase, the overview auto-show while play hasn't begun (else the
	// immediate re-arm), and the return to the real world (collision + style un-ghost).
	// EnterCitadel() is assert-only budget binding in the original — the port binds the
	// budget at CitadelBeginEdit instead (Slice 1), so there is nothing to do here.
	if (!IsValid(Battle) || Team < 0 || Team >= 4 || !Battle->TeamClass[Team])
	{
		return;
	}
	// ServerChangeClass updates PRI.PlayerType even when there is no current pawn to
	// morph (for example, a team choice during the respawn window). Keep the persistent
	// controller identity correct before touching the optional pawn presentation.
	if (AWoTPlayerState* PS = IsValid(Controller)
		? Controller->GetPlayerState<AWoTPlayerState>() : nullptr)
	{
		PS->SetFactionClass(Battle->TeamClass[Team]);
	}
	AWoTCharacter* Pawn = IsValid(Controller)
		? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;
	if (IsValid(Pawn))
	{
		Pawn->SetAppearanceClass(Battle->TeamClass[Team]);
	}

	AWoTPlayerController* WoTController = Cast<AWoTPlayerController>(Controller);
	if (IsValid(WoTController))
	{
		WoTController->ClientApplyCitadelSkin(
			WoTBattleFactionForClass(Battle->TeamClass[Team]));
		WoTController->bTeleportingDisabled = Battle->bTeleportersDisabled;
		// Play already begun -> straight to the field; else the overview shows until it does.
		WoTController->bShowOverview = Battle->bTeleportersDisabled;
	}
	if (!Battle->bTeleportersDisabled && IsValid(Pawn))
	{
		GrantBattleLoadout(Pawn);
	}

	// JoinTeam restores ordinary collision and rendering (giMPBattle.uc:334-350).
	if (IsValid(Pawn))
	{
		Pawn->SetNeutralGhost(false);
		if (UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent(); IsValid(Capsule))
		{
			Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		}
	}
}

UClass* AWoTBattleGameMode::GetDefaultPawnClassForController_Implementation(
	AController* InController)
{
	// giMPBattle: a team member's pawn IS the faction class (BattleInfo.TeamClass[Team]).
	// Login rebuilds Options before Super.Login, but passes its incoming SpawnClass through
	// unchanged (giMPBattle.uc:315-349). Therefore a neutral player keeps the URL-selected
	// faction pawn/ghost until JoinTeam morphs it to BattleInfo.TeamClass[Team].
	if (Battle && InController)
	{
		if (const AWoTPlayerState* PS = InController->GetPlayerState<AWoTPlayerState>())
		{
			if (PS->Team < Battle->NumTeams && PS->Team < 4 && Battle->TeamClass[PS->Team])
			{
				return Battle->TeamClass[PS->Team];
			}
		}
	}
	return Super::GetDefaultPawnClassForController_Implementation(InController);
}

void AWoTBattleGameMode::LeaveTeam(AController* Controller)
{
	// giMPBattle.LeaveTeam (giMPBattle.uc:280-302): LeaveCitadel (drop the budget binding +
	// strip the inventory), freeze the neutral player's teleporting, auto-show the overview,
	// and ghost them — SetCollision(true,false,false) + replicated STY_Modulated look.
	// If play has begun, arm the join nag.
	AWoTPlayerController* WoTController = Cast<AWoTPlayerController>(Controller);
	AWoTCharacter* Pawn = IsValid(Controller)
		? Cast<AWoTCharacter>(Controller->GetPawn()) : nullptr;

	// WOTPlayer.LeaveCitadel (WOTPlayer.uc:4429-4445): destroy all NON-SEAL
	// inventory — a carried seal survives going neutral (!Inv.IsA('Seal'), live
	// since the G13 carry loop). The budget binding is per-edit-session in the
	// port (Slice 1).
	if (IsValid(Pawn))
	{
		if (UWoTInventoryComponent* Inv =
			Pawn->FindComponentByClass<UWoTInventoryComponent>(); IsValid(Inv))
		{
			TArray<AWoTInventory*> ToDrop(Inv->Items);
			for (AWoTInventory* Item : ToDrop)
			{
				if (IsValid(Item) && !Item->IsA<AWoTSeal>())
				{
					Inv->DeleteInventory(Item);
					Item->Destroy();
				}
			}
		}
	}

	if (IsValid(WoTController))
	{
		WoTController->bTeleportingDisabled = true;
		WoTController->bShowOverview = true;
	}

	// The neutral ghost: pawns pass through it (bBlockActors/bBlockPlayers false).
	if (IsValid(Pawn))
	{
		Pawn->SetNeutralGhost(true);
		if (UCapsuleComponent* Capsule = Pawn->GetCapsuleComponent(); IsValid(Capsule))
		{
			Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		}
	}

	// If gameplay has begun, encourage the player to (re)join a team.
	if (CanTeleport() && IsValid(WoTController))
	{
		WoTController->JoinTeamTimeout = MaxSpectatorPeriod;
	}
}
