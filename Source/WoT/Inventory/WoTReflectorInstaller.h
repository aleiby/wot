#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "WoTReflectorInstaller.generated.h"

class UWoTReflector;

// Port of Angreal.ReflectorInstaller — the AngrealInventory base for artifacts
// that install reflectors in the caster on cast (the shields/reflect family).
// Spec: exports/script/Angreal/ReflectorInstaller.uc. On cast it installs each
// ReflectorClass on the caster for Duration seconds (InstallReflectorEffect
// inlined, cf. the leech installers), then spends a charge.
UCLASS(Abstract)
class AWoTReflectorInstaller : public AWoTAngrealInventory
{
	GENERATED_BODY()

public:
	AWoTReflectorInstaller();

	// Shared lifespan for every installed reflector (Duration=0 => permanent).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector") float Duration = 20.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Reflector")
	TArray<TSubclassOf<UWoTReflector>> ReflectorClasses;

	virtual void CastArtifact() override;   // Cast()
};
