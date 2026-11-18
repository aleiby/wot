#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTFrontEndTestData.h"
#include "../FrontEnd/WoTFrontEndGroundTrace.h"
#include "../Payload/WoTNeutralResources.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTBSPPayload.h"
#include "../WoTPayloadSubsystem.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include <limits>

namespace
{
constexpr uint8 EntryNF_NotCsg = 0x01;
constexpr uint8 EntryNF_IsNew = 0x20;

FWoTRuntimeWorldNode MakeHorizontalNode(
	double Z, uint8 Flags = 0, uint8 VertexCount = 4)
{
	FWoTRuntimeWorldNode Node;
	Node.Plane[0] = 0.0f;
	Node.Plane[1] = 0.0f;
	Node.Plane[2] = 1.0f;
	Node.Plane[3] = static_cast<float>(Z);
	Node.Flags = Flags;
	Node.VertexCount = VertexCount;
	return Node;
}

bool LoadAuthenticatedBSP(const FString& LogicalId,
	TSharedPtr<const FWoTBSPPayloadData>& BSP, FString& Error)
{
	auto Object = FWoTRuntimeRegistry::Get().ReadObject(
		{LogicalId, EWoTNeutralResourceKind::BSP},
		1024ull * 1024ull * 1024ull,
		TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
	if (Object.IsError())
	{
		Error = MoveTemp(Object).GetError().ToString();
		return false;
	}
	TSharedRef<FWoTBSPPayloadData> Decoded = MakeShared<FWoTBSPPayloadData>();
	if (!UWoTBSPPayloadLibrary::DecodePayload(
		Object.GetValue().Payload(), *Decoded, Error))
	{
		return false;
	}
	BSP = Decoded;
	return true;
}

const FWoTRuntimeWorldProperty* FindProperty(
	const FWoTRuntimeWorldActor& Actor, FStringView Name)
{
	for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
	{
		if (Property.ArrayIndex == 0
			&& FStringView(Property.Name).Equals(Name, ESearchCase::IgnoreCase))
		{
			return &Property;
		}
	}
	return nullptr;
}

bool ReadVectorProperty(const FWoTRuntimeWorldActor& Actor,
	FStringView Name, FVector& Out)
{
	const FWoTRuntimeWorldProperty* Property = FindProperty(Actor, Name);
	if (Property == nullptr
		|| Property->Type != EWoTRuntimeWorldPropertyType::Struct
		|| !Property->StructName.Equals(TEXT("Vector"), ESearchCase::IgnoreCase)
		|| Property->Value.Num() != 3 * sizeof(float))
	{
		return false;
	}
	float Values[3];
	FMemory::Memcpy(Values, Property->Value.GetData(), sizeof(Values));
	if (!FMath::IsFinite(Values[0]) || !FMath::IsFinite(Values[1])
		|| !FMath::IsFinite(Values[2]))
	{
		return false;
	}
	Out = FVector(Values[0], Values[1], Values[2]);
	return true;
}

bool ReadFloatProperty(const FWoTRuntimeWorldActor& Actor,
	FStringView Name, float& Out)
{
	const FWoTRuntimeWorldProperty* Property = FindProperty(Actor, Name);
	if (Property == nullptr
		|| Property->Type != EWoTRuntimeWorldPropertyType::Float
		|| Property->Value.Num() != sizeof(float))
	{
		return false;
	}
	FMemory::Memcpy(&Out, Property->Value.GetData(), sizeof(Out));
	return FMath::IsFinite(Out);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEntryGroundTraceSemanticsTest,
	"WoT.FrontEnd.GroundTrace.SolidSpaceSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEntryGroundTraceSemanticsTest::RunTest(const FString&)
{
	FString Error;
	FVector Hit(111.0, 222.0, 333.0);
	const TArray<FWoTRuntimeWorldNode> Floor = {MakeHorizontalNode(10.0)};
	TestEqual(TEXT("solid floor is the first outside-to-solid boundary"),
		WoTTraceEntryGround(Floor, false, FVector(0, 0, 500),
			FVector(0, 0, -500), Hit, Error),
		EWoTEntryTraceResult::Hit);
	TestTrue(TEXT("floor hit is the raw plane boundary"),
		Hit.Equals(FVector(0, 0, 10), 0.0001));
	TestTrue(TEXT("valid hit has no diagnostic"), Error.IsEmpty());

	Hit = FVector::ZeroVector;
	TestEqual(TEXT("rotated segment crosses the same solid boundary"),
		WoTTraceEntryGround(Floor, false, FVector(100, 0, 500),
			FVector(-100, 0, -500), Hit, Error),
		EWoTEntryTraceResult::Hit);
	TestTrue(TEXT("rotated hit uses segment interpolation"),
		Hit.Equals(FVector(2, 0, 10), 0.0001));

	for (const TPair<uint8, uint8>& NonSolid : {
		TPair<uint8, uint8>(EntryNF_NotCsg, 4),
		TPair<uint8, uint8>(EntryNF_IsNew, 4),
		TPair<uint8, uint8>(0, 0)})
	{
		TArray<FWoTRuntimeWorldNode> PortalThenFloor = {
			MakeHorizontalNode(20.0, NonSolid.Key, NonSolid.Value),
			MakeHorizontalNode(10.0)};
		PortalThenFloor[0].Back = 1;
		Hit = FVector::ZeroVector;
		TestEqual(TEXT("non-solid split does not become the hit"),
			WoTTraceEntryGround(PortalThenFloor, true, FVector(0, 0, 500),
				FVector(0, 0, -500), Hit, Error),
			EWoTEntryTraceResult::Hit);
		TestTrue(TEXT("trace continues through non-solid split to floor"),
			Hit.Equals(FVector(0, 0, 10), 0.0001));
	}

	TestEqual(TEXT("outside endpoint exactly on the plane remains clear"),
		WoTTraceEntryGround(Floor, false, FVector(0, 0, 500),
			FVector(0, 0, 10), Hit, Error),
		EWoTEntryTraceResult::Miss);
	TestEqual(TEXT("starting on the plane toward solid hits at start"),
		WoTTraceEntryGround(Floor, false, FVector(0, 0, 10),
			FVector(0, 0, 0), Hit, Error),
		EWoTEntryTraceResult::Hit);
	TestTrue(TEXT("on-plane solid hit is the segment start"),
		Hit.Equals(FVector(0, 0, 10), 0.0001));
	TestEqual(TEXT("segment wholly above the floor misses"),
		WoTTraceEntryGround(Floor, false, FVector(0, 0, 500),
			FVector(0, 0, 20), Hit, Error),
		EWoTEntryTraceResult::Miss);
	TestEqual(TEXT("segment starting in solid hits immediately"),
		WoTTraceEntryGround(Floor, false, FVector(0, 0, 0),
			FVector(0, 0, 500), Hit, Error),
		EWoTEntryTraceResult::Hit);
	TestTrue(TEXT("start-inside hit is the segment start"),
		Hit.Equals(FVector(0, 0, 0), 0.0001));
	TestEqual(TEXT("zero-length outside segment misses"),
		WoTTraceEntryGround(Floor, false, FVector(0, 0, 20),
			FVector(0, 0, 20), Hit, Error),
		EWoTEntryTraceResult::Miss);
	TestEqual(TEXT("zero-length inside segment hits at start"),
		WoTTraceEntryGround(Floor, false, FVector(0, 0, 0),
			FVector(0, 0, 0), Hit, Error),
		EWoTEntryTraceResult::Hit);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEntryGroundTraceInvalidTest,
	"WoT.FrontEnd.GroundTrace.RejectsMalformedTraversal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEntryGroundTraceInvalidTest::RunTest(const FString&)
{
	FString Error;
	FVector Hit = FVector::ZeroVector;
	TArray<FWoTRuntimeWorldNode> Invalid = {MakeHorizontalNode(10.0)};
	Invalid[0].Front = 7;
	TestEqual(TEXT("out-of-range child is invalid"),
		WoTTraceEntryGround(Invalid, false, FVector(0, 0, 20),
			FVector(0, 0, 30), Hit, Error),
		EWoTEntryTraceResult::Invalid);
	TestFalse(TEXT("invalid child reports a diagnostic"), Error.IsEmpty());

	Invalid = {MakeHorizontalNode(10.0)};
	Invalid[0].Plane[2] = 0.0f;
	TestEqual(TEXT("degenerate plane is invalid"),
		WoTTraceEntryGround(Invalid, false, FVector(0, 0, 20),
			FVector(0, 0, -20), Hit, Error),
		EWoTEntryTraceResult::Invalid);

	Invalid = {MakeHorizontalNode(10.0)};
	Invalid[0].Front = 0;
	const double StartedAt = FPlatformTime::Seconds();
	TestEqual(TEXT("reachable BSP cycle is invalid"),
		WoTTraceEntryGround(Invalid, false, FVector(0, 0, 20),
			FVector(0, 0, 30), Hit, Error),
		EWoTEntryTraceResult::Invalid);
	TestTrue(TEXT("cycle rejection is bounded"),
		FPlatformTime::Seconds() - StartedAt < 1.0);

	const double NaN = std::numeric_limits<double>::quiet_NaN();
	Invalid = {MakeHorizontalNode(10.0)};
	TestEqual(TEXT("nonfinite segment is invalid"),
		WoTTraceEntryGround(Invalid, false, FVector(NaN, 0, 20),
			FVector(0, 0, -20), Hit, Error),
		EWoTEntryTraceResult::Invalid);
	Invalid[0].Plane[3] = std::numeric_limits<float>::infinity();
	TestEqual(TEXT("nonfinite plane is invalid"),
		WoTTraceEntryGround(Invalid, false, FVector(0, 0, 20),
			FVector(0, 0, -20), Hit, Error),
		EWoTEntryTraceResult::Invalid);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTEntryGroundTraceRealEntryTest,
	"WoT.FrontEnd.GroundTrace.RealRetailRainAnchors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTEntryGroundTraceRealEntryTest::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString EntryPath = WoTReferencePath(TEXT("original/Maps/WheelOfTime.wot"));
	if (!FPaths::FileExists(EntryPath))
	{
		AddError(TEXT("required original/Maps/WheelOfTime.wot is missing"));
		return false;
	}
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FString Error;
	if (!TestTrue(TEXT("genuine entry imports into authenticated weather data"),
		WoTFrontEndTest_BuildRealWeatherStageFixture(Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTRuntimeWorldResource World;
	FWoTBootstrapStatus Loaded = World.LoadFirstUse({
		Fixture.Descriptor.WorldLogicalId, EWoTNeutralResourceKind::World});
	if (!TestFalse(TEXT("genuine world authenticates and decodes"),
		Loaded.IsError()))
	{
		if (Loaded.IsError()) AddError(MoveTemp(Loaded).GetError().ToString());
		return false;
	}
	TSharedPtr<const FWoTBSPPayloadData> BSP;
	if (!TestTrue(TEXT("genuine BSP authenticates and decodes"),
		LoadAuthenticatedBSP(Fixture.Descriptor.BspLogicalId, BSP, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestNotNull(TEXT("genuine BSP payload exists"), BSP.Get())) return false;
	TestFalse(TEXT("genuine entry root is serialized inside"), BSP->bRootOutside);
	TestEqual(TEXT("genuine entry keeps 999 collision nodes"),
		World.Nodes().Num(), 999);

	struct FExpectedRain
	{
		const TCHAR* Name;
		FVector Start;
	};
	const FExpectedRain Expected[] = {
		{TEXT("RainCyd0"), FVector(393.09747314453125, 178.79849243164062,
			646.5062866210938)},
		{TEXT("RainCyd1"), FVector(380.20367431640625, 40.1873779296875,
			646.5062866210938)},
		{TEXT("RainCyd2"), FVector(211.87644958496094, 29.324295043945312,
			646.5062866210938)},
		{TEXT("RainCyd3"), FVector(253.34994506835938, 152.35409545898438,
			646.5062866210938)},
		{TEXT("RainCyd4"), FVector(448.4789123535156, 18.203044891357422,
			646.5062866210938)},
		{TEXT("RainCyd5"), FVector(175.38243103027344, 188.05523681640625,
			646.5062866210938)},
	};
	int32 Traced = 0;
	for (const FExpectedRain& Reference : Expected)
	{
		const FWoTRuntimeWorldActor* Actor = World.Actors().FindByPredicate(
			[&Reference](const FWoTRuntimeWorldActor& Candidate)
			{
				return Candidate.Identity.EndsWith(Reference.Name)
					&& Candidate.ClassIdentity.EndsWith(TEXT(".RainCyd"));
			});
		if (!TestNotNull(*FString::Printf(TEXT("actual %s exists"), Reference.Name),
			Actor)) continue;
		FVector Start;
		float TraceDepth = 0.0f;
		if (!TestTrue(*FString::Printf(TEXT("actual %s location decodes"),
			Reference.Name), ReadVectorProperty(*Actor, TEXT("Location"), Start))
			|| !TestTrue(*FString::Printf(TEXT("actual %s trace depth decodes"),
				Reference.Name), ReadFloatProperty(
					*Actor, TEXT("TraceDepth"), TraceDepth)))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("actual %s authored start is literal"),
			Reference.Name), Start.Equals(Reference.Start, 0.0001));
		TestEqual(*FString::Printf(TEXT("actual %s trace depth is literal"),
			Reference.Name), TraceDepth, 5000.0f);
		FVector Hit = FVector::ZeroVector;
		Error.Reset();
		const EWoTEntryTraceResult Result = WoTTraceEntryGround(
			World.Nodes(), BSP->bRootOutside, Start,
			Start + FVector(0, 0, -TraceDepth), Hit, Error);
		TestEqual(*FString::Printf(TEXT("actual %s reaches BSP ground"),
			Reference.Name), Result, EWoTEntryTraceResult::Hit);
		const FVector LiteralRawBoundary(
			Reference.Start.X, Reference.Start.Y, -56.0);
		// The source oracle uses legacy float32 partitioning while the runtime
		// helper uses FVector's modern precision. One thousandth of a uu covers
		// those arithmetic ULPs without admitting the 0.5uu wrapper pullback.
		TestTrue(*FString::Printf(TEXT("actual %s raw boundary matches source"),
			Reference.Name), Hit.Equals(LiteralRawBoundary, 0.001));
		TestTrue(*FString::Printf(TEXT("actual %s trace has no diagnostic"),
			Reference.Name), Error.IsEmpty());
		++Traced;
	}
	TestEqual(TEXT("all six authenticated rain actors execute the runtime helper"),
		Traced, 6);
	return !HasAnyErrors();
}

#endif
