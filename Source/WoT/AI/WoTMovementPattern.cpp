#include "WoTMovementPattern.h"
#include "WoTBrainComponent.h"
#include "../World/WoTRuntimeStructInheritance.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../WoTRetailBuilderCanonical.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

bool FWoTMovementPattern::Evaluate(float Value, EWoTMovementRelation Relation, float Comparison)
{
	const bool Equal = FMath::Abs(Value - Comparison) < 0.0001f;
	switch (Relation)
	{
	case EWoTMovementRelation::None: return true;
	case EWoTMovementRelation::Equal: return Equal;
	case EWoTMovementRelation::Less: return Value < Comparison;
	case EWoTMovementRelation::LessOrEqual: return Value < Comparison || Equal;
	case EWoTMovementRelation::GreaterOrEqual: return Value > Comparison || Equal;
	case EWoTMovementRelation::Greater: return Value > Comparison;
	}
	return false;
}

bool FWoTMovementPattern::AdjustWaypoint(FVector& Waypoint, const FVector& PawnLocation,
	const FVector& GoalLocation, float Distance, float Required, EWoTMovementRelation Relation) const
{
	const float Difference = Distance - Required;
	if (Evaluate(Difference, Relation, GoalDistanceTolerance)) return false;
	FVector Direction = GoalLocation == Waypoint ? PawnLocation - Waypoint : GoalLocation - Waypoint;
	Direction.Z = 0;
	Waypoint += Direction.GetSafeNormal() * Difference;
	return true;
}

int32 FWoTMovementPatternState::Select(const FWoTMovementPattern& Pattern,
	float Gap, float PawnSpeed, float Now)
{
	for (int32 Tested = 0; Tested < 16; ++Tested, Advance())
	{
		const FWoTMovementElement& E = Pattern.Elements[Cursor];
		if (E.bEnabled
			&& Pattern.Evaluate(Gap - E.ApplicationDistance, E.ApplicationRelation, Pattern.GoalDistanceTolerance)
			&& Pattern.Evaluate(Now - LastSelectionTimes[Cursor], E.IntervalRelation, E.Interval)
			&& Pattern.Evaluate(PawnSpeed - E.Velocity, E.VelocityRelation, E.Velocity))
		{
			LastSelectionTimes[Cursor] = Now;
			return Cursor;
		}
	}
	return INDEX_NONE;
}

namespace
{
	// Strict readers retain zero-valued omitted members and reject malformed data.
	struct FReader
	{
		const FWoTRuntimeWorldProperty& Struct;
		bool bOK = true;
		const FWoTRuntimeWorldProperty* Member(const TCHAR* Name) { return WoTRuntimeWorldStructMember(Struct, Name); }
		void Read(const TCHAR* Name, float& Value) { if (const auto* P = Member(Name)) bOK &= WoTRuntimeWorldPropertyFloat(*P, Value); }
		void Read(const TCHAR* Name, bool& Value) { if (const auto* P = Member(Name)) bOK &= WoTRuntimeWorldPropertyBool(*P, Value); }
		void Read(const TCHAR* Name, FVector& Value) { if (const auto* P = Member(Name)) bOK &= WoTRuntimeWorldPropertyVector(*P, Value); }
		void Read(const TCHAR* Name, EWoTMovementRelation& Value)
		{
			if (const auto* P = Member(Name)) { uint8 V = 0; bOK &= WoTRuntimeWorldPropertyByte(*P, V) && V <= 5; Value = static_cast<EWoTMovementRelation>(V); }
		}
		void Read(const TCHAR* Name, FName& Value)
		{
			if (const auto* P = Member(Name))
			{
				if (P->Type == EWoTRuntimeWorldPropertyType::Name) Value = FName(*P->NameValue);
				else if (P->Type == EWoTRuntimeWorldPropertyType::Object) Value = FName(*P->ObjectIdentity);
				else bOK = false;
			}
		}
	};
}

bool FWoTMovementPattern::Decode(const FWoTRuntimeClassDefaults& Defaults,
	const FString& Identity, FWoTMovementPattern& Out, FString& Error)
{
	if (!Defaults.ClassIsA(Identity, TEXT("Legend.MovementPattern")))
	{ Error = TEXT("invalid movement pattern class: ") + Identity; return false; }
	Out = FWoTMovementPattern(); Out.ClassIdentity = FName(*Identity);
	FWoTRuntimeWorldProperty Header;
	Header.Type = EWoTRuntimeWorldPropertyType::Struct;
	for (const TCHAR* Name : {TEXT("PatternName"), TEXT("PatternElementIterClass"), TEXT("GoalDistanceTolerance"), TEXT("PreReqDistanceRelation"), TEXT("PreReqGoalDistance")})
		if (const auto* P = Defaults.FindClassProperty(Identity, Name)) Header.StructMembers.Add(*P);
	FReader H{Header};
	H.Read(TEXT("PatternName"), Out.PatternName); H.Read(TEXT("PatternElementIterClass"), Out.IteratorClass);
	H.Read(TEXT("GoalDistanceTolerance"), Out.GoalDistanceTolerance);
	H.Read(TEXT("PreReqDistanceRelation"), Out.PrerequisiteRelation); H.Read(TEXT("PreReqGoalDistance"), Out.PrerequisiteDistance);
	bool bOK = H.bOK;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		FWoTRuntimeWorldProperty Cell;
		if (!WoTRuntimeStructInheritance::Resolve(Defaults, Identity, TEXT("MovementPatternElements"), Index, Cell))
		{ Error = FString::Printf(TEXT("%s missing movement cell %d"), *Identity, Index); return false; }
		if (Cell.Type != EWoTRuntimeWorldPropertyType::Struct)
		{ Error = TEXT("malformed movement element: ") + Identity; return false; }
		FReader R{Cell}; FWoTMovementElement& E = Out.Elements[Index];
		R.Read(TEXT("MPE_bEnabled"), E.bEnabled);
		R.Read(TEXT("MPE_ApplicationRel"), E.ApplicationRelation); R.Read(TEXT("MPE_ApplicationGoalDistance"), E.ApplicationDistance);
		R.Read(TEXT("MPE_SelectionIntervalRel"), E.IntervalRelation); R.Read(TEXT("MPE_SelectionInterval"), E.Interval);
		R.Read(TEXT("MPE_GoalVelocitySizeRel"), E.VelocityRelation); R.Read(TEXT("MPE_GoalVelocitySize"), E.Velocity);
		R.Read(TEXT("MPE_bApplySpeed"), E.bApplySpeed); R.Read(TEXT("MPE_Speed"), E.Speed);
		R.Read(TEXT("MPE_Name"), E.Name); R.Read(TEXT("MPE_PreHint"), E.PreHint); R.Read(TEXT("MPE_PostHint"), E.PostHint);
		if (const auto* P = R.Member(TEXT("MPE_FocusParams")))
		{ FReader F{*P}; F.Read(TEXT("FP_bSelectFocus"), E.bSelectFocus); F.Read(TEXT("FP_AlternateSelectorFcs"), E.AlternateFocus); bOK &= F.bOK; }
		if (const auto* P = R.Member(TEXT("MPE_WaypointParams")))
		{
			FReader W{*P}; auto& V = E.Waypoint;
			W.Read(TEXT("WP_bSelectWaypoint"), V.bSelectWaypoint); W.Read(TEXT("WP_bApplyOffsetDirection"), V.bApplyDirection);
			W.Read(TEXT("WP_OffsetDirection"), V.Direction); W.Read(TEXT("WP_bApplyOffsetDistance"), V.bApplyDistance);
			W.Read(TEXT("WP_OffsetDistance"), V.Distance); W.Read(TEXT("WP_ReqTravelDistanceRel"), V.TravelRelation);
			W.Read(TEXT("WP_ReqTravelDistance"), V.TravelDistance); W.Read(TEXT("WP_ReqGoalDistanceRel"), V.GoalRelation);
			W.Read(TEXT("WP_ReqGoalDistance"), V.GoalDistance); W.Read(TEXT("WP_bRequireReachable"), V.bRequireReachable);
			bOK &= W.bOK;
		}
		bOK &= R.bOK;
	}
	if (!bOK) Error = TEXT("malformed movement pattern: ") + Identity;
	return bOK;
}

bool FWoTMovementBinding::Decode(const FWoTRuntimeClassDefaults& Defaults, const FString& Factory,
	int32 Index, bool bIsComponent, FWoTMovementBinding& Out, FString& Error)
{
	Out = FWoTMovementBinding(); Out.HandlerIndex = Index; Out.bComponent = bIsComponent;
	FWoTRuntimeWorldProperty Cell;
	if (!WoTRuntimeStructInheritance::Resolve(Defaults, Factory,
		bIsComponent ? TEXT("ComponentHandlerCreateParams") : TEXT("HandlerCreateParams"), Index, Cell)) return true;
	const FString Field = FString::Printf(TEXT("%s.%s[%d]"), *Factory,
		bIsComponent ? TEXT("ComponentHandlerCreateParams") : TEXT("HandlerCreateParams"), Index);
	if (Cell.Type != EWoTRuntimeWorldPropertyType::Struct)
	{ Error = TEXT("movement binding requires a struct: ") + Field; return false; }
	FReader R{Cell}; bool bEnabled = false; R.Read(TEXT("bEnabled"), bEnabled);
	if (!R.bOK) { Error = TEXT("malformed movement handler: ") + Factory; return false; }
	const auto* Template = R.Member(TEXT("HandlerTemplate"));
	if (Template && Template->Type != EWoTRuntimeWorldPropertyType::Struct)
	{ Error = TEXT("movement binding requires a struct: ") + Field + TEXT(".HandlerTemplate"); return false; }
	if (!bEnabled || !Template) return true;
	FReader T{*Template}; FName PatternClass;
	T.Read(TEXT("HT_MovementPatternClass"), PatternClass);
	T.Read(TEXT("HT_SelectorClassWpt"), Out.WaypointSelector); T.Read(TEXT("HT_SelectorClassFcs"), Out.FocusSelector);
	if (!T.bOK) { Error = TEXT("malformed movement template: ") + Factory; return false; }
	return PatternClass.IsNone() || FWoTMovementPattern::Decode(Defaults, PatternClass.ToString(), Out.Pattern, Error);
}

void FWoTMovementPattern::Serialize(TArray<uint8>& Out) const
{
	using namespace WoTRetailBuilderCanonical;
	Name(Out, ClassIdentity); Name(Out, PatternName); Name(Out, IteratorClass);
	Float(Out, GoalDistanceTolerance); Integer(Out, static_cast<uint8>(PrerequisiteRelation)); Float(Out, PrerequisiteDistance);
	for (const FWoTMovementElement& E : Elements)
	{
		Bool(Out, E.bEnabled); Integer(Out, static_cast<uint8>(E.ApplicationRelation)); Float(Out, E.ApplicationDistance);
		Integer(Out, static_cast<uint8>(E.IntervalRelation)); Float(Out, E.Interval);
		Integer(Out, static_cast<uint8>(E.VelocityRelation)); Float(Out, E.Velocity);
		Bool(Out, E.bApplySpeed); Float(Out, E.Speed); Name(Out, E.Name); Name(Out, E.PreHint); Name(Out, E.PostHint);
		Bool(Out, E.bSelectFocus); Name(Out, E.AlternateFocus);
		const auto& W = E.Waypoint;
		Bool(Out, W.bSelectWaypoint); Bool(Out, W.bApplyDirection);
		Float(Out, W.Direction.X); Float(Out, W.Direction.Y); Float(Out, W.Direction.Z);
		Bool(Out, W.bApplyDistance); Float(Out, W.Distance);
		Integer(Out, static_cast<uint8>(W.TravelRelation)); Float(Out, W.TravelDistance);
		Integer(Out, static_cast<uint8>(W.GoalRelation)); Float(Out, W.GoalDistance); Bool(Out, W.bRequireReachable);
	}
}

void FWoTMovementBinding::Serialize(TArray<uint8>& Out) const
{
	using namespace WoTRetailBuilderCanonical;
	Integer(Out, HandlerIndex); Bool(Out, bComponent); Name(Out, WaypointSelector); Name(Out, FocusSelector); Pattern.Serialize(Out);
}

bool FWoTMovementManager::InitMovement(UWoTBrainComponent& Brain, AActor& Pawn,
	EWoTGoalIndex Goal, const FWoTMovementBinding* Binding, TFunctionRef<bool(FVector&)> OrdinaryWaypoint)
{
	PreHint = PostHint = ElementName = NAME_None; bAppliedPattern = false; SelectedIndex = INDEX_NONE;
	SuggestedSpeed = Brain.GetGoal(Goal).SuggestedSpeed;
	const FVector Origin = Pawn.GetActorLocation();
	float Radius, Height; Pawn.GetSimpleCollisionCylinder(Radius, Height);
	float Gap = 0;
	if (Binding && !Binding->Pattern.ClassIdentity.IsNone()
		&& Brain.GetGoalDistanceToCylinder(Goal, Origin, Radius, Height, Gap))
	{
		const FWoTMovementPattern& Pattern = Binding->Pattern;
		if (Pattern.Evaluate(Gap - Pattern.PrerequisiteDistance, Pattern.PrerequisiteRelation, Pattern.GoalDistanceTolerance))
		{
			auto& Runtime = States.FindOrAdd(Binding->HandlerIndex + (Binding->bComponent ? 100 : 0));
			SelectedIndex = Runtime.Select(Pattern, Gap, Pawn.GetVelocity().Size(), Pawn.GetWorld()->GetTimeSeconds());
			if (SelectedIndex != INDEX_NONE)
			{
				const auto& E = Pattern.Elements[SelectedIndex]; const auto& W = E.Waypoint;
				FVector Seed = Origin;
				if (W.bSelectWaypoint && !OrdinaryWaypoint(Seed)) Seed = Origin;
				FVector Direction = W.bApplyDirection ? Pawn.GetActorRotation().RotateVector(W.Direction.GetSafeNormal()) : (Seed - Origin).GetSafeNormal();
				if (Direction.Size() < 0.0001f) Direction = Pawn.GetActorForwardVector();
				Destination = Seed + Direction * (W.bApplyDistance ? W.Distance + Radius : 0);
				SuggestedSpeed = E.bApplySpeed ? E.Speed : Brain.GetGoal(Goal).SuggestedSpeed;
				Pattern.AdjustWaypoint(Destination, Origin, Destination, FVector::Dist(Destination, Origin) - Radius, W.TravelDistance + Radius, W.TravelRelation);
				float Distance; FVector GoalLocation; float GoalRadius, GoalHeight;
				if (Brain.GetGoalDistanceToCylinder(Goal, Destination, Radius, Height, Distance)
					&& Brain.GetGoalCylinder(Goal, GoalLocation, GoalRadius, GoalHeight))
					Pattern.AdjustWaypoint(Destination, Origin, GoalLocation, Distance, W.GoalDistance, W.GoalRelation);
				Brain.InitGoalWithVector(EWoTGoalIndex::CurrentWaypoint, Destination);
				Brain.GetGoal(EWoTGoalIndex::CurrentWaypoint).SuggestedSpeed = SuggestedSpeed;
				bAppliedPattern = !W.bRequireReachable || Brain.IsGoalReachable(EWoTGoalIndex::CurrentWaypoint);
				Runtime.Advance(); // application consumes the cell even when unreachable
				if (bAppliedPattern) { PreHint = E.PreHint; PostHint = E.PostHint; ElementName = E.Name; return true; }
			}
		}
	}
	// Prerequisite adjustment is overwritten by the ordinary selector in retail.
	if (!OrdinaryWaypoint(Destination)) Destination = Origin;
	else if (!Binding || !Binding->WaypointSelector.ToString().EndsWith(TEXT("WaypointSelectorHolding")))
		SuggestedSpeed = Brain.GetGoal(Goal).SuggestedSpeed;
	Brain.InitGoalWithVector(EWoTGoalIndex::CurrentWaypoint, Destination);
	Brain.GetGoal(EWoTGoalIndex::CurrentWaypoint).SuggestedSpeed = SuggestedSpeed;
	return false;
}
