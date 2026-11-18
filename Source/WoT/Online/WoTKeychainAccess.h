#pragma once

#include "CoreMinimal.h"

/**
 * Whether the next EOS sign-in will make macOS ask for keychain access.
 *
 * The EOS SDK keeps the Device ID credential in the login keychain (generic
 * password, service "com.epicgames.sdk.connect.access_token", account = the
 * EOS ProductId). An item created by an earlier, differently signed build
 * makes macOS ask before this build may read it, and the SDK reads it on the
 * game thread: the game freezes behind the system dialog before any in-game
 * text can be drawn. The front end therefore
 * explains the prompt first, and only when it is actually coming.
 */
enum class EWoTKeychainAccess : uint8
{
	/** Readable without asking, or no item yet (the SDK creates it silently). */
	NoPrompt,
	WillPrompt,
	/** Any other keychain answer: not decidable. */
	Unknown,
};

namespace WoTKeychainAccess
{
	/**
	 * Pure decision over a SecItemCopyMatching status taken with keychain UI
	 * disabled: errSecSuccess and errSecItemNotFound need no prompt; the
	 * legacy login keychain answers errSecAuthFailed (and the data-protection
	 * keychain errSecInteractionNotAllowed) when a dialog would have appeared.
	 */
	EWoTKeychainAccess Classify(int32 OSStatus);

	/**
	 * Mac: a no-UI read of the EOS credential item for AccountId. Never shows a
	 * dialog and never logs or keeps the item data. Other platforms: NoPrompt.
	 * OutDetail carries the status code for the heartbeat.
	 */
	EWoTKeychainAccess Probe(const FString& AccountId, FString* OutDetail = nullptr);

	const TCHAR* LexToString(EWoTKeychainAccess Access);

#if WITH_AUTOMATION_TESTS
	void SetProbeOverrideForTesting(TFunction<EWoTKeychainAccess()> Override);
#endif
}
