#include "WoTBalefireFX.h"
#include "WoTSound.h"

#include "WoTAmbientSound.h"
#include "WoTMeshFX.h"
#include "WoTMeshLight.h"
#include "WoTParticleSprayer.h"
#include "WoTSpriteBillboard.h"
#include "WoTWeaveFX.h"
#include "WoTUE1LightRegistry.h"

#include "Components/PointLightComponent.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "Sound/SoundBase.h"
#include "UObject/UnrealType.h"

namespace
{
	void SetNumericPresentationProperty(AActor& Actor, FName Name, double Value)
	{
		if (FNumericProperty* Property = FindFProperty<FNumericProperty>(
			Actor.GetClass(), Name))
		{
			void* Address = Property->ContainerPtrToValuePtr<void>(&Actor);
			if (Property->IsFloatingPoint())
			{
				Property->SetFloatingPointPropertyValue(Address, Value);
			}
			else if (Property->IsInteger())
			{
				Property->SetIntPropertyValue(Address,
					static_cast<uint64>(FMath::RoundToInt64(Value)));
			}
		}
	}

	void SetBoolPresentationProperty(AActor& Actor, FName Name, bool bValue)
	{
		if (FBoolProperty* Property = FindFProperty<FBoolProperty>(
			Actor.GetClass(), Name))
		{
			Property->SetPropertyValue(
				Property->ContainerPtrToValuePtr<void>(&Actor), bValue);
		}
	}

	void ApplyTimelineRemovalPresentation(AActor& Target)
	{
		// BalefireEffect.RemoveFromTimeline: PHYS_None, collision off, frozen
		// animation, STY_Translucent, ScaleGlow=5, AmbientGlow=250. Reflect the
		// actor-level knobs where that port type exposes them, then enforce the
		// same presentation component-generically for decorations/movers/walls.
		Target.SetActorTickEnabled(false);
		Target.SetActorEnableCollision(false);
		SetNumericPresentationProperty(Target, TEXT("ScaleGlow"), 5.0);
		SetNumericPresentationProperty(Target, TEXT("AmbientGlow"), 250.0);
		SetBoolPresentationProperty(Target, TEXT("bStyleTranslucent"), true);

		TArray<UPrimitiveComponent*> Primitives;
		Target.GetComponents<UPrimitiveComponent>(Primitives);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			if (!IsValid(Primitive)) { continue; }
			Primitive->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Primitive->SetSimulatePhysics(false);
		}
		if (UProjectileMovementComponent* Move =
			Target.FindComponentByClass<UProjectileMovementComponent>())
		{
			Move->StopMovementImmediately();
			Move->Deactivate();
		}
		if (ACharacter* Character = Cast<ACharacter>(&Target))
		{
			if (UCharacterMovementComponent* Move = Character->GetCharacterMovement())
			{
				Move->StopMovementImmediately();
				Move->DisableMovement();
			}
		}

		// First route actor types that already expose the established display seam.
		if (UWoTMeshLight* MeshLight = Target.FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->ForceRuntimeBake();
			// Timeline removal freezes the presentation for its final half-second;
			// retire the periodic light rebake so it cannot overwrite the forced
			// translucent/overbright MIDs below before the actor disappears.
			MeshLight->DestroyComponent();
		}

		UMaterialInterface* TranslucentMaster = Cast<UMaterialInterface>(
			FSoftObjectPath(TEXT("/Game/WoT/Materials/M_WoT_MeshTranslucent."
				"M_WoT_MeshTranslucent")).TryLoad());
		TArray<UMeshComponent*> Meshes;
		Target.GetComponents<UMeshComponent>(Meshes);
		for (UMeshComponent* Mesh : Meshes)
		{
			if (!IsValid(Mesh)) { continue; }
			Mesh->SetComponentTickEnabled(false);
			if (USkeletalMeshComponent* Skeletal = Cast<USkeletalMeshComponent>(Mesh))
			{
				Skeletal->bPauseAnims = true;
				Skeletal->GlobalAnimRateScale = 0.0f;
			}
			for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
			{
				UMaterialInterface* Source = Mesh->GetMaterial(Slot);
				if (!IsValid(Source)) { continue; }
				UTexture* Texture = nullptr;
				FLinearColor KeyColor = FLinearColor::Black;
				Source->GetTextureParameterValue(
					FHashedMaterialParameterInfo(TEXT("Tex")), Texture);
				Source->GetVectorParameterValue(
					FHashedMaterialParameterInfo(TEXT("KeyColor")), KeyColor);
				if (!IsValid(Texture))
				{
					TArray<UTexture*> Used;
					Source->GetUsedTextures(Used, EMaterialQualityLevel::Num,
						true, ERHIFeatureLevel::Num, true);
					Texture = Used.IsEmpty() ? nullptr : Used[0];
				}
				UMaterialInstanceDynamic* MID = Mesh->CreateDynamicMaterialInstance(
					Slot, IsValid(TranslucentMaster) ? TranslucentMaster : Source);
				if (!IsValid(MID)) { continue; }
				if (IsValid(Texture))
				{
					MID->SetTextureParameterValue(TEXT("Tex"), Texture);
				}
				MID->SetVectorParameterValue(TEXT("KeyColor"), KeyColor);
				MID->SetScalarParameterValue(TEXT("MLScaleGlow"), 5.0f);
				const float Overbright = 5.0f * 0.5f + 250.0f / 256.0f;
				MID->SetVectorParameterValue(TEXT("MLAmbient"),
					FLinearColor(Overbright, Overbright, Overbright, 1.0f));
			}
		}
	}

	FRotator PawnFollowRotation(const APawn& Pawn)
	{
		// Shipped Pawn.ViewRotation is the controller rotation for a controlled
		// pawn, independent of PlayerCameraManager/free-camera view. A simulated
		// proxy has no controller, so retain UE's actor-yaw + RemoteViewPitch
		// reconstruction through GetBaseAimRotation.
		return Pawn.GetController()
			? Pawn.GetViewRotation() : Pawn.GetBaseAimRotation();
	}
}

AWoTBalefireCorona::AWoTBalefireCorona()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	HandSprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("BFHand"));
	HandSprite->SetupAttachment(RootComponent);
	HandSprite->DrawScale = 0.5f;
	Light = CreateDefaultSubobject<UPointLightComponent>(TEXT("Light"));
	Light->SetupAttachment(RootComponent);
	Light->SetVisibility(false);
	Ambient = CreateDefaultSubobject<UWoTAmbientSoundComponent>(TEXT("LoopBF"));
	Ambient->SetupAttachment(RootComponent);
}

void AWoTBalefireCorona::Configure(APawn* InPawn)
{
	FollowPawn = InPawn;
	if (const FWoTSpriteEffectRow* Row = WoTMeshFX::FindSprite(TEXT("BalefireHand")))
	{
		HandSprite->DrawScale = Row->DrawScale;
		HandSprite->ScaleGlow = Row->ScaleGlow;
		HandSprite->SetAnimatedTextureSeries(Row->FrameIdentities, Row->TexWidth,
			Row->TexHeight, Row->MinFrameRate, Row->MaxFrameRate);
	}
	// This radius-five gather actor is BalefireCorona; BFExpSprayer is the
	// delayed radius-twelve impact burst spawned by AWoTBalefireAfterglow.
	WoTUE1LightRegistry::Configure(Light, TEXT("BalefireCorona"));
	if (USoundBase* Loop = WoTSound::LoadBySubPath(
		TEXT("Angreal/LoopBF"), true))
	{
		Ambient->ConfigureUE1Ambient(Loop, 72, 255, 64);
		Ambient->SetVolumeMultiplier(0.0f);
		Ambient->Play();
	}

	static const FName Configs[] = {
		TEXT("BFSprayerPurpleCorona"), TEXT("BFSprayerPurpleSpark"),
		TEXT("BFSprayerWhiteCorona"), TEXT("BFSprayerAPurpleCorona"),
		TEXT("BFSprayerBlueSpark"), TEXT("BFSprayerPF04")};
	static const FVector Corners[] = {
		FVector(30, 0, 0), FVector(-30, 0, 0), FVector(0, 30, 0),
		FVector(0, -30, 0), FVector(0, 0, 30), FVector(0, 0, -30)};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Configs); ++Index)
	{
		const FVector Local = Corners[Index];
		const FVector WorldPos = GetActorLocation() + GetActorRotation().RotateVector(Local);
		const FRotator Inward = (GetActorLocation() - WorldPos).Rotation();
		if (AWoTParticleSprayer* S = WoTWeaveFX::SpawnLocal(
			*GetWorld(), Configs[Index], WorldPos, Inward))
		{
			Sprayers.Add(S);
		}
	}
	LastLocation = GetActorLocation();
}

void AWoTBalefireCorona::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!IsValid(FollowPawn)) { Destroy(); return; }
	const FRotator View = PawnFollowRotation(*FollowPawn);
	FVector Eye = FollowPawn->GetActorLocation();
	Eye.Z += FollowPawn->BaseEyeHeight;
	const FVector Desired = Eye + View.RotateVector(FVector(50.0f, 0.0f, -30.0f));
	const FVector Shift = Desired - LastLocation;
	SetActorLocation(Desired);
	AddActorLocalRotation(FRotator(100000.0f, 80000.0f, 60000.0f)
		* (360.0f / 65536.0f) * DeltaSeconds);
	static const FVector Faces[] = {
		FVector(30, 0, 0), FVector(-30, 0, 0), FVector(0, 30, 0),
		FVector(0, -30, 0), FVector(0, 0, 30), FVector(0, 0, -30)};
	for (int32 Index = 0; Index < Sprayers.Num(); ++Index)
	{
		AWoTParticleSprayer* S = Sprayers[Index];
		if (IsValid(S))
		{
			const FVector Face = Desired + GetActorRotation().RotateVector(Faces[Index]);
			S->SetActorLocation(Face);
			S->ShiftParticles(Shift);
			S->SetActorRotation((Desired - Face).Rotation());
		}
	}
	LastLocation = Desired;
	FadeAlpha = FMath::Min(FadeAlpha + DeltaSeconds * 0.5f, 1.0f);
	Ambient->SetVolumeMultiplier(Ambient->BaseVolumeMultiplier * FadeAlpha);
}

void AWoTBalefireCorona::Destroyed()
{
	const FVector Eject = IsValid(FollowPawn)
		? PawnFollowRotation(*FollowPawn).Vector() * 10000.0f : FVector::ZeroVector;
	for (AWoTParticleSprayer* S : Sprayers)
	{
		if (IsValid(S))
		{
			S->bOn = false;
			S->Gravity = Eject;
			S->SetLifeSpan(2.0f);
		}
	}
	Ambient->Stop();
	Super::Destroyed();
}

AWoTBalefireRemovalMarker::AWoTBalefireRemovalMarker()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bNetTemporary = true;
	bAlwaysRelevant = true;
	SetReplicatingMovement(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetLifeSpan(2.25f);
}

void AWoTBalefireRemovalMarker::BeginPlay()
{
	Super::BeginPlay();
	WoTWeaveFX::SpawnExplosion(*GetWorld(), TEXT("BFExplodeA"), GetActorLocation());
	if (bRemoveTarget && IsValid(Target))
	{
		ApplyTimelineRemovalPresentation(*Target);
	}
}

void AWoTBalefireRemovalMarker::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Elapsed += DeltaSeconds;
	if (!bReleasedTarget && Elapsed >= 0.5f)
	{
		bReleasedTarget = true;
		if (bRemoveTarget && IsValid(Target)
			&& (HasAuthority() || !Target->GetIsReplicated()))
		{
			// Replicated targets are destroyed by authority/channel teardown. Retail
			// map decorations do not replicate, so the replicated marker is also the
			// server decision that removes each client's stable local map actor.
			Target->Destroy();
		}
	}
	if (!bSpawnedAfterglow && Elapsed >= 2.0f)
	{
		bSpawnedAfterglow = true;
		WoTBalefireFX::SpawnAfterglow(*GetWorld(), GetActorLocation());
	}
}

AWoTBalefireAfterglow::AWoTBalefireAfterglow()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	InitialLifeSpan = 1.8f;
}

void AWoTBalefireAfterglow::BeginPlay()
{
	Super::BeginPlay();
	WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("BFExpSprayer"),
		GetActorLocation(), FRotator::ZeroRotator);
	Globe = WoTMeshFX::SpawnLocal(*GetWorld(), TEXT("BFExpGlobe"),
		GetActorLocation());
}

void AWoTBalefireAfterglow::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	FrameAccumulator += DeltaSeconds;
	while (FrameAccumulator >= 1.0f / 30.0f)
	{
		FrameAccumulator -= 1.0f / 30.0f;
		GlobeScale += (4.0f - GlobeScale) / 4.0f;
	}
	if (IsValid(Globe))
	{
		Globe->SetEffectDrawScale(GlobeScale);
		Globe->SetEffectGlow(1.0f - GlobeScale / 4.0f);
	}
}

void AWoTBalefireRemovalMarker::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTBalefireRemovalMarker, Target);
	DOREPLIFETIME(AWoTBalefireRemovalMarker, bRemoveTarget);
}

AWoTBalefireCorona* WoTBalefireFX::SpawnGather(UWorld& World, APawn* Pawn)
{
	if (!IsValid(Pawn) || World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	FVector Eye = Pawn->GetActorLocation();
	Eye.Z += Pawn->BaseEyeHeight;
	const FRotator View = PawnFollowRotation(*Pawn);
	const FTransform At(View, Eye + View.RotateVector(FVector(50, 0, -30)));
	AWoTBalefireCorona* Corona = World.SpawnActorDeferred<AWoTBalefireCorona>(
		AWoTBalefireCorona::StaticClass(), At, Pawn, Pawn,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Corona) { Corona->FinishSpawning(At); Corona->Configure(Pawn); }
	return Corona;
}

AWoTBalefireRemovalMarker* WoTBalefireFX::SpawnRemoval(UWorld& World,
	AActor* Target, const FVector& Location, bool bRemoveTarget)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_Client) { return nullptr; }
	const FTransform At(FRotator::ZeroRotator, Location);
	AWoTBalefireRemovalMarker* Marker = World.SpawnActorDeferred<AWoTBalefireRemovalMarker>(
		AWoTBalefireRemovalMarker::StaticClass(), At, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Marker)
	{
		Marker->Target = Target;
		Marker->bRemoveTarget = bRemoveTarget;
		Marker->FinishSpawning(At);
	}
	return Marker;
}

AWoTBalefireAfterglow* WoTBalefireFX::SpawnAfterglow(
	UWorld& World, const FVector& Location)
{
	if (World.bIsTearingDown || World.GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}
	return World.SpawnActor<AWoTBalefireAfterglow>(Location,
		FRotator::ZeroRotator);
}
