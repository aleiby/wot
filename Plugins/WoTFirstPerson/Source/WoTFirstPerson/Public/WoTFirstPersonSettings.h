#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "WoTFirstPersonSettings.generated.h"

/** The optional held-artifact presentation's persisted, live settings. */
UCLASS(Config=GameUserSettings)
class WOTFIRSTPERSON_API UWoTFirstPersonSettings : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Config)
	bool bShowHeldArtifact = true;

	UPROPERTY(Config)
	float Motion = 1.0f;

	UPROPERTY(Config)
	float ModelFieldOfView = 70.0f;

	static UWoTFirstPersonSettings& Get();

	/** Use the live CDO: setters persist before notifying consumers. */
	void SetShowHeldArtifact(bool bValue);
	void SetMotion(float Value);
	void SetModelFieldOfView(float Value);
	void ToggleShowHeldArtifact();
	void StepMotion();
	void StepModelFieldOfView();

	FSimpleMulticastDelegate OnChanged;

private:
	void PersistChange();
};
