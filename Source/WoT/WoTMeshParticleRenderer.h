#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "WoTMeshParticleRenderer.generated.h"

class UMaterialInstanceDynamic;
class UTexture;
class UWoTVertexAnimComponent;

// Shared UE1 Mesh bParticles presentation: a pooled, camera-facing Engine Plane
// at every logical vertex of a hidden, still-animating vertex-mesh pose driver.
UCLASS()
class WOT_API UWoTMeshParticleRenderer : public UInstancedStaticMeshComponent
{
	GENERATED_BODY()

public:
	UWoTMeshParticleRenderer();

	void InitializeParticleRenderer(UWoTVertexAnimComponent* InPoseDriver,
		bool bInClearAndLogOnPoseFailure = false);
	void SetParticleTexture(UTexture* Texture);
	void UpdateParticleQuads(float DrawScale, float ScaleGlow, uint8 AmbientGlow = 0,
		TConstArrayView<FVector> AdditionalLogicalVertices = {});

	static float ComputeRetailParticleColor(float ScaleGlow, uint8 AmbientGlow = 0);
	UMaterialInstanceDynamic* GetParticleMaterial() const { return ParticleMaterial; }
	FIntPoint GetParticleTextureSize() const
	{
		return FIntPoint(ParticleTexWidth, ParticleTexHeight);
	}

private:
	UPROPERTY(Transient) TObjectPtr<UWoTVertexAnimComponent> PoseDriver;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> ParticleMaterial;
	TArray<FVector> PoseScratch;
	TArray<FTransform> TransformScratch;
	float LastAppliedParticleColor = -1.0f;
	int32 ParticleTexWidth = 128;
	int32 ParticleTexHeight = 128;
	bool bClearAndLogOnPoseFailure = false;
	bool bLoggedPoseReadFailure = false;
};
