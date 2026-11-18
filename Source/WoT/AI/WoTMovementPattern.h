#pragma once

#include "CoreMinimal.h"

class FWoTRuntimeClassDefaults;
class UWoTBrainComponent;
class AActor;
enum class EWoTGoalIndex : uint8;

// Numeric order is MovementPattern.ERelationalRequirement in the shipped CDO.
enum class EWoTMovementRelation : uint8 { None, Equal, Less, LessOrEqual, GreaterOrEqual, Greater };

struct FWoTMovementWaypoint
{
	bool bSelectWaypoint = false;
	bool bApplyDirection = false;
	FVector Direction = FVector::ZeroVector;
	bool bApplyDistance = false;
	float Distance = 0;
	EWoTMovementRelation TravelRelation = EWoTMovementRelation::None;
	float TravelDistance = 0;
	EWoTMovementRelation GoalRelation = EWoTMovementRelation::None;
	float GoalDistance = 0;
	bool bRequireReachable = true;
};

struct FWoTMovementElement
{
	bool bEnabled = false;
	EWoTMovementRelation ApplicationRelation = EWoTMovementRelation::None;
	float ApplicationDistance = 0;
	EWoTMovementRelation IntervalRelation = EWoTMovementRelation::None;
	float Interval = 0;
	EWoTMovementRelation VelocityRelation = EWoTMovementRelation::None;
	float Velocity = 0;
	bool bApplySpeed = false;
	float Speed = 0;
	FName Name, PreHint, PostHint;
	bool bSelectFocus = false;
	FName AlternateFocus;
	FWoTMovementWaypoint Waypoint;
};

// Immutable, generation-owned defaults. Mutable cursor/times NEVER live here.
struct FWoTMovementPattern
{
	FName ClassIdentity, PatternName, IteratorClass;
	float GoalDistanceTolerance = 10;
	EWoTMovementRelation PrerequisiteRelation = EWoTMovementRelation::None;
	float PrerequisiteDistance = 0;
	FWoTMovementElement Elements[16];

	static bool Evaluate(float Value, EWoTMovementRelation Relation, float Comparison);
	bool AdjustWaypoint(FVector& Waypoint, const FVector& PawnLocation,
		const FVector& GoalLocation, float Distance, float Required, EWoTMovementRelation Relation) const;
	static bool Decode(const FWoTRuntimeClassDefaults& Defaults, const FString& Identity,
		FWoTMovementPattern& Out, FString& Error);
	void Serialize(TArray<uint8>& Out) const;
};

struct FWoTMovementPatternState
{
	int32 Cursor = 0;
	float LastSelectionTimes[16] = {};
	int32 Select(const FWoTMovementPattern& Pattern, float Gap, float PawnSpeed, float Now);
	void Advance() { Cursor = (Cursor + 1) % 16; }
};

struct FWoTMovementBinding
{
	int32 HandlerIndex = INDEX_NONE;
	bool bComponent = false;
	FName WaypointSelector, FocusSelector;
	FWoTMovementPattern Pattern;
	static bool Decode(const FWoTRuntimeClassDefaults& Defaults, const FString& Factory,
		int32 Index, bool bComponent, FWoTMovementBinding& Out, FString& Error);
	void Serialize(TArray<uint8>& Out) const;
};

// One manager per possessed pawn. A handler owns its own circular state even
// when two handlers reference the same pattern class.
struct FWoTMovementManager
{
	TMap<int32, FWoTMovementPatternState> States;
	FName PreHint, PostHint, ElementName;
	FVector Destination = FVector::ZeroVector;
	float SuggestedSpeed = 0;
	bool bAppliedPattern = false;
	int32 SelectedIndex = INDEX_NONE;

	bool InitMovement(UWoTBrainComponent& Brain, AActor& Pawn, EWoTGoalIndex Goal,
		const FWoTMovementBinding* Binding, TFunctionRef<bool(FVector&)> OrdinaryWaypoint);
};
