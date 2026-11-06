#include "WoTExplodingWall.h"
#include "WoTRandom.h"

#include "AI/WoTNPCController.h"
#include "Projectiles/WoTProjectile.h"
#include "WoTSound.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"

AWoTExplodingWall::AWoTExplodingWall()
{
	PrimaryActorTick.bCanEverTick = false;

	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	Capsule = CreateDefaultSubobject<UWoTFlatCylinderComponent>(TEXT("Capsule"));
	Capsule->SetupAttachment(RootComponent);
	// UE1 CollisionRadius=32/CollisionHeight=32 (WOTExplodingWall.uc defaults).
	Capsule->SetCylinderSize(32.0f, 32.0f);
	ApplyOccluderCollision();

	// exploded (uc ActivatedBy(0)); the remaining four UE1 array slots stay None.
	ActivatedBy.Init(NAME_None, 5);
	ActivatedBy[0] = FName(TEXT("exploded"));

	// RemoteRole=ROLE_SimulatedProxy. Static — no movement replication; the
	// break replicates as actor teardown (Explode ends in Destroy) and the
	// fragments replicate themselves.
	bReplicates = true;
}

void AWoTExplodingWall::ApplyOccluderCollision()
{
	// Retail bProjTarget=True with bBlockActors/bBlockPlayers false: weaves
	// dispatch Touch on the wall, while pawns walk through it. Overlap ONLY the WoTProjectile
	// object channel (ECC_GameTraceChannel1, Config/DefaultEngine.ini).
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetGenerateOverlapEvents(true);
	Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
}

void AWoTExplodingWall::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// PLACED actors reload with the serialized collision-profile template over
	// the constructor's responses (FBodyInstance::LoadProfileData — the
	// AWoTSkyBox clobber, PORT_NOTES finding #3), so re-assert the occluder
	// contract at init regardless of what the umap carries.
	ApplyOccluderCollision();
}

void AWoTExplodingWall::BeginPlay()
{
	Super::BeginPlay();
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->RegisterTag(TagName, this);
}

float AWoTExplodingWall::TakeDamage(float DamageAmount,
	const FDamageEvent& DamageEvent, AController* EventInstigator, AActor*)
{
	// Exploding.TakeDamage (WOTExplodingWall.uc:41-60).
	if (!HasAuthority() || bExploded || bOnlyTriggerable)
	{
		return 0.0f;
	}

	FName DamageType(TEXT("All"));
	FVector Momentum = FVector::ZeroVector;
	if (DamageEvent.IsOfType(FWoTDamageEvent::ClassID))
	{
		const FWoTDamageEvent& Ev = static_cast<const FWoTDamageEvent&>(DamageEvent);
		if (!Ev.WoTDamageType.IsNone())
		{
			DamageType = Ev.WoTDamageType;
		}
		Momentum = Ev.UE1Momentum;
	}
	// The wall's gate is EXACT-match only (uc:50-57): incoming 'All' bypasses,
	// anything else must equal an ActivatedBy entry. Deliberately NO
	// ActivatedBy=='All' wildcard and NO DamageTypeContains element clause —
	// both are ExplodingMover.uc:45 additions the wall never had (PORT_NOTES
	// G35; this exact-only gate is why the six MP authors are inert: nothing
	// shipped emits their 'break01').
	if (DamageType != FName(TEXT("All")))
	{
		bool bAbort = true;
		for (const FName Allowed : ActivatedBy)
		{
			if (DamageType == Allowed)
			{
				bAbort = false;
			}
		}
		if (bAbort)
		{
			return 0.0f;
		}
	}

	const int32 AppliedDamage = FMath::TruncToInt(DamageAmount);
	Health -= AppliedDamage;
	if (Health <= 0)
	{
		Explode(EventInstigator ? EventInstigator->GetPawn() : nullptr, Momentum);
	}
	return static_cast<float>(AppliedDamage);
}

void AWoTExplodingWall::WoTTrigger(AActor*, APawn* EventInstigator)
{
	// Exploding.Trigger (uc:36-39): unconditional — no bOnlyTriggerable gate,
	// no ActivatedBy/Health check. Momentum is the facing (Vector(Rotation)).
	Explode(EventInstigator, GetActorForwardVector());
}

void AWoTExplodingWall::Explode(APawn* EventInstigator, const FVector& Momentum)
{
	// Exploding.Explode (uc:63-113), authority-only and one-shot.
	if (!HasAuthority() || bExploded)
	{
		return;
	}
	bExploded = true;

	// Retail fires Event BEFORE `Instigator = EventInstigator` (uc:69-73), so
	// the chain carries the stale pre-explosion Instigator (in practice None).
	// Normalized to the real instigator, exactly like the mover port — same
	// INTENTIONAL DIVERGENCE ruling (no MP author has an Event; PORT_NOTES).
	GetWorld()->GetSubsystem<UWoTEventSubsystem>()->FireEvent(
		EventName, this, EventInstigator);
	// Retail MakeNoise(1.0) when instigated (uc:74-75). The noise ORIGIN is the
	// wall (CheckNoiseHearing tests CanHear(Location)) and
	// the heard actor is the wall too (eventHearNoise(Loudness, this)) — so the
	// retail dispatch classifies a non-pawn and acquires nothing. Passing the
	// wall through the G18 seam reproduces both; passing the instigator would
	// hand nearby NPCs shooter intel the retail break never gave (review F2).
	// The instigator gate is retail's (!NoiseOwner -> return).
	if (EventInstigator)
	{
		AWoTNPCController::BroadcastNoise(GetWorld(), this, 1.0f, EventInstigator);
	}
	// PlaySound(BreakingSound, SLOT_None, 2.0) (uc:77); all six MP authors
	// leave it None — the shipped break is silent.
	if (!BreakingSound.IsNone())
	{
		WoTSound::PlayOneShot(this, WoTSound::LoadBySubPath(BreakingSound),
			GetActorLocation(), 1600.0f, 1.0f, 2.0f);
	}

	// uc:79-110: wall/wood debris launches from rest; only glass inherits the
	// killing blow's momentum (and the retail Owner-forwarding spawn).
	SpawnFragments(EWoTFragmentType::Wall, NumWallChunks, WallParticleSize,
		FVector::ZeroVector, WallTextureName);
	SpawnFragments(EWoTFragmentType::Wood, NumWoodChunks, WoodParticleSize,
		FVector::ZeroVector, WoodTextureName);
	SpawnFragments(EWoTFragmentType::Glass, NumGlassChunks, GlassParticleSize,
		Momentum, GlassTextureName);

	// uc:112 — unlike the mover (which stays as a spent open brush), the wall
	// is GONE. Replicated teardown removes the remote proxies.
	Destroy();
}

void AWoTExplodingWall::SpawnFragments(EWoTFragmentType Type, int32 Count,
	float Scale, const FVector& Momentum, FName SkinName)
{
	FActorSpawnParameters Params;
	Params.Owner = Type == EWoTFragmentType::Glass ? GetOwner() : nullptr;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector Location = GetActorLocation() + ExplosionDimensions * WoTRandom::VRand();
		if (AWoTFragment* Fragment = GetWorld()->SpawnActor<AWoTFragment>(
			AWoTFragment::StaticClass(), Location, FRotator::ZeroRotator, Params))
		{
			Fragment->Init(Type, ExplosionSize, Momentum, Scale, SkinName);
		}
	}
}
