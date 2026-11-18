#include "WoTCollisionBlocker.h"

#include "AI/WoTNPCCharacter.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/ConvexElem.h"

UWoTFlatCylinderComponent::UWoTFlatCylinderComponent()
{
	SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	SetGenerateOverlapEvents(false);
}

void UWoTFlatCylinderComponent::SetCylinderSize(
	float InRadius, float InHalfHeight)
{
	CylinderRadius = FMath::Max(0.1f, InRadius);
	CylinderHalfHeight = FMath::Max(0.1f, InHalfHeight);
	CylinderBodySetup = nullptr;
	UpdateBounds();
	MarkRenderTransformDirty();
	if (IsRegistered())
	{
		RecreatePhysicsState();
	}
}

void UWoTFlatCylinderComponent::SetAnalyticCursorTrace(bool bEnabled)
{
	if (bAnalyticCursorTrace == bEnabled) return;
	bAnalyticCursorTrace = bEnabled;
	CylinderBodySetup = nullptr;
	if (IsRegistered()) RecreatePhysicsState();
}

bool UWoTFlatCylinderComponent::ExactLineTrace(const FVector& Start,
	const FVector& End, FHitResult& OutHit) const
{
	const FTransform& Xf = GetComponentTransform();
	const FVector A = Xf.InverseTransformPosition(Start);
	const FVector B = Xf.InverseTransformPosition(End);
	const FVector D = B - A;
	float Best = TNumericLimits<float>::Max();
	FVector BestNormal = FVector::ZeroVector;
	const float R2 = FMath::Square(CylinderRadius);
	auto Accept = [&](float T, const FVector& N)
	{
		if (T >= 0.0f && T <= 1.0f && T < Best)
		{
			const FVector P = A + T * D;
			if (FMath::Abs(P.Z) <= CylinderHalfHeight + KINDA_SMALL_NUMBER
				&& P.X * P.X + P.Y * P.Y <= R2 + KINDA_SMALL_NUMBER)
			{
				Best = T; BestNormal = N;
			}
		}
	};
	const float AA = D.X * D.X + D.Y * D.Y;
	if (AA > SMALL_NUMBER)
	{
		const float BB = 2.0f * (A.X * D.X + A.Y * D.Y);
		const float CC = A.X * A.X + A.Y * A.Y - R2;
		const float Disc = BB * BB - 4.0f * AA * CC;
		if (Disc >= 0.0f)
		{
			const float Root = FMath::Sqrt(Disc);
			for (float T : {(-BB - Root) / (2.0f * AA),
				(-BB + Root) / (2.0f * AA)})
			{
				const FVector P = A + T * D;
				Accept(T, FVector(P.X, P.Y, 0.0f).GetSafeNormal());
			}
		}
	}
	if (FMath::Abs(D.Z) > SMALL_NUMBER)
	{
		Accept((-CylinderHalfHeight - A.Z) / D.Z, FVector(0, 0, -1));
		Accept(( CylinderHalfHeight - A.Z) / D.Z, FVector(0, 0,  1));
	}
	if (Best == TNumericLimits<float>::Max()) return false;
	OutHit = FHitResult(this->GetOwner(), const_cast<UWoTFlatCylinderComponent*>(this),
		FMath::Lerp(Start, End, Best),
		Xf.TransformVectorNoScale(BestNormal).GetSafeNormal());
	OutHit.TraceStart = Start;
	OutHit.TraceEnd = End;
	OutHit.Time = Best;
	OutHit.Distance = FVector::Distance(Start, OutHit.ImpactPoint);
	OutHit.bBlockingHit = true;
	return true;
}

UBodySetup* UWoTFlatCylinderComponent::GetBodySetup()
{
	if (CylinderBodySetup)
	{
		return CylinderBodySetup;
	}
	CylinderBodySetup = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
	if (!CylinderBodySetup)
	{
		return nullptr;
	}
	CylinderBodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	CylinderBodySetup->bNeverNeedsCookedCollisionData = false;
	FKConvexElem Convex;
	constexpr int32 SideCount = 16;
	Convex.VertexData.Reserve(SideCount * 2);
	for (int32 Side = 0; Side < SideCount; ++Side)
	{
		const float Angle = 2.0f * UE_PI * static_cast<float>(Side)
			/ static_cast<float>(SideCount);
		// Cursor shapes are conservative broad phases.  Their real hit boundary
		// is ExactLineTrace; circumscribing prevents diagonal false negatives.
		const float BroadRadius = bAnalyticCursorTrace
			? CylinderRadius / FMath::Cos(UE_PI / SideCount) : CylinderRadius;
		const float X = BroadRadius * FMath::Cos(Angle);
		const float Y = BroadRadius * FMath::Sin(Angle);
		Convex.VertexData.Emplace(X, Y, -CylinderHalfHeight);
		Convex.VertexData.Emplace(X, Y, CylinderHalfHeight);
	}
	Convex.UpdateElemBox();
	CylinderBodySetup->AggGeom.ConvexElems.Add(MoveTemp(Convex));
	CylinderBodySetup->InvalidatePhysicsData();
	CylinderBodySetup->CreatePhysicsMeshes();
	return CylinderBodySetup;
}

FBoxSphereBounds UWoTFlatCylinderComponent::CalcBounds(
	const FTransform& LocalToWorld) const
{
	const FVector Extent(CylinderRadius, CylinderRadius, CylinderHalfHeight);
	return FBoxSphereBounds(FBox(-Extent, Extent)).TransformBy(LocalToWorld);
}

bool UWoTFlatCylinderComponent::IsZeroExtent() const
{
	return CylinderRadius <= UE_KINDA_SMALL_NUMBER
		|| CylinderHalfHeight <= UE_KINDA_SMALL_NUMBER;
}

AWoTCollisionBlocker::AWoTCollisionBlocker()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	Capsule = CreateDefaultSubobject<UWoTFlatCylinderComponent>(TEXT("Capsule"));
	RootComponent = Capsule;
	Capsule->SetCylinderSize(40.0f, 40.0f);
	Capsule->SetGenerateOverlapEvents(false);
}

void AWoTCollisionBlocker::Configure(bool bInPlayerOnly, float Radius,
	float HalfHeight)
{
	Configure(bInPlayerOnly ? EWoTCollisionBlockerMode::Players
		: EWoTCollisionBlockerMode::All, Radius, HalfHeight);
}

void AWoTCollisionBlocker::Configure(EWoTCollisionBlockerMode InMode,
	float Radius, float HalfHeight)
{
	Mode = InMode;
	bPlayerOnly = Mode == EWoTCollisionBlockerMode::Players;
	bMonstersOnly = Mode == EWoTCollisionBlockerMode::Monsters;
	Capsule->SetCylinderSize(Radius, HalfHeight);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Capsule->SetGenerateOverlapEvents(Mode == EWoTCollisionBlockerMode::Actor);
	if (Mode == EWoTCollisionBlockerMode::Actor)
	{
		Capsule->SetCollisionObjectType(ECC_WorldDynamic);
		Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	}
	else if (Mode != EWoTCollisionBlockerMode::All)
	{
		Capsule->SetCollisionObjectType(ECC_WorldStatic);
		Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	}
	else
	{
		Capsule->SetCollisionObjectType(ECC_WorldStatic);
		Capsule->SetCollisionProfileName(TEXT("BlockAll"));
	}
	if ((Mode == EWoTCollisionBlockerMode::Players
		|| Mode == EWoTCollisionBlockerMode::Monsters) && HasActorBegunPlay())
	{
		InstallClassIgnores();
	}
}

void AWoTCollisionBlocker::BeginPlay()
{
	Super::BeginPlay();
	if (Mode == EWoTCollisionBlockerMode::Players
		|| Mode == EWoTCollisionBlockerMode::Monsters) InstallClassIgnores();
}

void AWoTCollisionBlocker::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ActorSpawnedHandle.IsValid() && GetWorld())
	{
		GetWorld()->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
		ActorSpawnedHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void AWoTCollisionBlocker::InstallClassIgnores()
{
	UWorld* World = GetWorld();
	if (!World) return;
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		ApplyClassIgnore(*It);
	}
	if (!ActorSpawnedHandle.IsValid())
	{
		ActorSpawnedHandle = World->AddOnActorSpawnedHandler(
			FOnActorSpawned::FDelegate::CreateUObject(
				this, &AWoTCollisionBlocker::ApplyClassIgnore));
	}
}

void AWoTCollisionBlocker::ApplyClassIgnore(AActor* Actor)
{
	APawn* Pawn = Cast<APawn>(Actor);
	if (!Pawn || !Capsule || (Mode != EWoTCollisionBlockerMode::Players
		&& Mode != EWoTCollisionBlockerMode::Monsters)) return;
	const bool bIsMonster = Pawn->IsA<AWoTNPCCharacter>();
	const bool bIgnore = Mode == EWoTCollisionBlockerMode::Players
		? bIsMonster : !bIsMonster;
	if (!bIgnore) return;
	Capsule->IgnoreActorWhenMoving(Pawn, true);
	if (UCapsuleComponent* PawnCapsule = Pawn->FindComponentByClass<UCapsuleComponent>())
	{
		PawnCapsule->IgnoreActorWhenMoving(this, true);
	}
}
