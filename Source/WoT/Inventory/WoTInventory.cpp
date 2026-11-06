#include "WoTInventory.h"
#include "../AI/WoTPathNodeIterator.h"
#include "../WoTRetailText.h"

#include "WoTInventoryComponent.h"
#include "../AI/WoTNPCCharacter.h"
#include "../WoTCharacter.h"
#include "../WoTEvents.h"
#include "../WoTGameMode.h"
#include "../WoTPlayerController.h"
#include "../WoTSound.h"
#include "../WoTSoundTable.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTZoneVolume.h"
#include "../WoTPayloadSubsystem.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeActorVisuals.h"

#include "Components/SphereComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UObjectIterator.h"

const AWoTInventory* AWoTInventory::FindDefaultByOriginalName(FName OriginalClassName)
{
	if (OriginalClassName.IsNone())
	{
		return nullptr;
	}
	// Presentation-perf cache only — no lookup-semantics change. The HUD queries this
	// per populated hand per frame; a full TObjectIterator<UClass> scan there was
	// HUD-frame hot. Hits are cached by the queried name (weak: a stale entry re-scans),
	// and misses fall through to the scan so a class loaded later still resolves.
	static TMap<FName, TWeakObjectPtr<const AWoTInventory>> DefaultCache;
	if (const TWeakObjectPtr<const AWoTInventory>* Cached =
		DefaultCache.Find(OriginalClassName))
	{
		if (const AWoTInventory* CachedDefault = Cached->Get())
		{
			return CachedDefault;
		}
		DefaultCache.Remove(OriginalClassName);
	}
	const FString Original = OriginalClassName.ToString();
	const FString PortName = TEXT("WoT") + Original;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->IsChildOf(AWoTInventory::StaticClass()) &&
			(It->GetName() == Original || It->GetName() == PortName))
		{
			if (const AWoTInventory* Default = Cast<AWoTInventory>(It->GetDefaultObject()))
			{
				DefaultCache.Add(OriginalClassName, Default);
				return Default;
			}
			return nullptr;
		}
	}
	return nullptr;
}

AWoTInventory::AWoTInventory()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(30.0f);   // CollisionRadius=30/CollisionHeight=30
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Collision->SetGenerateOverlapEvents(true);
	RootComponent = Collision;

	Mesh = CreateDefaultSubobject<UWoTVertexAnimComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	bReplicates = true;
	NetPriority = 1.4f;
	StatusIconSelected = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/UI/StatusIconSelected.StatusIconSelected")));
	StatusIconFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/UI/StatusIconFrame.StatusIconFrame")));
}

float AWoTInventory::GetPickupCollisionRadius() const
{
	return Collision ? Collision->GetScaledSphereRadius() : 0.0f;
}

void AWoTInventory::BindStatusIcons(const TCHAR* IconName, const TCHAR* FrameName)
{
	BindStatusIcons(TEXT("Angreal"), IconName, FrameName);
}

void AWoTInventory::BindStatusIcons(const TCHAR* Directory, const TCHAR* IconName,
                                    const TCHAR* FrameName)
{
	StatusIcon = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/%s/%s.%s"), Directory, IconName, IconName)));
	StatusIconFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/%s/%s.%s"), Directory, FrameName, FrameName)));
}

void AWoTInventory::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	// UE1 LoadLocalized: Title/Description/Quote/PickupMessage/... come from the
	// purchaser's retail .int (never compiled in), on every machine that has the
	// item (not replicated: class data).
	FWoTRetailText::ApplyLocalizedDefaults(this);
	EnsureRuntimeVisualPresentation();
	ApplyRuntimeVisualPresentation();
	if (HasAuthority())
	{
		Collision->OnComponentBeginOverlap.AddDynamic(this, &AWoTInventory::OnPickupOverlap);
		// WOTInventory.SetInitialState: items spawned held skip the Pickup state
		GotoItemState(bHeldItem ? EWoTInventoryState::Held : EWoTInventoryState::Pickup);
	}
}

void AWoTInventory::ConfigureRuntimeVisualPresentation(
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
	bRotatingPickup = Presentation.bRotatingPickup;
	PickupRotationRate = Presentation.RotationRate;
}

bool AWoTInventory::ConfigureRuntimeClassDefaultVisuals(
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
		OutError = TEXT("the retail inventory class has no effective Mesh property");
		return false;
	}
	ConfigureRuntimeVisualPresentation(Presentation);
	return true;
}

void AWoTInventory::EnsureRuntimeVisualPresentation()
{
	if (bHasRuntimeVisualPresentation) return;
	FString Error;
	FWoTRuntimeClassDefaults* Defaults =
		UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
	if (Defaults == nullptr) return;
	FString NativeLeaf = GetClass()->GetName();
	if (NativeLeaf.StartsWith(TEXT("WoT"), ESearchCase::CaseSensitive))
		NativeLeaf.RightChopInline(3);
	FString RetailClass;
	if (!Defaults->FindClassIdentityByLeaf(NativeLeaf, RetailClass)) return;
	FWoTRuntimeActorVisualPresentation Presentation;
	if (WoTRuntimeReadClassVisualPresentation(
		*Defaults, RetailClass, Presentation, Error) && Presentation.HasMesh())
	{
		ConfigureRuntimeVisualPresentation(Presentation);
	}
}

void AWoTInventory::ApplyRuntimeVisualPresentation()
{
	if (!bHasRuntimeVisualPresentation || !Mesh) return;
	FString Error;
	if (!WoTRuntimeApplyActorVisualPresentation(
		*Mesh, RuntimeVisualPresentation, Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("WOT PICKUP-VISUAL actor=%s error=%s"),
			*GetPathName(), *Error);
	}
}

void AWoTInventory::GotoItemState(EWoTInventoryState NewState)
{
	ItemState = NewState;                    // replicated -> OnRep_ItemState on proxies
	ApplyItemStatePresentation(NewState);    // visibility/collision/tick (both authority + proxy)
	if (NewState == EWoTInventoryState::Sleeping)
	{
		bFalling = false;
		// state Sleeping: Sleep(RespawnTime) then respawn — server-only timer.
		GetWorldTimerManager().SetTimer(RespawnTimer, this,
			&AWoTInventory::RespawnTimerExpired, FMath::Max(RespawnTime, 0.1f));
	}
}

void AWoTInventory::ApplyItemStatePresentation(EWoTInventoryState State)
{
	switch (State)
	{
	case EWoTInventoryState::Pickup:
		BecomePickup();
		break;

	case EWoTInventoryState::Sleeping:
		// hidden pickup (the RespawnTimer is set server-only in GotoItemState).
		BecomePickup();
		SetActorHiddenInGame(true);
		SetActorEnableCollision(false);
		SetActorTickEnabled(false);
		break;

	case EWoTInventoryState::Held:
		BecomeItem();
		break;
	}
}

void AWoTInventory::OnRep_ItemState()
{
	// A proxy re-derives its rendering from the replicated state — reproducing UE1's native
	// bHidden/collision replication, which UE5 does not honor by writing the bit alone.
	ApplyItemStatePresentation(ItemState);
}

void AWoTInventory::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTInventory, ItemState);
	DOREPLIFETIME(AWoTInventory, bDropped);
	// Owner-only citadel-editor resource count (WOTInventory.uc:31-36 `bNetOwner`).
	DOREPLIFETIME_CONDITION(AWoTInventory, Count, COND_OwnerOnly);
}

void AWoTInventory::BecomePickup()
{
	SetActorHiddenInGame(false);
	SetActorEnableCollision(!bCollisionHeldByPool);
	// bRotatingPickup: PHYS_Rotating, RotationRate Yaw=5000 (UE1 units/sec).
	// Dropped items skip the spin (Pickup@Dropped bypasses the Begin-label
	// PHYS_Rotating block) but still tick the fall on authority.
	SetActorTickEnabled((bRotatingPickup && !bDropped) || bFalling);
}

void AWoTInventory::BecomeItem()
{
	bFalling = false;
	bCollisionHeldByPool = false;   // the pool's hold ends with the handoff
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
	GetWorldTimerManager().ClearTimer(RespawnTimer);
}

bool AWoTInventory::TraceFallingMovement(const FVector& Start,
	const FVector& End, FHitResult& OutHit) const
{
	// Retail bCollideWorld falling blocks on level geometry only — never pawns
	// (ordinary items are non-blocking to actors) and never trigger shapes.
	return WoTVisBlockingSweep(GetWorld(), Start, End, FQuat::Identity,
		FCollisionShape::MakeSphere(Collision->GetScaledSphereRadius()),
		this, OutHit);
}

void AWoTInventory::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (HasAuthority() && ItemState == EWoTInventoryState::Pickup && bFalling)
	{
		FWoTInventoryFallState State;
		State.Location = GetActorLocation();
		State.Velocity = FallingVelocity;
		StepFalling(State, DeltaTime, Mass, Buoyancy,
			[this](const FVector& Location, FWoTInventoryFallZone& Zone)
			{
				FWoTZoneMovement Effect;
				AWoTZoneVolume::GetMovementEffectAt(GetWorld(), Location, Effect);
				Zone.Gravity = Effect.bOverrideGravity
					? Effect.Gravity : FVector(0.0f, 0.0f, -950.0f);
				Zone.Current = Effect.Current;
				Zone.FluidFriction = Effect.FluidFriction;
				Zone.bWater = Effect.bWater;
			},
			[this](const FVector& Start, const FVector& End, FHitResult& Hit)
			{
				return TraceFallingMovement(Start, End, Hit);
			});
		SetActorLocation(State.Location, false);
		FallingVelocity = State.Velocity;
		if (State.bLanded)
		{
			Landed(State.LandedNormal);
		}
	}
	if (ItemState == EWoTInventoryState::Pickup && bRotatingPickup && !bDropped
		&& !bFalling)
	{
		AddActorLocalRotation(PickupRotationRate * DeltaTime);
	}
}

void AWoTInventory::OnPickupOverlap(UPrimitiveComponent*, AActor* Other,
                                    UPrimitiveComponent*, int32, bool,
                                    const FHitResult&)
{
	// state Pickup, function Touch
	if (!HasAuthority() || ItemState != EWoTInventoryState::Pickup)
	{
		return;
	}
	// Inventory.DropFrom retains Instigator as the pawn that dropped the item.
	// UE1's flat cylinders begin tangent for a Seal; UE5 overlap delivery can see
	// the carrier before the falling item has moved away and immediately give it
	// back. Suppress only that transient falling overlap. Once landed, the former
	// carrier may pick the item up normally.
	if (bFalling && Other == GetInstigator())
	{
		return;
	}
	TryPickupBy(Other);
}

bool AWoTInventory::TryPickupBy(AActor* Other)
{
	// Inventory.Touch: if ValidTouch (living player + PickupQuery), take it. The
	// PickupQuery step is where an AngrealInventory steals a duplicate's charges
	// instead of granting a second copy — so a spawn pool must route its handoff
	// through HERE (not a bare GiveTo), or pool dups would stack up.
	if (!ValidTouch(Other))
	{
		return false;
	}
	APawn* P = CastChecked<APawn>(Other);
	SpawnCopy(P);
	if (APlayerController* PC = Cast<APlayerController>(P->GetController()))
	{
		// Inventory.Touch notifies with type 'Pickup' (the original engine's
		// Inventory class; shipped Engine is source-stripped): the message lands in
		// the HUD hand-message slot, not the console.
		if (AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(PC))
		{
			WoTPC->ClientTypedMessage(PickupMessage, FName(TEXT("Pickup")));
		}
		else
		{
			PC->ClientMessage(PickupMessage);
		}
	}
	// Engine.Inventory PlaySound(PickupSound): the server's Touch — heard by
	// every player in range (UE1's native PlaySound), volume 1, radius 1600.
	WoTSound::PlayReplicatedAsset(*GetWorld(), PickupSound, GetActorLocation(),
		WoTSound::DefaultPlaySoundRadius);
	return true;
}

bool AWoTInventory::ValidTouch(AActor* Other)
{
	// Engine.Inventory.ValidTouch: a LIVING PLAYER pawn
	// only — bIsPlayer && Health > 0. The port's established bIsPlayer identity
	// is "not AWoTNPCCharacter"; unlike IsPlayerControlled(), this preserves the
	// unpossessed plain-character fixtures while rejecting NPC inventory carriers.
	// The Health gate is now live (a corpse can no longer consume a pickup).
	APawn* P = Cast<APawn>(Other);
	if (!P || P->IsA<AWoTNPCCharacter>()
		|| !P->FindComponentByClass<UWoTInventoryComponent>())
	{
		return false;
	}
	if (const AWoTCharacter* Ch = Cast<AWoTCharacter>(P))
	{
		if (Ch->Health <= 0)
		{
			return false;   // dead pawns don't pick up
		}
	}
	AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	if (Game && !Game->PickupQuery(P, this))
	{
		return false;
	}
	if (EventName != NAME_None)
	{
		if (UWoTEventSubsystem* Events = GetWorld()->GetSubsystem<UWoTEventSubsystem>())
		{
			Events->FireEvent(EventName, P, P);
		}
	}
	return true;
}

AWoTInventory* AWoTInventory::SpawnCopy(APawn* Other)
{
	AWoTInventory* Copy = this;
	AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	if (Game && Game->ShouldRespawn(this))
	{
		// deferred so bHeldItem is set before SetInitialState — the copy must
		// never enter the Pickup state (it would re-touch the same pawn)
		const FTransform Xform(GetActorRotation(), GetActorLocation());
		Copy = GetWorld()->SpawnActorDeferred<AWoTInventory>(GetClass(), Xform,
			Other, Other, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		Copy->bHeldItem = true;
		InitializeCopy(Copy);
		Copy->FinishSpawning(Xform);
		GotoItemState(EWoTInventoryState::Sleeping);
	}
	Copy->RespawnTime = 0.0f;
	Copy->bHeldItem = true;
	Copy->GiveTo(Other);
	return Copy;
}

void AWoTInventory::InitializeCopy(AWoTInventory* Copy)
{
	// Inventory.InitializeCopy: Tag + Event carry over
	Copy->Tags = Tags;
	Copy->EventName = EventName;
}

void AWoTInventory::GiveTo(APawn* Other)
{
	SetInstigator(Other);
	if (bDropped)
	{
		// Collected again: leave the dropped lifecycle (spin suppression and
		// the drop-scoped movement replication end with it).
		bDropped = false;
		bFalling = false;
		SetReplicateMovement(false);
	}
	GotoItemState(EWoTInventoryState::Held);
	if (UWoTInventoryComponent* Inv =
	        Other->FindComponentByClass<UWoTInventoryComponent>())
	{
		Inv->AddInventory(this);
	}
}

void AWoTInventory::SetRespawn()
{
	AWoTGameMode* Game = GetWorld()->GetAuthGameMode<AWoTGameMode>();
	if (Game && Game->ShouldRespawn(this))
	{
		GotoItemState(EWoTInventoryState::Sleeping);
	}
	else
	{
		Destroy();
	}
}

void AWoTInventory::DropFrom(FVector StartLocation)
{
	if (!TeleportTo(StartLocation, GetActorRotation()))
	{
		return;
	}
	RespawnTime = 0.0f;   // don't respawn
	bHeldItem = false;
	FallingVelocity = FVector::ZeroVector;
	bFalling = true;
	bDropped = true;
	// UE1's DumbProxy→SimulatedProxy net staging collapses into UE5 movement
	// replication, scoped to dropped items: placed pickups keep their
	// client-local spin without per-pickup transform traffic.
	SetReplicateMovement(true);
	if (UWoTInventoryComponent* Inv = GetOwnerInventory())
	{
		Inv->DeleteInventory(this);
	}
	GotoItemState(EWoTInventoryState::Pickup);
}

void AWoTInventory::Landed(const FVector&)
{
	const FRotator OldRotation = GetActorRotation();
	SetActorRotation(FRotator(0.0f, OldRotation.Yaw, OldRotation.Roll));
	bFalling = false;
	// A dropped item rests where it fell — no PHYS_Rotating resume (retail
	// entered Pickup at the Dropped label, past the Begin-label spin block).
	SetActorTickEnabled(bRotatingPickup && !bDropped);
}

void AWoTInventory::StepFalling(FWoTInventoryFallState& State, float DeltaTime,
	float InMass, float InBuoyancy,
	TFunctionRef<void(const FVector&, FWoTInventoryFallZone&)> ZoneAt,
	TFunctionRef<bool(const FVector&, const FVector&, FHitResult&)> Trace)
{
	State.bLanded = false;
	State.LandedNormal = FVector::ZeroVector;
	State.ApexFraction = -1.0f;
	if (!State.bFalling || DeltaTime <= 0.0f)
	{
		return;
	}

	auto Move = [&State, &Trace](const FVector& Delta, FHitResult& Hit)
	{
		const FVector Start = State.Location;
		const FVector End = Start + Delta;
		Hit = FHitResult();
		Hit.Time = 1.0f;
		if (Trace(Start, End, Hit) && Hit.Time < 1.0f)
		{
			Hit.Time = FMath::Clamp(Hit.Time, 0.0f, 1.0f);
			State.Location = FMath::Lerp(Start, End, Hit.Time);
			return true;
		}
		Hit.Time = 1.0f;
		State.Location = End;
		return false;
	};
	auto FinishLanding = [&State](const FVector& Normal)
	{
		State.bFalling = false;
		State.bLanded = true;
		State.LandedNormal = Normal;
	};
	auto TwoWallAdjust = [](const FVector& DesiredDir, FVector& Delta,
		const FVector& HitNormal, const FVector& OldHitNormal, float HitTime)
	{
		if (FVector::DotProduct(OldHitNormal, HitNormal) <= 0.0f)
		{
			const FVector NewDir = FVector::CrossProduct(HitNormal, OldHitNormal).GetSafeNormal();
			Delta = FVector::DotProduct(Delta, NewDir) * (1.0f - HitTime) * NewDir;
			if (FVector::DotProduct(DesiredDir, Delta) < 0.0f)
			{
				Delta *= -1.0f;
			}
		}
		else
		{
			Delta = (Delta - HitNormal * FVector::DotProduct(Delta, HitNormal))
				* (1.0f - HitTime);
			if (FVector::DotProduct(Delta, DesiredDir) <= 0.0f)
			{
				Delta = FVector::ZeroVector;
			}
		}
	};

	float RemainingTime = DeltaTime;
	int32 Iterations = 0;
	bool bAdjustApex = false;
	while (RemainingTime > 0.0f && Iterations < 8 && State.bFalling)
	{
		++Iterations;
		float TimeTick = RemainingTime > 0.1f
			? FMath::Min(0.1f, RemainingTime * 0.5f) : RemainingTime;
		RemainingTime -= TimeTick;
		const FVector OldLocation = State.Location;
		FVector OldVelocity = State.Velocity;

		FWoTInventoryFallZone Zone;
		ZoneAt(State.Location, Zone);
		auto AverageVelocity = [&Zone, InMass, InBuoyancy](
			const FVector& StartVelocity, float Time)
		{
			if (!Zone.bWater)
			{
				return StartVelocity + 0.5f * Zone.Gravity * Time;
			}
			return StartVelocity * (1.0f - 2.0f * Zone.FluidFriction * Time)
				+ 0.5f * Zone.Gravity
					* (1.0f - InBuoyancy / FMath::Max(1.0f, InMass)) * Time;
		};
		State.Velocity = AverageVelocity(OldVelocity, TimeTick);

		if (!bAdjustApex
			&& ((OldVelocity.Z > 0.0f) != (State.Velocity.Z > 0.0f))
			&& FMath::Abs(OldVelocity.Z) > 5.0f
			&& FMath::Abs(State.Velocity.Z) > 5.0f)
		{
			bAdjustApex = true;
			const float Part = FMath::Abs(OldVelocity.Z)
				/ (FMath::Abs(OldVelocity.Z) + FMath::Abs(State.Velocity.Z));
			State.ApexFraction = Part;
			if (Part * TimeTick > 0.015f && (1.0f - Part) * TimeTick > 0.015f)
			{
				RemainingTime += TimeTick * (1.0f - Part);
				TimeTick *= Part;
				State.Velocity = AverageVelocity(OldVelocity, TimeTick);
			}
		}
		else
		{
			bAdjustApex = false;
		}

		const FVector Adjusted = (State.Velocity + Zone.Current) * TimeTick;
		FHitResult Hit;
		if (Move(Adjusted, Hit))
		{
			if (Hit.Normal.Z > 0.7f)
			{
				RemainingTime += TimeTick * (1.0f - Hit.Time);
				if (Hit.Time > 0.1f && Hit.Time * TimeTick > 0.003f)
				{
					State.Velocity = (State.Location - OldLocation) / (TimeTick * Hit.Time);
				}
				FinishLanding(Hit.Normal);
				return;
			}

			const FVector OldHitNormal = Hit.Normal;
			FVector Delta = (Adjusted
				- Hit.Normal * FVector::DotProduct(Adjusted, Hit.Normal))
				* (1.0f - Hit.Time);
			if (FVector::DotProduct(Delta, Adjusted) >= 0.0f)
			{
				FHitResult SecondHit;
				if (Move(Delta, SecondHit))
				{
					if (SecondHit.Normal.Z > 0.7f)
					{
						FinishLanding(SecondHit.Normal);
						return;
					}
					const FVector DesiredDir = Adjusted.GetSafeNormal();
					TwoWallAdjust(DesiredDir, Delta, SecondHit.Normal,
						OldHitNormal, SecondHit.Time);
					const bool bDitch = OldHitNormal.Z > 0.0f
						&& SecondHit.Normal.Z > 0.0f && Delta.Z == 0.0f
						&& FVector::DotProduct(SecondHit.Normal, OldHitNormal) < 0.0f;
					FHitResult ThirdHit;
					const bool bThirdHit = Move(Delta, ThirdHit);
					if (bDitch || (bThirdHit && ThirdHit.Normal.Z > 0.7f))
					{
						FinishLanding(bThirdHit ? ThirdHit.Normal : SecondHit.Normal);
						return;
					}
				}
			}
			const float OldZ = OldVelocity.Z;
			OldVelocity = (State.Location - OldLocation) / TimeTick;
			OldVelocity.Z = OldZ;
		}

		State.Velocity = (State.Location - OldLocation) / TimeTick - Zone.Current;
		if (State.Velocity.Z < OldVelocity.Z || OldVelocity.Z >= 0.0f)
		{
			State.Velocity = 2.0f * State.Velocity - OldVelocity;
		}
		if (State.Velocity.SizeSquared() > FMath::Square(2500.0f))
		{
			State.Velocity = State.Velocity.GetSafeNormal() * 2500.0f;
		}
	}
}

void AWoTInventory::RespawnTimerExpired()
{
	// state Sleeping Begin: PlaySound(RespawnSound), PlaySpawnEffect, Pickup
	// (Engine.Inventory — a server PlaySound, replicated to players in range).
	WoTSound::PlayReplicatedAsset(*GetWorld(), RespawnSound, GetActorLocation(),
		WoTSound::DefaultPlaySoundRadius);
	GotoItemState(EWoTInventoryState::Pickup);
}

void AWoTInventory::Destroyed()
{
	if (UWoTInventoryComponent* Inv = GetOwnerInventory())
	{
		Inv->DeleteInventory(this);
	}
	Super::Destroyed();
}

bool AWoTInventory::HandlePickupQuery(AWoTInventory* Item)
{
	// base Inventory: same class = already have one, abort pickup
	return Item->GetClass() == GetClass();
}

UWoTInventoryComponent* AWoTInventory::GetOwnerInventory() const
{
	const APawn* P = Cast<APawn>(GetOwner());
	return P ? P->FindComponentByClass<UWoTInventoryComponent>() : nullptr;
}

void AWoTInventory::ClientMessageToOwner(const FString& Msg, FName Type) const
{
	const APawn* P = Cast<APawn>(GetOwner());
	APlayerController* PC = P ? Cast<APlayerController>(P->GetController()) : nullptr;
	if (!PC)
	{
		return;
	}
	// WOTPlayer.ClientMessage's typed routing lives on the WoT controller; a bare
	// controller keeps the stock console path.
	if (AWoTPlayerController* WoTPC = Cast<AWoTPlayerController>(PC))
	{
		WoTPC->ClientTypedMessage(Msg, Type);
	}
	else
	{
		PC->ClientMessage(Msg);
	}
}
