#include "WoTWall.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTSoundTable.h"


#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

// ----------------------------- WallSlab ------------------------------------ //

AWoTWallSlab::AWoTWallSlab()
{
	bReplicates = true;
	bAlwaysRelevant = true;   // WallSlab.uc default
	TrapType = TEXT("WallSlab");

	if (Capsule)
	{
		// Collision 28 x 142, blocking pawns (the barricade).
		Capsule->InitCapsuleSize(28.0f, 142.0f);
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Capsule->SetCollisionProfileName(TEXT("BlockAllDynamic"));
	}

	// The visible slab mesh binds in BeginPlay (BindTrapVisual); collision lives on the capsule.
	SlabMesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("SlabMesh"));
	SlabMesh->SetupAttachment(Capsule);
	SlabMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// WallSlab.uc:86-92 display defaults (6b).
	VisualMeshName = FName(TEXT("WallSlab"));
	VisualScale = FVector(1.0f);
}

void AWoTWallSlab::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// WallSlab.uc: `unreliable if(Role==Authority)` — all-clients. UE property rep is
	// eventually-reliable (no unreliable-property option); latest-value-wins is the intent.
	DOREPLIFETIME(AWoTWallSlab, WithstandDamage);
}

float AWoTWallSlab::GetSlabCollisionHalfHeight() const
{
	return Capsule ? Capsule->GetUnscaledCapsuleHalfHeight() : 0.0f;
}

int32 AWoTWallSlab::GetStage() const
{
	// WallSlab.TakeDamage thresholds key off class'WallSlab'.default.WithstandDamage (150).
	const int32 Full = GetDefault<AWoTWallSlab>()->WithstandDamage;
	if (WithstandDamage > Full * 2 / 3) { return 0; }   // Stage1 (base skin)
	if (WithstandDamage > Full * 1 / 3) { return 1; }   // Stage2
	if (WithstandDamage > 0)            { return 2; }   // Stage3
	return 3;                                            // destroyed
}

float AWoTWallSlab::TakeDamage(float Damage, const FDamageEvent& DamageEvent,
                               AController* /*EventInstigator*/, AActor* /*DamageCauser*/)
{
	// WallSlab.TakeDamage: only the authority applies damage; WithstandDamage replicates and clients
	// re-derive the stage. IsInactive(None) — ignore the instigator (only the editing gate applies).
	if (IsPreviewMode() || !HasAuthority() || IsInactive(nullptr))
	{
		return 0.0f;
	}

	// Cosmetic burst at the hit point.
	FVector HitLoc = GetActorLocation();
	if (DamageEvent.IsOfType(FPointDamageEvent::ClassID))
	{
		HitLoc = static_cast<const FPointDamageEvent&>(DamageEvent).HitInfo.ImpactPoint;
	}
	WoTTrapHelpers::SpawnTrapBurst(*GetWorld(), HitLoc);
	WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/BreakWL")), GetActorLocation(), 1600.0f);   // WallSlab.uc:43

	WithstandDamage -= FMath::RoundToInt(Damage);
	if (WithstandDamage <= 0)
	{
		WoTSound::PlayReplicated(*GetWorld(), FName(TEXT("WoTTraps/DestroyedWL")), GetActorLocation(), 1600.0f);   // WallSlab.uc:60
		Destroy();
		return Damage;
	}
	ApplyStagePresentation();   // authority updates its own visual; clients via OnRep
	return Damage;
}

void AWoTWallSlab::OnRep_WithstandDamage()
{
	ApplyStagePresentation();
}

void AWoTWallSlab::BeginPlay()
{
	Super::BeginPlay();
	BindTrapVisual(SlabMesh, VisualMeshName, VisualScale);
	ApplyStagePresentation();
}

void AWoTWallSlab::ApplyStagePresentation()
{
	const int32 Stage = GetStage();
	if (Stage < 1 || !SlabMesh)
	{
		return;   // full health keeps the base skin (the shipped Stage1 line is commented out)
	}
	const TCHAR* Skin = Stage == 1 ? TEXT("TrapWall2") : TEXT("TrapWall3");
	SlabMesh->SetAllSkinOverrides(FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/WoTTraps/%s.%s"), Skin, Skin)));
}

// ------------------------------- Wall -------------------------------------- //

AWoTWall::AWoTWall()
{
	// Server-only invisible manager: RemoteRole=ROLE_None, bHidden, no collision.
	bReplicates = false;
	SetActorHiddenInGame(true);
	if (Capsule)
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	SlabClass = AWoTWallSlab::StaticClass();
}

int32 AWoTWall::GetSlabWidth() const
{
	const AWoTWallSlab* SlabCDO = *SlabClass
		? SlabClass->GetDefaultObject<AWoTWallSlab>() : nullptr;
	return SlabCDO ? SlabCDO->SlabWidth : 0;
}

float AWoTWall::GetSlabHalfHeight() const
{
	const AWoTWallSlab* SlabCDO = *SlabClass
		? SlabClass->GetDefaultObject<AWoTWallSlab>() : nullptr;
	return SlabCDO ? SlabCDO->GetSlabCollisionHalfHeight() : 0.0f;
}

bool AWoTWall::CalcLocation(FVector& StartLocation, FVector& StartNormal)
{
	UWorld* World = GetWorld();
	if (!World || !*SlabClass
		|| FMath::Abs(StartNormal.Z) > KINDA_SMALL_NUMBER)
	{
		return false;
	}

	const int32 SlabWidthUnits = GetSlabWidth();
	if (SlabWidthUnits <= 0)
	{
		return false;
	}

	NumSlabs = 0;
	FHitResult Hit;
	const auto HitLevel = [&](const FVector& TraceStart, const FVector& TraceEnd)
	{
		return WoTRetailActorTrace(World, TraceStart, TraceEnd, this, Hit)
			&& Hit.bBlockingHit && Hit.GetComponent()
			&& Hit.GetComponent()->GetCollisionObjectType() == ECC_WorldStatic;
	};

	// Wall.uc first verifies that the supplied horizontal axis points at level geometry.
	if (!HitLevel(StartLocation, StartLocation + TraceDistance * StartNormal))
	{
		return false;
	}

	if (!HitLevel(StartLocation,
		StartLocation + TraceDistance * FVector(0.0f, 0.0f, -1.0f)))
	{
		return false;
	}
	StartLocation = Hit.ImpactPoint;

	// UE5 adaptation: after the down-trace, StartLocation sits ON the floor surface,
	// where a HORIZONTAL trace self-grazes the floor in Chaos and returns Width~0
	// (a 1-slab wall). Lift the across trace a few UU off the floor to reach the far
	// wall; WallStop.Z is flattened to Floor.Z below, so the room width is unchanged.
	const FVector AcrossStart = StartLocation + FVector(0.0f, 0.0f, 2.0f);
	if (!HitLevel(AcrossStart, AcrossStart + TraceDistance * StartNormal))
	{
		return false;
	}
	Floor = StartLocation;
	WallStop = Hit.ImpactPoint;
	WallStop.Z = Floor.Z;

	const float SlabWidth = static_cast<float>(SlabWidthUnits);
	const float Width = (Floor - WallStop).Size();
	NumSlabs = FMath::TruncToInt(Width / SlabWidth) + 1;
	if (NumSlabs > MaxSlabs)
	{
		return false;
	}

	// Preserve Wall.uc's historical advance-before-trace sampling at 1.5, 2.5, ... widths.
	FVector SlabLocation = StartLocation + (SlabWidth / 2.0f) * StartNormal;
	for (int32 SlabIndex = 0; SlabIndex < NumSlabs; ++SlabIndex)
	{
		SlabLocation += StartNormal * SlabWidth;
		const bool bHit = WoTRetailActorTrace(World, SlabLocation,
			SlabLocation + TraceDistance * FVector(0.0f, 0.0f, -1.0f),
			this, Hit) && Hit.bBlockingHit;
		const bool bLevel = bHit && Hit.GetComponent()
			&& Hit.GetComponent()->GetCollisionObjectType() == ECC_WorldStatic;
		if (!bLevel && !(bHit && SameLogicalActor(Hit.GetActor())))
		{
			return false;
		}
		if (bLevel && Hit.ImpactPoint.Z < Floor.Z - FloorTolerance)
		{
			return false;
		}
		if (bHit)
		{
			WallStop.Z = FMath::Min(WallStop.Z, Hit.ImpactPoint.Z);
		}
	}

	StartLocation.Z = WallStop.Z + GetSlabHalfHeight() / 2.0f;
	return true;
}

bool AWoTWall::DeployResource(FVector StartLocation, FVector StartNormal)
{
	ResetPreviewChildren();
	if (!CalcLocation(StartLocation, StartNormal))
	{
		return false;
	}

	// The final altar ownership check and remove authorization both key off the manager.
	SetActorLocation(StartLocation);

	const float SlabWidth = static_cast<float>(GetSlabWidth());
	for (int32 SlabIndex = 0; SlabIndex < NumSlabs; ++SlabIndex)
	{
		const FVector OccupancyLocation = Floor
			+ (SlabWidth / 2.0f + SlabIndex * SlabWidth) * StartNormal;
		if (AnyActorsInArea(OccupancyLocation, SlabWidth) != nullptr)
		{
			return false;
		}
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	// Wall.uc:212 calls ValidateTrap() BEFORE spawning slabs: Trap.ValidateTrap
	// (Trap.uc:185-204) does SetOwner(None) on every final placement (owner drives net
	// relevancy — retail cleared it for exactly that reason; the tutorial-trigger poll is
	// deferred, as in Pit). The slabs stay owned by this manager, per the .uc spawn.
	if (!IsPreviewMode())
	{
		SetOwner(nullptr);
	}

	FVector SpawnLocation = StartLocation + (SlabWidth / 2.0f) * StartNormal;
	for (int32 SlabIndex = 0; SlabIndex < NumSlabs; ++SlabIndex)
	{
		const FTransform SlabTransform(
			GetSlabRotation(SlabIndex, StartNormal), SpawnLocation);
		AWoTWallSlab* Slab = World->SpawnActorDeferred<AWoTWallSlab>(
			*SlabClass, SlabTransform, this, GetInstigator(),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!IsValid(Slab))
		{
			// Wall.uc:245-249: a failed slab spawn KEEPS the partial wall and still
			// returns true (the resource is charged). Unreachable under AlwaysSpawn;
			// kept faithful.
			return true;
		}
		Slab->SetDamageInstigatorController(
			ResolveDamageInstigatorController());
		if (IsPreviewMode())
		{
			Slab->SetPreviewMode(true);
			ConfigurePreviewChild(Slab);
		}
		Slab->FinishSpawning(SlabTransform);
		if (IsPreviewMode())
		{
			ConfigurePreviewChild(Slab);
		}
		SpawnLocation += StartNormal * SlabWidth;
	}
	return true;
}

bool AWoTWall::RemoveResource()
{
	if (bLocked)
	{
		return false;
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AWoTWallSlab> It(World); It; ++It)
		{
			AWoTWallSlab* Slab = *It;
			if (IsValid(Slab) && Slab->GetOwner() == this)
			{
				Slab->Destroy();
			}
		}
	}
	return Super::RemoveResource();
}

FRotator AWoTWall::GetSlabRotation(int32 SlabIndex, const FVector& Normal) const
{
	FRotator Rotation = Normal.Rotation();
	switch (SlabIndex % 4)
	{
	case 1:
		Rotation.Yaw += 90.0f;
		break;
	case 2:
		Rotation.Yaw -= 90.0f;
		break;
	case 3:
		Rotation.Yaw += 180.0f;
		break;
	default:
		break;
	}
	return Rotation.GetNormalized();
}

void AWoTWall::BeginEditingResource(uint8 PlacedByTeam)
{
	Super::BeginEditingResource(PlacedByTeam);
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AWoTWallSlab> It(World); It; ++It)
		{
			AWoTWallSlab* Slab = *It;
			if (IsValid(Slab) && Slab->GetOwner() == this)
			{
				Slab->BeginEditingResource(PlacedByTeam);
			}
		}
	}
}

void AWoTWall::EndEditingResource()
{
	Super::EndEditingResource();
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AWoTWallSlab> It(World); It; ++It)
		{
			AWoTWallSlab* Slab = *It;
			if (IsValid(Slab) && Slab->GetOwner() == this)
			{
				Slab->EndEditingResource();
			}
		}
	}
}

void AWoTWall::Destroyed()
{
	// Wall.Destroyed: tear down every slab this wall owns (the manager owns its spawned segments).
	if (UWorld* W = GetWorld())
	{
		for (TActorIterator<AWoTWallSlab> It(W); It; ++It)
		{
			AWoTWallSlab* Slab = *It;
			if (IsValid(Slab) && Slab->GetOwner() == this)
			{
				Slab->Destroy();
			}
		}
	}
	Super::Destroyed();
}

// --------------------------- WallInventory --------------------------------- //

AWoTWallInventory::AWoTWallInventory()
{
	// WallInventory.uc defaultproperties (SpawnTempResource defaults true —
	// the wall is player-spawned, unlike the pre-placed portcullis/staircase).
	ResourceClass = AWoTWall::StaticClass();
	Count = 2;
	// WallInventory.uc:20,23: StatusIconFrame=Texture'WoTTraps.UI.M_SWall' /
	// StatusIcon=Texture'WoTTraps.UI.I_SWall'.
	BindStatusIcons(TEXT("WoTTraps"), TEXT("I_SWall"), TEXT("M_SWall"));
}
