#include "WoTFrontEndViewModel.h"

void FWoTFrontEndViewModel::ShowMain()
{
	Screen = EWoTFrontEndScreen::Main;
	StatusText.Reset();
	bStatusError = false;
}

void FWoTFrontEndViewModel::ShowPlayerConfig(EWoTPlayerFaction SavedFaction,
	const FString& SavedPlayerName, const FString& InPlatformDefaultPlayerName,
	EWoTPlayerSkinColor SavedSkinColor)
{
	PlayerConfigReturn = Screen == EWoTFrontEndScreen::Play
		? EWoTFrontEndScreen::Play : EWoTFrontEndScreen::Main;
	Screen = EWoTFrontEndScreen::PlayerConfig;
	PendingFaction = FWoTFrontEndModel::NormalizeFaction(SavedFaction);
	PendingSkinColor = FWoTFrontEndModel::NormalizeSkinColor(SavedSkinColor);
	PlatformDefaultPlayerName = FWoTFrontEndModel::SanitizePlayerName(
		InPlatformDefaultPlayerName);
	PendingPlayerName = FWoTFrontEndModel::SanitizePlayerName(
		SavedPlayerName.IsEmpty()
			? InPlatformDefaultPlayerName : SavedPlayerName);
	StatusText.Reset();
}

void FWoTFrontEndViewModel::SelectPendingSkinColor(EWoTPlayerSkinColor Color)
{
	PendingSkinColor = FWoTFrontEndModel::NormalizeSkinColor(Color);
}

void FWoTFrontEndViewModel::CyclePendingSkinColor(int32 Direction)
{
	if (Direction == 0)
	{
		return;
	}
	const TArray<FWoTSkinChoice>& Choices = FWoTFrontEndModel::SkinChoices();
	int32 Index = Choices.IndexOfByPredicate([this](const FWoTSkinChoice& Choice)
	{
		return Choice.Color == PendingSkinColor;
	});
	if (Index == INDEX_NONE)
	{
		Index = 0;
	}
	Index = (Index + (Direction < 0 ? -1 : 1) + Choices.Num()) % Choices.Num();
	PendingSkinColor = Choices[Index].Color;
}

void FWoTFrontEndViewModel::SelectPendingFaction(EWoTPlayerFaction Faction)
{
	PendingFaction = FWoTFrontEndModel::NormalizeFaction(Faction);
}

void FWoTFrontEndViewModel::SelectPendingPlayerName(const FString& Name)
{
	PendingPlayerName = FWoTFrontEndModel::FilterPlayerNameInput(Name);
}

FWoTPlayerConfig FWoTFrontEndViewModel::SavePlayerConfig()
{
	FWoTPlayerConfig Saved;
	Saved.Faction = PendingFaction;
	Saved.SkinColor = PendingSkinColor;
	Saved.PlayerName = FWoTFrontEndModel::SanitizePlayerName(PendingPlayerName);
	if (Saved.PlayerName.IsEmpty()
		|| Saved.PlayerName == PlatformDefaultPlayerName)
	{
		Saved.PlayerName.Reset();
	}
	CancelPlayerConfig();
	return Saved;
}

void FWoTFrontEndViewModel::CancelPlayerConfig()
{
	PendingPlayerName.Reset();
	PlatformDefaultPlayerName.Reset();
	if (PlayerConfigReturn == EWoTFrontEndScreen::Play)
	{
		// Return to the retained Play state (maps, host request, sessions).
		Screen = EWoTFrontEndScreen::Play;
		StatusText.Reset();
		return;
	}
	ShowMain();
}

void FWoTFrontEndViewModel::ShowControls()
{
	Screen = EWoTFrontEndScreen::Controls;
	StatusText.Reset();
}

void FWoTFrontEndViewModel::ShowHardware()
{
	Screen = EWoTFrontEndScreen::Hardware;
	StatusText.Reset();
}

void FWoTFrontEndViewModel::ShowPlay(
	const TArray<FWoTFrontEndMapEntry>& InMaps)
{
	Screen = EWoTFrontEndScreen::Play;
	Maps = InMaps;
	Sessions.Reset();
	Operation = EWoTFrontEndAsyncOperation::None;
	SelectedMapIndex = Maps.IsEmpty() ? INDEX_NONE : 0;
	PreviousBattleCitadelCount = 2;
	if (const FWoTFrontEndMapEntry* Selected = GetSelectedMap())
	{
		HostRequest = FWoTFrontEndModel::DefaultHostRequest(Selected->PackageName);
		HostRequest.MapTitle = Selected->DisplayName.IsEmpty()
			? Selected->ShortName : Selected->DisplayName;
		const int32 Citadels =
			FWoTFrontEndModel::BattleCitadelCount(Selected->PackageName);
		if (Citadels > 0 && Citadels != PreviousBattleCitadelCount)
		{
			FWoTFrontEndModel::UpdateSealGoal(
				HostRequest, Citadels, Citadels);
			PreviousBattleCitadelCount = Citadels;
		}
		StatusText.Reset();
	}
	else
	{
		HostRequest = FWoTHostRequest();
		HostRequest.MapPackage.Reset();
		StatusText = TEXT("No playable maps were found under /Game/WoT/Maps.");
	}
}

bool FWoTFrontEndViewModel::BackToMain()
{
	// A search may be abandoned (Play auto-fetches on entry, so one is often
	// running): its late completion is ignored, as FinishSearch requires the
	// Searching operation. Hosting, joining and the rest still hold Back.
	if (!CanLeaveOrSwitchNetwork())
	{
		return false;
	}
	AbandonSearch();
	ShowMain();
	return true;
}

const FWoTFrontEndMapEntry* FWoTFrontEndViewModel::GetSelectedMap() const
{
	return Maps.IsValidIndex(SelectedMapIndex) ? &Maps[SelectedMapIndex] : nullptr;
}

bool FWoTFrontEndViewModel::SelectMap(const FString& PackageName)
{
	for (int32 Index = 0; Index < Maps.Num(); ++Index)
	{
		if (Maps[Index].PackageName == PackageName)
		{
			SelectedMapIndex = Index;
			const FWoTHostRequest Defaults =
				FWoTFrontEndModel::DefaultHostRequest(PackageName);
			HostRequest.MapPackage = Defaults.MapPackage;
			HostRequest.MapTitle = Maps[Index].DisplayName.IsEmpty()
				? Maps[Index].ShortName : Maps[Index].DisplayName;
			HostRequest.FragLimit = Defaults.FragLimit;
			HostRequest.TimeLimit = Defaults.TimeLimit;
			const int32 Citadels =
				FWoTFrontEndModel::BattleCitadelCount(PackageName);
			if (Citadels > 0)
			{
				if (Citadels != PreviousBattleCitadelCount)
				{
					FWoTFrontEndModel::UpdateSealGoal(
						HostRequest, Citadels, Citadels);
				}
				PreviousBattleCitadelCount = Citadels;
				if (Citadels > 2)
				{
					HostRequest.bCycleLevels = false;
				}
				FWoTFrontEndModel::UpdateBattlePlayerLimits(
					HostRequest, Citadels);
			}
			else
			{
				HostRequest.bCycleLevels = true;
			}
			return true;
		}
	}
	return false;
}

void FWoTFrontEndViewModel::SetSealGoal(int32 SealGoal)
{
	const int32 Citadels =
		FWoTFrontEndModel::BattleCitadelCount(HostRequest.MapPackage);
	if (Citadels > 0)
	{
		FWoTFrontEndModel::UpdateSealGoal(HostRequest, SealGoal, Citadels);
	}
}

void FWoTFrontEndViewModel::SetSealBudget(int32 SealBudget)
{
	const int32 Citadels =
		FWoTFrontEndModel::BattleCitadelCount(HostRequest.MapPackage);
	if (Citadels > 0)
	{
		FWoTFrontEndModel::UpdateSealBudget(HostRequest, SealBudget, Citadels);
	}
}

int32 FWoTFrontEndViewModel::GetMinSealBudget() const
{
	const int32 Citadels =
		FWoTFrontEndModel::BattleCitadelCount(HostRequest.MapPackage);
	return FWoTFrontEndModel::MinSealBudget(
		HostRequest.SealGoal, Citadels > 0 ? Citadels : 2);
}

int32 FWoTFrontEndViewModel::GetMaxSealBudget() const
{
	return FWoTFrontEndModel::MaxSealBudget(HostRequest.SealGoal);
}

void FWoTFrontEndViewModel::SetMaxPlayers(int32 MaxPlayers)
{
	HostRequest.MaxPlayers = FMath::Clamp(MaxPlayers, 2, 16);
	const int32 Citadels =
		FWoTFrontEndModel::BattleCitadelCount(HostRequest.MapPackage);
	if (Citadels > 0)
	{
		FWoTFrontEndModel::UpdateBattlePlayerLimits(HostRequest, Citadels);
	}
}

void FWoTFrontEndViewModel::SetMaxTeamSize(int32 MaxTeamSize)
{
	HostRequest.MaxTeamSize = MaxTeamSize;
	const int32 Citadels =
		FWoTFrontEndModel::BattleCitadelCount(HostRequest.MapPackage);
	FWoTFrontEndModel::UpdateBattlePlayerLimits(
		HostRequest, Citadels > 0 ? Citadels : 2);
}

void FWoTFrontEndViewModel::SetAutoTeamSelect(bool bEnabled)
{
	HostRequest.bAutoTeamSelect = bEnabled;
	const int32 Citadels =
		FWoTFrontEndModel::BattleCitadelCount(HostRequest.MapPackage);
	FWoTFrontEndModel::UpdateBattlePlayerLimits(
		HostRequest, Citadels > 0 ? Citadels : 2);
}

void FWoTFrontEndViewModel::SetMaxClientTickRate(int32 TickRate)
{
	HostRequest.MaxClientTickRate =
		FWoTFrontEndModel::NormalizeMaxClientTickRate(TickRate);
}

void FWoTFrontEndViewModel::SetAdminPassword(const FString& Password)
{
	FString Sanitized;
	if (FWoTFrontEndModel::SanitizeHostPassword(Password, Sanitized))
	{
		HostRequest.AdminPassword = MoveTemp(Sanitized);
	}
}

void FWoTFrontEndViewModel::SetGamePassword(const FString& Password)
{
	FString Sanitized;
	if (FWoTFrontEndModel::SanitizeHostPassword(Password, Sanitized))
	{
		HostRequest.GamePassword = MoveTemp(Sanitized);
	}
}

FName FWoTFrontEndViewModel::HostTitleLocalizationId() const
{
	return FWoTFrontEndModel::IsBattleMap(HostRequest.MapPackage)
		? FName(TEXT("menuStartBattleServer.MenuTitle"))
		: FName(TEXT("menuStartArenaServer.MenuTitle"));
}

bool FWoTFrontEndViewModel::CanHost() const
{
	return IsIdleOrSearching() && GetSelectedMap() != nullptr
		&& FWoTFrontEndModel::IsPlayableMap(HostRequest.MapPackage);
}

bool FWoTFrontEndViewModel::BeginOperation(
	EWoTFrontEndAsyncOperation NewOperation, const FString& InStatus)
{
	if (!CanStartAsyncAction())
	{
		return false;
	}
	Operation = NewOperation;
	StatusText = InStatus;
	bStatusError = false;
	return true;
}

bool FWoTFrontEndViewModel::FinishOperation(
	EWoTFrontEndAsyncOperation ExpectedOperation, const FString& InStatus)
{
	if (Operation != ExpectedOperation)
	{
		return false;
	}
	Operation = EWoTFrontEndAsyncOperation::None;
	StatusText = InStatus;
	return true;
}

bool FWoTFrontEndViewModel::BeginHost()
{
	if (!CanHost())
	{
		return false;
	}
	AbandonSearch();
	return BeginOperation(
		EWoTFrontEndAsyncOperation::Hosting, TEXT("Creating session..."));
}

void FWoTFrontEndViewModel::FinishHost(bool bSucceeded,
	const FString& FailureText)
{
	// Player-facing text stays short; the session diagnostic readout goes to
	// the durable bootstrap heartbeat instead (SWoTFrontEndRoot).
	if (FinishOperation(EWoTFrontEndAsyncOperation::Hosting,
		bSucceeded ? FString(TEXT("Session created. Starting match..."))
			: (FailureText.IsEmpty()
				? FString(TEXT("Could not create the session.")) : FailureText)))
	{
		bStatusError = !bSucceeded;
	}
}

bool FWoTFrontEndViewModel::BeginSearch()
{
	if (!BeginOperation(EWoTFrontEndAsyncOperation::Searching,
		TEXT("Searching for sessions...")))
	{
		return false;
	}
	Sessions.Reset();
	return true;
}

void FWoTFrontEndViewModel::FinishSearch(bool bSucceeded,
	const TArray<FWoTSessionListEntry>& Results, const FString& FailureText)
{
	if (Operation != EWoTFrontEndAsyncOperation::Searching)
	{
		return;
	}
	Sessions = bSucceeded ? Results : TArray<FWoTSessionListEntry>();
	if (!bSucceeded)
	{
		FinishOperation(EWoTFrontEndAsyncOperation::Searching,
			FailureText.IsEmpty()
				? FString(TEXT("Session search failed.")) : FailureText);
		bStatusError = true;
	}
	else if (Sessions.IsEmpty())
	{
		FinishOperation(EWoTFrontEndAsyncOperation::Searching,
			TEXT("No sessions found."));
	}
	else
	{
		FinishOperation(EWoTFrontEndAsyncOperation::Searching,
			FString::Printf(TEXT("Found %d session(s)."), Sessions.Num()));
	}
}

bool FWoTFrontEndViewModel::BeginJoin(const FString& ServerName)
{
	return BeginOperation(EWoTFrontEndAsyncOperation::Joining,
		ServerName.IsEmpty()
			? FString(TEXT("Joining session..."))
			: FString::Printf(TEXT("Joining %s..."), *ServerName));
}

void FWoTFrontEndViewModel::FinishJoin(bool bSucceeded)
{
	if (!bSucceeded)
	{
		if (FinishOperation(EWoTFrontEndAsyncOperation::Joining,
			TEXT("Could not join the session.")))
		{
			bStatusError = true;
		}
	}
	else
	{
		// OSS acceptance only resolves the address. The engine still has to
		// establish the pending net connection and finish client travel. This
		// also covers an overlay invite, which has no preceding UI Joining state.
		Operation = EWoTFrontEndAsyncOperation::Connecting;
		StatusText = TEXT("Session joined. Connecting...");
	}
}

bool FWoTFrontEndViewModel::BeginDirectConnect()
{
	if (!IsIdleOrSearching())
	{
		return false;
	}
	AbandonSearch();
	return BeginOperation(EWoTFrontEndAsyncOperation::Connecting,
		TEXT("Connecting..."));
}

void FWoTFrontEndViewModel::RejectDirectConnect()
{
	if (FinishOperation(EWoTFrontEndAsyncOperation::Connecting,
		TEXT("Enter a valid host name or IPv4 address, optionally with a port.")))
	{
		bStatusError = true;
	}
}

void FWoTFrontEndViewModel::ReportConnectionFailure(const FString& Reason)
{
	// A fallback travel can recreate the menu before it consumes the retained
	// failure, so restoring the idle state must also work on a fresh model.
	Operation = EWoTFrontEndAsyncOperation::None;
	StatusText = Reason.IsEmpty() ? TEXT("Connection failed.") : Reason;
	bStatusError = true;
}

bool FWoTFrontEndViewModel::BeginResetIdentity()
{
	return BeginOperation(EWoTFrontEndAsyncOperation::ResettingIdentity,
		TEXT("Resetting the online identity..."));
}

void FWoTFrontEndViewModel::FinishResetIdentity(bool bSucceeded)
{
	if (FinishOperation(EWoTFrontEndAsyncOperation::ResettingIdentity,
		bSucceeded
			? TEXT("Online identity reset. A new anonymous identity is created the next time you play online.")
			: TEXT("Could not reset the online identity.")))
	{
		bStatusError = !bSucceeded;
		if (bSucceeded)
		{
			// The reset also forgets the disclosure: back to LAN until the
			// player chooses Internet (and accepts) again.
			HostRequest.bLAN = true;
		}
	}
}

void FWoTFrontEndViewModel::ReportNotice(const FString& Text, bool bError)
{
	if (!CanStartAsyncAction())
	{
		return;
	}
	StatusText = Text;
	bStatusError = bError;
}
