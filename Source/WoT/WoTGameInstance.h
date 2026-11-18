#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "WoTFrontEnd.h"
#include "WoTGameInstance.generated.h"

class FWoTControllerUIInputRegistration;

// The menu world is intentionally controller-owned. Persistence belongs only
// to UWoTPreferencesSubsystem; this object retains a non-config editor/test
// fallback and a runtime-only cache override for legacy fixture construction.
UCLASS()
class UWoTGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// Registers the controller UI input preprocessor for this instance's game
	// viewport (not on dedicated servers) and removes it at shutdown.
	virtual void Init() override;
	virtual void Shutdown() override;

	EWoTPlayerFaction GetSelectedFaction() const;
	EWoTPlayerSkinColor GetSelectedSkinColor() const;
	EWoTPlayerSkinColor GetCitadelSkinColor() const;
	FString GetSelectedClassOption() const;
	FString GetPlayerName() const;
	const FString& GetPayloadCacheDir() const { return PayloadCacheDir; }

	// Runtime menu calls route persistence through the typed preference owner.
	void SetSelectedFaction(EWoTPlayerFaction NewFaction, bool bSave = true);
	void SetSelectedSkinColor(EWoTPlayerSkinColor NewColor, bool bSave = true);
	void SetCitadelSkinColor(EWoTPlayerSkinColor NewColor, bool bSave = true);
	void SetPlayerName(const FString& NewName, bool bSave = true);
	void SetPayloadCacheDir(const FString& NewDirectory, bool bSave = true);

private:
	EWoTPlayerFaction SelectedFaction = EWoTPlayerFaction::AesSedai;
	EWoTPlayerSkinColor SelectedSkinColor = EWoTPlayerSkinColor::Base;
	EWoTPlayerSkinColor CitadelSkinColor = EWoTPlayerSkinColor::Base;
	FString PlayerName;

	TSharedPtr<FWoTControllerUIInputRegistration> ControllerUIInput;

	// Non-persistent compatibility seam used by existing cache fixtures. Empty
	// selects <ProjectSavedDir>/PayloadCache.
	FString PayloadCacheDir;
};
