#include "WoTLocalPlayer.h"

#include "Engine/GameInstance.h"
#include "HAL/PlatformProcess.h"
#include "SceneView.h"
#include "WoTFrontEnd.h"
#include "WoTLocalPlayerHooks.h"
#include "WoTMacComputerName.h"
#include "WoTPreferencesSubsystem.h"

void UWoTLocalPlayer::GetViewPoint(FMinimalViewInfo& OutViewInfo) const
{
	Super::GetViewPoint(OutViewInfo);
	// A local view owner composes the final pose after the camera and modifiers.
	FWoTLocalPlayerHooks::ComposeView(*this, OutViewInfo);
}

bool UWoTLocalPlayer::GetProjectionData(FViewport* Viewport,
	FSceneViewProjectionData& ProjectionData, int32 StereoViewIndex) const
{
	if (!Super::GetProjectionData(Viewport, ProjectionData, StereoViewIndex))
	{
		return false;
	}
	// A stereo device supplies its own projection after the ordinary near-plane
	// calculation, so an owned near plane is applied last, keeping each eye's
	// asymmetric frustum.
	FWoTLocalPlayerHooks::ApplyNearClip(*this, ProjectionData.ProjectionMatrix);
	return true;
}

void UWoTLocalPlayer::PlayerRemoved()
{
	FWoTLocalPlayerHooks::DetachAll(*this);
	Super::PlayerRemoved();
}

void UWoTLocalPlayer::BeginDestroy()
{
	FWoTLocalPlayerHooks::PlayerDestroyed(*this);
	Super::BeginDestroy();
}

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
