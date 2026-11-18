#include "WoTFireWall.h"
#include "../Assets/WoTRuntimeAssets.h"

#include "../Projectiles/WoTProjectile.h"   // FWoTDamageEvent
#include "../WoTParticleSprayer.h"
#include "../WoTTeleporter.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

// Activated firewall rows are payload-owned. The procedural flame textures use
// flipbook particle masters; the fractal subsystem swaps their Tex to live
// runtime fields after spawn (FireWallSprayer1.uc:90-111).
// Materials come from Tools/import/import_flame_fx.py.
namespace
{
	TUniquePtr<FWoTFireWallSet>& InstalledFireWallRegistry()
	{
		static TUniquePtr<FWoTFireWallSet> Set;
		return Set;
	}

	const FWoTFireWallSet* ActiveFireWallRegistry()
	{
		if (const TUniquePtr<FWoTFireWallSet>& Installed = InstalledFireWallRegistry())
		{
			return Installed.Get();
		}
		return nullptr;
	}

	FString FireWallMaterialSlugPath(FName Slug, const TCHAR* Kind)
	{
		return FString::Printf(TEXT("/Game/WoT/Materials/Instances/MI_PS%sFB_%s.MI_PS%sFB_%s"),
			Kind, *Slug.ToString(), Kind, *Slug.ToString());
	}
}

void FWoTFireWallBuilder::Install(FWoTFireWallSet&& Set)
{
	InstalledFireWallRegistry() = MakeUnique<FWoTFireWallSet>(MoveTemp(Set));
}

#if WITH_AUTOMATION_TESTS
void FWoTFireWallBuilder::Reset()
{
	InstalledFireWallRegistry().Reset();
}
#endif

AWoTFireWallSlab::AWoTFireWallSlab()
{
	// TrapType is explicit port policy: no shipped trap sets TrapType (GetTrapID falls
	// back to the CLASS name), and 'FireWallSlab' is not in the TrapDetect table — shipped
	// firewalls were NOT detectable. Clearing undoes the WallSlab ctor tag our port uses to stand
	// in for the class-name fallback (the 3c note guessed "detects as wall parts" — wrong).
	TrapType = NAME_None;
	// The flame capsule overlaps so entrants join the periodic-damage set
	// (FireWallSlab.uc:39-40).
	FlameVolume = CreateDefaultSubobject<UCapsuleComponent>(TEXT("FlameVolume"));
	FlameVolume->SetupAttachment(Capsule);
	FlameVolume->InitCapsuleSize(22.0f, 22.0f);
	FlameVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	FlameVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	FlameVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

void AWoTFireWallSlab::BeginPlay()
{
	if (const FWoTFireWallSet* Set = ActiveFireWallRegistry())
	{
		// Retail defaults become available only after family activation.
		const FWoTFireWallDefaults& Defaults = Set->Defaults;
		Health = Defaults.Health;
		DamageAmount = Defaults.DamageAmount;
		DamageInterval = Defaults.DamageInterval;
		SlabWidth = Defaults.SlabWidth;
		VisualMeshName = Defaults.SlabMesh;
		VisualScale = FVector(Defaults.SlabDrawScale);
		if (Capsule)
		{
			Capsule->InitCapsuleSize(
				Defaults.CollisionRadius, Defaults.CollisionHeight);
		}
		if (FlameVolume && !Set->Sprayers.IsEmpty())
		{
			FlameVolume->SetRelativeLocation(Set->Sprayers[0].Offset);
		}
	}
	// AWoTWallSlab binds VisualMeshName/VisualScale here, so apply the active
	// defaults before delegating.  This remains a runtime read, never a CDO read.
	Super::BeginPlay();
	if (IsPreviewMode())
	{
		return;
	}
	SpawnFlameSprayers();
	if (HasAuthority())
	{
		// FireWallSprayer1.Tick: every DamageInterval, burn everyone still standing in the flame.
		GetWorldTimerManager().SetTimer(
			BurnTimer, this, &AWoTFireWallSlab::BurnTouching, DamageInterval, /*bLoop*/true);
	}
}

void AWoTFireWallSlab::NotifyActorBeginOverlap(AActor* Other)
{
	Super::NotifyActorBeginOverlap(Other);
	// FireWallSprayer1.Touch: burn on entry (authority; DamageActor re-gates the rest).
	if (HasAuthority() && Cast<APawn>(Other))
	{
		DamageActor(Other);
	}
}

void AWoTFireWallSlab::BurnTouching()
{
	// FireWallSprayer1.DamageTouching: every pawn inside the flame cylinder.
	TArray<AActor*> Touching;
	FlameVolume->GetOverlappingActors(Touching, APawn::StaticClass());
	for (AActor* Other : Touching)
	{
		DamageActor(Other);
	}
}

bool AWoTFireWallSlab::HasActiveBurnTimerForTest() const
{
	return GetWorld() && GetWorld()->GetTimerManager().IsTimerActive(BurnTimer);
}

void AWoTFireWallSlab::DamageActor(AActor* Other)
{
	if (IsPreviewMode())
	{
		return;
	}
	// FireWallSlab.DamageActor: the owner trap's gate (team/editing; missing owner reads active —
	// the .uc's explicit None check).
	const AWoTTrap* OwnerTrap = Cast<AWoTTrap>(GetOwner());
	if ((OwnerTrap && OwnerTrap->IsInactive(Other)) || !Other)
	{
		return;
	}
	if (!IsHidden() && Health > 0)
	{
		// The flame consumes itself as it burns ('Touched'), and the victim takes Fire.
		APawn* P = Cast<APawn>(Other);
		FWoTDamageEvent SelfEv;
		SelfEv.WoTDamageType = TEXT("Touched");
		SelfEv.UE1Momentum = FVector::ZeroVector;   // FireWallSlab.uc:177
		SelfEv.HitInfo.ImpactPoint = GetActorLocation();
		TakeDamage(DamageAmount, SelfEv, P ? P->GetController() : nullptr, this);

		FWoTDamageEvent FireEv;
		FireEv.WoTDamageType = TEXT("xxFxx");
		FireEv.UE1Momentum = FVector::ZeroVector;   // FireWallSlab.uc:178
		FireEv.HitInfo.ImpactPoint = GetActorLocation();
		// .uc passes the slab's Instigator (the placing player — unset until the deferred deploy
		// wires it) for kill credit.
		Other->TakeDamage(DamageAmount, FireEv,
			ResolveDamageInstigatorController(), this);
	}
}

float AWoTFireWallSlab::TakeDamage(float DamageAmount_, const FDamageEvent& /*DamageEvent*/,
                                   AController* EventInstigator, AActor* /*DamageCauser*/)
{
	// FireWallSlab.TakeDamage — full replacement of the WallSlab stage logic (no Super).
	if (IsPreviewMode() || !HasAuthority())
	{
		return 0.0f;
	}
	const AWoTTrap* OwnerTrap = Cast<AWoTTrap>(GetOwner());
	if (OwnerTrap && EventInstigator && OwnerTrap->IsInactive(EventInstigator->GetPawn()))
	{
		return 0.0f;
	}
	if (!IsHidden())
	{
		Health = FMath::Max(Health - FMath::TruncToInt(DamageAmount_), 0);
		ApplyFlameScale();   // clients ride OnRep_Health
		if (Health <= 0)
		{
			Destroy();   // Destroyed() fades the sprayers out, everywhere
		}
		return DamageAmount_;
	}
	return 0.0f;
}

void AWoTFireWallSlab::SpawnFlameSprayers()
{
	// The shipped simulated Tick's lazy spawn (client + server) at BeginPlay: one
	// AWoTParticleSprayer per bundle, aimed up (rotator(vect(0,0,1))), local-only.
	UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}
	const FWoTFireWallSet* Set = ActiveFireWallRegistry();
	if (Set == nullptr) return;
	const TArray<FWoTFireWallSprayer>& Rows = Set->Sprayers;
	for (int32 i = 0; i < Rows.Num() && i < 3; ++i)
	{
		if (IsValid(FlameSprayers[i]))
		{
			continue;
		}
		const FWoTFireWallSprayer& Row = Rows[i];
		const FTransform At(FRotator(90, 0, 0), GetActorLocation() + Row.Offset);
		// DEFERRED: the sprayer's BeginPlay builds its ISM passes from these fields.
		AWoTParticleSprayer* S = World->SpawnActorDeferred<AWoTParticleSprayer>(
			AWoTParticleSprayer::StaticClass(), At, this, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!S)
		{
			continue;
		}
		S->Spread = Row.Spread;
		S->Volume = Row.Volume;
		S->MinVolume = Row.MinVolume;
		S->VolumeScalePct = Row.VolumeScalePct;
		S->VisibilityRadius = Row.VisibilityRadius;
		S->VisibilityHeight = Row.VisibilityHeight;
		S->Gravity = Row.Gravity;
		S->bModulateOnly = Row.bModulateOnly;
		for (const FWoTFireWallTemplate& T : Row.Templates)
		{
			S->Templates.Add(T.Template);
			if (Row.bModulateOnly)
			{
				const FString Texture = FString::Printf(
					TEXT("/Game/WoT/Textures/ParticleSystems/%s.%s"),
					*T.TextureIdentity.ToString().RightChop(
						T.TextureIdentity.ToString().Find(TEXT("__")) + 2),
					*T.TextureIdentity.ToString().RightChop(
						T.TextureIdentity.ToString().Find(TEXT("__")) + 2));
				S->AddMaterials.Add(nullptr);
				S->ModMaterials.Add(UWoTRuntimeAssets::CreateModulatedSpriteMaterial(
					S, FSoftObjectPath(Texture)));
			}
			else
			{
				S->AddMaterials.Add(UWoTRuntimeAssets::ResolveMaterialPath(
					FSoftObjectPath(FireWallMaterialSlugPath(T.TextureIdentity, TEXT("Add")))));
				S->ModMaterials.Add(UWoTRuntimeAssets::ResolveMaterialPath(
					FSoftObjectPath(FireWallMaterialSlugPath(T.TextureIdentity, TEXT("Mod")))));
			}
		}
		S->FinishSpawning(At);
		FlameSprayers[i] = S;
	}
	ApplyFlameScale();
}

void AWoTFireWallSlab::ApplyFlameScale()
{
	// FireWallSlab.ScaleFlameSprayer: Scalar = Health / default.Health. Recomputed
	// from the config rows each time (never cumulative); live particles keep their
	// spawn-time values, exactly UE1's mutate-the-template behavior.
	const FWoTFireWallSet* Set = ActiveFireWallRegistry();
	if (Set == nullptr) return;
	const int32 FullHealth = Set->Defaults.Health;
	const float Scalar = FullHealth > 0 ? static_cast<float>(Health) / FullHealth : 0.0f;
	const TArray<FWoTFireWallSprayer>& Rows = Set->Sprayers;
	for (int32 i = 0; i < Rows.Num() && i < 3; ++i)
	{
		AWoTParticleSprayer* S = FlameSprayers[i];
		if (!IsValid(S))
		{
			continue;
		}
		S->bOn = !IsHidden();   // the shipped Tick's bOn = !bHidden
		S->Volume = Rows[i].Volume * Scalar;
		S->MinVolume = Rows[i].MinVolume * Scalar;
		const int32 NumT = FMath::Min(S->Templates.Num(), Rows[i].Templates.Num());
		for (int32 j = 0; j < NumT; ++j)
		{
			const FWoTParticleTemplate& D = Rows[i].Templates[j].Template;
			S->Templates[j].MaxScaleGlow = D.MaxScaleGlow * Scalar;
			S->Templates[j].MinScaleGlow = D.MinScaleGlow * Scalar;
			S->Templates[j].MaxDrawScale = D.MaxDrawScale * Scalar;
			S->Templates[j].MinDrawScale = D.MinDrawScale * Scalar;
		}
	}
}

void AWoTFireWallSlab::Destroyed()
{
	// FireWallSlab.Destroyed (simulated): stop emitting, fade out staggered.
	const FWoTFireWallSet* Set = ActiveFireWallRegistry();
	if (Set == nullptr)
	{
		Super::Destroyed();
		return;
	}
	const TArray<FWoTFireWallSprayer>& Rows = Set->Sprayers;
	for (int32 i = 0; i < Rows.Num() && i < 3; ++i)
	{
		if (IsValid(FlameSprayers[i]))
		{
			FlameSprayers[i]->bOn = false;
			FlameSprayers[i]->SetLifeSpan(Rows[i].FadeLifeSpan);
			FlameSprayers[i] = nullptr;
		}
	}
	Super::Destroyed();
}

void AWoTFireWallSlab::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// replication { unreliable if(Role==Authority) Health } — the flame-scale consumer.
	DOREPLIFETIME(AWoTFireWallSlab, Health);
}

AWoTFireWall::AWoTFireWall()
{
	// Native binding retained as explicit port policy.
	SlabClass = AWoTFireWallSlab::StaticClass();
}

void AWoTFireWall::BeginPlay()
{
	if (const FWoTFireWallSet* Set = ActiveFireWallRegistry())
	{
		MaxSlabs = Set->Defaults.MaxSlabs;
	}
	Super::BeginPlay();
}

int32 AWoTFireWall::GetSlabWidth() const
{
	if (const FWoTFireWallSet* Set = ActiveFireWallRegistry())
	{
		return Set->Defaults.SlabWidth;
	}
	return Super::GetSlabWidth();
}

float AWoTFireWall::GetSlabHalfHeight() const
{
	if (const FWoTFireWallSet* Set = ActiveFireWallRegistry())
	{
		return Set->Defaults.CollisionHeight;
	}
	return Super::GetSlabHalfHeight();
}

FRotator AWoTFireWall::GetSlabRotation(
	int32 /*SlabIndex*/, const FVector& Normal) const
{
	// FireWall.GetSlabRotation: rotator(StartNormal) with no i%4 yaw cycle.
	return Normal.Rotation();
}

AActor* AWoTFireWall::AnyActorsInArea(const FVector& Loc, float Radius) const
{
	if (AActor* Blocker = Super::AnyActorsInArea(Loc, Radius))
	{
		return Blocker;
	}

	// FireWall.AnyActorsInArea's second RadiusActors pass rejects placement near a
	// teleporter. Matches UE1's native RadiusActors iterator
	// (execRadiusActors): it walks every actor of the class regardless of collision
	// state — the pass deliberately omits the first pass's !bHidden/bCollideActors
	// filter, and a destination-only (empty-URL) teleporter runs with its collision
	// turned off (Teleporter.PostBeginPlay) — and uses the radius-only distance test
	// dist < SearchDistance*2 + Teleporter.CollisionRadius before the explicit
	// VSize < 1000 gate. An overlap query would be unfaithful here: it needs
	// query-enabled collision and would weigh capsule height.
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	for (TActorIterator<AWoTTeleporter> It(World); It; ++It)
	{
		AWoTTeleporter* Teleporter = *It;
		if (!IsValid(Teleporter))
		{
			continue;
		}
		const float TeleporterRadius = Teleporter->Collision
			? Teleporter->Collision->GetScaledCapsuleRadius() : 0.0f;
		const float Distance = (Teleporter->GetActorLocation() - Loc).Size();
		if (Distance < Radius * 2.0f + TeleporterRadius && Distance < 1000.0f)
		{
			return Teleporter;
		}
	}
	return nullptr;
}

AWoTFireWallInventory::AWoTFireWallInventory()
{
	// Native binding retained as explicit port policy.
	ResourceClass = AWoTFireWall::StaticClass();
	// FireWallInventory.uc:20-23 supplies the native inventory presentation.
	BindStatusIcons(TEXT("WoTTraps"), TEXT("I_SFire"), TEXT("M_SFire"));
}

void AWoTFireWallInventory::BeginPlay()
{
	if (const FWoTFireWallSet* Set = ActiveFireWallRegistry())
	{
		Count = Set->Defaults.InventoryCount;
	}
	Super::BeginPlay();
}
