#pragma once

#include "CoreMinimal.h"
#include "WoTIKTrailer.h"
#include "../WoTRetailProjectileTarget.h"
#include "WoTMashadarSegment.generated.h"

class AWoTMashadarGuide;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UWoTMeshParticleRenderer;
class USphereComponent;

// One body segment of a Mashadar tendril: an IK trailer joint carrying a collision
// volume that deals Spirit ('xxxxS') touch damage to enemies the body sweeps over.
// The chain of these — head segment at the guide, the rest trailing back to the lair —
// is the tendril's damaging reach. Spec: exports/script/WoTPawns/MashadarTrailer.uc.
//
// Damage flows through the guide: a hit on any segment hurts the whole tendril
// (TakeDamage -> guide), and segments only bite while the guide is bActive.
// MashTip/MashSeg drive the shipped per-current-vertex MyTex2 sprite cloud;
// collision + damage remain server-authoritative.
UCLASS()
class AWoTMashadarSegment : public AWoTIKTrailer, public IWoTRetailProjectileTarget
{
	GENERATED_BODY()

public:

	// MashTip (head) / MashSeg (body) is the hidden animation/collision driver.
	// Retail bParticles draws one camera-facing MyTex2 quad at every CURRENT-pose
	// vertex. bRandomFrame is intentionally absent: neither shipped leaf authors it.
	UPROPERTY(Replicated) bool bIsHead = false;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<class UWoTVertexAnimComponent> DisplayMesh;
	UPROPERTY(VisibleAnywhere, Category = "WoT")
	TObjectPtr<UInstancedStaticMeshComponent> ParticleQuads;
	// FadeIn: ScaleGlow 0 -> random 0.2-0.4 at +0.01/frame (~30Hz); FadeAway:
	// -0.02/frame with DrawScale +0.01/frame, authority destroys at 0.
	void FadeIn();
	AWoTMashadarSegment();

	// The tendril brain we belong to (damage routes through it; we bite only while it
	// is bActive). Weak — the guide owns our lifetime, not the reverse.
	UPROPERTY() TWeakObjectPtr<AWoTMashadarGuide> Guide;

	// Spirit damage per contact (MashadarTrailer.uc Damage 1).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") int32 Damage = 1;

	// After this many consecutive no-effect bites (e.g. a shielded victim) this segment
	// gives up and deactivates the tendril (MashadarTrailer.uc NoDamageTolerance 3).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") int32 NoDamageTolerance = 3;

	// Collision half-size of a body segment (MashadarTrailer takes its extent
	// from the mesh).
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Mashadar") float TouchRadius = 24.0f;

	// Fade this segment away and cascade down the chain
	// (MashadarTrailer.uc FadeAway/Destroyed).
	void FadeAway();
	virtual void Tick(float DeltaTime) override;

	// Bite an actor swept into (or handed to) this segment: Spirit-damage an enemy while
	// the guide is active (MashadarTrailer.uc:52 Touch). The overlap handler and tests
	// both go through here.
	void BiteTarget(AActor* Other);

	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	static float ComputeRetailParticleColor(
		float InScaleGlow, uint8 AmbientGlow = 0);
	UMaterialInstanceDynamic* GetParticleMaterial() const;
	FIntPoint GetParticleTextureSize() const;
	float GetCurrentScaleGlow() const { return ScaleGlow; }

private:
	void BindDisplay();
	void ApplyGlow();
	void InitializeParticleRenderer();
	void UpdateParticleQuads();
	UWoTMeshParticleRenderer* GetParticleRenderer() const;

	enum class EFade : uint8 { In, Hold, Away };
	EFade Fade = EFade::In;
	float ScaleGlow = 0.0f;
	float DesiredScaleGlow = 0.3f;
	float BaseDrawScale = 0.3f;
	float DrawScaleNow = 0.3f;
	bool bDisplayBound = false;

public:
	virtual void Destroyed() override;
	virtual float TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	                         AController* EventInstigator, AActor* DamageCauser) override;

private:
	// Enemy swept into our volume: bite it (MashadarTrailer.uc:52 Touch).
	UFUNCTION()
	void OnSegmentOverlap(UPrimitiveComponent* OverlappedComp, AActor* Other,
	                      UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	                      bool bFromSweep, const FHitResult& Sweep);

	UPROPERTY() TObjectPtr<USphereComponent> TouchVolume;

	int32 NoDamageCounter = 0;
	// Port of MashadarTrailer.uc's FadingAway state gate: once a segment starts the
	// cascade, Destroyed() must not walk the chain a second time.
	bool bFadingAway = false;
};
