#include "WoTSkyBox.h"

#include "Components/SceneCaptureComponentCube.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/TextureRenderTargetCube.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

namespace
{
TAutoConsoleVariable<int32> CVarWoTSkyWindowCubeSize(
	TEXT("wot.SkyWindowCubeSize"), 1024,
	TEXT("Sky-window child-scene cube face size: 1024 (default), 512, or 256."),
	ECVF_Default);
}

int32 WoTSkyWindowCaptureSize()
{
	const int32 Requested = CVarWoTSkyWindowCubeSize.GetValueOnAnyThread();
	if (Requested <= 256) return 256;
	if (Requested <= 512) return 512;
	return 1024;
}

AWoTSkyBox::AWoTSkyBox()
{
	PrimaryActorTick.bCanEverTick = true;
	// Re-transformed every frame to camera-lock, so the mesh must be Movable.
	if (UStaticMeshComponent* SMC = GetStaticMeshComponent())
	{
		SMC->SetMobility(EComponentMobility::Movable);
		SMC->SetCastShadow(false);
		// The sky mesh envelops the whole world every frame — it must never be
		// query-collidable, or every pawn's FindFloor start-penetrates it and
		// walking velocity zeroes map-wide. The PROFILE
		// must be NoCollision, not just the enabled flag: PostLoad reapplies
		// the serialized profile template over placed instances
		// (FBodyInstance::LoadProfileData), which clobbered a bare
		// SetCollisionEnabled against the inherited BlockAllDynamic profile.
		SMC->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		// The camera sits AT the mesh pivot inside the bounds; LOD screen-size
		// selection degenerates there and can pick an empty auto-generated LOD.
		// The sky is always full-detail: pin LOD0.
		SMC->SetForcedLodModel(1);
	}
	SkyCapture = CreateDefaultSubobject<USceneCaptureComponentCube>(
		TEXT("SkyWindowCapture"));
	SkyCapture->SetupAttachment(GetRootComponent());
	SkyCapture->SetMobility(EComponentMobility::Movable);
	SkyCapture->SetAbsolute(false, true, true);
	SkyCapture->bAutoActivate = false;
	SkyCapture->bCaptureEveryFrame = false;
	SkyCapture->bCaptureOnMovement = false;
	SkyCapture->bAlwaysPersistRenderingState = false;
	SkyCapture->CaptureSource = SCS_SceneColorHDR;
}

bool AWoTSkyBox::InitializeSkyWindowCapture()
{
	UStaticMeshComponent* SkyMesh = GetStaticMeshComponent();
	if (SkyMesh == nullptr || SkyCapture == nullptr) return false;

	SkyCaptureTarget = NewObject<UTextureRenderTargetCube>(
		this, TEXT("SkyWindowTarget"), RF_Transient);
	if (SkyCaptureTarget == nullptr) return false;
	SkyCaptureTarget->ClearColor = FLinearColor::Black;
	SkyCaptureTarget->bAutoGenerateMips = false;
	SkyCaptureTarget->bForceLinearGamma = true;
	SkyCaptureTarget->Init(WoTSkyWindowCaptureSize(), PF_B8G8R8A8);

	// Capture only this child scene. The main camera sees the opaque window
	// sheets, never the up-scaled sky mesh behind the level.
	SkyMesh->SetVisibleInSceneCaptureOnly(true);
	SkyCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	SkyCapture->ClearShowOnlyComponents();
	SkyCapture->ShowOnlyComponent(SkyMesh);
	for (UStaticMeshComponent* Layer : SkyLayers)
		if (Layer != nullptr) SkyCapture->ShowOnlyComponent(Layer);
	SkyCapture->TextureTarget = SkyCaptureTarget;
	SkyCapture->bCaptureEveryFrame = true;
	SkyCapture->Activate(true);
	return true;
}

UStaticMeshComponent* AWoTSkyBox::AddSkyLayer(UStaticMesh* Mesh,
	UMaterialInterface* Material, const int32 SortPriority)
{
	UStaticMeshComponent* Root = GetStaticMeshComponent();
	if (Mesh == nullptr || Material == nullptr || Root == nullptr) return nullptr;
	UStaticMeshComponent* Layer = NewObject<UStaticMeshComponent>(this,
		FName(*FString::Printf(TEXT("SkyLayer_%02d"), SkyLayers.Num())), RF_Transient);
	if (Layer == nullptr) return nullptr;
	// Same contract as the sky mesh itself: movable with the camera lock, never
	// collidable, full detail, and only ever drawn by this plan's cube capture.
	Layer->SetMobility(EComponentMobility::Movable);
	Layer->SetupAttachment(Root);
	Layer->SetCastShadow(false);
	Layer->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	Layer->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Layer->SetForcedLodModel(1);
	Layer->SetStaticMesh(Mesh);
	Layer->SetMaterial(0, Material);
	Layer->SetTranslucentSortPriority(SortPriority);
	Layer->SetVisibleInSceneCaptureOnly(true);
	AddInstanceComponent(Layer);
	// A deferred spawn registers it in FinishSpawning; a live sky must now.
	if (Root->IsRegistered()) Layer->RegisterComponent();
	SkyLayers.Add(Layer);
	return Layer;
}

void AWoTSkyBox::SetSkyWindowCaptureActive(const bool bActive)
{
	if (SkyCapture == nullptr || SkyCaptureTarget == nullptr) return;
	if (SkyCapture->bCaptureEveryFrame == bActive) return;
	SkyCapture->bCaptureEveryFrame = bActive;
	if (bActive) SkyCapture->CaptureSceneDeferred();
}

void AWoTSkyBox::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Heal stale placed instances: maps re-saved while the clobber was live
	// (e.g. a PPV re-save) serialized the blocking state
	// explicitly, so enforce the no-collision contract at init regardless of
	// what the umap carries.
	if (UStaticMeshComponent* SMC = GetStaticMeshComponent())
	{
		SMC->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		SMC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AWoTSkyBox::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Pin the sky pivot (the SkyZoneInfo origin, on which the mesh is centered)
	// to the current view point, apply the sky rotation, scale up beyond the
	// world. worldPos(v) = View + SkyRotation*(SkyScale*v), so a vertex at
	// (P - SkyOrigin) appears in direction SkyRotation*(P - SkyOrigin),
	// independent of the view position (infinite parallax, like the UE1 child
	// frame re-rooted at the sky zone).
	//
	// This per-tick transform renders correctly in every -game mode, including
	// -RenderOffscreen captures (verified with WoTEOFProbe). An earlier
	// "runtime transforms never draw" diagnosis was wrong: the placed box
	// was invisible because setup_childsky.py's hide loop overrode its own
	// material slots with M_WoT_SkyHidden (fixed in the script; maps re-saved).
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	FVector ViewLoc; FRotator ViewRot;
	PC->GetPlayerViewPoint(ViewLoc, ViewRot);
	SetActorTransform(FTransform(SkyRotation, ViewLoc, FVector(SkyScale)));
}
