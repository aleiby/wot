#pragma once

#include "CoreMinimal.h"
#include "Engine/LocalPlayer.h"
#include "WoTLocalPlayer.generated.h"

/** Supplies the authenticated optional player-name preference to UE travel. */
UCLASS()
class UWoTLocalPlayer : public ULocalPlayer
{
	GENERATED_BODY()

public:
	virtual FString GetNickname() const override;

	/** Pure seam for the preferred-name/fallback choice. */
	static FString ResolveNickname(const FString& Preferred,
		const FString& DefaultName);

	/**
	 * The unset default: this machine's computer name, the same
	 * in the Player Config field and on the login URL. Not ULocalPlayer's platform
	 * nickname: under the Null subsystem that is "<hostname>-<LoginId>", which the
	 * twenty-character cut left as e.g. "DESKTOP-4KQ8ZT2-51E0".
	 */
	static FString DefaultPlayerName();

	/** Pure seam: a sanitized computer name without a trailing ".local", or "Player". */
	static FString NormalizeComputerName(const FString& ComputerName);

	/**
	 * Pure seam: the friendly computer name when it survives sanitizing, else the
	 * host name. On the Mac the friendly name is the Sharing name players know
	 * ("Robin’s MacBook Pro"); a wholly non-ASCII one falls back to the host name.
	 * NormalizeComputerName still runs last on whichever is chosen.
	 */
	static FString PreferComputerName(const FString& FriendlyName,
		const FString& HostName);
};
