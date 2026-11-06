#pragma once

#include "CoreMinimal.h"
#include "WoTFrontEnd.h"
#include "WoTFrontEndCatalog.h"
#include "WoTSessionSubsystem.h"

enum class EWoTFrontEndScreen : uint8
{
	Main,
	PlayerConfig,
	Play,
	Controls,
	Hardware
};

enum class EWoTFrontEndAsyncOperation : uint8
{
	None,
	Hosting,
	Searching,
	Joining,
	Connecting,
	/** Logout + delete of the internet-play (EOS) Device ID. */
	ResettingIdentity
};

struct FWoTPlayerConfig
{
	EWoTPlayerFaction Faction = EWoTPlayerFaction::AesSedai;
	EWoTPlayerSkinColor SkinColor = EWoTPlayerSkinColor::Base;
	/** Empty means to keep following the platform nickname. */
	FString PlayerName;
};

// Widget-free screen state and transitions. Slate is only a projection of this
// object, which keeps navigation/default/async behavior headless-testable.
class FWoTFrontEndViewModel
{
public:
	EWoTFrontEndScreen GetScreen() const { return Screen; }
	EWoTFrontEndAsyncOperation GetOperation() const { return Operation; }
	const FString& GetStatusText() const { return StatusText; }
	/** True while the status line reports a failure rather than progress. */
	bool IsStatusError() const { return bStatusError; }

	void ShowMain();
	/** Retail Player Config (name and faction). Opened from Main or Multiplayer;
	 * Save/Back return to the screen that opened it, preserving Play state. */
	void ShowPlayerConfig(EWoTPlayerFaction SavedFaction,
		const FString& SavedPlayerName, const FString& PlatformDefaultPlayerName,
		EWoTPlayerSkinColor SavedSkinColor = EWoTPlayerSkinColor::Base);
	void SelectPendingFaction(EWoTPlayerFaction Faction);
	EWoTPlayerFaction GetPendingFaction() const { return PendingFaction; }
	void SelectPendingSkinColor(EWoTPlayerSkinColor Color);
	void CyclePendingSkinColor(int32 Direction);
	EWoTPlayerSkinColor GetPendingSkinColor() const { return PendingSkinColor; }
	void SelectPendingPlayerName(const FString& Name);
	const FString& GetPendingPlayerName() const { return PendingPlayerName; }
	FWoTPlayerConfig SavePlayerConfig();
	void CancelPlayerConfig();
	EWoTFrontEndScreen GetPlayerConfigReturnScreen() const { return PlayerConfigReturn; }
	void ShowControls();
	void ShowHardware();

	void ShowPlay(const TArray<FWoTFrontEndMapEntry>& InMaps);
	bool BackToMain();
	const TArray<FWoTFrontEndMapEntry>& GetMaps() const { return Maps; }
	const FWoTFrontEndMapEntry* GetSelectedMap() const;
	bool SelectMap(const FString& PackageName);
	FName HostTitleLocalizationId() const;
	const FWoTHostRequest& GetHostRequest() const { return HostRequest; }
	void SetServerName(const FString& ServerName) { HostRequest.ServerName = ServerName; }
	void SetFragLimit(int32 FragLimit) { HostRequest.FragLimit = FragLimit; }
	void SetTimeLimit(int32 TimeLimit) { HostRequest.TimeLimit = TimeLimit; }
	void SetSealGoal(int32 SealGoal);
	void SetSealBudget(int32 SealBudget);
	int32 GetMinSealBudget() const;
	int32 GetMaxSealBudget() const;
	bool IsBattleMapSelected() const
		{ return FWoTFrontEndModel::IsBattleMap(HostRequest.MapPackage); }
	void SetMaxPlayers(int32 MaxPlayers);
	void SetMaxTeamSize(int32 MaxTeamSize);
	void SetAutoTeamSelect(bool bEnabled);
	void SetCycleLevels(bool bEnabled) { HostRequest.bCycleLevels = bEnabled; }
	void SetMaxClientTickRate(int32 TickRate);
	void SetAdminPassword(const FString& Password);
	void SetGamePassword(const FString& Password);
	void SetLAN(bool bLAN) { HostRequest.bLAN = bLAN; }
	bool IsInternet() const { return !HostRequest.bLAN; }
	bool CanHost() const;
	bool CanStartAsyncAction() const { return Operation == EWoTFrontEndAsyncOperation::None; }
	/** Play lists the network on entry, so a search is often running. Only
	 * Refresh and Join wait for it: Back, the LAN/Internet choice, Host and
	 * Direct Connect cut it short, and Player Config runs beside it. */
	bool IsIdleOrSearching() const
	{
		return Operation == EWoTFrontEndAsyncOperation::None
			|| Operation == EWoTFrontEndAsyncOperation::Searching;
	}
	bool CanLeaveOrSwitchNetwork() const { return IsIdleOrSearching(); }
	/** Drops a running search; FinishSearch then ignores its late result. */
	void AbandonSearch()
	{
		if (Operation == EWoTFrontEndAsyncOperation::Searching)
		{
			Operation = EWoTFrontEndAsyncOperation::None;
		}
	}

	/** Cuts a running search short. */
	bool BeginHost();
	/** A non-empty FailureText replaces the generic failure line (internet
	 * play reports why: not configured, sign-in failed, ...). */
	void FinishHost(bool bSucceeded, const FString& FailureText = FString());
	bool BeginSearch();
	void FinishSearch(bool bSucceeded, const TArray<FWoTSessionListEntry>& Results,
		const FString& FailureText = FString());
	bool BeginJoin(const FString& ServerName);
	void FinishJoin(bool bSucceeded);
	bool BeginResetIdentity();
	void FinishResetIdentity(bool bSucceeded);
	/** Idle status line (no operation): an internet notice or refusal. */
	void ReportNotice(const FString& Text, bool bError);
	/** Cuts a running search short. */
	bool BeginDirectConnect();
	void RejectDirectConnect();
	void ReportConnectionFailure(const FString& Reason);
	const TArray<FWoTSessionListEntry>& GetSessions() const { return Sessions; }

private:
	bool BeginOperation(EWoTFrontEndAsyncOperation NewOperation,
		const FString& InStatus);
	bool FinishOperation(EWoTFrontEndAsyncOperation ExpectedOperation,
		const FString& InStatus);

	EWoTFrontEndScreen Screen = EWoTFrontEndScreen::Main;
	EWoTFrontEndAsyncOperation Operation = EWoTFrontEndAsyncOperation::None;
	EWoTPlayerFaction PendingFaction = EWoTPlayerFaction::AesSedai;
	EWoTPlayerSkinColor PendingSkinColor = EWoTPlayerSkinColor::Base;
	FString PendingPlayerName;
	FString PlatformDefaultPlayerName;
	EWoTFrontEndScreen PlayerConfigReturn = EWoTFrontEndScreen::Main;
	TArray<FWoTFrontEndMapEntry> Maps;
	int32 SelectedMapIndex = INDEX_NONE;
	// Arena selections do not disturb this: the next Battle map compares against
	// the most recently selected Battle citadel count. A new host screen starts
	// from retail's two-citadel configuration.
	int32 PreviousBattleCitadelCount = 2;
	FWoTHostRequest HostRequest;
	TArray<FWoTSessionListEntry> Sessions;
	FString StatusText;
	bool bStatusError = false;
};
