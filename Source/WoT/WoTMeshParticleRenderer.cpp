#include "WoTMeshParticleRenderer.h"

#include "WoTVertexAnimComponent.h"
#include "WoTSpriteBillboard.h"

#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTMeshParticles, Log, All);

UWoTMeshParticleRenderer::UWoTMeshParticleRenderer()
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);
	NumCustomDataFloats = 1;
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetWorldLocationAndRotation(FVector::ZeroVector, FQuat::Identity);
	SetTranslucentSortPriority(91);
	if (!IsRunningDedicatedServer())
	{
		static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(
			TEXT("/Engine/BasicShapes/Plane.Plane"));
		if (Plane.Succeeded()) SetStaticMesh(Plane.Object);
	}
}

void UWoTMeshParticleRenderer::InitializeParticleRenderer(
	UWoTVertexAnimComponent* InPoseDriver,
	const bool bInClearAndLogOnPoseFailure)
{
	PoseDriver = InPoseDriver;
	bClearAndLogOnPoseFailure = bInClearAndLogOnPoseFailure;
	if ((GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
		|| !InPoseDriver)
	{
		return;
	}
	UMaterialInterface* Parent = Cast<UMaterialInterface>(FSoftObjectPath(
		TEXT("/Game/WoT/Materials/M_WoT_ParticleAdd.M_WoT_ParticleAdd")).TryLoad());
	if (Parent)
	{
		ParticleMaterial = UMaterialInstanceDynamic::Create(Parent, GetOwner());
		SetMaterial(0, ParticleMaterial);
	}
}

void UWoTMeshParticleRenderer::SetParticleTexture(UTexture* Texture)
{
	if (!Texture) return;
	if (const UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
	{
		ParticleTexWidth = Texture2D->GetSizeX();
		ParticleTexHeight = Texture2D->GetSizeY();
#if WITH_EDITORONLY_DATA
		// A freshly imported editor texture has no platform size under null RHI.
		// Its source dimensions are still the authored USize/VSize; runtime
		// textures have platform data and never enter this fallback.
		if (ParticleTexWidth == 0 || ParticleTexHeight == 0)
		{
			const FIntPoint ImportedSize = Texture2D->GetImportedSize();
			ParticleTexWidth = ImportedSize.X;
			ParticleTexHeight = ImportedSize.Y;
		}
#endif
	}
	else
	{
		ParticleTexWidth = static_cast<int32>(Texture->GetSurfaceWidth());
		ParticleTexHeight = static_cast<int32>(Texture->GetSurfaceHeight());
	}
	if (ParticleMaterial)
	{
		ParticleMaterial->SetTextureParameterValue(TEXT("Tex"), Texture);
	}
}

float UWoTMeshParticleRenderer::ComputeRetailParticleColor(
	const float ScaleGlow, const uint8 AmbientGlow)
{
	float Color = FMath::Clamp(
		ScaleGlow * 0.5f + static_cast<float>(AmbientGlow) / 256.0f,
		0.0f, 1.0f);
	// UE1's mesh renderer applies ScaleGlow again for every value except exactly
	// one. The discontinuity is retail behavior, not a smoothing opportunity.
	if (ScaleGlow != 1.0f)
	{
		Color = FMath::Clamp(Color * ScaleGlow, 0.0f, 1.0f);
	}
	return Color;
}

void UWoTMeshParticleRenderer::UpdateParticleQuads(
	const float DrawScale, const float ScaleGlow, const uint8 AmbientGlow,
	const TConstArrayView<FVector> AdditionalLogicalVertices)
{
	if ((GetWorld() && GetWorld()->GetNetMode() == NM_DedicatedServer)
		|| !PoseDriver || !ParticleMaterial)
	{
		return;
	}
	if (!PoseDriver->GetCurrentPoseVertexPositions(PoseScratch))
	{
		if (!bClearAndLogOnPoseFailure) return;
		// A stale pool is still visible. Runtime pose failure therefore empties it;
		// the hidden surface is never used as a fallback.
		ClearInstances();
		if (!bLoggedPoseReadFailure)
		{
			bLoggedPoseReadFailure = true;
			UE_LOG(LogWoTMeshParticles, Warning,
				TEXT("%s cannot read bParticles current pose; drawing no particles"),
				GetOwner() ? *GetOwner()->GetPathName() : TEXT("<unowned>"));
		}
		return;
	}

	const int32 PreviousInstanceCount = GetInstanceCount();
	APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0);
	const FQuat Face = Camera
		? WoTSpriteFacing::ScreenAlignedQuat(Camera->GetCameraRotation())
		: FQuat::Identity;
	const FVector QuadScale(
		DrawScale * static_cast<float>(ParticleTexWidth) / 100.0f,
		DrawScale * static_cast<float>(ParticleTexHeight) / 100.0f,
		1.0f);
	const float Color = ComputeRetailParticleColor(ScaleGlow, AmbientGlow);
	const FTransform& MeshTransform = PoseDriver->GetComponentTransform();
	const int32 PoseInstanceCount = PoseScratch.Num() + AdditionalLogicalVertices.Num();
	const int32 InstanceCount = FMath::Max(PreviousInstanceCount, PoseInstanceCount);
	TransformScratch.SetNumUninitialized(InstanceCount);
	for (int32 Vertex = 0; Vertex < PoseScratch.Num(); ++Vertex)
	{
		TransformScratch[Vertex] = FTransform(
			Face, MeshTransform.TransformPosition(PoseScratch[Vertex]), QuadScale);
	}
	for (int32 Vertex = 0; Vertex < AdditionalLogicalVertices.Num(); ++Vertex)
	{
		TransformScratch[PoseScratch.Num() + Vertex] = FTransform(
			Face, MeshTransform.TransformPosition(AdditionalLogicalVertices[Vertex]),
			QuadScale);
	}
	// Pool slots never shrink during normal animation/rebinding. Collapse an
	// unused tail instead of reallocating it.
	for (int32 Vertex = PoseInstanceCount; Vertex < InstanceCount; ++Vertex)
	{
		TransformScratch[Vertex] = FTransform(
			FQuat::Identity, GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector,
			FVector(0.0001f));
	}
	if (PreviousInstanceCount == 0 && InstanceCount > 0)
	{
		AddInstances(TransformScratch, false, true, false);
	}
	else
	{
		for (int32 Instance = PreviousInstanceCount; Instance < InstanceCount; ++Instance)
		{
			AddInstance(TransformScratch[Instance], true);
		}
	}
	BatchUpdateInstancesTransforms(0, TransformScratch, true, false, true);

	const bool bGlowChanged = Color != LastAppliedParticleColor;
	const int32 FirstCustomDataInstance = bGlowChanged ? 0 : PreviousInstanceCount;
	for (int32 Instance = FirstCustomDataInstance; Instance < InstanceCount; ++Instance)
	{
		SetCustomDataValue(Instance, 0, Color, false);
	}
	if (bGlowChanged) LastAppliedParticleColor = Color;
	MarkRenderStateDirty();
}
