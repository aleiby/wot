#include "WoTDecoration.h"
#include "WoTRandom.h"

#include "WoTMeshLight.h"
#include "WoTPayloadSubsystem.h"
#include "WoTSpriteBillboard.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "AI/WoTNPCCharacter.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

AWoTDecoration::AWoTDecoration()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("Mesh"));
	Collision = CreateDefaultSubobject<UWoTFlatCylinderComponent>(TEXT("Collision"));
	RootComponent = Collision;
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Collision->SetCylinderSize(22.0f, 22.0f);
	ApplyRetailCollision();
	MeshLight = CreateDefaultSubobject<UWoTMeshLight>(TEXT("MeshLight"));
	Sprite = CreateDefaultSubobject<UWoTSpriteBillboardComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(Collision);
	// Only DT_Sprite decorations draw it; the rest must not pay for its tick.
	Sprite->PrimaryComponentTick.bStartWithTickEnabled = false;
}

void AWoTDecoration::ConfigureRuntimePhysics(uint8 Physics,
	const FRotator& RotationRate, bool bFixedRotationDir)
{
	// Actor EPhysics: PHYS_Rotating is 5. The retail fixed-direction leg
	// continuously adds RotationRate * DeltaTime (UE1 physRotating).
	constexpr uint8 PhysicsRotating = 5;
	RuntimeRotationRate = RotationRate;
	bRuntimeFixedRotation = Physics == PhysicsRotating && bFixedRotationDir
		&& !RotationRate.IsNearlyZero();
	SetActorTickEnabled(bRuntimeFixedRotation);
}

void AWoTDecoration::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bRuntimeFixedRotation && Mesh)
	{
		// ProjectDecoration leaves the UE1 cylinder world-axis-aligned and puts
		// authored rotation on Mesh, so physics rotation belongs there as well.
		Mesh->SetRelativeRotation(
			Mesh->GetRelativeRotation() + RuntimeRotationRate * DeltaSeconds);
	}
	if (bRuntimeDistanceFade && GetWorld())
	{
		FVector ViewLocation = FVector::ZeroVector;
		FRotator ViewRotation = FRotator::ZeroRotator;
		APlayerController* Viewer = GetWorld()->GetFirstPlayerController();
		if (Viewer)
		{
			Viewer->GetPlayerViewPoint(ViewLocation, ViewRotation);
			const FVector Delta = ViewLocation - GetActorLocation();
			const float Distance2D = Delta.Size2D();
			const bool bVisible = Distance2D < RuntimeVisibilityRadius
				&& FMath::Abs(Delta.Z) < RuntimeVisibilityHeight;
			const float Fade = bVisible
				? FMath::Clamp((RuntimeVisibilityRadius - Distance2D) / 500.0f,
					0.0f, 1.0f)
				: 0.0f;
			ScaleGlow = RuntimeDistanceFadeBaseGlow * Fade;
			if (Mesh) Mesh->SetVisibility(bVisible && Fade > 0.0f, true);
			if (Sprite) Sprite->SetVisibility(bVisible && Fade > 0.0f, true);
			if (MeshLight) MeshLight->ForceRuntimeBake();
		}
	}
}

void AWoTDecoration::ConfigureRuntimeDistanceFade(float VisibilityRadius,
	float VisibilityHeight)
{
	bRuntimeDistanceFade = true;
	RuntimeVisibilityRadius = VisibilityRadius;
	RuntimeVisibilityHeight = VisibilityHeight;
	RuntimeDistanceFadeBaseGlow = ScaleGlow;
	Style = 3;
	bStyleTranslucent = true;
	if (Mesh) Mesh->Style = 3;
	SetActorTickEnabled(true);
}

void AWoTDecoration::ConfigureRuntimeSpritePresentation(
	TArray<FString> TextureIdentities, float TextureWidth,
	float TextureHeight, float MinFrameRate, float MaxFrameRate)
{
	bRuntimeSprite = true;
	RuntimeSpriteTextureIdentities = MoveTemp(TextureIdentities);
	RuntimeSpriteTextureWidth = TextureWidth;
	RuntimeSpriteTextureHeight = TextureHeight;
	RuntimeSpriteMinFrameRate = MinFrameRate;
	RuntimeSpriteMaxFrameRate = MaxFrameRate;
	ApplyRuntimeSpritePresentation();
}

void AWoTDecoration::ApplyRuntimeSpritePresentation()
{
	if (!bRuntimeSprite || bRuntimeSpriteApplied || !Sprite || !Mesh) return;
	// DrawActorSprite derives size from DrawScale * Texture.U/VSize and clamps
	// ScaleGlow to one. The billboard applies that same
	// scale/color rule every tick while collision stays on the actor.
	Mesh->SetVisibility(false, true);
	Sprite->SetComponentTickEnabled(true);
	Sprite->DrawScale = RuntimeVisualPresentation.DrawScale;
	Sprite->ScaleGlow = RuntimeVisualPresentation.ScaleGlow;
	Sprite->bModulateOnly = RuntimeVisualPresentation.Style == 4;
	Sprite->SetAnimatedTextureSeries(RuntimeSpriteTextureIdentities,
		RuntimeSpriteTextureWidth, RuntimeSpriteTextureHeight,
		RuntimeSpriteMinFrameRate, RuntimeSpriteMaxFrameRate);
	bRuntimeSpriteApplied = true;
}

void AWoTDecoration::ConfigureRuntimeCollision(bool bInCollideActors,
	bool bInBlockActors, bool bInBlockPlayers, float Radius, float HalfHeight)
{
	bCollideActors = bInCollideActors;
	bBlockActors = bInBlockActors;
	bBlockPlayers = bInBlockPlayers;
	bHasCollisionDimensions = Radius > 0.0f && HalfHeight > 0.0f;
	Collision->SetCylinderSize(Radius, HalfHeight);
	ApplyRetailCollision();
}

void AWoTDecoration::ApplyRetailCollision()
{
	if (!Collision) return;
	const bool bHasShape = bHasCollisionDimensions;
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetGenerateOverlapEvents(bCollideActors && bHasShape);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionEnabled(bCollideActors && bHasShape
		? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
	if (!bCollideActors || !bHasShape) return;
	if (bBlockActors || bBlockPlayers)
		Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	// Retail projectiles carry bCollideActors but no block flags. Their contact
	// with a non-brush actor is Touch, not HitWall: preserve pawn/NPC blocking,
	// but overlap the projectile object channel and let IsTouchTarget consult the
	// actual retained bProjTarget/bBlockActors/bBlockPlayers values.
	Collision->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
}

void AWoTDecoration::ConfigureRuntimeVisualPresentation(
	const FWoTRuntimeActorVisualPresentation& Presentation)
{
	RuntimeVisualPresentation = Presentation;
	bHasRuntimeVisualPresentation = true;
	ScaleGlow = Presentation.ScaleGlow;
	AmbientGlow = Presentation.AmbientGlow;
	Style = Presentation.Style;
	bUnlit = Presentation.bUnlit;
	bStyleTranslucent = Presentation.bStyleTranslucent;
	if (Mesh)
	{
		Mesh->Style = Presentation.Style;
		Mesh->bMeshCurvy = Presentation.bMeshCurvy;
	}
}

bool AWoTDecoration::ConfigureRuntimeClassDefaultVisuals(
	FStringView ClassIdentity, FString& OutError)
{
	FWoTRuntimeClassDefaults* Defaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(OutError);
	if (Defaults == nullptr) return false;
	FWoTRuntimeActorVisualPresentation Presentation;
	if (!WoTRuntimeReadClassVisualPresentation(
		*Defaults, ClassIdentity, Presentation, OutError)) return false;
	if (!Presentation.HasMesh())
	{
		OutError = TEXT("the retail decoration class has no effective Mesh property");
		return false;
	}
	ConfigureRuntimeVisualPresentation(Presentation);
	return true;
}

void AWoTDecoration::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	ApplyRetailCollision();
	if (!bHasRuntimeVisualPresentation || !Mesh) return;
	FString Error;
	if (RuntimeVisualPresentation.HasMesh()
		&& !WoTRuntimeApplyActorVisualPresentation(
		*Mesh, RuntimeVisualPresentation, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("WOT DECORATION-VISUAL actor=%s error=%s"),
			*GetPathName(), *Error);
	}
	ApplyRuntimeSpritePresentation();
}

void AWoTDecoration::BeginPlay()
{
	Super::BeginPlay();
	InstallClassIgnores();
	if (!Mesh || !Mesh->HasDisplayMesh())
	{
		return;
	}
	if (!PoseSequence.IsNone())
	{
		// Level-placed corpse props hold their authored pose (AnimSequence +
		// frozen AnimFrame — e.g. Battle4_01's Hound at 'Swim').
		Mesh->SetFrameAndStop(PoseSequence, PoseFrame);
	}
	else if (PlayAnimCycle.Num() > 0)
	{
		Mesh->OnAnimEnd.AddDynamic(this, &AWoTDecoration::OnCycleAnimEnd);
		CycleIndex = 0;
		CycleRate = WoTRandom::FRandRange(MinAnimRate, MaxAnimRate);
		Mesh->PlayAnim(PlayAnimCycle[0], CycleRate);
	}
	else if (bAutoAnimate)
	{
		Mesh->LoopAnim(AnimationSequence, WoTRandom::FRandRange(MinAnimRate, MaxAnimRate));
	}
}

void AWoTDecoration::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ActorSpawnedHandle.IsValid() && GetWorld())
	{
		GetWorld()->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
		ActorSpawnedHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void AWoTDecoration::InstallClassIgnores()
{
	if (!bCollideActors || bBlockActors == bBlockPlayers || !GetWorld()) return;
	for (TActorIterator<APawn> It(GetWorld()); It; ++It) ApplyClassIgnore(*It);
	ActorSpawnedHandle = GetWorld()->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(
			this, &AWoTDecoration::ApplyClassIgnore));
}

void AWoTDecoration::ApplyClassIgnore(AActor* Actor)
{
	APawn* Pawn = Cast<APawn>(Actor);
	if (!Pawn || !Collision || bBlockActors == bBlockPlayers) return;
	const bool bMonster = Pawn->IsA<AWoTNPCCharacter>();
	const bool bIgnore = bMonster ? !bBlockActors : !bBlockPlayers;
	if (!bIgnore) return;
	Collision->IgnoreActorWhenMoving(Pawn, true);
	if (UCapsuleComponent* PawnCapsule =
		Pawn->FindComponentByClass<UCapsuleComponent>())
		PawnCapsule->IgnoreActorWhenMoving(this, true);
}

float AWoTDecoration::TakeDamage(float DamageAmount, const FDamageEvent& DamageEvent,
	AController* EventInstigator, AActor* DamageCauser)
{
	LastDamageTaken = DamageAmount;
	return Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
}

void AWoTDecoration::OnCycleAnimEnd(FName)
{
	if (PlayAnimCycle.Num() == 0)
	{
		return;
	}
	CycleIndex = (CycleIndex + 1) % PlayAnimCycle.Num();
	Mesh->PlayAnim(PlayAnimCycle[CycleIndex], CycleRate);
}
