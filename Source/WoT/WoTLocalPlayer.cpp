#include "WoTLocalPlayer.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "WoTFrontEnd.h"
#include "WoTMacComputerName.h"
#include "WoTPreferencesSubsystem.h"

FString UWoTLocalPlayer::GetNickname() const
{
	FString Preferred;
	if (const UGameInstance* Instance = GetGameInstance())
	{
		if (const UWoTPreferencesSubsystem* Preferences =
			Instance->GetSubsystem<UWoTPreferencesSubsystem>())
		{
			if (Preferences->HasLoadedFullPreferences())
			{
				Preferred = Preferences->GetPreferences().PlayerName;
			}
		}
	}
	return ResolveNickname(Preferred, DefaultPlayerName());
}

FString UWoTLocalPlayer::ResolveNickname(const FString& Preferred,
	const FString& DefaultName)
{
	const FString Sanitized = FWoTFrontEndModel::SanitizePlayerName(Preferred);
	return Sanitized.IsEmpty() ? DefaultName : Sanitized;
}

FString UWoTLocalPlayer::DefaultPlayerName()
{
#if PLATFORM_MAC
	return NormalizeComputerName(PreferComputerName(
		WoTMacFriendlyComputerName(), FPlatformProcess::ComputerName()));
#else
	return NormalizeComputerName(FPlatformProcess::ComputerName());
#endif
}

FString UWoTLocalPlayer::PreferComputerName(const FString& FriendlyName,
	const FString& HostName)
{
	return FWoTFrontEndModel::SanitizePlayerName(FriendlyName).IsEmpty()
		? HostName : FriendlyName;
}

FString UWoTLocalPlayer::NormalizeComputerName(const FString& ComputerName)
{
	// macOS host names carry the mDNS ".local" suffix; it is not part of the name.
	FString Name = ComputerName;
	Name.RemoveFromEnd(TEXT(".local"), ESearchCase::IgnoreCase);
	Name = FWoTFrontEndModel::SanitizePlayerName(Name);
	return Name.IsEmpty() ? FString(TEXT("Player")) : Name;
}
