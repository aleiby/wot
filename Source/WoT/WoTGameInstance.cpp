#include "WoTGameInstance.h"

#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "UI/WoTControllerUIInput.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#include "WoTPreferencesSubsystem.h"

void UWoTGameInstance::Init()
{
	Super::Init();
	if (!FWoTControllerUIInput::ShouldRegisterForGameInstance(
		IsDedicatedServerInstance(), FSlateApplication::IsInitialized(),
		GIsAutomationTesting))
	{
		return;
	}
	// Scoped to this instance's game viewport, resolved per event (the
	// viewport is created after Init): its window family in a game, and the
	// viewport widget itself in the editor, where PIE shares the editor's
	// window, so PIE never touches the editor's own UI.
	TWeakObjectPtr<UWoTGameInstance> WeakInstance(this);
	ControllerUIInput = FWoTControllerUIInput::Register(
		[WeakInstance]() -> TSharedPtr<SWidget>
		{
			const UWoTGameInstance* Instance = WeakInstance.Get();
			const UGameViewportClient* Viewport = Instance
				? Instance->GetGameViewportClient() : nullptr;
			if (Viewport == nullptr)
			{
				return nullptr;
			}
			if (GIsEditor)
			{
				return Viewport->GetGameViewportWidget();
			}
			return Viewport->GetWindow();
		});
}

void UWoTGameInstance::Shutdown()
{
	// Dropping the registration unregisters the preprocessor and restores
	// the default navigation config when it was the last one.
	ControllerUIInput.Reset();
	Super::Shutdown();
}

EWoTPlayerFaction UWoTGameInstance::GetSelectedFaction() const
{
	if (const UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		if (Typed->HasAdoptedAuthenticatedEarlyUI())
		{
			return FWoTFrontEndModel::NormalizeFaction(
				Typed->GetPreferences().SelectedFaction);
		}
	}
	return FWoTFrontEndModel::NormalizeFaction(SelectedFaction);
}

EWoTPlayerSkinColor UWoTGameInstance::GetSelectedSkinColor() const
{
	if (const UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		if (Typed->HasLoadedFullPreferences())
		{
			return FWoTFrontEndModel::NormalizeSkinColor(
				Typed->GetPreferences().SelectedSkinColor);
		}
	}
	return FWoTFrontEndModel::NormalizeSkinColor(SelectedSkinColor);
}

EWoTPlayerSkinColor UWoTGameInstance::GetCitadelSkinColor() const
{
	if (const UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		if (Typed->HasLoadedFullPreferences())
		{
			return FWoTFrontEndModel::NormalizeSkinColor(
				Typed->GetPreferences().CitadelSkinColor);
		}
	}
	return FWoTFrontEndModel::NormalizeSkinColor(CitadelSkinColor);
}

FString UWoTGameInstance::GetSelectedClassOption() const
{
	return FWoTFrontEndModel::ClassOption(GetSelectedFaction());
}

FString UWoTGameInstance::GetPlayerName() const
{
	if (const UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		if (Typed->HasLoadedFullPreferences())
		{
			return Typed->GetPreferences().PlayerName;
		}
	}
	return PlayerName;
}

void UWoTGameInstance::SetSelectedFaction(EWoTPlayerFaction NewFaction, bool bSave)
{
	SelectedFaction = FWoTFrontEndModel::NormalizeFaction(NewFaction);
	if (UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		FWoTBootstrapStatus Status =
			Typed->SetSelectedFaction(SelectedFaction, bSave);
		if (Status.IsError())
		{
			UE_LOG(LogTemp, Error, TEXT("Typed faction preference failed: %s"),
				*Status.GetError().ToString());
		}
	}
}

void UWoTGameInstance::SetSelectedSkinColor(EWoTPlayerSkinColor NewColor,
	bool bSave)
{
	SelectedSkinColor = FWoTFrontEndModel::NormalizeSkinColor(NewColor);
	if (UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		FWoTBootstrapStatus Status =
			Typed->SetSelectedSkinColor(SelectedSkinColor, bSave);
		if (Status.IsError())
		{
			UE_LOG(LogTemp, Error, TEXT("Typed skin preference failed: %s"),
				*Status.GetError().ToString());
		}
	}
}

void UWoTGameInstance::SetCitadelSkinColor(EWoTPlayerSkinColor NewColor,
	bool bSave)
{
	CitadelSkinColor = FWoTFrontEndModel::NormalizeSkinColor(NewColor);
	if (UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		FWoTBootstrapStatus Status =
			Typed->SetCitadelSkinColor(CitadelSkinColor, bSave);
		if (Status.IsError())
		{
			UE_LOG(LogTemp, Error, TEXT("Typed citadel skin preference failed: %s"),
				*Status.GetError().ToString());
		}
	}
}

void UWoTGameInstance::SetPlayerName(const FString& NewName, bool bSave)
{
	PlayerName = FWoTFrontEndModel::SanitizePlayerName(NewName);
	if (UWoTPreferencesSubsystem* Typed =
		GetSubsystem<UWoTPreferencesSubsystem>())
	{
		FWoTBootstrapStatus Status = Typed->SetPlayerName(PlayerName, bSave);
		if (Status.IsError())
		{
			UE_LOG(LogTemp, Error, TEXT("Typed player-name preference failed: %s"),
				*Status.GetError().ToString());
		}
	}
}

void UWoTGameInstance::SetPayloadCacheDir(const FString& NewDirectory, bool bSave)
{
	PayloadCacheDir = NewDirectory;
	(void)bSave;
}
