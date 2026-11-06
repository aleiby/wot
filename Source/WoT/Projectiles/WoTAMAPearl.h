#pragma once

#include "CoreMinimal.h"
#include "WoTAngrealProjectile.h"
#include "WoTAMAPearl.generated.h"

USTRUCT()
struct FWoTAMAAffectedPawnState
{
	GENERATED_BODY()

	UPROPERTY() TArray<TWeakObjectPtr<class UWoTReflector>> InstalledReflectors;
	UPROPERTY() TWeakObjectPtr<class AWoTPlayerController> GlowController;
};

// Port of Angreal.AMAPearl (expands AngrealProjectile) — the anti-magic dome that
// backs the Unravel (Remove Curse) and Aura of Unraveling (AMA) ter'angreal.
// Spec: exports/script/Angreal/AMAPearl.uc (+ RCAMAPearl.uc for Remove Curse's
// smaller/shorter variant).
//
// A stationary bubble that expands from InitialScale to FinalScale, holds for
// Duration, then shrinks away — scaling its affect radius (ProjCollisionRadius)
// as it grows. It rides the base AngrealProjectile ProjTouch proximity poll: any
// pawn caught inside is "affected" (AffectPawn) once on entry. Remove Curse's
// affect strips the victim's active bad weaves; see AffectPawn.
//
// AffectPawn = RemoveCurseEffect + the dead-zone: it (1) stops the victim's
// current cast, (2) strips their removable, not-ours leeches AND reflectors and
// untaints their artifacts, and (3) seals them in the dead-zone — installs a
// NoCast + a NoBadEffects reflector (Priority 255) for Duration. UnAffectPawn (on
// exit) and the dome's own EndPlay tear those two back off. Remove Curse's small
// dome (RCAMAPearl) inherits all of this; AMA's full-size dome is the same, just
// planted by a landmine.
//
// Its widened poll absorbs weaves, resolves pearl priority, triggers aura-tagged
// actors, and drives sparks, sounds, shell-light easing, fade, and owner tint.
UCLASS()
class AWoTAMAPearl : public AWoTAngrealProjectile
{
	GENERATED_BODY()

public:
	AWoTAMAPearl();

	UPROPERTY(VisibleAnywhere, Category = "WoT|Visual")
	TObjectPtr<class UWoTVertexAnimComponent> DomeVisual;
	UPROPERTY(VisibleAnywhere, Category = "WoT|Visual")
	TObjectPtr<class UPointLightComponent> DomeLight;

	// AMAPearl scale/timing knobs (RCAMAPearl overrides FinalScale/Duration/rate).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float InitialScale = 0.1f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float FinalScale = 20.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float InitialScaleRate = 40.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float ScaleAccel = -20.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float Duration = 8.0f;
	// Decay (DT_Fade): after Duration the dome fades out over FadeTime seconds while
	// still growing (FadingExpandRate) and still affecting anyone inside, then dies.
	// RCAMAPearl overrides these (0.2 / 2.0).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float FadeTime = 0.5f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float FadingExpandRate = 5.0f;
	// The affect radius at scale 1.0 (SetSize scales it) — AMAPearl's default.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|AMA") float BaseAffectRadius = 28.0f;
	UPROPERTY(EditAnywhere, Category = "WoT|AMA") uint8 EndLightRadius = 25;
	UPROPERTY(EditAnywhere, Category = "WoT|AMA") uint8 Priority = 1;
	UPROPERTY(EditAnywhere, Category = "WoT|AMA") float BaseScaleGlow = 1.0f;
	UPROPERTY(VisibleAnywhere, Category = "WoT|AMA") float SpawnTime = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") TSoftObjectPtr<USoundBase> AffectSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") TSoftObjectPtr<USoundBase> UnAffectSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") TSoftObjectPtr<USoundBase> AbsorbSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") TSoftObjectPtr<USoundBase> ActivateSound;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Sounds") TSoftObjectPtr<USoundBase> DeActivateSound;

	// Kick off the expand -> hold -> shrink lifecycle.
	void Go();
	void ConfigureRemoveCurseVariant();
	int32 GetAbsorptionCountForTest() const { return AbsorptionCountForTest; }
	int32 GetAntiMagicTriggerCountForTest() const { return AntiMagicTriggerCountForTest; }
	int32 GetPawnPassCountForTest() const { return PawnPassCountForTest; }
	float GetCurrentLightRadiusForTest() const { return CurrentLightRadius; }
	float GetEndTimeForTest() const { return EndTime; }

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UFUNCTION() void OnRep_AutoGo();
	// Proximity poll hooks (a pawn entered / left the dome).
	virtual void ProjTouch(AActor* Other) override;
	virtual void ProjUnTouch(AActor* Other) override;

protected:
	// RemoveCurseEffect + dead-zone: strip the pawn's removable, not-ours weaves,
	// untaint, stop their cast, and seal them in a NoCast/NoBadEffects dead-zone.
	virtual void AffectPawn(APawn* Victim);
	// Peel the dead-zone reflectors this dome put on the pawn back off.
	virtual void UnAffectPawn(APawn* Victim);

private:
	// Push the current DrawScale into both the affect radius and dome visual.
	void ApplyScale();
	void ApplyAlpha(float Scalar);
	void PlayOneShot(const TSoftObjectPtr<USoundBase>& Sound, const FVector& Location);
	void PlayServerSound(const TSoftObjectPtr<USoundBase>& Sound, const FVector& Location);

	// The NoCast + NoBadEffects reflectors and the exact controller that received
	// the additive +4 glow, per affected pawn.
	UPROPERTY() TMap<TObjectPtr<APawn>, FWoTAMAAffectedPawnState> AffectedPawns;

	enum class EPhase : uint8 { Idle, Expanding, Expanded, Fading };
	EPhase Phase = EPhase::Idle;
	float CurScale = 0.1f;
	float ScaleRate = 0.0f;
	float HoldUntil = 0.0f;
	float CurrentAlphaScalar = 1.0f;
	float CurrentLightRadius = 0.0f;
	float FullLightIntensity = 0.0f;
	int32 AbsorptionCountForTest = 0;
	int32 AntiMagicTriggerCountForTest = 0;
	int32 PawnPassCountForTest = 0;
	UPROPERTY(ReplicatedUsing=OnRep_AutoGo) bool bAutoGo = false;
	// When the hold ends (AMAPearl.uc:48 EndTime) — unlike HoldUntil it is NOT
	// reused as the fade deadline; late entries truncate their seal to it.
	float EndTime = 0.0f;
};

// RCAMAPearl is a real class-default variant so its translucent mesh seat and
// timing arrive correctly in the initial replication bunch on every client.
UCLASS()
class AWoTRCAMAPearl : public AWoTAMAPearl
{
	GENERATED_BODY()
public:
	AWoTRCAMAPearl();
};
