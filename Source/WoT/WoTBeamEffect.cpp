#include "WoTBeamEffect.h"
#include "Assets/WoTRetailIdentity.h"
#include "Assets/WoTRuntimeAssets.h"

#include "WoTCarcass.h"
#include "WoTMeshLight.h"
#include "WoTMeshFX.h"
#include "WoTParticleSprayer.h"
#include "WoTSpriteBillboard.h"
#include "WoTWeaveFX.h"
#include "WoTVertexAnimComponent.h"
#include "AI/WoTPathNode.h"
#include "AI/WoTPathNodeIterator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTBeamEffect, Log, All);

AWoTBeamEffect::AWoTBeamEffect()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(false);
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	NetPriority = 6.0f;
	// LightningStreak02.uc: the only vertex-animated AWoTBeamEffect config.
	AmbientGlow = 250.0f;
	bUnlit = true;
	const uint64 Entropy = FPlatformTime::Cycles64()
		^ static_cast<uint64>(reinterpret_cast<UPTRINT>(this));
	CosmeticRandom.Initialize(static_cast<int32>(Entropy ^ (Entropy >> 32)));
}

void AWoTBeamEffect::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWoTBeamEffect, Config);
	DOREPLIFETIME(AWoTBeamEffect, Start);
	DOREPLIFETIME(AWoTBeamEffect, End);
	DOREPLIFETIME(AWoTBeamEffect, StartActor);
	DOREPLIFETIME(AWoTBeamEffect, EndActor);
}

void AWoTBeamEffect::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer)
	{
		SetActorTickEnabled(false);   // segments are local cosmetics
	}
}

void AWoTBeamEffect::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// Config resolves lazily for call sites that fill it after spawning; Chain
	// Lightning uses deferred spawn so clients receive complete initial state.
	if (Config.IsNone())
	{
		return;
	}
	if (!bConfigured)
	{
		bConfigured = true;
		if (Config == FName(TEXT("BalefireLine")))
		{
			// BalefireLine.uc: Sparks01 every 18uu at DrawScale 2, 1.2s glow fade.
			bStreak = false;
			SegmentLength = 18.0f;
			SpriteScale = 2.0f;
			FadeTime = 1.2f;
			Remaining = FadeTime;
		}
		else   // LightningStreak02 (the only other shipped config)
		{
			bStreak = true;
			SegmentLength = 64.0f;
		}
	}
	FVector A = Start;
	if (StartActor)
	{
		A = StartActor->GetActorLocation();
		if (!bStreak)
		{
			if (const APawn* P = Cast<APawn>(StartActor.Get()))
			{
				A.Z += P->BaseEyeHeight;
			}
		}
	}
	const FVector ImpactLocation = EndActor
		? EndActor->GetActorLocation() : FVector(End);
	FVector B = ImpactLocation;
	if (const APawn* EndPawn = Cast<APawn>(EndActor.Get()))
	{
		float Radius = 0.0f;
		float HalfHeight = 0.0f;
		EndPawn->GetSimpleCollisionCylinder(Radius, HalfHeight);
		FVector TowardSource = A - B;
		TowardSource.Z = 0.0f;
		B += TowardSource.GetSafeNormal() * Radius;
	}
	// The server actor's relevancy origin follows the visible link rather than
	// remaining at the default world origin where it was originally spawned.
	SetActorLocation((A + B) * 0.5f);
	if (FadeTime > 0.0f)
	{
		Remaining = FMath::Max(0.0f, Remaining - DeltaSeconds);
	}
	if (bStreak)
	{
		if (EndActor && !IsValid(EndpointImpact))
		{
			EndpointImpact = WoTMeshFX::SpawnLocal(*GetWorld(), TEXT("LSImpact"),
				ImpactLocation, FRotator::ZeroRotator);
			EndpointSparks = WoTWeaveFX::SpawnLocal(*GetWorld(), TEXT("LightningSparks"),
				B, (A - B).Rotation());
		}
		ImpactRollUnits = FMath::Fmod(ImpactRollUnits + 25000.0f * DeltaSeconds, 65536.0f);
		if (IsValid(EndpointImpact))
		{
			EndpointImpact->SetActorLocation(ImpactLocation);
			FRotator ImpactRot = FVector::UpVector.Rotation();
			ImpactRot.Roll = ImpactRollUnits * (360.0f / 65536.0f);
			EndpointImpact->SetActorRotation(ImpactRot);
		}
		if (IsValid(EndpointSparks))
		{
			EndpointSparks->SetActorLocation(B);
			EndpointSparks->SetActorRotation((A - B).Rotation());
		}
		LayStreak(A, B);
	}
	else
	{
		LayLine(A, B);
	}
}

void AWoTBeamEffect::Destroyed()
{
	if (IsValid(EndpointImpact)) { EndpointImpact->Destroy(); }
	WoTWeaveFX::FadeOut(EndpointSparks, 2.0f);
	Super::Destroyed();
}

namespace
{
	UMaterialInterface* BeamMaterial(const TCHAR* Name)
	{
		return UWoTRuntimeAssets::ResolveMaterialPath(FSoftObjectPath(FString::Printf(
			TEXT("/Game/WoT/Materials/Instances/%s.%s"), Name, Name)));
	}
}

void AWoTBeamEffect::LayLine(const FVector& A, const FVector& B)
{
	// The LineRI walk: a camera-facing sprite every SegmentLength until the
	// remaining run is shorter than one segment.
	if (!ModQuads || !AddQuads)
	{
		UStaticMesh* Plane = Cast<UStaticMesh>(FSoftObjectPath(
			TEXT("/Engine/BasicShapes/Plane.Plane")).TryLoad());
		if (!Plane)
		{
			VisibleSegmentCount = 0;
			return;
		}
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			if ((Pass && AddQuads) || (!Pass && ModQuads))
			{
				continue;
			}
			UInstancedStaticMeshComponent* C = NewObject<UInstancedStaticMeshComponent>(
				this, Pass ? TEXT("AddQuads") : TEXT("ModQuads"));
			if (!C)
			{
				continue;
			}
			C->SetupAttachment(RootComponent);
			C->RegisterComponent();
			C->SetStaticMesh(Plane);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->NumCustomDataFloats = 1;
			C->SetUsingAbsoluteLocation(true);
			C->SetUsingAbsoluteRotation(true);
			C->SetWorldLocationAndRotation(FVector::ZeroVector, FQuat::Identity);
			C->SetTranslucentSortPriority(Pass ? 91 : 90);
			C->SetMaterial(0, BeamMaterial(Pass
				? TEXT("MI_PSAdd_ParticleSystems__Sparks01")
				: TEXT("MI_PSMod_ParticleSystems__Sparks01")));
			(Pass ? AddQuads : ModQuads) = C;
		}
	}
	if (!ModQuads || !AddQuads)
	{
		VisibleSegmentCount = 0;
		return;
	}
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	const FQuat Face = Cam
		? WoTSpriteFacing::ScreenAlignedQuat(Cam->GetCameraRotation())
		: FQuat::Identity;
	const float Glow = FadeTime > 0.0f ? FMath::Clamp(Remaining / FadeTime, 0.0f, 1.0f)
	                                   : 1.0f;
	const FVector Step = (B - A).GetSafeNormal() * SegmentLength;
	// world size = DrawScale x USize/VSize (Sparks01 is 32x32; plane is 100uu)
	const FVector Scale(SpriteScale * 32.0f / 100.0f, SpriteScale * 32.0f / 100.0f, 1.0f);
	const float Distance = FVector::Distance(A, B);
	const int32 Needed = Distance > KINDA_SMALL_NUMBER
		? FMath::CeilToInt(Distance / SegmentLength) : 0;
	int32 Used = 0;
	for (; Used < Needed; ++Used)
	{
		const FVector Loc = A + Step * static_cast<float>(Used);
		for (UInstancedStaticMeshComponent* C : {ModQuads.Get(), AddQuads.Get()})
		{
			while (C->GetInstanceCount() <= Used)
			{
				C->AddInstance(FTransform(FQuat::Identity, A, FVector(0.0001f)), true);
			}
			C->UpdateInstanceTransform(Used, FTransform(Face, Loc, Scale), true);
			C->SetCustomDataValue(Used, 0, Glow);
		}
	}
	VisibleSegmentCount = Used;
	for (UInstancedStaticMeshComponent* C : {ModQuads.Get(), AddQuads.Get()})
	{
		for (int32 i = Used; i < C->GetInstanceCount(); ++i)
		{
			C->UpdateInstanceTransform(i, FTransform(FQuat::Identity, A, FVector(0.0001f)),
			                           true);
		}
		C->MarkRenderStateDirty();
	}
}

void AWoTBeamEffect::LayStreak(const FVector& A, const FVector& B)
{
	// The original UStreakRI::CreateStreak/ConnectPoints, rewritten
	// around the stamped retail graph. Each run returns the first point one
	// interval beyond its final placed origin; the next run continues there.
	LastSegmentOrigins.Reset();
	if (A == B)
	{
		VisibleSegmentCount = 0;
		for (UWoTVertexAnimComponent* Segment : Segments)
		{
			if (IsValid(Segment))
			{
				Segment->SetVisibility(false);
			}
		}
		return;
	}

	PlannedRotations.Reset();
	int32 TotalNeeded = 0;
	auto ConnectPoints = [&](FVector P1, const FVector& P2)
	{
		const FVector Delta = P2 - P1;
		const FVector Interval = Delta.GetSafeNormal() * SegmentLength;
		const FRotator Rotation = Delta.Rotation();
		while (FVector::Dist(P2, P1) > SegmentLength)
		{
			if (LastSegmentOrigins.Num() < MaxStreakSegments)
			{
				LastSegmentOrigins.Add(P1);
				PlannedRotations.Add(Rotation);
			}
			++TotalNeeded;
			P1 += Interval;
		}
		return P1;
	};

	// The original UStreakRI uses TRACE_VisBlocking. The source/owner capsule
	// containing A is not geometry and must not force BuildPath every tick.
	const bool bDirect = !WoTVisBlockingTraceHits(GetWorld(), A, B, this);
	bool bBentPath = false;
	if (bDirect)
	{
		ConnectPoints(A, B);
	}
	else
	{
		if (StreakIterator.BuildPath(GetWorld(), A, B, false))
		{
			bBentPath = true;
			FVector ReachedPoint = A;
			AWoTPathNode* PreviousNode = nullptr;
			for (AWoTPathNode* Node = StreakIterator.GetFirst(); Node;
				Node = StreakIterator.GetNext())
			{
				if (PreviousNode && PreviousNode->IsTeleporter()
					&& Node->IsTeleporter())
				{
					ReachedPoint = Node->GetActorLocation();
				}
				else
				{
					ReachedPoint = ConnectPoints(
						ReachedPoint, Node->GetActorLocation());
				}
				PreviousNode = Node;
			}
			ConnectPoints(ReachedPoint, B);
		}
		else
		{
			// Retail's empty iterator naturally fell through to the closing run.
			ConnectPoints(A, B);
		}
	}

	if (TotalNeeded > MaxStreakSegments && !bLoggedStreakOverflow)
	{
		bLoggedStreakOverflow = true;
		UE_LOG(LogWoTBeamEffect, Warning,
			TEXT("%s %s streak needs %d segments; safety cap is %d%s"),
			*GetName(), *Config.ToString(), TotalNeeded, MaxStreakSegments,
			bBentPath ? TEXT(" (bent path)") : TEXT(""));
	}

	// The pooled LSbolt visual follows the already-computed origins. Planning is
	// asset-independent so headless specs can inspect the exact continuation walk.

	const int32 Needed = LastSegmentOrigins.Num();
	int32 Used = 0;
	for (; Used < Needed; ++Used)
	{
		const TConstArrayView<FString> TextureIdentities =
			WoTMeshFX::LightningStreakTextures();
		if (TextureIdentities.IsEmpty()) break;
		const FString& TextureIdentity =
			TextureIdentities[CosmeticRandom.RandHelper(TextureIdentities.Num())];
		// "Pkg/Name", grouped only when the name collides inside its package
		// (WoTRetailTextureSubPath). The registry holds Angreal__LB_A01, so the
		// unconditionally grouped "Angreal/Skins/LB_A01" never resolved.
		FWoTRetailObjectPath RetailPath;
		if (!WoTParseRetailObjectPath(TextureIdentity, RetailPath)) break;
		const FSoftObjectPath TexturePath(FString::Printf(
			TEXT("/Game/WoT/Textures/%s.%s"),
			*WoTRetailTextureSubPath(RetailPath), *RetailPath.Name));
		if (!Segments.IsValidIndex(Used))
		{
			UWoTVertexAnimComponent* Seg = NewObject<UWoTVertexAnimComponent>(
				this, *FString::Printf(TEXT("Seg%d"), Used));
			if (!Seg)
			{
				break;
			}
			Seg->SetupAttachment(RootComponent);
			Seg->SetUsingAbsoluteLocation(true);
			Seg->SetUsingAbsoluteRotation(true);
			Seg->SetUsingAbsoluteScale(true);
			Seg->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Seg->SetCastShadow(false);
			Seg->SetOwnerNoSee(false);
			Seg->SetOnlyOwnerSee(false);
			if (!Seg->SetVertexMesh(TEXT("LSbolt"), TEXT("Angreal")))
			{
				Seg->DestroyComponent();
				break;
			}
			// The frame's texture is the actor Skin, not MultiSkins[0]: all six
			// LSbolt triangles use mesh texture 1, an empty mesh slot that UE1
			// draws with Owner->Skin (StreakRI randomizes it per frame). Mesh
			// texture 0 holds LB_A01 and no triangle uses it, so a skin set on
			// index 0 left the bolt on the master's neutral default.
			Seg->SetAllSkinOverrides(TexturePath);
			// Register only after the visual is complete so UWoTMeshLight's
			// BeginPlay bake sees and reparents the LSbolt material immediately.
			Seg->RegisterComponent();
			Segments.Add(Seg);
			if (UWoTMeshLight* MeshLight = FindComponentByClass<UWoTMeshLight>())
			{
				// Later pooled segments share the first segment's owner-level light
				// component, so explicitly include each newly registered mesh.
				MeshLight->ForceRuntimeBake();
			}
		}
		if (!Segments.IsValidIndex(Used) || !IsValid(Segments[Used]))
		{
			break;
		}
		if (Segments[Used]->IsRegistered())
		{
			Segments[Used]->SetAllSkinOverrides(TexturePath);
		}
		++StreakTextureAssignmentCount;
		Segments[Used]->SetWorldTransform(FTransform(
			PlannedRotations[Used], LastSegmentOrigins[Used], FVector::OneVector));
		Segments[Used]->SetVisibility(true);
	}
	VisibleSegmentCount = Used;
	for (int32 i = Used; i < Segments.Num(); ++i)
	{
		Segments[i]->SetVisibility(false);
	}
}
