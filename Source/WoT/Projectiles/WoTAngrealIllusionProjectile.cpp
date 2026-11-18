#include "WoTAngrealIllusionProjectile.h"

#include "../WoTAppearEffect.h"
#include "../WoTPlayerColorFX.h"
#include "WoTSeekingProjectile.h"
#include "../AI/WoTNPCCharacter.h"
#include "../WoTAnimTable.h"
#include "../WoTCharacter.h"
#include "../WoTMeshLight.h"
#include "../WoTPlayerState.h"
#include "../WoTVertexAnimComponent.h"
#include "../Inventory/WoTAngrealInventory.h"
#include "../Inventory/WoTIllusionTimerLeech.h"

#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Net/UnrealNetwork.h"

AWoTAngrealIllusionProjectile::AWoTAngrealIllusionProjectile()
{
	ImageCollision = CreateDefaultSubobject<UCapsuleComponent>(TEXT("ImageCollision"));
	ImageCollision->SetupAttachment(Collision);
	ImageCollision->SetCollisionObjectType(ECC_GameTraceChannel1);
	ImageCollision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ImageCollision->SetCollisionResponseToAllChannels(ECR_Ignore);
	ImageCollision->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	ImageCollision->SetGenerateOverlapEvents(true);

	// The inherited sphere only drives AngrealProjectile's outgoing Touch path;
	// an illusion is passive, and needs the copied pawn capsule instead.
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DisplayMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("DisplayMesh"));
	DisplayMesh->SetupAttachment(ImageCollision);
	DisplayMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	ProjectileMovement->bAutoActivate = false;   // Physics=PHYS_None
	ProjectileMovement->Deactivate();
	bNetTemporary = false;
	InitialLifeSpan = 20.0f;
	bProjTarget = true;
	FadePercent = 0.1f;
	bAnimate = true;
	SetActorHiddenInGame(true);   // AcquireImage unhides after copying a valid pawn
}

void AWoTAngrealIllusionProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		ReplicatedOwner = GetOwner();
	}
	else
	{
		OnRep_ReplicatedOwner();
	}
	MyShowSelf();
}

void AWoTAngrealIllusionProjectile::SetSourceAngreal(AWoTAngrealInventory* Source)
{
	Super::SetSourceAngreal(Source);
	if (!HasAuthority())
	{
		return;
	}

	AWoTCharacter* Caster = SourceAngreal
		? Cast<AWoTCharacter>(SourceAngreal->GetOwner()) : nullptr;
	// WOTPlayer in retail maps to a non-NPC AWoTCharacter in the port.
	if (!IsValid(Caster) || Caster->IsNPCPawn())
	{
		return;
	}

	// AngrealIllusionProjectile.uc:48-60: replace any old timer, inherit the
	// projectile source, copy the projectile's REMAINING lifespan, then attach it
	// to the caster so the source angreal supplies the Personal Illusion frame.
	RemoveTimerIcon();
	Timer = NewObject<UWoTIllusionTimerLeech>(Caster);
	Timer->InitializeWithProjectile(this);
	Timer->Lifespan = GetLifeSpan();
	Timer->AttachTo(Caster);
}

void AWoTAngrealIllusionProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (GetWorld() && GetWorld()->GetTimeSeconds() > NextShowSelfTime)
	{
		MyShowSelf();
	}
	// AngrealIllusionProjectile.uc:150-159: the last FadePercent of the class-
	// default lifespan switches to STY_Translucent and linearly lowers ScaleGlow.
	const AWoTAngrealIllusionProjectile* Default =
		GetClass()->GetDefaultObject<AWoTAngrealIllusionProjectile>();
	const float FadeTime = Default ? Default->InitialLifeSpan * FadePercent : 0.0f;
	if (FadeTime > 0.0f && GetLifeSpan() <= FadeTime)
	{
		bStyleTranslucent = true;
		ScaleGlow = GetLifeSpan() / FadeTime;
		if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
		{
			MeshLight->ForceRuntimeBake();
		}
	}
}

void AWoTAngrealIllusionProjectile::AcquireImage(APawn* Image)
{
	if (!IsValid(Image) || !IsValid(DisplayMesh) || !IsValid(ImageCollision))
	{
		return;
	}

	USkeletalMeshComponent* SourceMesh = nullptr;
	if (const ACharacter* CharacterImage = Cast<ACharacter>(Image))
	{
		SourceMesh = CharacterImage->GetMesh();
	}
	if (!IsValid(SourceMesh))
	{
		SourceMesh = Image->FindComponentByClass<USkeletalMeshComponent>();
	}
	if (IsValid(SourceMesh))
	{
		if (const UWoTVertexAnimComponent* VertexSource =
			Cast<UWoTVertexAnimComponent>(SourceMesh))
		{
			DisplayMesh->CopyDisplayMeshFrom(VertexSource);
		}
		else
		{
			DisplayMesh->SetSkeletalMesh(SourceMesh->GetSkeletalMeshAsset());
		}
		DisplayMesh->SetRelativeTransform(SourceMesh->GetRelativeTransform());
		for (int32 Slot = 0; Slot < SourceMesh->GetNumMaterials(); ++Slot)
		{
			DisplayMesh->SetMaterial(Slot, SourceMesh->GetMaterial(Slot));
		}
		DisplayMesh->SetVisibility(true, true);
		if (bAnimate)
		{
			// WaitingIdle loops the image's 'Wait' animation-table SLOT
			// (AngrealIllusionProjectile.uc:204-211 TweenLoopSlotAnim): a Trolloc's
			// Wait resolves to Breath, etc. Literal 'Wait' is the no-table fallback.
			FName WaitSequence(TEXT("Wait"));
			float WaitRate = 1.0f;
			if (const AWoTCharacter* ImageCharacter = Cast<AWoTCharacter>(Image))
			{
				if (const FWoTAnimTableEntry* Pick = WoTAnimTable::PickSlotAnim(
					ImageCharacter->AnimTableName, FName(TEXT("Wait")), ImageCharacter))
				{
					WaitSequence = Pick->AnimName;
					WaitRate = WoTAnimTable::ResolveRate(Pick);
				}
			}
			if (DisplayMesh->FindSequence(WaitSequence))
			{
				DisplayMesh->LoopAnim(WaitSequence, WaitRate);
			}
		}
	}
	if (const UCapsuleComponent* SourceCapsule =
		Image->FindComponentByClass<UCapsuleComponent>())
	{
		ImageCollision->SetCapsuleSize(
			SourceCapsule->GetUnscaledCapsuleRadius(),
			SourceCapsule->GetUnscaledCapsuleHalfHeight());
	}
	// AngrealIllusionProjectile.uc:124 copies PlayerReplicationInfo.Team, not
	// the disguise/apparent-team view used by AI faction checks.
	if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Image))
	{
		Team = NPC->AssignedTeam;
	}
	else if (const AWoTPlayerState* PlayerState =
		Image->GetPlayerState<AWoTPlayerState>())
	{
		Team = PlayerState->Team;
	}
	SetActorHiddenInGame(false);
	// AngrealIllusionProjectile.uc:139-150: the appear rings around the fresh
	// decoy, in the caster's PlayerColor (SetColors() no-arg defaults to Blue for
	// a non-player owner). Authority-only — the effect actor replicates.
	if (HasAuthority())
	{
		if (UWorld* World = GetWorld())
		{
			const FName Color = WoTPlayerColorFX::Resolve(
				Cast<APawn>(ReplicatedOwner), TEXT("Blue"));
			AWoTAppearEffect::SpawnColored(*World, this, Color, Color);
		}
	}
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		// The image mesh binds after component BeginPlay; rebake once against the
		// copied materials, as the runtime weave-mesh binders do.
		MeshLight->ForceRuntimeBake();
	}
	ForceNetUpdate();
}

void AWoTAngrealIllusionProjectile::OnRep_ReplicatedOwner()
{
	AcquireImage(Cast<APawn>(ReplicatedOwner));
}

void AWoTAngrealIllusionProjectile::RemoveTimerIcon()
{
	if (IsValid(Timer))
	{
		Timer->Unattach();
	}
	Timer = nullptr;
}

void AWoTAngrealIllusionProjectile::Destroyed()
{
	// AngrealIllusionProjectile.uc:180-194: illusion death/expiry owns the timer
	// teardown, so no countdown survives its producer.
	RemoveTimerIcon();
	Super::Destroyed();
}

void AWoTAngrealIllusionProjectile::MyShowSelf()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AWoTSeekingProjectile> It(World); It; ++It)
	{
		AWoTSeekingProjectile* Seeker = *It;
		if (!IsValid(Seeker) || !Seeker->bNotifySeePlayer)
		{
			continue;
		}
		FVector Delta = GetActorLocation() - Seeker->GetActorLocation();
		const float Height = FMath::Abs(Delta.Z);
		Delta.Z = 0.0f;
		if (Delta.Size() <= Seeker->SeePlayerRadius
			&& Height <= Seeker->SeePlayerHeight)
		{
			Seeker->NotifySeePawn(this);
		}
	}
	NextShowSelfTime = World->GetTimeSeconds() + ShowSelfResolution;
}

void AWoTAngrealIllusionProjectile::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTAngrealIllusionProjectile, Team);
	DOREPLIFETIME(AWoTAngrealIllusionProjectile, ReplicatedOwner);
}
