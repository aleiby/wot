#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTAppearEffect.generated.h"

class AWoTParticleSprayer;

// ParticleSystems.AppearEffect (AppearEffect.uc): the generic "magically
// appears" producer — two counter-rotating AppearSprayer rings orbit the target
// at its collision radius, converging from ±CollisionHeight through its center
// over the effect duration (2 rev/s, retail SprayerRotationRate 131072), then
// the sprayers turn off and linger out (TurnOffParticles: bOn=false + 2s).
// Retail replicated the actor (RemoteRole=SimulatedProxy) with a simulated
// Tick; the port replicates the same identity fields and every machine drives
// its own local sprayers (the sprayer-cosmetics convention). The retail
// ActorRotator pair uses Min==Max fixed radius/rate — a fixed circular orbit —
// folded to an inline polar drive here (AWoTActorRotator remains the authored-
// level surface).
//
// The retail fade-in half (ShouldFade: bFadeIn && (bMultiplayerFade || SP)) is
// NOT ported: no shipped consumer sets bMultiplayerFade, so MP netmodes never
// fade — the ScaleGlow ramp is a standalone-only behavior (PORT_NOTES rider).
UCLASS()
class AWoTAppearEffect : public AActor
{
	GENERATED_BODY()

public:
	AWoTAppearEffect();

	// SetTopColor/SetBottomColor (AppearEffect.uc:77-142): PC_*/plain color name
	// -> corona texture name. Retail has no 'Yellow' case (only Gold reaches it);
	// the port's color seam hands out "Yellow" for PC_Gold, so both spellings map
	// to YellowCorona — the identical observable outcome. Unknown/None -> Cyan
	// (the retail default case).
	static FName CoronaForColor(FName Color);

	// SetColors' optional-argument fallbacks (uc:52-74): both empty -> Blue/Blue;
	// one empty -> both take the given color.
	static void ResolveColorPair(FName TopColor, FName BottomColor,
		FName& OutTop, FName& OutBottom);

	// Spawn with explicit corona sprite names (the pickup rarity pairs and the
	// summon sparkles set TopSprite/BottomSprite directly). Authority-only;
	// returns null off-authority or when the target is null/hidden (retail
	// SetAppearActor destroys the effect in that case).
	static AWoTAppearEffect* SpawnWithSprites(UWorld& World, AActor* AppearTarget,
		FName TopCorona, FName BottomCorona, float Duration = 1.5f,
		float TargetRadiusOverride = 0.0f,
		float TargetHalfHeightOverride = 0.0f);
	// Simulated first-tick consumers (MashadarCrack) create one local effect on
	// every machine, including a late-relevance client.
	static AWoTAppearEffect* SpawnLocalWithSprites(UWorld& World,
		AActor* AppearTarget, FName TopCorona, FName BottomCorona,
		float Duration = 1.5f, float TargetRadiusOverride = 0.0f,
		float TargetHalfHeightOverride = 0.0f);

	// Spawn via the retail SetColors path (player colors / literal pairs).
	static AWoTAppearEffect* SpawnColored(UWorld& World, AActor* AppearTarget,
		FName TopColor = NAME_None, FName BottomColor = NAME_None,
		float Duration = 1.5f);

	// Corona texture names ("CyanCorona") -> the AppearSprayer<Name> bundle rows.
	UPROPERTY(Replicated) FName TopSprite;
	UPROPERTY(Replicated) FName BottomSprite;
	UPROPERTY(Replicated) TObjectPtr<AActor> AppearTarget;
	// Retail LifeSpan default 1.5 (the ring-converge phase); ExtraFadeTime 1.0
	// rides on top (Initialize: LifeSpan += ExtraFadeTime).
	UPROPERTY(Replicated) float EffectDuration = 1.5f;
	// MashadarCrack has a logical 20x20 UE1 collision cylinder but the port's
	// decal presentation intentionally owns no collision primitive.
	UPROPERTY(Replicated) float TargetRadiusOverride = 0.0f;
	UPROPERTY(Replicated) float TargetHalfHeightOverride = 0.0f;

	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// First tick with a valid, visible target (retail Initialize).
	void InitializeRings();
	void UpdateRingPositions();
	// Retail TurnOffParticles: bOn=false + 2s linger, orbit drive stops.
	void TurnOffParticles();

	UPROPERTY(Transient) TObjectPtr<AWoTParticleSprayer> TopSprayer;
	UPROPERTY(Transient) TObjectPtr<AWoTParticleSprayer> BottomSprayer;
	bool bRings = false;
	bool bParticlesOff = false;
	float Elapsed = 0.0f;
	float RingOffset = 0.0f;
	float HeightShiftRate = 0.0f;
	float OrbitRadius = 0.0f;
	float OrbitPhaseRadians = 0.0f;

	static constexpr float ExtraFadeTime = 1.0f;          // uc:300
	static constexpr float SprayerRotationRate = 131072.0f;   // uc:298, UE1 units/s
};
