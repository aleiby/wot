#include "WoTFogLight.h"

#include "WoTZoneVolume.h"
#include "WoTPresentationPolicy.h"
#include "Payload/WoTPayloadActivation.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "UObject/UObjectGlobals.h"

AWoTFogLight::AWoTFogLight()
{
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	SetActorHiddenInGame(true);

	auto MakeSphere = [&](const TCHAR* Name, int32 SortPriority)
	{
		UStaticMeshComponent* C = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		C->SetupAttachment(Root);
		C->SetVisibility(false, true);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetTranslucentSortPriority(SortPriority);
		// The engine sphere is 100uu across, scaled R/50. The material pair
		// draws the sphere's INNER faces (depth test off, scene-depth clamped
		// integral), which keeps the fog with the camera inside the volume.
		// The importer's negative X scale does not get there on its own: UE
		// reverses culling for a negative-determinant transform, so the
		// sphere drew its outer faces and the fog vanished as the camera
		// entered it (Arena_05). Reversed culling draws the
		// inner faces for either sign of the scale.
		C->bReverseCulling = true;
		return C;
	};
	// Same two-pass per-channel screen split (and sort order) as the coronas:
	// Modulate below Additive.
	SphereMod = MakeSphere(TEXT("SphereMod"), 100);
	SphereAdd = MakeSphere(TEXT("SphereAdd"), 101);
}

void AWoTFogLight::BeginPlay()
{
	Super::BeginPlay();
	WorldReadinessHandle = RegisterWoTBootstrapWorldReadinessListener(
		FWoTBootstrapWorldReadinessEvent::FDelegate::CreateUObject(
			this, &AWoTFogLight::HandleWorldReadinessChanged));
	ActivateForAuthorizedWorld();
}

void AWoTFogLight::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterWoTBootstrapWorldReadinessListener(WorldReadinessHandle);
	DeactivateForUnauthorizedWorld();
	Super::EndPlay(EndPlayReason);
}

void AWoTFogLight::HandleWorldReadinessChanged(bool bReady)
{
	if (bReady)
	{
		ActivateForAuthorizedWorld();
	}
	else
	{
		DeactivateForUnauthorizedWorld();
	}
}

void AWoTFogLight::ActivateForAuthorizedWorld()
{
	if (bAuthorizedWorldActive || !IsWoTBootstrapWorldReady()
		|| !WoTPresentationAnimationEnabled(GetWorld()))
	{
		return;
	}
	bAuthorizedWorldActive = true;
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!SphereMesh)
	{
		return;
	}
	SphereMod->SetStaticMesh(SphereMesh);
	SphereAdd->SetStaticMesh(SphereMesh);
	if (bRuntimeProjected)
	{
		if (BindRuntimeSphere())
		{
			SetActorHiddenInGame(false);
		}
		return;
	}
	SphereMod->SetVisibility(true, true);
	SphereAdd->SetVisibility(true, true);
	// The fog gate owns the later camera-zone visibility decision. Visible is
	// the authorized initial state for a viewer already inside a fog zone.
	SetActorHiddenInGame(false);
}

bool AWoTFogLight::BindRuntimeSphere()
{
	// import_map.py place_foglights: one additive sphere per light with the
	// extraction parameters on M_WoT_VolumeFog; the modulate pass is hidden
	// (the port ships the proven single additive pass).
	UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(
		nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UMaterialInterface* Master = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_VolumeFog.M_WoT_VolumeFog"));
	UMaterialInstanceDynamic* Fog = Master
		? UMaterialInstanceDynamic::Create(Master, this) : nullptr;
	if (!SphereMesh || !Fog)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("%s: M_WoT_VolumeFog is absent; fog light stays hidden"),
			*GetName());
		return false;
	}
	Fog->SetVectorParameterValue(TEXT("FogColor"), FogColor);
	Fog->SetScalarParameterValue(TEXT("VolB"), VolBrightness);
	Fog->SetScalarParameterValue(TEXT("R"), Radius);
	const float Scale = Radius / 50.0f;
	SphereAdd->SetStaticMesh(SphereMesh);
	SphereAdd->SetRelativeScale3D(FVector(-Scale, Scale, Scale));
	SphereAdd->SetMaterial(0, Fog);
	SphereMod->SetVisibility(false, true);
	SphereAdd->SetVisibility(true, true);
	return true;
}

void AWoTFogLight::DeactivateForUnauthorizedWorld()
{
	bAuthorizedWorldActive = false;
	SetActorHiddenInGame(true);
	if (SphereMod)
	{
		SphereMod->SetVisibility(false, true);
		SphereMod->SetStaticMesh(nullptr);
	}
	if (SphereAdd)
	{
		SphereAdd->SetVisibility(false, true);
		SphereAdd->SetStaticMesh(nullptr);
	}
}

void UWoTFogGateSubsystem::Tick(float DeltaTime)
{
	if (!IsWoTBootstrapWorldReady())
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (!bScanned)
	{
		bScanned = true;
		for (TActorIterator<AWoTFogLight> It(World); It; ++It)
		{
			FogLights.Add(*It);
		}
		for (TActorIterator<AWoTZoneVolume> It(World); It; ++It)
		{
			if (It->bFogZone)
			{
				FogZones.Add(*It);
			}
		}
		if (FogLights.Num() == 0)
		{
			return;
		}
	}
	if (FogLights.Num() == 0)
	{
		return;
	}

	// Viewer position: the first local player's camera (the shot harness's
	// camera actor is its view target, so this covers replays too).
	FVector Cam = FVector::ZeroVector;
	bool bHaveCam = false;
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (PC->PlayerCameraManager)
		{
			Cam = PC->PlayerCameraManager->GetCameraLocation();
			bHaveCam = true;
		}
	}

	bool bInFog = false;
	if (bHaveCam)
	{
		for (const TObjectPtr<AWoTZoneVolume>& Z : FogZones)
		{
			if (IsValid(Z) && Z->ContainsPoint(Cam))
			{
				bInFog = true;
				break;
			}
		}
	}
	if (bInFog == bLastInFog)
	{
		return;
	}
	bLastInFog = bInFog;
	for (const TObjectPtr<AWoTFogLight>& F : FogLights)
	{
		if (IsValid(F))
		{
			F->SetActorHiddenInGame(!bInFog);
		}
	}
}

bool UWoTFogGateSubsystem::IsTickable() const
{
	return IsWoTBootstrapWorldReady() && Super::IsTickable();
}
