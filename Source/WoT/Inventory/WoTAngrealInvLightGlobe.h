#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealInventory.h"
#include "../WoTEvents.h"
#include "../WoTLightGlobe.h"
#include "WoTAngrealInvLightGlobe.generated.h"

class AWoTWeaveSpriteEffect;

// Port of Angreal.AngrealInvLightGlobe — the Light Sphere ter'angreal. Spec:
// exports/script/Angreal/AngrealInvLightGlobe.uc. Non-launcher (the Levitate
// pattern): Cast toggles a following AWoTLightGlobe on/off; the light
// deliberately survives artifact switching (UnCast only turns it off when the
// owner is dead), drops/empties tear it down, and GiveTo folds the owner's
// color through the WoTPlayerColorFX seam (retail's WOTPlayer.PlayerColor +
// mesh-name faction map). A visible pickup shows the LightGlobeStar glow
// sprite at the rotated GlowOffset. Riders (PORT_NOTES "Light Sphere pixies"): the WaysZone
// AI legs are SP-scope (WaysZone is authored only by the Mission_08 maps and
// no NPC MP loadout carries this artifact), so GetPriority is the retail
// every-other-case -MaxInt; the SP tutorial's Trigger->Tag turn-off author is
// ported as IWoTTriggerable + TagName (map import wiring rides the SP slice).
UCLASS()
class AWoTAngrealInvLightGlobe : public AWoTAngrealInventory, public IWoTTriggerable
{
	GENERATED_BODY()

public:
	AWoTAngrealInvLightGlobe();

	// Type of light to use (replicated like retail; class identity carries the
	// color). Base AWoTLightGlobe = the retail default Class'Angreal.LightGlobe'.
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Light Globe")
	TSubclassOf<AWoTLightGlobe> LightType;

	// Sprite offset from the pickup model, relative to rotation.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Light Globe")
	FVector LightGlobeGlowOffset = FVector(0.0f, 7.0f, 3.0f);

	// UE1 Actor.Tag for the SP tutorial's Trigger->artifact turn-off routing.
	UPROPERTY(EditAnywhere, Category = "WoT|Light Globe")
	FName TagName;

	// The live light (authority handle; the globe actor replicates itself).
	UPROPERTY() TObjectPtr<AWoTLightGlobe> Light;

	virtual void CastArtifact() override;     // Cast() — the toggle
	virtual void UnCastArtifact() override;   // UnCast() — dead-owner turn-off only
	virtual void GoEmpty() override;
	virtual void DropFrom(FVector StartLocation) override;
	virtual void GiveTo(APawn* Other) override;
	virtual void BecomePickup() override;
	virtual void BecomeItem() override;
	virtual void Tick(float DeltaTime) override;
	virtual void Destroyed() override;
	virtual void BeginPlay() override;
	virtual float GetPriority() const override;
	virtual void WoTTrigger(AActor* Other, APawn* EventInstigator) override;
	virtual FName GetUE1Tag() const override { return TagName; }
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// AngrealInvLightGlobe.SetColor: PC_*/plain color name -> LightType class.
	void SetColor(FName ColorName);

	AWoTWeaveSpriteEffect* GetGlobeStarForTest() const { return Globe; }

protected:
	// AngrealInvLG*.PreBeginPlay body, shared by the five variants: on
	// authority, spawn a base AngrealInvLightGlobe of VariantColor, copy the
	// placed instance's charge bounds across AFTER its full init (the retail
	// PreBeginPlay order — the replacement's SetCharges reads ITS defaults),
	// then self-destroy. Returns the replacement (spec probe).
	AWoTAngrealInvLightGlobe* ReplaceWithBaseColor(FName VariantColor);

private:
	void UpdateGlobeStar();
	FVector StarLocation() const;

	// Per-machine pickup glow sprite (retail's simulated Globe).
	UPROPERTY() TObjectPtr<AWoTWeaveSpriteEffect> Globe;
};

// The five map-placement helpers (AngrealInvLG*.uc), flat like retail (each
// expands AngrealInvLightGlobe directly).
UCLASS()
class AWoTAngrealInvLGBlue : public AWoTAngrealInvLightGlobe
{
	GENERATED_BODY()
public:
	AWoTAngrealInvLGBlue() { LightType = AWoTLightGlobeBlue::StaticClass(); }
	virtual void PostInitializeComponents() override
	{
		Super::PostInitializeComponents();
		ReplaceWithBaseColor(TEXT("Blue"));
	}
};

UCLASS()
class AWoTAngrealInvLGGreen : public AWoTAngrealInvLightGlobe
{
	GENERATED_BODY()
public:
	AWoTAngrealInvLGGreen() { LightType = AWoTLightGlobeGreen::StaticClass(); }
	virtual void PostInitializeComponents() override
	{
		Super::PostInitializeComponents();
		ReplaceWithBaseColor(TEXT("Green"));
	}
};

UCLASS()
class AWoTAngrealInvLGPurple : public AWoTAngrealInvLightGlobe
{
	GENERATED_BODY()
public:
	AWoTAngrealInvLGPurple() { LightType = AWoTLightGlobePurple::StaticClass(); }
	virtual void PostInitializeComponents() override
	{
		Super::PostInitializeComponents();
		ReplaceWithBaseColor(TEXT("Purple"));
	}
};

UCLASS()
class AWoTAngrealInvLGRed : public AWoTAngrealInvLightGlobe
{
	GENERATED_BODY()
public:
	AWoTAngrealInvLGRed() { LightType = AWoTLightGlobeRed::StaticClass(); }
	virtual void PostInitializeComponents() override
	{
		Super::PostInitializeComponents();
		ReplaceWithBaseColor(TEXT("Red"));
	}
};

UCLASS()
class AWoTAngrealInvLGYellow : public AWoTAngrealInvLightGlobe
{
	GENERATED_BODY()
public:
	AWoTAngrealInvLGYellow() { LightType = AWoTLightGlobeYellow::StaticClass(); }
	virtual void PostInitializeComponents() override
	{
		Super::PostInitializeComponents();
		ReplaceWithBaseColor(TEXT("Yellow"));
	}
};
