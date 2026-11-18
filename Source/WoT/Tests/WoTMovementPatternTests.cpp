#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"
#if WITH_AUTOMATION_TESTS
#include "WoTTestAIClasses.h"
#include "../AI/WoTMovementPattern.h"
#include "../AI/WoTCreatureData.h"
#include "../RetailData/WoTDevRetailFamilies.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeWorldProperties.h"
#include "../World/WoTRuntimeStructInheritance.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace { constexpr auto MovementFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter; }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMovementMalformedBinding, "WoT.Parity.F1.MovementPattern.MalformedBindingFailsClosed", MovementFlags)
bool FWoTMovementMalformedBinding::RunTest(const FString&)
{
	for(int32 Case=0;Case<3;++Case)
	{
		FWoTRuntimeClassDefaultRecord Record; Record.Identity=TEXT("Fixture.Factory");
		FWoTRuntimeWorldProperty Cell; Cell.Name=TEXT("HandlerCreateParams"); Cell.ArrayIndex=21;
		Cell.Type=Case==0 ? EWoTRuntimeWorldPropertyType::Integer : EWoTRuntimeWorldPropertyType::Struct;
		if(Case==0) Cell.Value.SetNumZeroed(4);
		else
		{
			Cell.StructName=TEXT("HandlerCreateParams");
			FWoTRuntimeWorldProperty Enabled; Enabled.Name=TEXT("bEnabled"); Enabled.Type=EWoTRuntimeWorldPropertyType::Boolean; Enabled.Value.Add(Case==1 ? 1 : 0);
			Cell.StructMembers.Add(Enabled);
			FWoTRuntimeWorldProperty Template; Template.Name=TEXT("HandlerTemplate"); Template.Type=EWoTRuntimeWorldPropertyType::Integer; Template.Value.SetNumZeroed(4);
			Cell.StructMembers.Add(Template);
		}
		Record.Properties.Add(Cell); FWoTRuntimeClassDefaults Defaults; FString Error;
		TestTrue(TEXT("malformed typed-field fixture loads"),Defaults.LoadFixture({Record},Error));
		FWoTMovementBinding Binding;
		TestFalse(*FString::Printf(TEXT("malformed binding shape%d fails closed"),Case),FWoTMovementBinding::Decode(Defaults,Record.Identity,21,false,Binding,Error));
		TestTrue(TEXT("diagnostic identifies factory and malformed field"),Error.Contains(TEXT("Fixture.Factory")) && Error.Contains(Case==0 ? TEXT("HandlerCreateParams") : TEXT("HandlerTemplate")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMovementInheritance, "WoT.Parity.F1.MovementPattern.PackedStructInheritanceAndCanonical", MovementFlags)
bool FWoTMovementInheritance::RunTest(const FString&)
{
	FWoTRuntimeWorldProperty Base, Child;
	Base.Type = Child.Type = EWoTRuntimeWorldPropertyType::Struct;
	Base.StructName = Child.StructName = TEXT("Vector");
	Base.Value.SetNumZeroed(12);
	int32 Coordinate=0;
	for (const TCHAR* Name : {TEXT("X"),TEXT("Y"),TEXT("Z")})
	{
		FWoTRuntimeWorldProperty Member; Member.Name=Name; Member.Type=EWoTRuntimeWorldPropertyType::Float;
		const float Value=++Coordinate; Member.Value.SetNumUninitialized(4); FMemory::Memcpy(Member.Value.GetData(),&Value,4);
		Base.StructMembers.Add(Member);
	}
	Child.StructMembers.Add(Base.StructMembers[0]); const float Override=-1;
	FMemory::Memcpy(Child.StructMembers[0].Value.GetData(),&Override,4);
	WoTRuntimeStructInheritance::Overlay(Base,Child);
	FVector Result; TestTrue(TEXT("overlaid vector retains packed-reader contract"),WoTRuntimeWorldPropertyVector(Base,Result));
	TestTrue(TEXT("partial X edit preserves inherited Y/Z and refreshes packed bytes"),Result.Equals(FVector(-1,2,3)));
	FWoTCreatureSet A,B; FWoTMovementPattern Pattern; Pattern.ClassIdentity=TEXT("UnboundFixture");
	A.MovementPatterns.Add(Pattern.ClassIdentity,Pattern); B=A;
	B.MovementPatterns.FindChecked(Pattern.ClassIdentity).Elements[15].Speed=650;
	TestNotEqual(TEXT("disabled unbound definition participates in canonical identity"),FWoTCreatureDataBuilder::CanonicalDigest(A),FWoTCreatureDataBuilder::CanonicalDigest(B));
	FString Error; TestFalse(TEXT("equivalence compares unbound definitions"),FWoTCreatureDataBuilder::AreEquivalent(A,B,Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMovementRelations, "WoT.Parity.F1.MovementPattern.RelationsCursorIsolation", MovementFlags)
bool FWoTMovementRelations::RunTest(const FString&)
{
	using R = EWoTMovementRelation;
	FWoTMovementPattern P;
	auto& E = P.Elements[7]; E.bEnabled = true; E.ApplicationRelation = R::GreaterOrEqual;
	E.ApplicationDistance = 32; E.IntervalRelation = R::GreaterOrEqual; E.Interval = 2;
	E.VelocityRelation = R::LessOrEqual; E.Velocity = 64;
	FWoTMovementPatternState A, B;
	TestEqual(TEXT("sixteen disabled indices retain initial cursor"), A.Cursor, 0);
	TestEqual(TEXT("initial zero timestamp enforces first interval"), A.Select(P, 42, 128, 1.99f), INDEX_NONE);
	TestEqual(TEXT("goal comparison is difference>=10, not gap>=32"), A.Select(P, 41.9f, 128, 2), INDEX_NONE);
	TestEqual(TEXT("moving pawn full speed compared to configured twice"), A.Select(P, 42, 128.01f, 2), INDEX_NONE);
	TestEqual(TEXT("<=64 accepts actual128"), A.Select(P, 42, 128, 2), 7);
	TestEqual(TEXT("selection stamps before application"), A.LastSelectionTimes[7], 2.0f);
	A.Advance(); TestEqual(TEXT("advance retains disabled index8"), A.Cursor, 8);
	TestEqual(TEXT("other pawn state remains independent"), B.LastSelectionTimes[7], 0.0f);
	TestEqual(TEXT("other pawn can select same cell at same time"), B.Select(P, 42, 0, 2), 7);
	TestEqual(TEXT("cooldown does not restart on failed scan"), A.Select(P, 42, 0, 3), INDEX_NONE);
	TestEqual(TEXT("failed full circular scan returns to original cursor"), A.Cursor, 8);
	TestEqual(TEXT("interval equality passes"), A.Select(P, 42, 0, 4), 7);
	TestTrue(TEXT("approx equality strictly below .0001"), P.Evaluate(.000099f, R::Equal, 0));
	TestFalse(TEXT("approx equality excludes .0001"), P.Evaluate(.0001f, R::Equal, 0));
	TestTrue(TEXT("<=48 goal threshold includes58"), P.Evaluate(58-48, R::LessOrEqual, 10));
	TestFalse(TEXT("<=48 excludes58.01"), P.Evaluate(58.01f-48, R::LessOrEqual, 10));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMovementGeometry, "WoT.Parity.F1.MovementPattern.WaypointAndFailureConsumption", MovementFlags)
bool FWoTMovementGeometry::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	auto* Pawn = World->SpawnActor<AWoTTestNPCCharacter>();
	auto* Brain = Cast<UWoTTestBrainComponent>(Pawn->Brain);
	WoTTest::TickWorld(World,1,.25f); // nonzero time proves failed application stamps
	Pawn->SetActorLocation(FVector(0,0,100)); Pawn->SetActorRotation(FRotator(30,90,20));
	Brain->InitGoalWithVector(EWoTGoalIndex::Threat, FVector(500,0,100));
	Brain->GetGoal(EWoTGoalIndex::Threat).SuggestedSpeed = 300;
	FWoTMovementBinding Binding; Binding.HandlerIndex = 21; Binding.Pattern.ClassIdentity = TEXT("Fixture");
	auto& E = Binding.Pattern.Elements[3]; E.bEnabled = true; E.PreHint = TEXT("AttemptAttack");
	E.Waypoint.bApplyDirection = true; E.Waypoint.Direction = FVector(1,1,1);
	E.Waypoint.bApplyDistance = true; E.Waypoint.Distance = 64;
	FWoTMovementManager M;
	const FVector Expected = Pawn->GetActorLocation() + Pawn->GetActorRotation().RotateVector(FVector(1,1,1).GetSafeNormal()) * (64 + Pawn->GetSimpleCollisionRadius());
	TestTrue(TEXT("valid selected cell applies"), M.InitMovement(*Brain,*Pawn,EWoTGoalIndex::Threat,&Binding,[](FVector& Out){ Out=FVector(999,999,999); return true; }));
	TestTrue(TEXT("full pawn rotation and normalized local offset plus collision radius"), M.Destination.Equals(Expected,.001));
	TestEqual(TEXT("unauthored speed inherits active goal"), M.SuggestedSpeed, 300.0f);
	TestEqual(TEXT("success transfers pre hint"), M.PreHint, FName(TEXT("AttemptAttack")));
	Brain->bForceReachable = false;
	// Clear visibility/reachability cache through normal goal assignment in manager.
	const float Now = World->GetTimeSeconds();
	TestTrue(TEXT("timestamp fixture has advanced real world time"),Now>0);
	TestFalse(TEXT("failed reachability falls back"), M.InitMovement(*Brain,*Pawn,EWoTGoalIndex::Threat,&Binding,[](FVector& Out){ Out=FVector(123,0,100); return true; }));
	TestEqual(TEXT("failed application consumes cursor"), M.States.FindChecked(21).Cursor, 4);
	TestEqual(TEXT("failed application consumes timestamp"), M.States.FindChecked(21).LastSelectionTimes[3], Now);
	TestTrue(TEXT("failed application suppresses hints"), M.PreHint.IsNone() && M.PostHint.IsNone());
	TestTrue(TEXT("ordinary fallback replaces candidate"), M.Destination.Equals(FVector(123,0,100)));
	Binding.Pattern.PrerequisiteRelation = EWoTMovementRelation::GreaterOrEqual;
	Binding.Pattern.PrerequisiteDistance = 1000;
	M.InitMovement(*Brain,*Pawn,EWoTGoalIndex::Threat,&Binding,[](FVector& Out){ Out=FVector(321,0,100); return true; });
	TestTrue(TEXT("prerequisite failure does not persist adjustment"), M.Destination.Equals(FVector(321,0,100)));
	Binding.Pattern.PrerequisiteRelation = EWoTMovementRelation::None;
	Binding.HandlerIndex = 23; Brain->bForceReachable = true;
	M.InitMovement(*Brain,*Pawn,EWoTGoalIndex::Threat,&Binding,[](FVector& Out){return false;});
	TestEqual(TEXT("same pattern class has separate handler state"),M.States.Num(),2);
	FWoTMovementPattern P; FVector Point(300,0,0);
	P.AdjustWaypoint(Point,FVector::ZeroVector,Point,280,120,EWoTMovementRelation::LessOrEqual);
	TestTrue(TEXT("travel correction uses pawn-to-waypoint when goal equals waypoint"), Point.Equals(FVector(140,0,0)));
	P.AdjustWaypoint(Point,FVector::ZeroVector,FVector(500,0,0),320,32,EWoTMovementRelation::LessOrEqual);
	TestTrue(TEXT("goal correction follows travel correction"), Point.Equals(FVector(428,0,0)));
	TestEqual(TEXT("signed overlapping cylinders"), UWoTBrainComponent::DistanceBetweenCylinders(FVector::ZeroVector,20,20,FVector(30,0,0),20,20), -10.0f);
	TestTrue(TEXT("vertical separation retains radial surface distance"), FMath::IsNearlyEqual(UWoTBrainComponent::DistanceBetweenCylinders(FVector::ZeroVector,20,20,FVector(30,0,50),20,20), FMath::Sqrt(200.0f)));
	TestEqual(TEXT("coincident axes use shipped zero normal"), UWoTBrainComponent::DistanceBetweenCylinders(FVector::ZeroVector,20,20,FVector::ZeroVector,20,20), 0.0f);
	WoTTest::DestroySpecWorld(World); return !HasAnyErrors();
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMovementBindings, "WoT.Parity.F1.MovementPattern.AuthoredBindings", MovementFlags)
bool FWoTMovementBindings::RunTest(const FString&)
{
	const auto* Defaults = WoTDevRetailClassDefaults(); if (!TestNotNull(TEXT("runtime defaults"), Defaults)) return false;
	struct FExpected { const TCHAR* Factory; const TCHAR* Reachable; const TCHAR* Pathable; const TCHAR* Visible; };
	const FExpected Cases[] = {
		{TEXT("Trolloc"),TEXT("WoTPawns.MovementPatternTrolloc0"),TEXT("Legend.MovementPatternNavigating"),TEXT("WoTPawns.MovementPatternTrolloc1")},
		{TEXT("Soldier"),TEXT("WoTPawns.MovementPatternSoldier0"),TEXT(""),TEXT("")},
		{TEXT("Archer"),TEXT("WoTPawns.MovementPatternArcher0"),TEXT("Legend.MovementPatternNavigating"),TEXT("Legend.MovementPatternThreat")},
		{TEXT("AngrealUser"),TEXT("WoTPawns.MovementPatternAngrealUser0"),TEXT(""),TEXT("")},
		{TEXT("Myrddraal"),TEXT("WoTPawns.MovementPatternMyrddraal0"),TEXT("Legend.MovementPatternNavigating"),TEXT("WoTPawns.MovementPatternMyrddraal1")},
		{TEXT("Questioner"),TEXT("WoTPawns.MovementPatternQuestioner0"),TEXT(""),TEXT("WoTPawns.MovementPatternQuestioner1")},
		{TEXT("BATrolloc"),TEXT("WoTPawns.MovementPatternBATrolloc0"),TEXT(""),TEXT("")},
		{TEXT("Legion"),TEXT("WoTPawns.MovementPatternLegion0"),TEXT(""),TEXT("")},
		{TEXT("TrollocMed"),TEXT("WoTPawns.MovementPatternTrollocMed0"),TEXT(""),TEXT("WoTPawns.MovementPatternTrollocMed1")},
		{TEXT("TrollocWeak"),TEXT("WoTPawns.MovementPatternTrollocWeak0"),TEXT(""),TEXT("")},
		{TEXT("Warder"),TEXT(""),TEXT(""),TEXT("")}
	};
	for (const auto& C : Cases)
	{
		int32 N=0;
		for (const TCHAR* Expected : {C.Reachable,C.Pathable,C.Visible})
		{
			FWoTMovementBinding B; FString Error; const int32 Index = N==0 ? 21 : N==1 ? 23 : 1;
			TestTrue(*FString::Printf(TEXT("decode %s handler%d"),C.Factory,Index), FWoTMovementBinding::Decode(*Defaults,FString(TEXT("WoTPawns.RangeHandlerFactory"))+C.Factory,Index,N==2,B,Error));
			TestEqual(*FString::Printf(TEXT("%s binding%d"),C.Factory,Index), B.Pattern.ClassIdentity,FName(Expected)); ++N;
		}
	}
	for (FName Row : {FName(TEXT("Sister")),FName(TEXT("Sitter")),FName(TEXT("TrollocStrong"))})
	{
		const auto* R = WoTCreatureData::Find(Row); TestNotNull(TEXT("inherited creature row"),R);
		if (R) TestEqual(TEXT("inherited reachable pattern"), R->MovementBindings[0].Pattern.ClassIdentity,
			Row==TEXT("TrollocStrong") ? FName(TEXT("WoTPawns.MovementPatternTrolloc0")) : FName(TEXT("WoTPawns.MovementPatternAngrealUser0")));
	}
	FWoTMovementPattern P; FString Error;
	TestTrue(TEXT("authored unbound Warder0 remains decodable"),FWoTMovementPattern::Decode(*Defaults,TEXT("WoTPawns.MovementPatternWarder0"),P,Error));
	FWoTMovementPattern::Decode(*Defaults,TEXT("WoTPawns.MovementPatternMyrddraal0"),P,Error);
	TestFalse(TEXT("disabled Myrddraal cell1 retained"),P.Elements[1].bEnabled);
	TestEqual(TEXT("disabled cell still has authored speed650"),P.Elements[1].Speed,650.0f);
	TestFalse(TEXT("last cell remains disabled"),P.Elements[15].bEnabled);
	TestTrue(TEXT("last cell inherits require reachable"),P.Elements[15].Waypoint.bRequireReachable);
	FWoTMovementPattern::Decode(*Defaults,TEXT("WoTPawns.MovementPatternTrolloc0"),P,Error);
	TestTrue(TEXT("child packed vector overrides inherited zero vector"),P.Elements[1].Waypoint.Direction.Equals(FVector(-1,-1,0)));
	TestTrue(TEXT("opposite diagonal retained independently"),P.Elements[2].Waypoint.Direction.Equals(FVector(-1,1,0)));
	struct FClassCells { const TCHAR* Leaf; uint16 EnabledMask; };
	const FClassCells Classes[] = {
		{TEXT("AngrealUser0"),15},{TEXT("Archer0"),31},{TEXT("BATrolloc0"),3},
		{TEXT("Legion0"),1},{TEXT("Myrddraal0"),29},{TEXT("Myrddraal1"),3},
		{TEXT("Questioner0"),31},{TEXT("Questioner1"),7},{TEXT("Soldier0"),31},
		{TEXT("Trolloc0"),31},{TEXT("Trolloc1"),7},{TEXT("TrollocMed0"),1},
		{TEXT("TrollocMed1"),7},{TEXT("TrollocWeak0"),31},{TEXT("Warder0"),31}
	};
	for(const auto& C:Classes)
	{
		TestTrue(*FString::Printf(TEXT("decode every authored class %s"),C.Leaf),FWoTMovementPattern::Decode(*Defaults,FString(TEXT("WoTPawns.MovementPattern"))+C.Leaf,P,Error));
		uint16 Mask=0; for(int32 I=0;I<16;++I) if(P.Elements[I].bEnabled) Mask|=1u<<I;
		TestEqual(*FString::Printf(TEXT("%s retains exact enabled/disabled indices"),C.Leaf),Mask,C.EnabledMask);
	}
	return !HasAnyErrors();
}
#endif
#endif
