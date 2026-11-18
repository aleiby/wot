#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WoTFrontEnd.h"
#include "WoTEarlyUI.h"
#include "Settings/WoTGameSettings.h"
#include "WoTPreferencesSubsystem.generated.h"

struct FWoTSavedServerPassword
{
	FString Server;
	FString Password;

	bool operator==(const FWoTSavedServerPassword& Other) const
	{
		return Server == Other.Server && Password == Other.Password;
	}
};

/** Complete path-free player preference state. */
struct FWoTPlayerPreferences
{
	FWoTEarlyUIV1 EarlyUI;
	EWoTPlayerFaction SelectedFaction = EWoTPlayerFaction::AesSedai;
	EWoTPlayerSkinColor SelectedSkinColor = EWoTPlayerSkinColor::Base;
	EWoTPlayerSkinColor CitadelSkinColor = EWoTPlayerSkinColor::Base;
	bool bAlwaysUseEpicRelay = false;
	/** Empty follows ULocalPlayer's platform nickname. */
	FString PlayerName;
	/** Retail-compatible MRU: at most ten server/password pairs. */
	TArray<FWoTSavedServerPassword> SavedPasswords;

	bool operator==(const FWoTPlayerPreferences& Other) const
	{
		return EarlyUI == Other.EarlyUI
			&& SelectedFaction == Other.SelectedFaction
			&& SelectedSkinColor == Other.SelectedSkinColor
			&& CitadelSkinColor == Other.CitadelSkinColor
			&& bAlwaysUseEpicRelay == Other.bAlwaysUseEpicRelay
			&& PlayerName == Other.PlayerName
			&& SavedPasswords == Other.SavedPasswords;
	}
};

/**
 * The only game-side preference owner. Before authenticated cache activation
 * it may adopt EarlyUI and apply typed neutral settings, but it cannot reopen
 * the file or touch payload/world/OSS state.
 */
UCLASS()
class UWoTPreferencesSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	FWoTBootstrapStatus AdoptAuthenticatedEarlyUI(
		FWoTAuthenticatedEarlyUI&& Authenticated);
	bool HasAdoptedAuthenticatedEarlyUI() const { return bAdoptedEarlyUI; }
	bool HasLoadedFullPreferences() const { return bLoadedFullPreferences; }
	const FWoTPlayerPreferences& GetPreferences() const { return Preferences; }
	const FWoTGameSettings& GetGameSettings() const { return GameSettings; }
	FWoTBootstrapStatus SaveGameSettings(const FWoTGameSettings& NewSettings);
	FWoTBootstrapStatus ApplyGameSettingsToInput(UPlayerInput& Input) const;

	/** Legal only after the verified cache activation is observable. */
	FWoTBootstrapStatus ApplyFullPreferencesAfterCacheActivation();
	FWoTBootstrapStatus SetSelectedFaction(
		EWoTPlayerFaction Faction, bool bSave);
	FWoTBootstrapStatus SetSelectedSkinColor(
		EWoTPlayerSkinColor Color, bool bSave);
	FWoTBootstrapStatus SetCitadelSkinColor(
		EWoTPlayerSkinColor Color, bool bSave);
	FWoTBootstrapStatus SetPlayerName(const FString& Name, bool bSave);
	bool GetSavedServerPassword(const FString& Server,
		FString& OutPassword) const;
	FWoTBootstrapStatus SaveServerPassword(const FString& Server,
		const FString& Password);
	FWoTBootstrapStatus Save(const FWoTPlayerPreferences& NewPreferences);

	/**
	 * One-time internet-play (Epic Online Services) disclosure. A separate
	 * marker beside the preference file - never a key in the closed EarlyUI
	 * codec - written when the player accepts the notice and deleted by
	 * "Reset online identity". Dormant (false / refused) before cache
	 * activation, like every other full preference.
	 */
	bool HasAcknowledgedOnlineIdentity() const;
	FWoTBootstrapStatus SetOnlineIdentityAcknowledged(bool bAcknowledged);

	/**
	 * One-time heads-up shown before WoT's first local-network action on a
	 * Mac, so the macOS Local Network prompt that follows is expected. Same
	 * marker discipline and dormancy as the online-identity disclosure.
	 */
	bool HasSeenLocalNetworkHeadsUp() const;
	FWoTBootstrapStatus SetLocalNetworkHeadsUpSeen();

	/** The last release commit whose bundled notes this profile dismissed. */
	bool HasSeenReleaseNotes(const FString& ReleaseKey) const;
	FWoTBootstrapStatus SetReleaseNotesSeen(const FString& ReleaseKey);

	/** Pending rollback notice written by the detached updater. */
	bool GetPendingUpdateFailedNotice(uint64& OutFailedRelease,
		uint64& OutRestoredRelease) const;
	FWoTBootstrapStatus SetUpdateFailedNoticeSeen(uint64 FailedRelease);

	/**
	 * A per-user side file beside the preference file (the notice markers'
	 * rule): <preference dir>/<Name>, or <test leaf>.<Name> while a spec
	 * overrides the preference path. False when no user directory resolves.
	 */
	static bool SideFilePath(const TCHAR* Name, FString& OutPath);
	/** Bounded side-file I/O. Guarded production keeps the held R ->
	 * Preferences handle through the final no-follow file operation; explicit
	 * automation paths retain their direct-path behavior. */
	static bool LoadSideFileBytes(const FString& Path, uint64 MaxBytes,
		TArray<uint8>& OutBytes);
	static bool SaveSideFileBytes(const FString& Path,
		TConstArrayView<uint8> Bytes);

#if WITH_AUTOMATION_TESTS
	static void SetCreationEnabledForTesting(bool bEnabled);
	static void SetDelayEarlyUIAdoptionForTesting(bool bEnabled);
	static TArray<uint8> SerializeForTesting(
		const FWoTPlayerPreferences& Value);
	/** Full leaf override so tests never touch the user's fixed preference file. */
	static void SetPreferencePathForTesting(const FString& Leaf);
	static bool HasPreferencePathForTesting();
	/** Intercepts bounded side-file access before disk I/O. A set result is
	 * returned to the caller; an unset result observes and allows real I/O. */
	static void SetSideFileAccessHookForTesting(
		TFunction<TOptional<bool>(const FString&, bool bWrite)>&& Hook);
	static FWoTBootstrapStatus SetUpdateFailedNoticePendingForTesting(
		uint64 FailedRelease, uint64 RestoredRelease);
	static void SetUpdateFailedNoticePersistenceFailureForTesting(bool bFail);
	/** The raw bounded read/remove behind full preferences and side files, so
	 * a guarded spec can tell "absent" (Success, !bOutExists) from an error. */
	static FWoTBootstrapStatus ReadPreferenceChildForTesting(
		const FString& Path, TArray<uint8>& OutBytes, bool& bOutExists);
	static FWoTBootstrapStatus RemovePreferenceChildForTesting(
		const FString& Path);
	void SetGameSettingsForTesting(const FWoTGameSettings& Settings)
	{
		GameSettings = Settings;
	}
#endif

private:
	FWoTBootstrapStatus AssignSelectedFactionAfterCacheActivation(
		EWoTPlayerFaction Faction);

	FWoTPlayerPreferences Preferences;
	FWoTGameSettings GameSettings = FWoTGameSettings::Defaults();
	bool bAdoptedEarlyUI = false;
	bool bLoadedFullPreferences = false;
};
