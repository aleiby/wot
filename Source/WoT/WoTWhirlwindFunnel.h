#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WoTWhirlwindFunnel.generated.h"

// Local-cosmetic DT_Mesh particle implementation for Angreal.WhirlwindSprayer.
// The shared AWoTParticleSprayer renders camera-facing static planes; the retail
// funnel instead emits the one-frame skeletal WhirlwindSlice mesh, so this
// focused actor keeps the exact particle lifecycle without widening every
// billboard sprayer call site.
UCLASS()
class AWoTWhirlwindFunnel : public AActor
{
	GENERATED_BODY()

public:
	AWoTWhirlwindFunnel();

	UPROPERTY(VisibleAnywhere, Category = "WoT") FName MeshName = TEXT("WhirlwindSlice");
	UPROPERTY(EditAnywhere, Category = "WoT") float Volume = 20.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float ParticleLifeSpan = 3.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") float GrowRate = 0.8f;
	UPROPERTY(EditAnywhere, Category = "WoT") float RotationYawRateUnits = 64000.0f;
	UPROPERTY(EditAnywhere, Category = "WoT") bool bOn = true;
	// UE1 actor lighting fields consumed reflectively by UWoTMeshLight.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") float ScaleGlow = 1.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") float AmbientGlow = 0.0f;
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") bool bUnlit = true;
	// UE1 Style=STY_Translucent: WhirlwindSprayer keeps ParticleSprayer's
	// default (ParticleSprayer.uc:441) and its mesh particles copy the
	// sprayer's render state with Skin = WWtop (the original sprayer renderer),
	// so the slices draw additive and their dark texels vanish.
	UPROPERTY(EditDefaultsOnly, Category = "WoT|Display") bool bStyleTranslucent = true;

	void SetHeight(float Height);
	void ShiftParticles(const FVector& Delta);
	float GetHeight() const { return HeightUU; }
	int32 GetLiveSliceCount() const;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	static constexpr int32 MaxSlices = 64;
	struct FSliceParticle
	{
		float RemainingLife = 0.0f;
		float DrawScale = 0.0f;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};

	UPROPERTY(Transient)
	TArray<TObjectPtr<class UWoTVertexAnimComponent>> SliceMeshes;
	FSliceParticle Particles[MaxSlices];
	int32 Cursor = 0;
	float SpawnAccumulator = 0.0f;
	float HeightUU = 150.0f;

	void SpawnSlice();
};
