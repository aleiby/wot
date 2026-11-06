#include "WoTSight.h"
#include "../WoTRandom.h"
#include "WoTPathNodeIterator.h"
#include "WoTBrainComponent.h"
#include "WoTNPCCharacter.h"
#include "WoTNPCController.h"
#include "../WoTCharacter.h"

#include "GameFramework/Pawn.h"

namespace
{
#if WITH_AUTOMATION_TESTS
	TFunction<bool(const FVector&, const FVector&)> GTraceOverride;
	TFunction<float()> GRandomOverride;
#endif

	bool ClearRay(const APawn* Observer, const FVector& Start, const FVector& End)
	{
#if WITH_AUTOMATION_TESTS
		if (GTraceOverride) return GTraceOverride(Start, End);
#endif
		return !WoTVisBlockingTraceHits(Observer ? Observer->GetWorld() : nullptr,
			Start, End, Observer);
	}
	float RetailRandom()
	{
#if WITH_AUTOMATION_TESTS
		if (GRandomOverride) return GRandomOverride();
#endif
		return WoTRandom::FRand();
	}
	bool ViewerIsPlayer(const APawn* Observer)
	{
		const AWoTCharacter* Character = Cast<AWoTCharacter>(Observer);
		return !Character || !Character->IsNPCPawn();
	}
	float ViewerSkill(const APawn* Observer)
	{
		const AWoTCharacter* Character = Cast<AWoTCharacter>(Observer);
		return Character ? Character->Skill : 0.0f;
	}
	const AActor* CurrentEnemy(const APawn* Observer)
	{
		const AWoTNPCController* Controller = Observer
			? Cast<AWoTNPCController>(Observer->GetController()) : nullptr;
		const UWoTBrainComponent* Brain = Controller ? Controller->GetBrain() : nullptr;
		if (Brain && Brain->IsGoalValid(EWoTGoalIndex::Threat))
			return Brain->GetGoal(EWoTGoalIndex::Threat).GoalActor.Get();
		return nullptr;
	}
}

namespace WoTSight
{
uint8 VisibilityOf(const AActor* Target)
{
	if (const AWoTCharacter* Character = Cast<AWoTCharacter>(Target))
		return Character->Visibility;
	return 128;
}

bool LineOfSightTo(const APawn* Observer, const AActor* Target,
	const FWoTLineOfSightParams& InParams)
{
	if (!Observer || !Target) return false;
	FWoTLineOfSightParams Params = InParams;
	AWoTCharacter* SightPawn = Cast<AWoTCharacter>(const_cast<APawn*>(Observer));
	// Every production WoT observer is an AWoTCharacter.  Keep harmless call-local
	// scratch for foreign APawn callers rather than restoring global lifetime.
	bool LocalLOSFlag = false;
	float LocalStimulus = 0.0f;
	FVector LocalLastSeeingPos = FVector::ZeroVector;
	FVector LocalLastSeenPos = FVector::ZeroVector;
	bool& LOSFlag = SightPawn ? SightPawn->bSightLOSFlag : LocalLOSFlag;
	float& Stimulus = SightPawn ? SightPawn->SightStimulus : LocalStimulus;
	FVector& LastSeeingPos = SightPawn
		? SightPawn->SightLastSeeingPos : LocalLastSeeingPos;
	FVector& LastSeenPos = SightPawn ? SightPawn->SightLastSeenPos : LocalLastSeenPos;
	bool bShowSelf = Params.bShowSelf;
	if (Target == Params.Enemy) bShowSelf = false;
	if (bShowSelf) LOSFlag = !LOSFlag;

	const FVector Delta = Target->GetActorLocation() - Observer->GetActorLocation();
	float DistSq = Delta.SizeSquared();
	float MaxDistSq = 0.0f;
	const bool bTargetPawn = Target->IsA<APawn>();
	if (bShowSelf)
	{
		if (bTargetPawn)
		{
			const float MaxDistance = Params.SightRadius
				* FMath::Min(1.0f, Params.TargetVisibility * 0.0078125f);
			MaxDistSq = FMath::Min(Params.bViewerIsPlayer
				? 16000000.0f : 12000000.0f, FMath::Square(MaxDistance));
		}
		else MaxDistSq = FMath::Square(Params.SightRadius);
		if (DistSq > MaxDistSq) return false;
		Stimulus = FVector::DotProduct(Delta.GetSafeNormal(),
			Observer->GetActorForwardVector()) - Params.PeripheralVision;
		Stimulus = Stimulus > 0.0f
			? 0.8f * Stimulus + 0.2f
			: 0.17f * Stimulus + 0.2f;
		if (Stimulus <= 0.0f) return false;
		const float HeightMod = FMath::Abs(Delta.Z)
			/ FMath::Max(1.0f, Params.Skill + 1.0f);
		DistSq = (DistSq + FMath::Square(HeightMod))
			/ FMath::Square(Stimulus);
		if (DistSq > MaxDistSq) return false;
		Stimulus = 1.0f;
	}
	else
	{
		if (Params.bViewerIsPlayer)
		{
			if (bTargetPawn)
			{
				const float MaxDistance = 5000.0f * FMath::Min(1.0f,
					(Params.TargetVisibility + 16.0f) * 0.015f);
				MaxDistSq = FMath::Min(25000000.0f, FMath::Square(MaxDistance));
			}
			else MaxDistSq = 16000000.0f;
		}
		else if (bTargetPawn)
		{
			const float MaxDistance = 4000.0f * FMath::Min(1.0f,
				(Params.TargetVisibility + 16.0f) * 0.015f);
			MaxDistSq = FMath::Min(16000000.0f, FMath::Square(MaxDistance));
		}
		else MaxDistSq = 9000000.0f;
		if (DistSq > MaxDistSq) return false;
	}

	const FVector Center = Target->GetActorLocation();
	const FVector ViewPoint = Observer->GetActorLocation()
		+ FVector(0, 0, Observer->BaseEyeHeight);
	if (Target == Params.Enemy)
	{
		if (ClearRay(Observer, ViewPoint, Center)
			|| ClearRay(Observer, Observer->GetActorLocation(), Center))
		{
			LastSeeingPos = Observer->GetActorLocation();
			LastSeenPos = Center;
			return true;
		}
		if (DistSq > 1000000.0f) return false;
	}
	else if (DistSq > 1000000.0f)
	{
		if (bTargetPawn)
		{
			if (!LOSFlag && DistSq > 0.5f * MaxDistSq) return false;
			if (!Params.bViewerIsPlayer && RetailRandom() < 0.5f) return false;
		}
		return ClearRay(Observer, ViewPoint, Center);
	}

	float TargetRadius = 0.0f, TargetHalfHeight = 0.0f;
	Target->GetSimpleCollisionCylinder(TargetRadius, TargetHalfHeight);
	if (!bShowSelf || !LOSFlag)
	{
		const FVector Body = Center + FVector(0, 0, 0.8f * TargetHalfHeight);
		if (ClearRay(Observer, ViewPoint, Body)) return true;
	}
	if (DistSq > 250000.0f || !bTargetPawn) return false;

	const float Radius = Observer->GetSimpleCollisionRadius();
	FVector Points[4] = {
		Center - FVector(Radius, -Radius, 0), Center + FVector(Radius, Radius, 0),
		Center - FVector(Radius, Radius, 0), Center + FVector(Radius, -Radius, 0)};
	int32 MinIndex = 0, MaxIndex = 0;
	float CurrentMin = Points[0].SizeSquared(), CurrentMax = CurrentMin;
	for (int32 Index = 1; Index < 4; ++Index)
	{
		const float Size = Points[Index].SizeSquared();
		if (Size > CurrentMax) { CurrentMax = Size; MaxIndex = Index; }
		else if (Size < CurrentMin) { CurrentMin = Size; MinIndex = Index; }
	}
	bool bSkip = LOSFlag;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (Index == MinIndex || Index == MaxIndex) continue;
		if (bSkip && bShowSelf) bSkip = false;
		else
		{
			bSkip = true;
			if (ClearRay(Observer, ViewPoint, Points[Index])) return true;
		}
	}
	return false;
}

bool ConeSight(const APawn* Observer, float PeripheralVision, float SightRadius,
	const AActor* Target, uint8 TargetVisibility)
{
	FWoTLineOfSightParams Params;
	Params.bShowSelf = true;
	Params.bViewerIsPlayer = ViewerIsPlayer(Observer);
	Params.SightRadius = SightRadius;
	Params.PeripheralVision = PeripheralVision;
	Params.Skill = ViewerSkill(Observer);
	Params.TargetVisibility = TargetVisibility;
	Params.Enemy = CurrentEnemy(Observer);
	return LineOfSightTo(Observer, Target, Params);
}

bool OmniSight(const APawn* Observer, const AActor* Target, uint8 TargetVisibility)
{
	FWoTLineOfSightParams Params;
	Params.bViewerIsPlayer = ViewerIsPlayer(Observer);
	Params.Skill = ViewerSkill(Observer);
	Params.TargetVisibility = TargetVisibility;
	Params.Enemy = CurrentEnemy(Observer);
	return LineOfSightTo(Observer, Target, Params);
}

bool TraceToTarget(const APawn* Observer, const AActor* Target)
{
	return OmniSight(Observer, Target, VisibilityOf(Target));
}

bool PawnCanSeeActor(const APawn* Observer, const AActor* Target,
	float FOVCos, bool bTraceActors)
{
	if (!Observer || !Target) return false;
	if (FMath::IsNearlyZero(FOVCos))
	{
		if (const AWoTNPCCharacter* NPC = Cast<AWoTNPCCharacter>(Observer))
			FOVCos = NPC->PeripheralVision;
	}
	const FVector View = ViewerIsPlayer(Observer)
		? Observer->GetViewRotation().Vector() : Observer->GetActorForwardVector();
	const FVector ToTarget = Target->GetActorLocation() - Observer->GetActorLocation();
	if (FVector::DotProduct(View.GetSafeNormal(), ToTarget.GetSafeNormal()) < FOVCos)
		return false;
	const FVector Start = Observer->GetActorLocation()
		+ FVector(0, 0, Observer->BaseEyeHeight);
	float Radius = 0.0f, HalfHeight = 0.0f;
	Target->GetSimpleCollisionCylinder(Radius, HalfHeight);
	const FVector Ends[3] = {Target->GetActorLocation(),
		Target->GetActorLocation() + FVector(0, 0, HalfHeight),
		Target->GetActorLocation() - FVector(0, 0, HalfHeight)};
	for (const FVector& End : Ends)
	{
		FHitResult Hit;
		if (bTraceActors)
		{
			const FVector Delta = End - Start;
			if (!WoTRetailActorTraceRecursive(Observer->GetWorld(), Start,
				Delta.GetSafeNormal(), Delta.Size(), Observer, Hit)
				|| Hit.GetActor() == Target) return true;
		}
		else if (!WoTVisBlockingTraceHits(Observer->GetWorld(), Start, End, Observer))
			return true;
	}
	return false;
}

#if WITH_AUTOMATION_TESTS
void SetTraceOverrideForTesting(
	TFunction<bool(const FVector&, const FVector&)> Override)
{
	GTraceOverride = MoveTemp(Override);
}
void SetRandomOverrideForTesting(TFunction<float()> Override)
{
	GRandomOverride = MoveTemp(Override);
}
void ResetStateForTesting(const APawn* Observer)
{
	if (AWoTCharacter* Character = Cast<AWoTCharacter>(const_cast<APawn*>(Observer)))
	{
		Character->bSightLOSFlag = false;
		Character->SightStimulus = 0.0f;
		Character->SightLastSeeingPos = FVector::ZeroVector;
		Character->SightLastSeenPos = FVector::ZeroVector;
	}
}
#endif
}
