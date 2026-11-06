#include "WoTSeal.h"
#include "WoTInventoryComponent.h"
#include "../WoTRainCyd.h"
#include "../WoTMover.h"

#include "../WoTBattleGameMode.h"
#include "../WoTBattleInfo.h"
#include "../WoTCharacter.h"
#include "../WoTFactionCharacters.h"
#include "../WoTMeshLight.h"
#include "../WoTMover.h"
#include "../WoTParticleSprayer.h"
#include "../WoTPlayerColorFX.h"
#include "../WoTPlayerController.h"
#include "../WoTPlayerState.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTWeaveFX.h"
#include "../AI/WoTNPCController.h"
#include "../AI/WoTPathNode.h"
#include "../AI/WoTPathNodeIterator.h"

#include "Components/SphereComponent.h"
#include "Components/PrimitiveComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "Net/UnrealNetwork.h"

namespace
{
	// Seal.uc:88-110's switch keys off the placer's PlayerClass.Name; the port
	// reads the pawn's (appearance-)class against the four faction characters
	// (the P02 appearance conventions).
	FName SealFactionForPawn(const APawn* Pawn)
	{
		const UClass* SourceClass = Pawn ? Pawn->GetClass() : nullptr;
		if (const AWoTCharacter* Character = Cast<AWoTCharacter>(Pawn))
		{
			if (Character->AppearanceClass)
			{
				SourceClass = *Character->AppearanceClass;
			}
		}
		if (!SourceClass)
		{
			return NAME_None;
		}
		if (SourceClass->IsChildOf(AWoTAesSedai::StaticClass()))
		{
			return FName(TEXT("AesSedai"));
		}
		if (SourceClass->IsChildOf(AWoTForsaken::StaticClass()))
		{
			return FName(TEXT("Forsaken"));
		}
		if (SourceClass->IsChildOf(AWoTHound::StaticClass()))
		{
			return FName(TEXT("Hound"));
		}
		if (SourceClass->IsChildOf(AWoTWhitecloak::StaticClass()))
		{
			return FName(TEXT("Whitecloak"));
		}
		return NAME_None;
	}
}

AWoTSeal::AWoTSeal()
{
	StatusIcon = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WOT/I_SSeal.I_SSeal")));
	StatusIconFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		TEXT("/Game/WoT/Textures/WOT/M_SSeal.M_SSeal")));
	// Seal defaultproperties (Seal.uc:442): selection-group 40 — a carried seal cycles
	// ahead of every angreal (lowest group is Absorb/AndilayRoot at 50) in the
	// original's InventoryGroup order.
	InventoryGroup = 40;
	bDisplayableInv = true;
	PickupSound = TSoftObjectPtr<USoundBase>(FSoftObjectPath(
		TEXT("/Game/WoT/Sounds/WOT/Pickup.Pickup")));
	// Seal.uc:456-457: CollisionRadius 17 / Height 3 — the base inventory's
	// 30uu touch sphere is nearly double the authored pickup range
	// (retro-review). Sphere-for-cylinder is the module convention; the
	// 17-vs-3 vertical divergence is a PORT_NOTES rider.
	if (Collision)
	{
		Collision->SetSphereRadius(17.0f);
	}
}

void AWoTSeal::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTSeal, ControllingFactionName);
	DOREPLIFETIME(AWoTSeal, CarryColorName);
	DOREPLIFETIME(AWoTSeal, bRestingOnAltar);
	DOREPLIFETIME(AWoTSeal, bGroundPlaced);
	DOREPLIFETIME(AWoTSeal, bEditingGlow);
}

void AWoTSeal::BeginEditingResource(int32 PlacedByTeam)
{
	if (!HasAuthority() || bPreviewSeal)
	{
		return;
	}
	Team = static_cast<uint8>(PlacedByTeam);
	bEditingGlow = true;
	ApplyEditingGlowPresentation();
}

void AWoTSeal::EndEditingResource()
{
	if (!HasAuthority() || bPreviewSeal)
	{
		return;
	}
	bEditingGlow = false;
	ApplyEditingGlowPresentation();
}

void AWoTSeal::OnRep_EditingGlow()
{
	ApplyEditingGlowPresentation();
}

void AWoTSeal::ApplyEditingGlowPresentation()
{
	// Seal.uc:132,137: AmbientGlow 64 while editing, 0 afterward. As with traps,
	// force the runtime UE1 mesh-light bake when this actor owns one; headless
	// worlds and data-only actors simply retain the reflected property value.
	AmbientGlow = bEditingGlow ? 64.0f : 0.0f;
	if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
	{
		MeshLight->ForceRuntimeBake();
	}
}

bool AWoTSeal::DeployResource(const FVector& StartLocation,
	const FVector& StartNormal)
{
	// Seal.uc:258-261: only a near-upward floor normal is legal.
	if (StartNormal.Z < 0.9f)
	{
		return false;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Seal.uc:263-271: the 16uu down trace must hit Level (retail BSP).
	FHitResult Hit;
	if (!WoTRetailActorTrace(World, StartLocation,
		StartLocation + FVector(0, 0, -16.0f), this, Hit)
		|| !Hit.bBlockingHit || !Hit.GetComponent()
		|| Hit.GetComponent()->GetCollisionObjectType() != ECC_WorldStatic)
	{
		return false;
	}
	const FVector HitLocation = Hit.ImpactPoint;

	// Seal.GetTouchingPathNode (Seal.uc:184-207): test the BASE of every
	// NavigationPoint cylinder, not a UE5 NavMesh projection.
	bool bFoundNode = false;
	const float RadiusSq = FMath::Square(PathNodeDeltaR);
	for (TActorIterator<AWoTPathNode> It(World); It; ++It)
	{
		const AWoTPathNode* Node = *It;
		if (!IsValid(Node))
		{
			continue;
		}
		const FVector NodeBase = Node->GetActorLocation()
			- FVector(0, 0, Node->NodeCollisionHeight);
		const FVector2D DeltaXY(NodeBase.X - HitLocation.X,
			NodeBase.Y - HitLocation.Y);
		if (FMath::Abs(NodeBase.Z - HitLocation.Z) <= PathNodeDeltaH
			&& DeltaXY.SizeSquared() <= RadiusSq)
		{
			bFoundNode = true;
			break;
		}
	}
	if (!bFoundNode || IsValid(AnyActorsInArea(HitLocation, PathNodeDeltaR)))
	{
		return false;
	}

	// Seal.uc:287-297 Show: placed seals are visible but collision-off forever.
	SetActorLocation(HitLocation + FVector(0, 0, 3.0f));
	SetActorRotation(FRotator::ZeroRotator);
	bHeldItem = false;
	RespawnTime = 0.0f;
	bDropped = true;
	SetReplicateMovement(true);
	if (!bPreviewSeal)
	{
		bGroundPlaced = true;
	}
	GotoItemState(EWoTInventoryState::Pickup);
	SetActorHiddenInGame(false);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
	return true;
}

AActor* AWoTSeal::AnyActorsInArea(const FVector& SearchLocation,
	float SearchDistance) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	auto BlocksSpot = [this, &SearchLocation](AActor* Candidate,
		bool bParticleSprayer) -> bool
	{
		if (!IsValid(Candidate) || Candidate == this
			|| Candidate->IsA<AWoTMover>() || Candidate->IsA<AWoTInventory>())
		{
			return false;
		}
		// ParticleSprayer is a BlockingActor before Seal.uc's !bHidden clause.
		if (!bParticleSprayer && Candidate->IsHidden())
		{
			return false;
		}
		float CandidateRadius = 0.0f;
		float CandidateHalfHeight = 0.0f;
		Candidate->GetSimpleCollisionCylinder(
			CandidateRadius, CandidateHalfHeight);
		const FVector CandidateLocation = Candidate->GetActorLocation();
		const FVector2D DeltaXY(CandidateLocation.X - SearchLocation.X,
			CandidateLocation.Y - SearchLocation.Y);
		return DeltaXY.SizeSquared() <= FMath::Square(CandidateRadius + 17.0f)
			&& FMath::Abs(CandidateLocation.Z - SearchLocation.Z)
				<= CandidateHalfHeight + 3.0f;
	};

	TArray<FOverlapResult> Overlaps;
	World->OverlapMultiByObjectType(Overlaps, SearchLocation, FQuat::Identity,
		FCollisionObjectQueryParams::AllObjects,
		FCollisionShape::MakeSphere(SearchDistance));
	for (const FOverlapResult& Overlap : Overlaps)
	{
		// UE1 RadiusActors never returned Level. Keep world geometry out of the
		// occupant set, following AWoTTrap::AnyActorsInArea.
		const UPrimitiveComponent* Component = Overlap.GetComponent();
		if (Component && Component->GetCollisionObjectType() == ECC_WorldStatic)
		{
			continue;
		}
		AActor* Candidate = Overlap.GetActor();
		if (BlocksSpot(Candidate,
			IsValid(Candidate) && Candidate->IsA<AWoTParticleSprayer>()))
		{
			return Candidate;
		}
	}

	// ParticleSprayer blocks even hidden and collision-less, so overlap queries
	// alone cannot reproduce the retail BlockingActor branch.
	for (TActorIterator<AWoTParticleSprayer> It(World); It; ++It)
	{
		if (BlocksSpot(*It, true))
		{
			return *It;
		}
	}
	return nullptr;
}

void AWoTSeal::OnRep_PlacementState()
{
	ApplyPlacedPresentation();
}

void AWoTSeal::ApplyPlacedPresentation()
{
	if (!bRestingOnAltar && !bGroundPlaced)
	{
		return;
	}
	bHeldItem = false;
	RespawnTime = 0.0f;
	bDropped = true;
	SetReplicateMovement(true);
	SetActorHiddenInGame(false);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
	UpdateIndicator();
}

AWoTBattleInfo* AWoTSeal::GetBattleInfo() const
{
	AWoTBattleGameMode* Game =
		GetWorld() ? GetWorld()->GetAuthGameMode<AWoTBattleGameMode>() : nullptr;
	return Game ? Game->Battle : nullptr;
}

void AWoTSeal::SetControllingTeam(AController* Player)
{
	if (bPreviewSeal)
	{
		return;
	}
	// Seal.uc:75 — no change if the placer is already the crediting team.
	AWoTPlayerState* PS = Player ? Player->GetPlayerState<AWoTPlayerState>() : nullptr;
	if (!PS || ControllingTeam == PS->Team)
	{
		return;
	}

	// Seal.uc:79 — the count is only tracked on a Battle game with its BattleInfo located.
	AWoTBattleInfo* Battle = GetBattleInfo();
	if (!Battle)
	{
		return;
	}

	if (ControllingTeam != 255)
	{
		Battle->DecrementTeamSealCount(ControllingTeam);
	}
	ControllingTeam = PS->Team;
	Battle->IncrementTeamSealCount(ControllingTeam);

	// Seal.uc:88-110: adopt the placer's faction skin/icon identity (replicated;
	// each machine applies it in OnRep / here on the authority).
	const FName Faction = SealFactionForPawn(Player->GetPawn());
	if (!Faction.IsNone() && Faction != ControllingFactionName)
	{
		ControllingFactionName = Faction;
		ApplyControllingFaction();
	}
}

void AWoTSeal::ClearControllingTeam()
{
	if (bPreviewSeal)
	{
		return;
	}
	// Seal.uc:61-67 — the reset-to-255 lives INSIDE the Battle guard: off a Battle game the
	// credit is left untouched (faithful). The faction skin persists (retail never resets it).
	AWoTBattleInfo* Battle = GetBattleInfo();
	if (Battle && ControllingTeam != 255)
	{
		Battle->DecrementTeamSealCount(ControllingTeam);
		ControllingTeam = 255;
	}
}

void AWoTSeal::OnRep_ControllingFactionName()
{
	ApplyControllingFaction();
}

void AWoTSeal::ApplyControllingFaction()
{
	if (ControllingFactionName.IsNone())
	{
		return;
	}
	const FString F = ControllingFactionName.ToString();
	StatusIcon = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		FString::Printf(TEXT("/Game/WoT/Textures/WOT/I_%sSeal.I_%sSeal"), *F, *F)));
	StatusIconFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(
		FString::Printf(TEXT("/Game/WoT/Textures/WOT/M_%sSeal.M_%sSeal"), *F, *F)));
	// MultiSkins[1] = S_<Faction>Seal (Seal.uc:93). Runtime meshes address the
	// authored texture index; SetSkinOverride maps that to the one-slot developer
	// cooked carrier when necessary.
	if (Mesh)
	{
		Mesh->SetSkinOverride(1, FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Textures/WOT/S_%sSeal.S_%sSeal"), *F, *F)));
	}
}

void AWoTSeal::GiveTo(APawn* Other)
{
	// Seal.uc:368-374: retire any existing indicator before the hand-off.
	ClearIndicator();
	bRestingOnAltar = false;   // leaving an altar (the hand-back path)
	bGroundPlaced = false;

	Super::GiveTo(Other);

	if (Other && GetOwner() == Other)   // successfully given (Seal.uc:378)
	{
		// Seal.uc:380-383: taking a credited seal from another team notifies
		// the losing team.
		AWoTPlayerState* PS = Other->GetPlayerState<AWoTPlayerState>();
		if (ControllingTeam != 255 && (!PS || PS->Team != ControllingTeam))
		{
			SendTakeNotification(ControllingTeam);
		}
		// Seal.uc:386-388: the carry indicator in the carrier's color — the
		// AUTHORITY resolves it (retail SetColor ran server-side); proxies
		// consume the replicated name.
		if (HasAuthority())
		{
			CarryColorName = WoTPlayerColorFX::Resolve(Other, FName(TEXT("Green")));
		}
		UpdateIndicator();
		// Seal.PickupFunction (Seal.uc:360-364): every successful pickup fans
		// the ownership-changed pawn event out.
		NotifyPawnsSealEvent(OwnershipChangedEvent, Other);
	}
}

void AWoTSeal::DropFrom(FVector StartLocation)
{
	FVector DropSpot = StartLocation;
	// Seal.uc:336 SetLocation reaches UE1's FindSpot:
	// axis/corner VisBlocking probes nudge a shallow penetration before the
	// NavigationPoint fallback is considered.
	if (!TryFindDropSpot(DropSpot))
	{
		if (!GetClosestPathNodeLocation(DropSpot))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("Seal drop fallback found no usable path node near %s"),
				*DropSpot.ToString());
		}
	}

	bRestingOnAltar = false;   // state coherence (banked seals are never dropped)
	bGroundPlaced = false;
	// Inventory::DropFrom uses TeleportTo, whose UE5 encroachment test rejects
	// the carrier overlap that UE1 permits at the start of a drop. TryFindDropSpot
	// already performed the retail world fit; seat without an actor-overlap query,
	// then enter the same dropped-pickup state as the base implementation.
	SetActorLocation(DropSpot, false, nullptr, ETeleportType::TeleportPhysics);
	RespawnTime = 0.0f;
	bHeldItem = false;
	FallingVelocity = FVector::ZeroVector;
	bFalling = true;
	bDropped = true;
	SetReplicateMovement(true);
	if (UWoTInventoryComponent* Inv = GetOwnerInventory())
	{
		Inv->DeleteInventory(this);
	}
	GotoItemState(EWoTInventoryState::Pickup);
	// Inventory.DropFrom is solid while falling; Seal.Landed relaxes it back to
	// the normal touch-only pickup cylinder.
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	// Landed restores the normal touch-only pickup response.
	NotifyPawnsSealEvent(OwnershipChangedEvent, nullptr);
}

void AWoTSeal::Landed(const FVector& HitNormal)
{
	Super::Landed(HitNormal);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

bool AWoTSeal::TraceFallingMovement(const FVector& Start,
	const FVector& End, FHitResult& OutHit) const
{
	UWorld* World = GetWorld();
	if (!World || !Collision)
	{
		return false;
	}
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	Objects.AddObjectTypesToQuery(ECC_Pawn);
	Objects.AddObjectTypesToQuery(ECC_GameTraceChannel1); // WoTProjectile
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTSealFalling), false, this);
	TArray<FHitResult> Hits;
	// Seal.uc authors CollisionRadius=17, CollisionHeight=3.  A UE sphere made
	// the live carrier a time-zero side wall; use a containing box only as the
	// broad phase and apply the flat-cylinder/initial-overlap rules below.
	constexpr float SealRadius = 17.0f;
	constexpr float SealHalfHeight = 3.0f;
	World->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, Objects,
		FCollisionShape::MakeBox(FVector(SealRadius, SealRadius, SealHalfHeight)),
		Params);
	Hits.Sort([](const FHitResult& A, const FHitResult& B)
	{
		return A.Distance < B.Distance;
	});
	for (const FHitResult& Hit : Hits)
	{
		FHitResult Candidate = Hit;
		UPrimitiveComponent* OtherComponent = Hit.GetComponent();
		AActor* Other = Hit.GetActor();
		if (!OtherComponent || !IsValid(Other))
		{
			continue;
		}
		const ECollisionChannel OtherType = OtherComponent->GetCollisionObjectType();
		const bool bMutuallyBlocking =
			Collision->GetCollisionResponseToChannel(OtherType) == ECR_Block
			&& OtherComponent->GetCollisionResponseToChannel(
				Collision->GetCollisionObjectType()) == ECR_Block;
		// Seal.uc SetCollision(true,true,true): pawns are blocking actors even
		// though UE5's generic WorldDynamic response matrix is touch-oriented.
		if (OtherType != ECC_Pawn
			&& !(WoTRainCydIsVisBlockingActor(Other) || bMutuallyBlocking))
		{
			continue;
		}
		if (OtherType == ECC_Pawn)
		{
			const FVector Motion = End - Start;
			if (WoTUE1Collision::CylindersOverlap(
				Start, SealRadius, SealHalfHeight, Other))
			{
				const FVector Outward = Start - Other->GetActorLocation();
				// UE1 MoveActor permits an initially-overlapping cylinder to move
				// tangentially or out; only motion deeper into it blocks.
				if (FVector2D(Motion.X, Motion.Y).Dot(
					FVector2D(Outward.X, Outward.Y)) >= 0.0f)
				{
					continue;
				}
			}
			else
			{
				// Chaos reports entry into the rounded capsule before the true UE1 flat
				// cylinders meet. Solve their swept overlap and use the broad hit only
				// to enumerate this pawn candidate.
				float OtherRadius = 0.0f, OtherHalfHeight = 0.0f;
				Other->GetSimpleCollisionCylinder(OtherRadius, OtherHalfHeight);
				const FVector Relative = Start - Other->GetActorLocation();
				const float CombinedRadius = SealRadius + FMath::Max(0.0f, OtherRadius);
				const float CombinedHeight = SealHalfHeight + FMath::Max(0.0f, OtherHalfHeight);
				float HorizontalEnter = 0.0f, HorizontalExit = 1.0f;
				const FVector2D RelXY(Relative.X, Relative.Y);
				const FVector2D MoveXY(Motion.X, Motion.Y);
				const float A = MoveXY.SizeSquared();
				if (A <= UE_SMALL_NUMBER)
				{
					if (RelXY.SizeSquared() > FMath::Square(CombinedRadius)) continue;
				}
				else
				{
					const float B = 2.0f * FVector2D::DotProduct(RelXY, MoveXY);
					const float C = RelXY.SizeSquared() - FMath::Square(CombinedRadius);
					const float Discriminant = B * B - 4.0f * A * C;
					if (Discriminant < 0.0f) continue;
					const float Root = FMath::Sqrt(Discriminant);
					HorizontalEnter = FMath::Max(0.0f, (-B - Root) / (2.0f * A));
					HorizontalExit = FMath::Min(1.0f, (-B + Root) / (2.0f * A));
				}
				float VerticalEnter = 0.0f, VerticalExit = 1.0f;
				if (FMath::Abs(Motion.Z) <= UE_SMALL_NUMBER)
				{
					if (FMath::Abs(Relative.Z) > CombinedHeight) continue;
				}
				else
				{
					const float Z0 = (-CombinedHeight - Relative.Z) / Motion.Z;
					const float Z1 = ( CombinedHeight - Relative.Z) / Motion.Z;
					VerticalEnter = FMath::Max(0.0f, FMath::Min(Z0, Z1));
					VerticalExit = FMath::Min(1.0f, FMath::Max(Z0, Z1));
				}
				const float Enter = FMath::Max(HorizontalEnter, VerticalEnter);
				const float Exit = FMath::Min(HorizontalExit, VerticalExit);
				if (Enter > Exit || Enter > 1.0f || Exit < 0.0f) continue;
				Candidate.Time = Enter;
				Candidate.Distance = Motion.Size() * Enter;
				Candidate.Location = Candidate.ImpactPoint = Start + Motion * Enter;
				Candidate.ImpactNormal = VerticalEnter >= HorizontalEnter
					? FVector(0, 0, FMath::Sign(Relative.Z + Motion.Z * Enter))
					: FVector(Relative.X + Motion.X * Enter,
						Relative.Y + Motion.Y * Enter, 0).GetSafeNormal();
			}
		}
		OutHit = Candidate;
		return true;
	}
	return false;
}

bool AWoTSeal::TryFindDropSpot(FVector& InOutLocation) const
{
	UWorld* World = GetWorld();
	if (!World) return false;
	const FVector Extent(17.0f, 17.0f, 3.0f);
	auto Fits = [&](const FVector& Location)
	{
		TArray<FOverlapResult> Overlaps;
		FCollisionObjectQueryParams Objects;
		Objects.AddObjectTypesToQuery(ECC_WorldStatic);
		Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WoTSealDropFit), false, this);
		World->OverlapMultiByObjectType(Overlaps, Location, FQuat::Identity,
			Objects, FCollisionShape::MakeBox(Extent), Params);
		for (const FOverlapResult& Overlap : Overlaps)
		{
			AActor* Actor = Overlap.GetActor();
			if (WoTRainCydIsVisBlockingActor(Actor) && !Actor->IsA<AWoTMover>())
				return false;
		}
		return true;
	};
	auto AdjustSpot = [&](FVector& Adjusted, const FVector& TraceDest,
		float TraceLength)
	{
		FHitResult Hit;
		if (WoTVisBlockingTrace(World, Adjusted, TraceDest, nullptr, Hit))
			Adjusted += Hit.ImpactNormal * (1.05f - Hit.Time) * TraceLength;
	};

	FVector Adjusted = InOutLocation;
	for (int32 Sign = -1; Sign <= 1; Sign += 2)
	{
		AdjustSpot(Adjusted, Adjusted + FVector(Sign * Extent.X, 0, 0), Extent.X);
		AdjustSpot(Adjusted, Adjusted + FVector(0, Sign * Extent.Y, 0), Extent.Y);
		AdjustSpot(Adjusted, Adjusted + FVector(0, 0, Sign * Extent.Z), Extent.Z);
	}
	if (Fits(Adjusted))
	{
		InOutLocation = Adjusted;
		return true;
	}

	const float TraceLength = Extent.Size() + 2.0f;
	for (int32 X = -1; X <= 1; X += 2)
		for (int32 Y = -1; Y <= 1; Y += 2)
			for (int32 Z = -1; Z <= 1; Z += 2)
				AdjustSpot(Adjusted,
					Adjusted + FVector(X * Extent.X, Y * Extent.Y, Z * Extent.Z),
					TraceLength);
	if (FVector::DistSquared(Adjusted, InOutLocation) > 1.5f * Extent.SizeSquared()
		|| !Fits(Adjusted))
		return false;
	InOutLocation = Adjusted;
	return true;
}

void AWoTSeal::BecomePickup()
{
	if (bRestingOnAltar || bGroundPlaced || bPreviewSeal)
	{
		// Banked/ground/preview presentation: visible, frozen, and NO touch cylinder — retail
		// never re-enabled collision through placement (SealAltar.uc:118-124;
		// the altar is the hand-back surface). The base Pickup presentation
		// enables collision, and UE5 updates overlaps synchronously on the
		// enable — a pawn standing at the seal's CURRENT location (the placer
		// on the altar, or a bystander at the stale pre-move spot) would
		// re-grant the seal mid-placement (retro-review). Runs on proxies too
		// via the replicated flag.
		SetActorHiddenInGame(false);
		SetActorEnableCollision(false);
		SetActorTickEnabled(false);
		UpdateIndicator();
		return;
	}
	Super::BecomePickup();
	// Seal.uc:403-408: a grounded seal never carries the indicator.
	UpdateIndicator();
}

void AWoTSeal::BecomeItem()
{
	Super::BecomeItem();
	// Per-machine carry watch (the retail replicated-sprayer equivalent): a
	// proxy reaching Held with a pawn owner grows the indicator.
	UpdateIndicator();
}

void AWoTSeal::Destroyed()
{
	// Seal.Destroyed order (Seal.uc:327-334): indicator, credit, Super, fan-out.
	ClearIndicator();
	ClearControllingTeam();
	Super::Destroyed();
	NotifyPawnsSealEvent(DestroyedEvent, nullptr);
}

void AWoTSeal::SendTakeNotification(uint8 NotifyTeam)
{
	// Seal.uc:410-422: every player on the losing team hears the possession
	// change and reads the center message.
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator();
	     It; ++It)
	{
		AWoTPlayerController* PC = Cast<AWoTPlayerController>(It->Get());
		AWoTPlayerState* PS = PC ? PC->GetPlayerState<AWoTPlayerState>() : nullptr;
		if (PS && PS->Team == NotifyTeam)
		{
			PC->ClientPlayUISound(FName(TEXT("WOT/PossesionChangeSound")), 1.0f);
			// CenterMessage(...,,true): the bEcho console copy (Seal.uc:419).
			PC->ClientCenterMessage(PossesionChangeStr, /*bEcho=*/true);
		}
	}
}

void AWoTSeal::NotifyPawnsSealEvent(FName SealEventName, APawn* EventInstigator)
{
	// Util.TriggerAllInstances(Self, class'Pawn', Instigator, EventName): retail
	// calls Trigger on EVERY pawn with the seal's Event swapped to EventName. The
	// only live consumer is Captain.Trigger's IsA('Seal') branch (Captain.uc:108
	// — GuardSeal captains re-plan); Grunt.Trigger handles Alarm/WotWeapon only
	// and WOTPlayer ignores seal triggers. EventName/Instigator are carried for
	// the call-shape parity (the captain gate reads neither).
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		if (AWoTNPCController* NPC = Cast<AWoTNPCController>(It->GetController()))
		{
			NPC->NotifySealTriggered();
		}
	}
}

void AWoTSeal::RestOnAltar(const FVector& Spot, const FRotator& Rot, AActor* Altar)
{
	bHeldItem = false;   // added in retail to fix the 45s disappearing seal
	RespawnTime = 0.0f;  // don't respawn
	bDropped = true;     // the Dropped label: no pickup spin
	// Set BEFORE the state change: the BecomePickup override reads it to keep
	// the touch cylinder off through the whole placement (the retro-review's
	// re-entrancy — the base presentation's collision enable fires overlap
	// updates synchronously at whatever location the seal currently has).
	bRestingOnAltar = true;
	bGroundPlaced = false;
	GotoItemState(EWoTInventoryState::Pickup);
	SetActorLocation(Spot);
	SetActorRotation(Rot);
	SetReplicateMovement(true);
	SetOwner(Altar);
}

void AWoTSeal::ClearIndicator()
{
	// Seal.uc:369-373/392-401: the indicator fades out (bOn off, 1s linger).
	if (Indicator)
	{
		WoTWeaveFX::FadeOut(Indicator, 1.0f);
		Indicator = nullptr;
	}
}

void AWoTSeal::OnRep_CarryColorName()
{
	UpdateIndicator();
}

void AWoTSeal::UpdateIndicator()
{
	APawn* Carrier = Cast<APawn>(GetOwner());
	const bool bCarried = GetItemState() == EWoTInventoryState::Held
		&& Carrier != nullptr && !bRestingOnAltar && !bGroundPlaced;
	if (!bCarried)
	{
		ClearIndicator();
		return;
	}
	// The authority-resolved color (replicated; the retail default case for
	// non-player carriers and unknown colors is GREEN).
	const FName Color = CarryColorName.IsNone()
		? FName(TEXT("Green")) : CarryColorName;
	const FName DesiredRow = FName(
		*FString::Printf(TEXT("CarrySealSprayer%s"), *Color.ToString()));
	// Reconcile: a coalesced carrier change or a late color rep must rebuild
	// (retail respawned the sprayer on every GiveTo — retro-review).
	if (Indicator
		&& (Indicator->ConfigName != DesiredRow
			|| Indicator->FollowActor.Get() != Carrier))
	{
		ClearIndicator();
	}
	if (Indicator)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// bGrouped=True: live particles SHIFT with the emitter (the original
	// sprayer's bShiftWithFollow), so the fountain stays attached rather than trailing.
	Indicator = WoTWeaveFX::SpawnLocal(*World, DesiredRow,
		Carrier->GetActorLocation() + FVector(0, 0, 75),
		Carrier->GetActorRotation(), Carrier, /*bFollowRotation=*/true,
		/*bShiftWithFollow=*/true);
	if (Indicator)
	{
		// PHYS_Trailer PrePivot (Z=75): the fountain rides over the carrier.
		Indicator->FollowOffset = FVector(0, 0, 75);
	}
}

void AWoTSeal::OnRep_Owner()
{
	Super::OnRep_Owner();
	// Owner vs ItemState replication order is arbitrary — re-derive the carry
	// indicator whenever the owner arrives on a proxy.
	UpdateIndicator();
}

bool AWoTSeal::GetClosestPathNodeLocation(FVector& NodeLocation) const
{
	// Seal.uc:152-182: walk the level's NavigationPointList (the projected world
	// stamps every retail NavigationPoint as an AWoTPathNode twin, the same list
	// GetTouchingPathNode uses), keep the closest by 3D distance from MinDist
	// 999999 with a strict '<', and rest the seal's cylinder on that node's base:
	// Location.Z - Node.CollisionHeight + the seal's CollisionHeight (3).
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const FVector BaseLocation = NodeLocation;
	bool bFound = false;
	float MinDist = 999999.0f;
	for (TActorIterator<AWoTPathNode> It(World); It; ++It)
	{
		const AWoTPathNode* Node = *It;
		if (!IsValid(Node))
		{
			continue;
		}
		const float Dist = FVector::Dist(BaseLocation, Node->GetActorLocation());
		if (Dist < MinDist)
		{
			bFound = true;
			MinDist = Dist;
			NodeLocation = Node->GetActorLocation();
			NodeLocation.Z -= Node->NodeCollisionHeight;
			NodeLocation.Z += 3.0f;
		}
	}
	return bFound;
}
AWoTSealInventory::AWoTSealInventory()
{
	// SealInventory.uc:15-24 defaultproperties.
	ResourceClass = AWoTSeal::StaticClass();
	Count = 1;
	SetActorHiddenInGame(true); // SealInventory.uc:23 bHidden=True.
	// SealInventory.uc:17,20: StatusIconFrame=Texture'WOT.UI.M_SSeal' /
	// StatusIcon=Texture'WOT.UI.I_SSeal' (the same pair the carried AWoTSeal binds).
	BindStatusIcons(TEXT("WOT"), TEXT("I_SSeal"), TEXT("M_SSeal"));
}
