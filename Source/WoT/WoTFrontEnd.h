#pragma once

#include "CoreMinimal.h"
#include "WoTFrontEnd.generated.h"

class UGameInstance;

// The four retail menuPlayer class choices. The enum is config-backed by
// UWoTGameInstance; travel always uses the package-qualified string below.
UENUM()
enum class EWoTPlayerFaction : uint8
{
	AesSedai,
	Forsaken,
	Hound,
	Whitecloak
};

UENUM()
enum class EWoTPlayerSkinColor : uint8
{
	Black,
	Blue,
	Red,
	Green,
	Base
};

struct FWoTSkinChoice
{
	EWoTPlayerSkinColor Color = EWoTPlayerSkinColor::Base;
	FString DisplayName;
};

struct FWoTFactionChoice
{
	EWoTPlayerFaction Faction = EWoTPlayerFaction::AesSedai;
	FString ClassOption;
	FString TeamDescription;
};

// Complete input captured before the asynchronous session-create operation.
// The menu supplies a package path from the playable-map catalog.
struct FWoTHostRequest
{
	FString MapPackage;
	// Decoded Engine.LevelInfo.Title selected by the authenticated map catalog.
	// Empty is allowed for non-menu callers; session creation resolves it again.
	FString MapTitle;
	FString ServerName = TEXT("WoT Server");
	int32 FragLimit = 20;
	int32 TimeLimit = 0;
	int32 SealGoal = 4;
	int32 SealBudget = 3;
	int32 MaxPlayers = 6;
	int32 MaxTeamSize = 3;
	int32 MaxClientTickRate = 0;
	bool bCycleLevels = true;
	bool bAutoTeamSelect = false;
	FString AdminPassword;
	FString GamePassword;
	bool bLAN = true;
};

// The return URL is withheld while an OSS session destroy is outstanding. The
// subsystem consumes it only after the destroy callback (or immediately when
// there is no named session, as with a direct-IP client).
enum class EWoTLeaveState : uint8
{
	Idle,
	DestroyingSession,
	ReturningToMenu
};

class FWoTLeaveStateMachine
{
public:
	static FString FrontEndURL();
	/** Pure build-policy seam: guarded cooks contain only WoTBootstrap. */
	static FString FrontEndURLForBuild(bool bGuardedBuild);

	EWoTLeaveState GetState() const { return State; }
	bool Begin(bool bHasNamedSession);
	bool CompleteDestroy();
	FString TakeReturnURL();
	void Reset();

private:
	EWoTLeaveState State = EWoTLeaveState::Idle;
};

// Pure front-end data and URL construction. Slate and the OSS both consume
// this seam so initial travel never grows a second, subtly different path.
class FWoTFrontEndModel
{
public:
	/** Ready + live payload capability gate for every normal menu/session edge. */
	static bool CanConstructNormalFrontEnd(const UGameInstance* GameInstance);
	static const TArray<FWoTFactionChoice>& FactionChoices();
	static EWoTPlayerFaction NormalizeFaction(EWoTPlayerFaction Faction);
	static FString ClassOption(EWoTPlayerFaction Faction);
	static FString TeamDescription(EWoTPlayerFaction Faction);
	static const TArray<FWoTSkinChoice>& SkinChoices();
	static EWoTPlayerSkinColor NormalizeSkinColor(EWoTPlayerSkinColor Color);
	static FString SkinOption(EWoTPlayerFaction Faction,
		EWoTPlayerSkinColor Color);
	static bool IsValidSkinOption(const FString& SkinOption);
	/** Live-edit filter; preserves edge spaces and "_" until commit/save. */
	static FString FilterPlayerNameInput(const FString& In);
	/** Retail's twenty-character, URL-safe plain-ASCII player alias. */
	static FString SanitizePlayerName(const FString& In);

	// Trim and validate a direct-connect target. Only DNS-style host names or
	// IPv4 spellings, optionally followed by a numeric port in [1, 65535], pass.
	static bool SanitizeDirectAddress(const FString& Address, FString& OutAddress);

	static bool IsArenaMap(const FString& MapPackage);
	static bool IsBattleMap(const FString& MapPackage);
	static bool IsPlayableMap(const FString& MapPackage);
	static FString ShortMapName(const FString& MapPackage);
	/** Battle2_/Battle3_/Battle4_ -> 2/3/4; zero for every other map. */
	static int32 BattleCitadelCount(const FString& MapPackage);
	static FWoTHostRequest DefaultHostRequest(const FString& MapPackage);
	static int32 MinSealBudget(int32 SealGoal, int32 NumCitadels);
	static int32 MaxSealBudget(int32 SealGoal);
	static int32 NormalizeMaxClientTickRate(int32 TickRate);
	static bool SanitizeHostPassword(const FString& Password,
		FString& OutPassword);
	static void UpdateBattlePlayerLimits(FWoTHostRequest& Request,
		int32 NumCitadels);
	/** menuStartBattleServer.uc:65-73, shared by UI, URL and game mode. */
	static void UpdateSealGoal(FWoTHostRequest& Request, int32 SealGoal,
		int32 NumCitadels);
	static void UpdateSealBudget(FWoTHostRequest& Request, int32 SealBudget,
		int32 NumCitadels);

	// Replace any existing Class option case-insensitively, preserve every
	// unrelated option and a trailing portal fragment, then append one retail
	// package-qualified Class option.
	static FString AppendClassOption(const FString& URL, EWoTPlayerFaction Faction);
	static FString AppendPlayerOptions(const FString& URL,
		EWoTPlayerFaction Faction, EWoTPlayerSkinColor SkinColor);

	// Caller-supplied map options/fragments are discarded before classification
	// and construction; the sanitized package must name a playable map.
	static FString BuildHostTravelURL(const FWoTHostRequest& Request,
		EWoTPlayerFaction Faction,
		EWoTPlayerSkinColor SkinColor = EWoTPlayerSkinColor::Base);
};
