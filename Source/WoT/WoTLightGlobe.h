#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTLightGlobe.generated.h"

class AWoTActorRotator;
class AWoTMeshEffect;
class AWoTParticleSprayer;
class UWoTAmbientSoundComponent;

// Port of Angreal.LightGlobe (+ the five color leaves): the invisible,
// always-relevant follow actor the Light Sphere ter'angreal spawns. Spec:
// exports/script/Angreal/LightGlobe.uc. Replicates only its identity
// (FollowActor, Color, LGLightRadius); each non-dedicated machine builds the
// retail cosmetic chain locally — PixieJarRotator (AWoTActorRotator) orbiting
// the glass PixieJar shell (WoTMeshFX row) around this actor, and the Pixie
// rotator orbiting the color PixieSprayer bundle (WoTWeaveFX row, LT_Steady
// light + LoopLG ambient) around the jar. Divergences (PORT_NOTES "P04"):
// the per-tick /MagicNumber follow ease is normalized to the era's 30Hz frame
// (frame-rate independent), the jar's bCollideWorld 9x9 cylinder is not
// ported (pure SetLocation cosmetic), and the retail
// FollowActorLocation/Rotation legs replicated only for non-relevant follow
// targets are skipped (each machine reads the followed actor directly).
UCLASS()
class AWoTLightGlobe : public AActor
{
	GENERATED_BODY()

public:
	AWoTLightGlobe();

	// LightGlobe.uc defaultproperties.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Light Globe")
	FVector FollowOffset = FVector(30.0f, 15.0f, 70.0f);
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Light Globe")
	float MagicNumber = 4.0f;
	// Blue, Green, Purple, Red or Yellow (class-fixed on the color leaves).
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Light Globe")
	FName Color = TEXT("Blue");
	// LightRadius override; < 0 keeps the PixieSprayer's authored 25.
	UPROPERTY(Replicated, EditDefaultsOnly, Category = "WoT|Light Globe")
	int32 LGLightRadius = -1;

	UPROPERTY(ReplicatedUsing = OnRep_FollowActor)
	TObjectPtr<AActor> FollowActor;

	// LightGlobe.Follow: bind to an actor (spawning the local cosmetics), or
	// pass null to retire — the authority destroys itself, exactly retail.
	void Follow(AActor* Other);

	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// Spec probes.
	AWoTActorRotator* GetJarRotatorForTest() const { return JarRotator; }
	AWoTMeshEffect* GetJarForTest() const { return Jar; }
	AWoTActorRotator* GetPixieForTest() const { return Pixie; }
	AWoTParticleSprayer* GetSprayerForTest() const { return Sprayer; }

protected:
	UFUNCTION()
	void OnRep_FollowActor();

private:
	void SpawnJarChain();

	UPROPERTY() TObjectPtr<AWoTActorRotator> JarRotator;
	UPROPERTY() TObjectPtr<AWoTMeshEffect> Jar;
	UPROPERTY() TObjectPtr<AWoTActorRotator> Pixie;
	UPROPERTY() TObjectPtr<AWoTParticleSprayer> Sprayer;
	UPROPERTY() TObjectPtr<UWoTAmbientSoundComponent> Loop;
};

// The color leaves carry only the Color default (LightGlobeBlue.uc is empty —
// the base default already reads Blue). Class identity is the replicated
// LightType the artifact carries, so all five exist like retail.
UCLASS()
class AWoTLightGlobeBlue : public AWoTLightGlobe
{
	GENERATED_BODY()
};

UCLASS()
class AWoTLightGlobeGreen : public AWoTLightGlobe
{
	GENERATED_BODY()
public:
	AWoTLightGlobeGreen() { Color = TEXT("Green"); }
};

UCLASS()
class AWoTLightGlobePurple : public AWoTLightGlobe
{
	GENERATED_BODY()
public:
	AWoTLightGlobePurple() { Color = TEXT("Purple"); }
};

UCLASS()
class AWoTLightGlobeRed : public AWoTLightGlobe
{
	GENERATED_BODY()
public:
	AWoTLightGlobeRed() { Color = TEXT("Red"); }
};

UCLASS()
class AWoTLightGlobeYellow : public AWoTLightGlobe
{
	GENERATED_BODY()
public:
	AWoTLightGlobeYellow() { Color = TEXT("Yellow"); }
};
