#include "WoTGameMode.h"
#include "WoTFrontEnd.h"
#include "WoTRandom.h"

#include "WoTCharacter.h"
#include "WoTEvents.h"
#include "WoTFactionCharacters.h"
#include "WoTGameState.h"
#include "WoTGameSession.h"
#include "WoTHUD.h"
#include "WoTLeafMap.h"
#include "WoTPlayerController.h"
#include "WoTPlayerStart.h"
#include "WoTPlayerState.h"
#include "WoTPreferencesSubsystem.h"
#include "WoTPasswordToken.h"
#include "WoTUE1Collision.h"
#include "WoTSessionSubsystem.h"
#include "AI/WoTNPCCharacter.h"
#include "AI/WoTCasterComponent.h"
#include "AI/WoTPathNodeIterator.h"
#include "AI/WoTSight.h"
#include "Inventory/WoTInventory.h"
#include "Inventory/WoTInventoryComponent.h"
#include "Inventory/WoTAngrealInventory.h"
#include "Inventory/WoTAngrealInvDart.h"
#include "Projectiles/WoTLegionProjectile.h"
#include "Traps/WoTTrap.h"
#include "World/WoTMapDisplay.h"
#include "World/WoTWorldMaterializer.h"
#include "Online/WoTInternetPlay.h"
#include "WoTBootstrapHeartbeat.h"
#include "WoTRetailText.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/NetConnection.h"
#include "Modules/ModuleManager.h"

#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "EngineUtils.h"
#include "Engine/NetConnection.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/GameSession.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

// A preceding unity translation unit may have included <windows.h>.
#ifdef FormatMessage
#undef FormatMessage
#endif

namespace
{
	void WoTGameMode_RecordClientLifecycle(AWoTGameMode* GameMode,
		APlayerController* Player, bool bLeaving)
	{
		UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
		if (World == nullptr || Player == nullptr
			|| World->GetNetMode() != NM_ListenServer || Player->IsLocalController())
		{
			return;
		}
		int32 RemoteClientCount = 0;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator();
			It; ++It)
		{
			APlayerController* Candidate = It->Get();
			if (IsValid(Candidate) && !Candidate->IsLocalController()
				&& (!bLeaving || Candidate != Player))
			{
				++RemoteClientCount;
			}
		}
		const APlayerState* State = Player->PlayerState;
		const FUniqueNetIdPtr UniqueId = State
			? State->GetUniqueId().GetUniqueNetId() : nullptr;
		const FString PlayerId = UniqueId.IsValid()
			? UniqueId->ToString() : FString();
		const UWoTSessionSubsystem* Sessions = World->GetGameInstance()
			? World->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>() : nullptr;
		const UNetDriver* Driver = World->GetNetDriver();
		WoTRecordBootstrapHeartbeat(
			bLeaving ? TEXT("client-leave") : TEXT("client-join"),
			EWoTBootstrapPhase::Ready, 0, {}, {}, FString::Printf(
				TEXT("player=%s provider=%s netdriver=%s count=%d"),
				*FWoTEOSConfig::RedactId(PlayerId),
				Sessions ? *Sessions->HostProviderForTelemetry() : TEXT("none"),
				Driver ? *Driver->GetClass()->GetName() : TEXT("none"),
				RemoteClientCount));
	}

	bool WoTGameMode_IsPersonalTravelOption(const FString& Option)
	{
		FString Key;
		if (!Option.Split(TEXT("="), &Key, nullptr))
		{
			Key = Option;
		}
		// FURL accepts ?-Key as an inherited-option removal directive. It must not let
		// a server clear a remote player's personal default either.
		if (Key.StartsWith(TEXT("-")))
		{
			Key.RightChopInline(1);
		}
		return Key.Equals(TEXT("Class"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("Skin"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("Face"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("Team"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("Name"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("Password"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("PasswordToken"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("AdminPassword"), ESearchCase::IgnoreCase)
			|| Key.Equals(TEXT("GamePassword"), ESearchCase::IgnoreCase);
	}

	// The REAL team of a pawn for scoring — giWOT.OnSameTeam compares pawn PRI teams
	// (giWOT.uc:181-192), reading the true PlayerReplicationInfo.Team, NOT the disguised
	// apparent team (that substitution is only for NPC faction/targeting checks). An NPC
	// presents its AssignedTeam (the original mirrored the summoner's team into its PRI);
	// a player presents PlayerState.Team; anything else is neutral/unassigned (255).
	uint8 RealTeamOfPawn(const APawn* P)
	{
		if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(P))
		{
			return NPC->AssignedTeam;
		}
		if (const AWoTPlayerState* PS = P ? P->GetPlayerState<AWoTPlayerState>() : nullptr)
		{
			return PS->Team;
		}
		return uint8(255);
	}

	FString WoTRulesText(const TCHAR* Section, const TCHAR* Key,
		const TCHAR* Fallback)
	{
		if (FWoTRetailText::EnsureLoaded())
		{
			if (const FString* Value = FWoTRetailText::FindSectionValue(
				Section, Key))
			{
				return *Value;
			}
		}
		return FString(Fallback);
	}

	FString WoTVoteText(const TCHAR* Key, const TCHAR* Fallback)
	{
		return WoTRulesText(TEXT("giWOT"), Key, Fallback);
	}

	bool WoTGameMode_IsSafeBanField(const FString& Value, int32 MaxLength)
	{
		if (Value.Len() > MaxLength)
		{
			return false;
		}
		for (TCHAR Character : Value)
		{
			if (Character < 0x21 || Character > 0x7e
				|| Character == TEXT('='))
			{
				return false;
			}
		}
		return true;
	}

	FString WoTGameMode_FindBanField(
		const TArray<FString>& Lines, const FString& Field)
	{
		const FString Prefix = Field + TEXT("=");
		FString Found;
		int32 Count = 0;
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				++Count;
				Found = Line.Mid(Prefix.Len());
			}
		}
		return Count == 1 ? Found : FString();
	}
}

AWoTGameMode::AWoTGameMode()
{
	GameSessionClass = AWoTGameSession::StaticClass();
	PrimaryActorTick.bCanEverTick = true;
	bPauseable = false; // giMP.uc:85
	// The shipped Arena spawns each player's menu-chosen faction class. This remains the
	// retail DefaultPlayerClass fallback when ?Class= is absent/invalid; the per-player
	// choice is selected by GetDefaultPawnClassForController. Battle overrides per team.
	DefaultPawnClass = AWoTAesSedai::StaticClass();
	PlayerControllerClass = AWoTPlayerController::StaticClass();
	PlayerStateClass = AWoTPlayerState::StaticClass();
	GameStateClass = AWoTGameState::StaticClass();
	// N6 debug read-out HUD (AWoTBattleGameMode inherits it). A text HUD binds no
	// assets, so it wires straight to the class — no leaf BP.
	HUDClass = AWoTHUD::StaticClass();
	// giMPArena's starting artifact (giMPArena.uc:11): the Dart, 25 charges.
	DefaultAngreal = AWoTAngrealInvDart::StaticClass();
	VoteRequests.SetNum(12);
}

bool AWoTGameMode::TrySetPlayerPause(APlayerController* RequestingPlayer,
	ENetMode NetMode)
{
	const AWoTPlayerState* State = RequestingPlayer
		? RequestingPlayer->GetPlayerState<AWoTPlayerState>() : nullptr;
	// giMP retains GameInfo's third pause leg: a server administrator may pause
	// a network match even when the ruleset itself is not pauseable.
	if (!RequestingPlayer || (!bPauseable && NetMode != NM_Standalone
		&& !(State && State->bAdmin)))
	{
		return false;
	}
	if (GetWorld() && GetWorld()->IsPaused())
	{
		return RequestingPlayer->SetPause(false);
	}
	if (AWoTPlayerController* WoTPlayer =
		Cast<AWoTPlayerController>(RequestingPlayer))
	{
		return RequestingPlayer->SetPause(true,
			FCanUnpause::CreateUObject(WoTPlayer,
				&AWoTPlayerController::CanUnpauseOwnedMatch));
	}
	return RequestingPlayer->SetPause(true);
}

bool AWoTGameMode::AllowPausing(APlayerController* PC)
{
	const AWoTPlayerState* State = PC
		? PC->GetPlayerState<AWoTPlayerState>() : nullptr;
	return Super::AllowPausing(PC) || (State && State->bAdmin);
}

TSubclassOf<AWoTCharacter> AWoTGameMode::ResolvePlayerClassOption(
	const FString& Options)
{
	FString ClassName = UGameplayStatics::ParseOption(Options, TEXT("Class"));
	ClassName.TrimStartAndEndInline();
	int32 PackageSeparator = INDEX_NONE;
	if (ClassName.FindLastChar(TEXT('.'), PackageSeparator))
	{
		const FString PackageName = ClassName.Left(PackageSeparator);
		if (!PackageName.Equals(TEXT("WOTPawns"), ESearchCase::IgnoreCase))
		{
			return AWoTAesSedai::StaticClass();
		}
		ClassName.RightChopInline(PackageSeparator + 1);
	}

	if (ClassName.Equals(TEXT("Forsaken"), ESearchCase::IgnoreCase))
	{
		return AWoTForsaken::StaticClass();
	}
	if (ClassName.Equals(TEXT("Hound"), ESearchCase::IgnoreCase))
	{
		return AWoTHound::StaticClass();
	}
	if (ClassName.Equals(TEXT("Whitecloak"), ESearchCase::IgnoreCase))
	{
		return AWoTWhitecloak::StaticClass();
	}
	// The explicit AesSedai form, an absent option, and every invalid class all
	// resolve to GameInfo.DefaultPlayerClass rather than accepting arbitrary pawns.
	return AWoTAesSedai::StaticClass();
}

FString AWoTGameMode::StripPersonalOptionsFromTravelURL(const FString& URL)
{
	// Keep the URL spelling/order stable: only remove complete option fields. The
	// fragment is outside the option list and must survive even when its preceding
	// option is personal.
	int32 FragmentIndex = INDEX_NONE;
	URL.FindChar(TEXT('#'), FragmentIndex);
	const FString Fragment = FragmentIndex == INDEX_NONE
		? FString() : URL.Mid(FragmentIndex);
	const FString URLWithoutFragment = FragmentIndex == INDEX_NONE
		? URL : URL.Left(FragmentIndex);

	int32 FirstOptionIndex = INDEX_NONE;
	if (!URLWithoutFragment.FindChar(TEXT('?'), FirstOptionIndex))
	{
		return URL;
	}

	FString Result = URLWithoutFragment.Left(FirstOptionIndex);
	TArray<FString> Options;
	URLWithoutFragment.Mid(FirstOptionIndex + 1).ParseIntoArray(
		Options, TEXT("?"), /*bCullEmpty*/ false);
	for (const FString& Option : Options)
	{
		if (!WoTGameMode_IsPersonalTravelOption(Option))
		{
			Result += TEXT("?");
			Result += Option;
		}
	}
	return Result + Fragment;
}

void AWoTGameMode::ProcessServerTravel(const FString& URL, bool bAbsolute)
{
	// UE will merge URL with the server's LastURL before its virtual client fan-out.
	// Retain retail's unmerged request for remote clients; Super still owns every part
	// of URL validation, seamless/non-seamless setup, and the listen host's merged URL.
	RemoteClientTravelURL = StripPersonalOptionsFromTravelURL(URL);
	bHasRemoteClientTravelURL = true;
	Super::ProcessServerTravel(URL, bAbsolute);
	bHasRemoteClientTravelURL = false;
	RemoteClientTravelURL.Reset();
}

FString AWoTGameMode::InitNewPlayer(APlayerController* NewPlayerController,
	const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal)
{
	// Login has already spawned the controller and its PlayerState, but PostLogin has
	// not yet called HandleStartingNewPlayer. Set both faction class and PlayerType
	// before Super can return control to the first-spawn path.
	if (AWoTPlayerState* PS = NewPlayerController
		? NewPlayerController->GetPlayerState<AWoTPlayerState>() : nullptr)
	{
		PS->SetFactionClass(ResolvePlayerClassOption(Options));
		PS->bAdmin = IsAdminPassword(
			UGameplayStatics::ParseOption(Options, TEXT("PasswordToken")));
		const FString Identity = StableIdentityKey(UniqueId);
		FString LoginAddress;
		if (!Identity.IsEmpty())
		{
			PendingLoginAddresses.RemoveAndCopyValue(Identity, LoginAddress);
		}
		if (LoginAddress.IsEmpty())
		{
			if (UNetConnection* Connection = NewPlayerController
				? NewPlayerController->GetNetConnection() : nullptr)
			{
				LoginAddress = Connection->LowLevelGetRemoteAddress(
					/*bAppendPort*/ false);
			}
		}
		if (!LoginAddress.IsEmpty())
		{
			PlayerLoginAddresses.Add(NewPlayerController,
				LoginAddress.Left(128));
		}
	}
	// GameInfo.Login: InSkin = ParseOption(Options, "Skin") -> SetMultiSkin at login.
	// No pawn exists yet in the UE5 split — the controller
	// carries the string and PossessedBy runs the SetMultiSkin gate per spawn.
	// Battle drops this option before Super.Login (giMPBattle.uc:403-415).
	if (ShouldRecordLoginSkinOption())
	{
		if (AWoTPlayerController* WoTController =
			Cast<AWoTPlayerController>(NewPlayerController))
		{
			WoTController->PersonalSkinURL =
				UGameplayStatics::ParseOption(Options, TEXT("Skin"));
		}
	}
	return Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
}

void AWoTGameMode::ChangeName(AController* Other, const FString& S,
	bool bNameChange)
{
	APlayerState* ChangingState = Other ? Other->PlayerState : nullptr;
	if (!ChangingState || S.IsEmpty()
		|| ChangingState->GetPlayerName().Equals(S, ESearchCase::IgnoreCase))
	{
		return;
	}

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		It; ++It)
	{
		const APlayerController* Candidate = It->Get();
		const APlayerState* CandidateState = Candidate ? Candidate->PlayerState : nullptr;
		if (Candidate != Other && CandidateState
			&& CandidateState->GetPlayerName().Equals(S, ESearchCase::IgnoreCase))
		{
			if (AWoTPlayerController* WoTController = Cast<AWoTPlayerController>(Other))
			{
				WoTController->ClientTypedMessage(S + WoTRulesText(TEXT("giCombatBase"),
					TEXT("NoNameChange"), TEXT(" is unavailable")), NAME_None);
			}
			return;
		}
	}

	if (bNameChange)
	{
		BroadcastTypedMessage(ChangingState->GetPlayerName() + WoTRulesText(
			TEXT("giCombatBase"), TEXT("GlobalNameChange"),
			TEXT(" is now known as ")) + S, NAME_None);
	}
	ChangingState->SetPlayerName(S);
}

void AWoTGameMode::InitGame(const FString& MapName, const FString& Options,
	FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	// giWOT/giCombatBase (and giMPBattle) localized text from the retail WoT.int.
	FWoTRetailText::ApplyLocalizedDefaults(this);

	// giCombatBase.InitGame: URL-driven match rules. Runs before BeginPlay, so
	// SeedMatchClock sees the parsed TimeLimit.
	// GameInfo.InitGame: Difficulty is a URL option too; the
	// shipped GameInfo default is 1.
	Difficulty = UGameplayStatics::GetIntOption(
		Options, TEXT("Difficulty"), Difficulty);
	FragLimit = UGameplayStatics::GetIntOption(Options, TEXT("FragLimit"), FragLimit);
	TimeLimit = UGameplayStatics::GetIntOption(Options, TEXT("TimeLimit"), TimeLimit);
	bChangeLevels =
		UGameplayStatics::GetIntOption(Options, TEXT("Cycle"), bChangeLevels ? 1 : 0) != 0;
	MaxPlayers = FMath::Clamp(UGameplayStatics::GetIntOption(
		Options, TEXT("MaxPlayers"), MaxPlayers), 2, 16);
	MaxClientTickRate = FWoTFrontEndModel::NormalizeMaxClientTickRate(
		UGameplayStatics::GetIntOption(
			Options, TEXT("MaxClientTickRate"), MaxClientTickRate));
	if (UGameplayStatics::HasOption(Options, TEXT("AdminPassword")))
	{
		FString Sanitized;
		if (!FWoTFrontEndModel::SanitizeHostPassword(
			UGameplayStatics::ParseOption(Options, TEXT("AdminPassword")),
			Sanitized))
		{
			ErrorMessage = TEXT("The host settings contain an invalid admin password.");
		}
		else
		{
			HostAdminPassword = MoveTemp(Sanitized);
		}
	}
	if (UGameplayStatics::HasOption(Options, TEXT("GamePassword")))
	{
		FString Sanitized;
		if (!FWoTFrontEndModel::SanitizeHostPassword(
			UGameplayStatics::ParseOption(Options, TEXT("GamePassword")),
			Sanitized))
		{
			ErrorMessage = TEXT("The host settings contain an invalid game password.");
		}
		else
		{
			HostGamePassword = MoveTemp(Sanitized);
		}
	}
	if ((!UGameplayStatics::HasOption(Options, TEXT("AdminPassword"))
			|| !UGameplayStatics::HasOption(Options, TEXT("GamePassword")))
		&& GetGameInstance())
	{
		if (const UWoTSessionSubsystem* Sessions =
			GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>())
		{
			FString ActiveAdminPassword;
			FString ActiveGamePassword;
			Sessions->GetActiveHostPasswords(
				ActiveAdminPassword, ActiveGamePassword);
			if (!UGameplayStatics::HasOption(Options, TEXT("AdminPassword")))
			{
				HostAdminPassword = MoveTemp(ActiveAdminPassword);
			}
			if (!UGameplayStatics::HasOption(Options, TEXT("GamePassword")))
			{
				HostGamePassword = MoveTemp(ActiveGamePassword);
			}
		}
	}
	LoadHostBans();
}

FString AWoTGameMode::StableIdentityKey(const FUniqueNetIdRepl& UniqueId)
{
	const FUniqueNetIdPtr Id = UniqueId.GetUniqueNetId();
	if (!Id.IsValid())
	{
		return FString();
	}
	const FString Result = Id->GetType().ToString() + TEXT(":") + Id->ToString();
	return WoTGameMode_IsSafeBanField(Result, 192) ? Result : FString();
}

void AWoTGameMode::LoadHostBans()
{
	HostBans.Reset();
#if WITH_AUTOMATION_TESTS
	if (GIsAutomationTesting
		&& !UWoTPreferencesSubsystem::HasPreferencePathForTesting())
	{
		return;
	}
#endif
	FString Path;
	TArray<uint8> Bytes;
	if (!UWoTPreferencesSubsystem::SideFilePath(TEXT("WoTHostBans"), Path)
		|| !UWoTPreferencesSubsystem::LoadSideFileBytes(
			Path, 16384, Bytes) || Bytes.IsEmpty())
	{
		return;
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return;
		}
	}
	TArray<ANSICHAR> Terminated;
	Terminated.Append(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		Bytes.Num());
	Terminated.Add('\0');
	const FString Text = UTF8_TO_TCHAR(Terminated.GetData());
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
	if (!Lines.Contains(TEXT("Schema=WoTHostBans-v1")))
	{
		return;
	}
	for (int32 Index = 0; Index < 50; ++Index)
	{
		const FString Identity = WoTGameMode_FindBanField(Lines,
			FString::Printf(TEXT("Ban%dIdentity"), Index));
		const FString Address = WoTGameMode_FindBanField(Lines,
			FString::Printf(TEXT("Ban%dAddress"), Index));
		if ((!Identity.IsEmpty()
			&& !WoTGameMode_IsSafeBanField(Identity, 192))
			|| (!Address.IsEmpty()
				&& !WoTGameMode_IsSafeBanField(Address, 128)))
		{
			continue;
		}
		if (!Identity.IsEmpty() || !Address.IsEmpty())
		{
			HostBans.Add({Identity, Address});
		}
	}
}

bool AWoTGameMode::SaveHostBans() const
{
#if WITH_AUTOMATION_TESTS
	if (GIsAutomationTesting
		&& !UWoTPreferencesSubsystem::HasPreferencePathForTesting())
	{
		return false;
	}
#endif
	FString Path;
	if (!UWoTPreferencesSubsystem::SideFilePath(TEXT("WoTHostBans"), Path))
	{
		return false;
	}
	FString Text(TEXT("Schema=WoTHostBans-v1\n"));
	for (int32 Index = 0; Index < HostBans.Num() && Index < 50; ++Index)
	{
		Text += FString::Printf(TEXT("Ban%dIdentity=%s\nBan%dAddress=%s\n"),
			Index, *HostBans[Index].Identity, Index, *HostBans[Index].Address);
	}
	FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return UWoTPreferencesSubsystem::SaveSideFileBytes(Path, Bytes);
}

bool AWoTGameMode::IsHostBanned(const FUniqueNetIdRepl& UniqueId,
	const FString& Address) const
{
	const FString Identity = StableIdentityKey(UniqueId);
	return HostBans.ContainsByPredicate(
		[&Identity, &Address](const FWoTHostBan& Ban)
		{
			return (!Identity.IsEmpty() && !Ban.Identity.IsEmpty()
				&& Identity.Equals(Ban.Identity, ESearchCase::CaseSensitive))
				|| (!Address.IsEmpty() && !Ban.Address.IsEmpty()
					&& Address.Equals(Ban.Address, ESearchCase::IgnoreCase));
		});
}

void AWoTGameMode::PreLogin(const FString& Options, const FString& Address,
	const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	const FString PasswordToken = UGameplayStatics::ParseOption(
		Options, TEXT("PasswordToken"));
	const bool bCapacityOverridePassword =
		IsCapacityOverridePassword(PasswordToken);
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	// AGameSession has its own generic capacity gate. Retail lets either host
	// password override capacity, so consume only that exact engine refusal and
	// let the rule-specific check below make the final admission decision.
	if (ErrorMessage == TEXT("Server full.") && bCapacityOverridePassword)
	{
		ErrorMessage.Reset();
	}
	// Retail applies its host block policy last: a blocked player remains
	// refused, but the block reason replaces an earlier capacity refusal.
	if (IsHostBanned(UniqueId, Address))
	{
		ErrorMessage = WoTRulesText(TEXT("GameInfo"), TEXT("IPBanned"),
			TEXT("This host has blocked this player."));
		return;
	}
	if (!ErrorMessage.IsEmpty()) return;
	int32 CurrentPlayers = 0;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		It; ++It)
	{
		if (IsValid(It->Get())) ++CurrentPlayers;
	}
	if (CurrentPlayers >= MaxPlayers && !bCapacityOverridePassword)
	{
		ErrorMessage = TEXT("The server has reached its player limit.");
		return;
	}
	if (!HostGamePassword.IsEmpty() && !bCapacityOverridePassword)
	{
		ErrorMessage = PasswordToken.IsEmpty()
			? TEXT("NEEDPW") : TEXT("WRONGPW");
		return;
	}
	const FString Identity = StableIdentityKey(UniqueId);
	if (!Identity.IsEmpty())
	{
		PendingLoginAddresses.Add(Identity, Address.Left(128));
	}
}

bool AWoTGameMode::IsAdminPassword(const FString& CandidateToken) const
{
	return !HostAdminPassword.IsEmpty()
		&& CandidateToken.Equals(WoTPasswordToken::Derive(HostAdminPassword),
			ESearchCase::CaseSensitive);
}

bool AWoTGameMode::IsCapacityOverridePassword(
	const FString& CandidateToken) const
{
	return IsAdminPassword(CandidateToken)
		|| (!HostGamePassword.IsEmpty()
			&& CandidateToken.Equals(WoTPasswordToken::Derive(HostGamePassword),
				ESearchCase::CaseSensitive));
}

bool AWoTGameMode::AdminLogin(
	AWoTPlayerController* Player, const FString& Password)
{
	AWoTPlayerState* State = Player
		? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
	// Unlike login admission, retail's explicit AdminLogin compares exactly.
	if (!State || HostAdminPassword.IsEmpty()
		|| !Password.Equals(HostAdminPassword, ESearchCase::CaseSensitive))
	{
		return false;
	}
	State->bAdmin = true;
	State->ForceNetUpdate();
	BroadcastTypedMessage(State->GetPlayerName()
		+ TEXT(" now has host moderation privileges."), NAME_None);
	return true;
}

void AWoTGameMode::AdminLogout(AWoTPlayerController* Player)
{
	AWoTPlayerState* State = Player
		? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!State || HostAdminPassword.IsEmpty() || !State->bAdmin)
	{
		return;
	}
	State->bAdmin = false;
	State->ForceNetUpdate();
	BroadcastTypedMessage(State->GetPlayerName()
		+ TEXT(" no longer has host moderation privileges."), NAME_None);
}

bool AWoTGameMode::ExecuteAdminCommand(
	AWoTPlayerController* Player, const FString& Command)
{
	const AWoTPlayerState* State = Player
		? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!State || !State->bAdmin)
	{
		return false;
	}
	FString Normalized;
	if (!NormalizeVoteRequest(Command, Normalized))
	{
		return false;
	}
	// Deliberately bounded to the four moderation/travel verbs shared with the
	// vote path. Remote admins never receive arbitrary host-console execution.
	ExecuteVoteRequest(Normalized, /*NumPlayers*/ 2);
	return true;
}

APlayerController* AWoTGameMode::ProcessClientTravel(FString& URL,
	bool bSeamless, bool bAbsolute)
{
	// AGameModeBase::ProcessServerTravel has already merged the relative request with
	// the server's LastURL. Retail instead sends the unmerged request to remote players
	// and adds the listen player's defaults only to Level.NextURL. Filtering only the
	// remote request recreates that ownership split without replacing UE's travel setup.
	APlayerController* LocalPlayerController = nullptr;
	const FString FallbackRemoteURL = StripPersonalOptionsFromTravelURL(URL);
	const FString& RemoteURL = bHasRemoteClientTravelURL
		? RemoteClientTravelURL : FallbackRemoteURL;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		It; ++It)
	{
		APlayerController* PlayerController = It->Get();
		if (Cast<UNetConnection>(PlayerController->Player) != nullptr)
		{
			PlayerController->ClientTravel(RemoteURL, TRAVEL_Relative, bSeamless);
		}
		else
		{
			LocalPlayerController = PlayerController;
			PlayerController->PreClientTravel(URL,
				bAbsolute ? TRAVEL_Absolute : TRAVEL_Relative, bSeamless);
		}
	}
	return LocalPlayerController;
}

UClass* AWoTGameMode::GetDefaultPawnClassForController_Implementation(
	AController* InController)
{
	if (const AWoTPlayerState* PS = InController
		? InController->GetPlayerState<AWoTPlayerState>() : nullptr)
	{
		if (PS->FactionClass)
		{
			return PS->FactionClass.Get();
		}
	}
	return Super::GetDefaultPawnClassForController_Implementation(InController);
}

void AWoTGameMode::BeginPlay()
{
	Super::BeginPlay();
	if (MaxClientTickRate > 0)
	{
		if (UNetDriver* Driver = GetWorld() ? GetWorld()->GetNetDriver() : nullptr)
		{
			Driver->SetNetServerMaxTickRate(MaxClientTickRate);
		}
	}

	// giCombatBase.PostBeginPlay: seed the match clock. RemainingTime counts down in
	// GameState::Tick; the flag tells the HUD to show remaining vs elapsed.
	SeedMatchClock();

	// giWOT.Map is the last map the host selected; the retail host menu saved the map
	// it launched (menuStartArenaServer.uc:141-146) and each rotation saved the next
	// (giCombatBase.uc:216-221), so it names the running level. A guarded world is a
	// neutral shell: use its adopted retail identity.
	if (Map.IsEmpty())
	{
		Map = WoTWorldMapIdentity(GetWorld());
	}

	// The base GameInfo runs a 1 Hz Timer; giCombatBase.Timer polls the clock from it.
	GetWorldTimerManager().SetTimer(
		MatchTimerHandle, this, &AWoTGameMode::Timer, 1.0f, /*loop*/ true);
}

bool AWoTGameMode::PickupQuery(APawn* Other, AWoTInventory* Item) const
{
	// giWOT.uc:208-226 inventory-hint branch is RETAIL-DEAD: giWOT ships
	// bShowInventoryHints/bHintOnly true, but no shipped class or map authors the
	// required WOTInventory.bShowInfoHint flag (T3D census 2026-07-20).
	const UWoTInventoryComponent* Inv =
		Other->FindComponentByClass<UWoTInventoryComponent>();
	return !Inv || !Inv->HandlePickupQuery(Item);
}

bool AWoTGameMode::ShouldRespawn(AWoTInventory* Item) const
{
	if (GetNetMode() == NM_Standalone)
	{
		return false;
	}
	return Item && Item->RespawnTime != 0.0f;
}

void AWoTGameMode::PlayerKilled(AController* Killer, AWoTCharacter* Victim, FName /*DamageType*/)
{
	if (!Victim || !HasAuthority())
	{
		return;
	}
	AController* VictimController = Victim->GetController();
	ScoreKill(Killer, Victim);

	// Legion.Killed (Legion.uc:469-484): put down every homing spirit locked onto the
	// victim so none hunt its respawn (the Level.Game.Killed seam; LegionProjectiles are
	// Legion-owned, so this is equivalent to the per-Legion AllActors scan).
	AWoTLegionProjectile::PutDownSeekersTargeting(GetWorld(), Victim);
	for (TActorIterator<AWoTCharacter> It(GetWorld()); It; ++It)
	{
		if (UWoTCasterComponent* Caster =
			It->FindComponentByClass<UWoTCasterComponent>())
		{
			Caster->NotifyTargetKilled(Victim);
		}
	}

	// giCombatBase.Killed: first to FragLimit ends the match. Uses the ORIGINAL killer — a
	// killer-less environment death returns early here even if ScoreKill credited a
	// SuicideInstigator (matching giCombatBase.Killed's `if (killer == None) return`).
	if (!bGameEnded && FragLimit > 0 && Killer)
	{
		if (const AWoTPlayerState* KillerPS = Killer->GetPlayerState<AWoTPlayerState>())
		{
			if (KillerPS->CalcScore() >= FragLimit)
			{
				EndGame(TEXT("fraglimit"));
			}
		}
	}
	// Pawn.Died returns immediately once EndGame has set bGameEnded, before the
	// retail pawn reaches Dying/unpossession. Keep the frag-limit victim possessed
	// so GameEnded retains its pawn and end-camera anchor.
	if (bGameEnded)
	{
		return;
	}

	// Free the victim's controller from the corpse and arm WOTPlayer.Dying's Fire gate.
	// ONLY player controllers respawn: a dead NPC keeps its AI controller (its brain
	// diverts to Dying) and the carcass-swap path destroys pawn + controller after
	// DestroyDelaySecs — routing an AWoTNPCController through UnPossess+RestartPlayer
	// would conjure a default pawn at a PlayerStart (the Slice 4 recon latent bug).
	AWoTPlayerController* PlayerController = Cast<AWoTPlayerController>(VictimController);
	if (!PlayerController)
	{
		return;
	}
	// The optional death recap's one fact: the item that killed this player, sent to the
	// victim alone and whatever this server's own settings. A client without the
	// onboarding aids receives it and does nothing.
	const FName KillingItem = KillingItemFor(Killer, *Victim);
	if (!KillingItem.IsNone())
	{
		PlayerController->ClientKilledBy(KillingItem);
	}
	PlayerController->UnPossess();
	PlayerController->ArmRespawnFire(Victim, RespawnDelay);
}

AController* AWoTGameMode::ActiveSuicideInstigator(const AWoTCharacter& VictimPawn) const
{
	// SuicideInstigator holds the instigator's controller (the persistent identity),
	// so it survives the caster's own pawn being replaced on respawn within the window.
	AController* InstigatorController = VictimPawn.SuicideInstigator.Get();
	if (InstigatorController
		&& VictimPawn.SuicideInstigationTime + SuicideInstigationRelevance
			> GetWorld()->GetTimeSeconds())
	{
		return InstigatorController;
	}
	return nullptr;
}

FName AWoTGameMode::KillingItemFor(const AController* Killer,
	const AWoTCharacter& Victim) const
{
	if (!Victim.KillingItemName.IsNone())
	{
		return Victim.KillingItemName;
	}
	// A fall or a hazard soon after a Whirlwind or a Swap Places names the displacing
	// angreal, on the same terms ScoreKill credits its caster with the kill.
	if (!Killer && ActiveSuicideInstigator(Victim))
	{
		return UWoTInventoryComponent::GetHandSetItemClassName(
			Victim.SuicideInstigatorItemClass);
	}
	return NAME_None;
}

void AWoTGameMode::ScoreKill(AController* Killer, AWoTCharacter* Victim)
{
	// giWOT.ScoreKill. (Team scoring deferred: Arena is a free-for-all.)
	AController* VictimController = Victim ? Victim->GetController() : nullptr;
	AWoTPlayerState* VictimPS =
		VictimController ? VictimController->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!VictimPS)
	{
		return;
	}
	ScoreKillIdentity(Killer, VictimController, Victim, VictimPS,
		VictimPS->GetPlayerName(), VictimPS->Team, Victim && Victim->bIsFemale);
}

void AWoTGameMode::ScoreKillIdentity(AController* Killer,
	AController* VictimController, AWoTCharacter* VictimPawn,
	AWoTPlayerState* VictimState, const FString& VictimName,
	uint8 VictimTeam, bool bVictimFemale)
{

	// giWOT.ScoreKill:110 — account for SuicideInstigators: a killer-less (environment)
	// death within SuicideInstigationRelevance of a displacement (Whirlwind / SwapPlaces)
	// credits the caster who put the victim there, instead of scoring a plain suicide.
	if (Killer == nullptr && VictimPawn)
	{
		if (AController* InstigatorController = ActiveSuicideInstigator(*VictimPawn))
		{
			Killer = InstigatorController;
		}
	}

	const bool bSelfKill = Killer != nullptr && Killer == VictimController;
	if (Killer == nullptr || bSelfKill)
	{
		if (VictimState) VictimState->Suicides += 1;
		// giWOT.ScoreKill:123-138 — announce the suicide (killer == victim) or accident
		// (no killer at all) with the gendered WoT.int fragment, typed 'WOTDeathMessage'.
		const FString& Conjunction = bSelfKill
			? (bVictimFemale ? SuicideConjunctionFemale : SuicideConjunctionMale)
			: (bVictimFemale ? AccidentConjunctionFemale : AccidentConjunctionMale);
		BroadcastTypedMessage(
			FormatMessage(VictimName + Conjunction + TEXT(".")),
			FName(TEXT("WOTDeathMessage")));
		return;
	}
	// giWOT.ScoreKill:141 — no frag credit for a friendly (same-team) kill; the victim still
	// takes a death either way. Compared on PAWN teams (giWOT.OnSameTeam reads pawn PRIs): an
	// NPC killer has no PlayerState, so the killer's team is its pawn's (AssignedTeam for a
	// summon, PlayerState team for a player). Free-for-all gives everyone a unique team, so
	// friendly fire only bites in team modes (and between a caster and their own summon).
	APawn* KillerPawn = Killer->GetPawn();
	const uint8 KillerTeam = RealTeamOfPawn(KillerPawn);
	if (KillerTeam != VictimTeam)
	{
		// giWOT.ScoreKill:143 — a summoned NPC's frag credits its OWNER when the owner is
		// on the NPC's team (a summon copies its caster's team). The NPC itself has no
		// PlayerState, so an un-owned NPC kill (or an owner who has since changed teams)
		// credits no one. A normal player kill has no SummonOwner and credits the killer.
		AController* Credited = Killer;
		if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(KillerPawn))
		{
			APawn* OwnerPawn = NPC->SummonOwner.Get();
			if (OwnerPawn && RealTeamOfPawn(OwnerPawn) == KillerTeam)
			{
				if (AController* OwnerC = OwnerPawn->GetController())
				{
					if (OwnerC->GetPlayerState<AWoTPlayerState>())
					{
						Credited = OwnerC;
					}
				}
			}
		}
		if (AWoTPlayerState* CreditPS = Credited->GetPlayerState<AWoTPlayerState>())
		{
			CreditPS->Kills += 1;
		}
	}
	// giWOT.ScoreKill:147-158 — every scored kill (own-team included) is announced with
	// the DIRECT killer's name. Retail summons carry a PRI whose PlayerName the
	// ResourceSpawner decorated ("<caster>'s <Class>", ResourceSpawner.uc:63); the
	// port's PlayerState-less NPC carries the same decoration in SummonDisplayName
	// (P05). An un-summoned NPC killer still reads as an empty name.
	const AWoTPlayerState* KillerPS = Killer->GetPlayerState<AWoTPlayerState>();
	FString KillerName = KillerPS ? KillerPS->GetPlayerName() : FString();
	if (KillerName.IsEmpty())
	{
		if (const AWoTNPCCharacter* KillerNPC = Cast<AWoTNPCCharacter>(KillerPawn))
		{
			KillerName = KillerNPC->SummonDisplayName;
		}
	}
	BroadcastTypedMessage(
		FormatMessage(VictimName + DeathConjunction +
			KillerName + TEXT(".")),
		FName(TEXT("WOTDeathMessage")));
	if (VictimState) VictimState->Deaths += 1;
}

FString AWoTGameMode::FormatMessage(const FString& Message)
{
	// giWOT.FormatMessage: hack off leading white-space (retail strips ' ' only), then
	// capitalize the first character.
	FString Formatted = Message;
	while (Formatted.StartsWith(TEXT(" "), ESearchCase::CaseSensitive))
	{
		Formatted.RightChopInline(1);
	}
	if (!Formatted.IsEmpty())
	{
		Formatted[0] = FChar::ToUpper(Formatted[0]);
	}
	return Formatted;
}

void AWoTGameMode::BroadcastTypedMessage(const FString& Message, FName Type)
{
	// GameInfo.BroadcastMessage: deliver to every player. (AllowsBroadcast's flood
	// throttle guards player chat; these authority-composed announcements skip it.)
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
	     It; ++It)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(It->Get()))
		{
			PC->ClientTypedMessage(Message, Type);
		}
	}
}

void AWoTGameMode::ScoreMashadarKill(AController* Killer, uint8 DefenderTeam,
	const FString& VictimDisplayName, int32& VictimDeaths,
	int32& VictimSuicides)
{
	ScoreKillIdentity(Killer, nullptr, nullptr, nullptr, VictimDisplayName,
		DefenderTeam, false);
	if (Killer)
	{
		++VictimDeaths;
	}
	else
	{
		++VictimSuicides;
	}
	// giCombatBase.Killed performs this after ScoreKill using the original direct
	// killer. The synthetic focus has no respawn path, but its frag can still end
	// the match exactly like any other scored victim.
	if (!bGameEnded && FragLimit > 0 && Killer)
	{
		if (const AWoTPlayerState* KillerPS =
			Killer->GetPlayerState<AWoTPlayerState>();
			KillerPS && KillerPS->CalcScore() >= FragLimit)
		{
			EndGame(TEXT("fraglimit"));
		}
	}
}

int32 AWoTGameMode::ReduceDamage(int32 Damage, FName /*DamageType*/,
                                 AWoTCharacter* /*Injured*/, APawn* /*InstigatedBy*/)
{
	// GameInfo.ReduceDamage default: no game-level reduction (the Battle mode overrides this).
	return Damage;
}

bool AWoTGameMode::ChangeTeam(AController* Controller, int32 /*RequestedTeam*/)
{
	// giCombatBase.ChangeTeam — the free-for-all version: ignore the request and assign the
	// smallest team number no other player holds, so each player is a unique team.
	AWoTPlayerState* PS = Controller ? Controller->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!PS)
	{
		return false;
	}
	// Scan up from 0 until we find a number no OTHER player is using (giCombatBase restarts
	// the scan each time it collides; equivalent to "smallest free number").
	int32 Team = 0;
	bool bCollision = true;
	while (bCollision)
	{
		bCollision = false;
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		     It; ++It)
		{
			AController* Other = It->Get();
			if (Other == Controller)
			{
				continue;
			}
			const AWoTPlayerState* OtherPS =
				Other ? Other->GetPlayerState<AWoTPlayerState>() : nullptr;
			if (OtherPS && OtherPS->Team == Team)
			{
				++Team;
				bCollision = true;
				break;
			}
		}
	}
	PS->Team = static_cast<uint8>(FMath::Clamp(Team, 0, 254));
	return true;
}

void AWoTGameMode::PostLogin(APlayerController* NewPlayer)
{
	// GameInfo.Login: ChangeTeam(newPlayer, InTeam) with InTeam defaulting to 255. Assign
	// before Super spawns the pawn, so the free-for-all unique team is set for scoring (and,
	// later, team-area start selection). The PlayerState already exists at PostLogin.
	ChangeTeam(NewPlayer, 255);
	Super::PostLogin(NewPlayer);
	RecordClientLifecycle(NewPlayer, /*bLeaving*/ false);
}

void AWoTGameMode::HandleStartingNewPlayer_Implementation(
	APlayerController* NewPlayer)
{
	Super::HandleStartingNewPlayer_Implementation(NewPlayer);
	if (!IsValid(NewPlayer) || NewPlayer->GetPawn() != nullptr
		|| (NewPlayer->PlayerState && NewPlayer->PlayerState->IsOnlyASpectator()))
	{
		return;
	}
	// GameInfo.Login rejects a connection whose initial PlayerPawn cannot be
	// spawned. Death restarts use the controller's retryable WaitingForSpawn
	// state instead and never pass through this initial-login seam.
	LastInitialSpawnFailureReason =
		TEXT("Unable to enter the game: every player start is blocked.");
	const FText Reason = FText::FromString(LastInitialSpawnFailureReason);
	NewPlayer->ClientReturnToMainMenuWithTextReason(Reason);
	if (GameSession)
	{
		GameSession->KickPlayer(NewPlayer, Reason);
	}
	else
	{
		NewPlayer->Destroy();
	}
}

void AWoTGameMode::Logout(AController* Exiting)
{
	// Record while the remote PlayerState still carries its product-user id.
	RecordClientLifecycle(Cast<APlayerController>(Exiting),
		/*bLeaving*/ true);
	// UE1 traps retained an Instigator PlayerPawn, but its PRI disappeared on
	// logout.  The port retains the controller across pawn replacement instead;
	// sever that identity at the matching production logout seam so later trap
	// kills become accidents rather than crediting a departed PlayerState.
	for (TActorIterator<AWoTTrap> It(GetWorld()); It; ++It)
	{
		It->ClearDamageInstigatorController(Exiting);
	}
	PlayerLoginAddresses.Remove(Cast<APlayerController>(Exiting));
	Super::Logout(Exiting);
}

void AWoTGameMode::RecordClientLifecycle(APlayerController* Player,
	bool bLeaving)
{
	WoTGameMode_RecordClientLifecycle(this, Player, bLeaving);
}

void AWoTGameMode::EndGame(FName /*Reason*/)
{
	// giCombatBase.EndGame — flag the match as decided and drive the clock to -1 so the
	// 1 Hz Timer counts through the GameRestartDelay window and forces the restart.
	bGameEnded = true;
	if (AWoTGameState* GS = GetGameState<AWoTGameState>())
	{
		GS->RemainingTime = -1.0f;
	}

	// GameInfo.EndGame: trigger every 'EndGame'-tagged actor.
	// G32 CENSUS: zero Tag=EndGame authors across all retail t3ds (MP and SP) — the
	// fan-out is call-shape parity, live only for future SP scripting hooks.
	if (UWorld* World = GetWorld())
	{
		if (UWoTEventSubsystem* Events = World->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->FireEvent(FName(TEXT("EndGame")), this, nullptr);
		}
	}

	// GameInfo.EndGame sends each player pawn into the GameEnded state; the port routes
	// that to the controller (freeze + arm the restart delay). This is what lets a
	// FragLimit-decided match rotate — a player's fire, after the delay, calls RestartGame.
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
	     It; ++It)
	{
		if (AWoTPlayerController* PC = Cast<AWoTPlayerController>(It->Get()))
		{
			PC->NotifyMatchEnded();
		}
	}
}

void AWoTGameMode::SeedMatchClock()
{
	if (AWoTGameState* GS = GetGameState<AWoTGameState>())
	{
		GS->RemainingTime = 60.0f * TimeLimit;   // giCombatBase.PostBeginPlay
		GS->bDisplayRemaining = (TimeLimit > 0);
	}
}

void AWoTGameMode::Timer()
{
	// Engine.GameInfo.Timer clears the single shared text
	// budget before derived match-clock work. This must run even when TimeLimit=0.
	SentText = 0;
	// giCombatBase.Timer (Super.Timer() up the chain is GameInfo housekeeping — a no-op
	// for the rules here). Only the TimeLimit clock is polled; FragLimit ends via Killed.
	if (TimeLimit <= 0)
	{
		return;
	}
	AWoTGameState* GS = GetGameState<AWoTGameState>();
	if (!GS)
	{
		return;
	}
	if (bGameEnded)
	{
		// Linger on the results for GameRestartDelay past 0:00, then rotate.
		if (GS->RemainingTime < -GameRestartDelay)
		{
			RestartGame();
		}
	}
	else if (GS->RemainingTime <= 0.0f)
	{
		EndGame(TEXT("timelimit"));
	}
}

bool AWoTGameMode::AllowsBroadcast(const AActor* Broadcaster,
	int32 MessageLength)
{
	(void)Broadcaster;
	// Engine GameInfo increments before comparing and uses a strict less-than. Keep
	// rejected text in the accumulator so flooding cannot recover until Timer.
	SentText += FMath::Max(MessageLength, 0);
	return SentText < 260;
}

FString AWoTGameMode::GetMapName(const FString& Prefix, const FString& MapName,
	int32 Dir, TConstArrayView<FString> InstalledMaps)
{
	// FindFiles("<Prefix>*.wot") is a case-insensitive wildcard; the per-Path merge
	// drops case-insensitive duplicates and keeps FindFiles'
	// order. The port's installed-map sources (the authenticated world list, the
	// asset registry) have no directory order of their own, so this reproduces the
	// order FindFirstFile returns on the NTFS install: the volume's B-tree order,
	// i.e. an ordinal compare of the upper-cased names.
	TArray<FString> MapNames;
	for (const FString& Candidate : InstalledMaps)
	{
		if (!Candidate.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!MapNames.ContainsByPredicate([&Candidate](const FString& Existing)
			{ return Existing.Equals(Candidate, ESearchCase::IgnoreCase); }))
		{
			MapNames.Add(Candidate);
		}
	}
	MapNames.Sort([](const FString& A, const FString& B)
		{ return A.ToUpper().Compare(B.ToUpper(), ESearchCase::CaseSensitive) < 0; });
	// appStrcmp: an exact, case-sensitive match.
	for (int32 Index = 0; Index < MapNames.Num(); ++Index)
	{
		if (MapNames[Index].Equals(MapName, ESearchCase::CaseSensitive))
		{
			int32 Offset = Index + Dir;
			if (Offset < 0)
			{
				Offset = MapNames.Num() - 1;
			}
			else if (Offset >= MapNames.Num())
			{
				Offset = 0;
			}
			return MapNames[Offset];
		}
	}
	return MapNames.Num() > 0 ? MapNames[0] : FString();
}

TArray<FString> AWoTGameMode::GetInstalledMapNames() const
{
#if WITH_AUTOMATION_TESTS
	if (bHasInstalledMapsForTesting)
	{
		return InstalledMapsForTesting;
	}
#endif
	// The authenticated worlds ARE the installed maps of a guarded build (the
	// retail Maps directory the cache was built from).
	TArray<FString> Installed = WoTAdoptedWorldMapIdentities();
#if !WOT_GUARDED_BOOTSTRAP
	if (Installed.IsEmpty())
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<
			FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(TEXT("/Game/WoT/Maps")), Assets,
			/*bRecursive*/ false);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
			{
				Installed.AddUnique(Asset.AssetName.ToString());
			}
		}
	}
#endif
	return Installed;
}

FString AWoTGameMode::TravelURLForMap(const FString& MapIdentity)
{
	if (MapIdentity.IsEmpty())
	{
		return FString();
	}
	if (const TOptional<FString> Shell = WoTGuardedShellPackageForMapIdentity(MapIdentity);
		Shell.IsSet())
	{
		return Shell.GetValue();
	}
#if WOT_GUARDED_BOOTSTRAP
	// Route admission refuses anything outside the exact guarded world routes.
	return FString();
#else
	return FString(TEXT("/Game/WoT/Maps/")) + MapIdentity;
#endif
}

void AWoTGameMode::NextMap()
{
	// Advance Map through the configured rotation (giWOT.uc:66-70).
	Map = GetMapName(MapPrefix, Map, 1, GetInstalledMapNames());
	PendingMapURL = TravelURLForMap(Map);
}

void AWoTGameMode::RestartGame()
{
	// giCombatBase.RestartGame: rotate the map once per decided match; otherwise fall
	// back to the base reload (GameInfo.RestartGame == ServerTravel "?Restart").
	if (bChangeLevels && !bAlreadyChanged)
	{
		bAlreadyChanged = true;

		const FString FromMap = Map;
		NextMap();
		if (!PendingMapURL.IsEmpty())
		{
			// The source logs and saves the chosen map before relative travel, which
			// preserves the running URL's options (giCombatBase.uc:219-221).
			UE_LOG(LogTemp, Log, TEXT("%s.RestartGame: Changing to %s (%s)"),
				*GetName(), *Map, *PendingMapURL);
			WoTRecordBootstrapHeartbeat(TEXT("map-rotation"),
				EWoTBootstrapPhase::Ready, 0, {}, {},
				FString::Printf(TEXT("from=%s next=%s url=%s prefix=%s"),
					*FromMap, *Map, *PendingMapURL, *MapPrefix));
			// Suppress the actual travel under automation (no packaged map to load); the
			// staged PendingMapURL still records the rotation decision for the spec.
			if (!GIsAutomationTesting)
			{
				GetWorld()->ServerTravel(PendingMapURL, /*bAbsolute*/ false);
			}
			return;
		}
	}

	// No rotation staged: reload the current match. GameInfo.RestartGame is literally
	// Level.ServerTravel("?Restart", false); AGameModeBase has no RestartGame of its own,
	// so issue that reload directly. Skipped under automation (no map to travel into).
	if (!GIsAutomationTesting)
	{
		GetWorld()->ServerTravel(TEXT("?Restart"), /*bAbsolute*/ false);
	}
}

bool AWoTGameMode::NormalizeVoteRequest(
	const FString& Request, FString& OutRequest) const
{
	OutRequest.Reset();
	if (Request.IsEmpty() || Request.Len() > 256) return false;
	for (const TCHAR Character : Request)
	{
		if (Character < TEXT(' ') || Character == TCHAR(0x7f)) return false;
	}
	FString Clean = Request;
	Clean.TrimStartAndEndInline();
	int32 Separator = INDEX_NONE;
	Clean.FindChar(TEXT(' '), Separator);
	const FString Verb =
		(Separator == INDEX_NONE ? Clean : Clean.Left(Separator)).ToUpper();
	FString Argument = Separator == INDEX_NONE ? FString() : Clean.Mid(Separator + 1);
	Argument.TrimStartAndEndInline();
	if (Verb == TEXT("RESTARTSERVER"))
	{
		if (!Argument.IsEmpty()) return false;
		OutRequest = Verb;
		return true;
	}
	if (Verb == TEXT("OPEN"))
	{
		if (Argument.IsEmpty() || Argument.Contains(TEXT(" "))
			|| Argument.Contains(TEXT("?")) || Argument.Contains(TEXT("#")))
		{
			return false;
		}
		for (const FString& Installed : GetInstalledMapNames())
		{
			if (Installed.Equals(Argument, ESearchCase::IgnoreCase)
				&& !TravelURLForMap(Installed).IsEmpty())
			{
				OutRequest = Verb + TEXT(" ") + Installed.ToUpper();
				return true;
			}
		}
		return false;
	}
	if (Verb != TEXT("KICK") && Verb != TEXT("KICKBAN")) return false;
	if (Argument.IsEmpty()) return false;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		It; ++It)
	{
		const AWoTPlayerController* Candidate = Cast<AWoTPlayerController>(It->Get());
		const APlayerState* State = Candidate ? Candidate->PlayerState : nullptr;
		if (State && State->GetPlayerName().Equals(
			Argument, ESearchCase::IgnoreCase))
		{
			OutRequest = Verb + TEXT(" ") + State->GetPlayerName().ToUpper();
			return true;
		}
	}
	return false;
}

void AWoTGameMode::BroadcastVoteCenterMessage(const FString& Message) const
{
	if (!GetWorld()) return;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		It; ++It)
	{
		if (AWoTPlayerController* Player = Cast<AWoTPlayerController>(It->Get()))
		{
			Player->ClientCenterMessage(Message);
		}
	}
}

void AWoTGameMode::VoteRequest(
	AWoTPlayerController* Player, const FString& Request)
{
	if (!HasAuthority() || !IsValid(Player) || !Player->PlayerState
		|| Player->PlayerState->IsOnlyASpectator())
	{
		return;
	}
	FString Normalized;
	if (!NormalizeVoteRequest(Request, Normalized))
	{
		Player->ClientCenterMessage(WoTVoteText(
			TEXT("VoteStr001"), TEXT("That vote request is unavailable.")));
		return;
	}

	int32 VoteIndex = VoteRequests.IndexOfByPredicate(
		[&Normalized](const FWoTVoteRequest& Vote)
		{
			return Vote.Request == Normalized;
		});
	if (VoteIndex == INDEX_NONE)
	{
		VoteIndex = VoteRequests.IndexOfByPredicate(
			[](const FWoTVoteRequest& Vote) { return Vote.Request.IsEmpty(); });
		if (VoteIndex == INDEX_NONE)
		{
			BroadcastVoteCenterMessage(WoTVoteText(
				TEXT("VoteStr004"), TEXT("Wait for an active vote to expire.")));
			BroadcastVoteCenterMessage(WoTVoteText(
				TEXT("VoteStr005"), TEXT("The active vote limit has been reached.")));
			return;
		}
		FWoTVoteRequest& NewVote = VoteRequests[VoteIndex];
		NewVote.Request = Normalized;
		NewVote.RemainingSeconds = VoteRequestTimeout;
		const FString Minutes = FString::Printf(
			TEXT("%.1f"), VoteRequestTimeout / 60.0f);
		BroadcastVoteCenterMessage(
			WoTVoteText(TEXT("VoteStr002"), TEXT("Vote expires in "))
			+ Minutes
			+ WoTVoteText(TEXT("VoteStr003"), TEXT(" minutes.")));
	}

	FWoTVoteRequest& Vote = VoteRequests[VoteIndex];
	Vote.Voters.Add(Player);
	int32 NumPlayers = 0;
	int32 NumVotes = 0;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		It; ++It)
	{
		AWoTPlayerController* Candidate = Cast<AWoTPlayerController>(It->Get());
		if (!IsValid(Candidate) || !Candidate->PlayerState
			|| Candidate->PlayerState->IsOnlyASpectator())
		{
			continue;
		}
		++NumPlayers;
		if (Vote.Voters.Contains(Candidate)) ++NumVotes;
	}
	const FString VoterName = Player->PlayerState
		? Player->PlayerState->GetPlayerName() : FString();
	BroadcastVoteCenterMessage(
		WoTVoteText(TEXT("VoteStr006"), TEXT("Vote request: "))
		+ Normalized
		+ WoTVoteText(TEXT("VoteStr007"), TEXT(" submitted by "))
		+ VoterName
		+ FString::Printf(TEXT(" (%d/%d)"), NumVotes, NumPlayers));
	if (NumPlayers == 0 || NumVotes * 3 >= NumPlayers * 2)
	{
		ExecuteVoteRequest(Normalized, NumPlayers);
		ResetVoteRequest(VoteIndex);
	}
}

void AWoTGameMode::ExecuteVoteRequest(const FString& Request, int32 NumPlayers)
{
#if WITH_AUTOMATION_TESTS
	LastExecutedVoteRequestForTest = Request;
#endif
	if (Request == TEXT("RESTARTSERVER"))
	{
		RestartGame();
		return;
	}
	if (Request.StartsWith(TEXT("OPEN ")))
	{
		const FString RequestedMap = Request.Mid(5);
		for (const FString& Installed : GetInstalledMapNames())
		{
			if (!Installed.Equals(RequestedMap, ESearchCase::IgnoreCase)) continue;
			PendingMapURL = TravelURLForMap(Installed);
			if (!PendingMapURL.IsEmpty() && !GIsAutomationTesting && GetWorld())
			{
				GetWorld()->ServerTravel(PendingMapURL, false);
			}
			return;
		}
		return;
	}
	if (NumPlayers <= 1) return;
	const bool bBan = Request.StartsWith(TEXT("KICKBAN "));
	const FString TargetName = Request.Mid(bBan ? 8 : 5);
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator();
		It; ++It)
	{
		APlayerController* Candidate = It->Get();
		if (!Candidate || !Candidate->PlayerState
			|| !Candidate->PlayerState->GetPlayerName().Equals(
				TargetName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		RemovePlayer(Candidate, bBan, FText::FromString(
			TEXT("Removed by a successful player vote.")));
		return;
	}
}

bool AWoTGameMode::RemovePlayer(APlayerController* Player, bool bBan,
	const FText& Reason)
{
	if (!IsValid(Player) || !Player->PlayerState)
	{
		return false;
	}
	if (bBan)
	{
		const TArray<FWoTHostBan> PreviousBans = HostBans;
		const FString Identity = StableIdentityKey(
			Player->PlayerState->GetUniqueId());
		FString Address = PlayerLoginAddresses.FindRef(Player).Left(128);
		if (!WoTGameMode_IsSafeBanField(Address, 128))
		{
			Address.Reset();
		}
		const bool bIdentitylessBot = Identity.IsEmpty() && Address.IsEmpty()
			&& Player->Player == nullptr;
		if (Identity.IsEmpty() && Address.IsEmpty() && !bIdentitylessBot)
		{
			return false;
		}
		if (!bIdentitylessBot)
		{
			HostBans.RemoveAll([&Identity, &Address](const FWoTHostBan& Ban)
			{
				// A public address can represent multiple players. Stable identities
				// deduplicate only themselves; address-only records retain the legacy
				// address deduplication used when no stable identity is available.
				if (!Identity.IsEmpty()) return Ban.Identity == Identity;
				return Ban.Identity.IsEmpty() && !Address.IsEmpty()
					&& Ban.Address.Equals(Address, ESearchCase::IgnoreCase);
			});
			HostBans.Insert({Identity, Address}, 0);
			if (HostBans.Num() > 50)
			{
				HostBans.SetNum(50);
			}
			// A network KICKBAN must never silently degrade to a one-session kick.
			if (!SaveHostBans())
			{
				HostBans = PreviousBans;
				return false;
			}
		}
	}
#if WITH_AUTOMATION_TESTS
	if (PlayerRemovalHookForTesting)
	{
		return PlayerRemovalHookForTesting(Player, Reason);
	}
#endif
	if (GameSession && GameSession->KickPlayer(Player, Reason))
	{
		return true;
	}
	return Player->Player == nullptr && Player->Destroy();
}

void AWoTGameMode::ResetVoteRequest(int32 Index)
{
	if (VoteRequests.IsValidIndex(Index)) VoteRequests[Index] = FWoTVoteRequest{};
}

void AWoTGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	for (int32 Index = 0; Index < VoteRequests.Num(); ++Index)
	{
		FWoTVoteRequest& Vote = VoteRequests[Index];
		if (Vote.Request.IsEmpty()) continue;
		Vote.RemainingSeconds -= DeltaSeconds;
		if (Vote.RemainingSeconds > 0.0f) continue;
		BroadcastVoteCenterMessage(
			WoTVoteText(TEXT("VoteStr008"), TEXT("Vote request "))
			+ Vote.Request
			+ WoTVoteText(TEXT("VoteStr009"), TEXT(" expired.")));
		ResetVoteRequest(Index);
	}
}

#if WITH_AUTOMATION_TESTS
int32 AWoTGameMode::ActiveVoteRequestCountForTesting() const
{
	int32 Count = 0;
	for (const FWoTVoteRequest& Vote : VoteRequests)
	{
		if (!Vote.Request.IsEmpty()) ++Count;
	}
	return Count;
}

float AWoTGameMode::VoteRequestRemainingForTesting(int32 ActiveIndex) const
{
	for (const FWoTVoteRequest& Vote : VoteRequests)
	{
		if (Vote.Request.IsEmpty()) continue;
		if (ActiveIndex-- == 0) return Vote.RemainingSeconds;
	}
	return 0.0f;
}
#endif

bool AWoTGameMode::ShouldSpawnAtStartSpot(AController* /*Player*/)
{
	// giCombatBase.FindPlayerStart is scored fresh on every spawn; never reuse a cached
	// StartSpot, or players would keep respawning at the same point.
	return false;
}

bool AWoTGameMode::CanPawnSeeStartForScoring(const APawn* Viewer,
	const AActor* Start)
{
	return Viewer && Start && WoTSight::OmniSight(
		Viewer, Start, WoTSight::VisibilityOf(Start));
}

AActor* AWoTGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	// giCombatBase.FindPlayerStart — score every PlayerStart and take the best.
	UWorld* World = GetWorld();
	APawn* PlayerPawn = Player ? Player->GetPawn() : nullptr;

	// The spawning player's team (InTeam) for the team-area weighting + the LOS team-gate.
	const AWoTPlayerState* PlayerPS = Player ? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
	const uint8 InTeam = PlayerPS ? PlayerPS->Team : uint8(255);

	float BestScore = -100000.0f;
	APlayerStart* BestStart = nullptr;
	const AWoTLeafMap* LeafMap = nullptr;
	if (TActorIterator<AWoTLeafMap> It(World); It)
	{
		LeafMap = *It;
	}

	for (TActorIterator<APlayerStart> StartIt(World); StartIt; ++StartIt)
	{
		APlayerStart* Start = *StartIt;
		// Intentional divergence (PORT_NOTES): retail FindPlayerStart can pick a
		// start whose spawn always fails (Battle3_01 PlayerStart27/28 sit in
		// solid BSP), and GameInfo.Login then rejects the joining player with
		// FailedSpawnMessage. Such a start is never chosen here, so a join or
		// restart lands on one of the map's spawnable starts.
		if (LeafMap != nullptr && LeafMap->IsPointInSolid(Start->GetActorLocation()))
		{
			continue;
		}
		// Randomize the base so repeated spawns spread across the arena.
		float Score = WoTRandom::RandRange(1000.0f, 2000.0f);

		// bSpawnInTeamArea: weight against starts outside the player's team area — heavily
		// against enemy starts, less so against neutral (giCombatBase.FindPlayerStart:120-133).
		// A WoT start's TeamNumber defaults 0, the retail byte default (T3D omits the prop
		// when unset — team-0 starts carry no line); plain non-WoT starts read as neutral (255).
		if (bSpawnInTeamArea)
		{
			const AWoTPlayerStart* WoTStart = Cast<AWoTPlayerStart>(Start);
			const uint8 StartTeam = WoTStart ? WoTStart->TeamNumber : uint8(255);
			if (StartTeam != InTeam)
			{
				Score -= (StartTeam == 255) ? 4000.0f : 20000.0f;
			}
		}

		for (TActorIterator<APawn> PawnIt(World); PawnIt; ++PawnIt)
		{
			APawn* Other = *PawnIt;
			// giCombatBase.uc:138 `OtherPlayer != Player` skips the spawning player's own
			// pawn (their outgoing corpse is already unpossessed, so also caught below).
			// Corpses are skipped too: retail reuses the single PlayerPawn (dead bodies
			// become Carcass actors, never Pawns), so a dead AWoTCharacter awaiting its
			// owner's respawn must not occupy-block or LOS-penalize a start.
			const AWoTCharacter* OtherWoT = Cast<AWoTCharacter>(Other);
			if (Other == PlayerPawn || (OtherWoT && OtherWoT->bDead))
			{
				continue;
			}
			const float Dist =
				FVector::Dist(Other->GetActorLocation(), Start->GetActorLocation());
			Score += FMath::Sqrt(Dist) / 64.0f;   // reward distance from other players

			// Original gates the overlap on the GameInfo's own collision cylinder (a
			// quirk); the port uses the occupying pawn's extent — the same "someone is
			// standing here" intent, per-pawn.
			const float Occupied =
				Other->GetSimpleCollisionRadius() + Other->GetSimpleCollisionHalfHeight();
			if (Dist < Occupied)
			{
				Score -= 1000000.0f;   // never spawn on top of a player
			}
			// giCombatBase.FindPlayerStart calls OtherPlayer.LineOfSightTo(Start) on the
			// pawn itself — no controller needed, so corpses and uncontrolled pawns count
			// too. The penalty applies only to potential enemies: a pawn with no PlayerState
			// or on a different team (giCombatBase.uc:143 — teammates in sight don't count).
			// Arena is a free-for-all (unique teams), so this still penalizes everyone there.
			else
			{
				const AController* OtherC = Other->GetController();
				const AWoTPlayerState* OtherPS =
					OtherC ? OtherC->GetPlayerState<AWoTPlayerState>() : nullptr;
				const bool bPotentialEnemy = (OtherPS == nullptr) || (OtherPS->Team != InTeam);
				if (bPotentialEnemy)
				{
					// giCombatBase.uc:146 calls LineOfSightTo(Start). For a
					// non-pawn target UE1's LineOfSightTo caps players at 4000uu
					// and NPCs at 3000uu, then uses the non-enemy center/body
					// TRACE_VisBlocking paths.
					const bool bHasLineOfSight =
						CanPawnSeeStartForScoring(Other, Start);
					if (bHasLineOfSight)
					{
						Score -= 1000.0f;   // penalize starts an enemy can already see
					}
				}
			}
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			BestStart = Start;
		}
	}

	// Fall back to the engine's picker only in the degenerate all-occupied / no-start case
	// (the original would return None here); never hand back a null start.
	return BestStart ? BestStart : Super::ChoosePlayerStart_Implementation(Player);
}

void AWoTGameMode::SetPlayerDefaults(APawn* PlayerPawn)
{
	Super::SetPlayerDefaults(PlayerPawn);   // engine default-inventory hook
	// giMPArena.AddDefaultInventory: Dart + 25 charges.
	if (DefaultAngreal)
	{
		GivePawnAngreal(PlayerPawn, DefaultAngreal, DefaultAngrealCharges);
	}
	// giWOT.AddDefaultInventory (giWOT.uc:74-97), reached through giMPArena's Super (so
	// AFTER the Dart): grant the pawn class's DefaultAngrealInventory (Air Pulse on every
	// MP faction pawn). Retail leaves charges to the spawn's own SetCharges seed —
	// Charges=0 rolls the same RandRange(MinInitial, MaxInitial); AirBurst's 999/999
	// (AngrealInvAirBurst.uc:372-373) makes that always 999. Same existing-item check +
	// PickupQuery + GiveTo semantics via GivePawnAngreal.
	if (const AWoTCharacter* WoTPawn = Cast<AWoTCharacter>(PlayerPawn))
	{
		if (WoTPawn->DefaultAngrealInventory)
		{
			GivePawnAngreal(PlayerPawn, WoTPawn->DefaultAngrealInventory, /*Charges*/ 0);
		}
	}
}

void AWoTGameMode::GivePawnAngreal(APawn* P, TSubclassOf<AWoTAngrealInventory> Class,
                                   int32 Charges)
{
	// giMP.GivePawnAngreal — never a class the pawn already carries. (No authority
	// guard: a GameMode only ever exists on the server, exactly like GameInfo.)
	if (!P || !Class)
	{
		return;
	}
	UWoTInventoryComponent* Inv = P->FindComponentByClass<UWoTInventoryComponent>();
	if (!Inv || Inv->FindInventoryType(Class))
	{
		return;
	}
	// Spawn deferred with bHeldItem set (the SpawnCopy pattern) so the item never
	// enters the collidable Pickup state — otherwise, spawned atop the pawn, it would
	// auto-pickup via its own overlap and the PickupQuery below would then see a
	// duplicate, steal its charges, and destroy the just-granted item.
	const FTransform Xform(P->GetActorRotation(), P->GetActorLocation());
	AWoTAngrealInventory* A = GetWorld()->SpawnActorDeferred<AWoTAngrealInventory>(
		Class, Xform, P, P, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!A)
	{
		return;
	}
	A->bHeldItem = true;
	A->FinishSpawning(Xform);
	// SetCharges already seeded CurCharges in BeginPlay; override to the requested
	// amount (0 => the class's own random MinInitial..MaxInitial spread).
	A->CurCharges = (Charges > 0)
		? Charges
		: WoTRandom::RandRange(A->MinInitialCharges, A->MaxInitialCharges);
	if (PickupQuery(P, A))
	{
		A->GiveTo(P);
	}
	else
	{
		A->Destroy();
	}
}
void AWoTGameMode::RestartPlayerAtPlayerStart(AController* NewPlayer,
	AActor* StartSpot)
{
	RestartStartSpot = StartSpot;
	Super::RestartPlayerAtPlayerStart(NewPlayer, StartSpot);
	RestartStartSpot.Reset();
}

APawn* AWoTGameMode::SpawnDefaultPawnAtTransform_Implementation(
	AController* NewPlayer, const FTransform& SpawnTransform)
{
	FTransform RetailSpawn = SpawnTransform;
	if (const APlayerStart* Start = Cast<APlayerStart>(RestartStartSpot.Get()))
	{
		UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
		// UE1 spawns at the authored NavigationPoint transform. SpawnActor then
		// calls FindSpot for the NEW pawn cylinder: shallow
		// world penetration is pushed clear, while an already-free high start is
		// left floating and falls normally. Do not infer a floor from the
		// PlayerStart's own 40uu cylinder and do not force Walking.
		if (UWorld* World = GetWorld(); World && PawnClass)
		{
			FActorSpawnParameters SpawnInfo;
			SpawnInfo.Instigator = GetInstigator();
			SpawnInfo.ObjectFlags |= RF_Transient;
			SpawnInfo.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			APawn* Spawned = World->SpawnActor<APawn>(
				PawnClass, RetailSpawn, SpawnInfo);
			if (Spawned)
			{
				FVector ResolvedLocation = RetailSpawn.GetLocation();
				const FRotator ResolvedRotation = RetailSpawn.Rotator();
				if (!WoTUE1Collision::FindTeleportSpotWorldOnly(
					*World, *Spawned, ResolvedLocation, ResolvedRotation))
				{
					Spawned->Destroy();
					return nullptr;
				}
				// UE1 FindSpot ends with a level point check of the adjusted
				// cylinder and moves it at most sqrt(1.5)*|Extent|,
				// so a start whose center lies in solid BSP never
				// spawns: Battle3_01's PlayerStart27/28 sit 44-90 uu under a
				// semisolid floor, and retail's RestartPlayer picks another start.
				// UE5's FindTeleportSpot resolves only the one-sided collision
				// mesh and pushed such a pawn out of the floor's underside: it
				// fell out of the world.
				if (TActorIterator<AWoTLeafMap> It(World);
					It && It->IsPointInSolid(ResolvedLocation))
				{
					Spawned->Destroy();
					return nullptr;
				}
				RetailSpawn.SetLocation(ResolvedLocation);
				Spawned->SetActorTransform(RetailSpawn, false, nullptr,
					ETeleportType::TeleportPhysics);
				// UE1 SpawnActor rejects actor encroachment after its level-only
				// FindSpot.  This preserves login/restart failure (and therefore the
				// dying-state retry) instead of seating two players at one start.
				if (WoTUE1Collision::EncroachesBlockingActor(*Spawned))
				{
					Spawned->Destroy();
					return nullptr;
				}
			}
			return Spawned;
		}
	}
	return Super::SpawnDefaultPawnAtTransform_Implementation(NewPlayer, RetailSpawn);
}
