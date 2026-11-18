#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTFrontEndTestData.h"
#include "../FrontEnd/WoTFrontEndSky.h"
#include "../Payload/WoTNeutralResources.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTRuntimeWorldResource.h"
#include "../WoTPayloadSubsystem.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#include "Misc/Paths.h"

#include <limits>

namespace
{
bool LoadSkyAuthenticatedBSP(const FString& LogicalId,
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

bool LoadWeatherFixture(FWoTFrontEndStageProductionFixture& Fixture,
	const FWoTFrontEndWeatherFixtureOptions& Options,
	FWoTRuntimeWorldResource& World,
	TSharedPtr<const FWoTBSPPayloadData>& BSP, FString& Error)
{
	if (!WoTFrontEndTest_BuildWeatherStageFixture(Fixture, Options, Error))
		return false;
	FWoTBootstrapStatus Loaded = World.LoadFirstUse({
		Fixture.Descriptor.WorldLogicalId, EWoTNeutralResourceKind::World});
	if (Loaded.IsError())
	{
		Error = MoveTemp(Loaded).GetError().ToString();
		return false;
	}
	return LoadSkyAuthenticatedBSP(Fixture.Descriptor.BspLogicalId, BSP, Error);
}

bool HasContiguousSections(const FWoTBSPPayloadData& Mesh)
{
	uint32 ExpectedFirst = 0;
	for (const FWoTBSPPayloadSection& Section : Mesh.Sections)
	{
		if (Section.FirstIndex != ExpectedFirst || Section.IndexCount == 0
			|| Section.IndexCount % 3 != 0 || Section.MinVertex > Section.MaxVertex
			|| Section.MaxVertex >= static_cast<uint32>(Mesh.Positions.Num()))
		{
			return false;
		}
		ExpectedFirst += Section.IndexCount;
	}
	return ExpectedFirst == static_cast<uint32>(Mesh.Indices.Num());
}

bool ReadSkyFloatProperty(const FWoTRuntimeWorldActor& Actor,
	FStringView Name, float& Out)
{
	for (const FWoTRuntimeWorldProperty& Property : Actor.Properties)
	{
		if (FStringView(Property.Name).Equals(Name, ESearchCase::IgnoreCase)
			&& Property.Type == EWoTRuntimeWorldPropertyType::Float
			&& Property.Value.Num() == sizeof(float))
		{
			FMemory::Memcpy(&Out, Property.Value.GetData(), sizeof(float));
			return FMath::IsFinite(Out);
		}
	}
	return false;
}

int32 FindTriangleForSurface(const FWoTBSPPayloadData& Source,
	uint32 Surface)
{
	return Source.TriangleSurfaces.IndexOfByKey(Surface);
}

bool AppendTriangleForSurface(const FWoTBSPPayloadData& Source,
	uint32 Surface, FWoTBSPPayloadData& Target)
{
	const int32 Triangle = FindTriangleForSurface(Source, Surface);
	if (Triangle == INDEX_NONE) return false;
	for (int32 Corner = 0; Corner < 3; ++Corner)
	{
		Target.Indices.Add(Source.Indices[Triangle * 3 + Corner]);
	}
	Target.TriangleSurfaces.Add(Surface);
	return true;
}

void AppendSection(FWoTBSPPayloadData& Target, uint32 FirstIndex)
{
	FWoTBSPPayloadSection& Section = Target.Sections.AddDefaulted_GetRef();
	Section.MaterialSlot = TEXT("DerivedBoundsFixture");
	Section.FirstIndex = FirstIndex;
	Section.IndexCount = static_cast<uint32>(Target.Indices.Num()) - FirstIndex;
	Section.MinVertex = 0;
	Section.MaxVertex = static_cast<uint32>(Target.Positions.Num() - 1);
	Section.bCollisionEnabled = true;
}

bool IsPlanOutputEmpty(const FWoTFrontEndSkyPlan& Plan)
{
	return Plan.WorldMesh.Positions.IsEmpty()
		&& Plan.WorldMesh.Indices.IsEmpty()
		&& Plan.WorldMesh.Sections.IsEmpty()
		&& Plan.WorldStyles.IsEmpty()
		&& Plan.SkyMesh.Positions.IsEmpty()
		&& Plan.SkyMesh.Indices.IsEmpty()
		&& Plan.SkyMesh.Sections.IsEmpty()
		&& Plan.SkyStyles.IsEmpty();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyPartitionTest,
	"WoT.FrontEnd.Sky.PartitionsCanonicalWeatherFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyPartitionTest::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bSharedSkyTexture = true;
	FWoTRuntimeWorldResource World;
	TSharedPtr<const FWoTBSPPayloadData> BSP;
	FString Error;
	if (!TestTrue(TEXT("canonical weather fixture decodes through production paths"),
		LoadWeatherFixture(Fixture, Options, World, BSP, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("canonical fixture is v3"), BSP->FormatVersion,
		static_cast<uint16>(3))) return false;
	TestEqual(TEXT("canonical fixture has four source triangles"),
		BSP->TriangleSurfaces.Num(), 4);
	TestEqual(TEXT("canonical fixture has four world surfaces"),
		World.Surfaces().Num(), 4);
	TestEqual(TEXT("canonical fixture has three zones"), World.Zones().Num(), 3);

	TArray<float> RainScaleGlow;
	for (const FWoTRuntimeWorldActor& Actor : World.Actors())
	{
		if (!Actor.ClassIdentity.EndsWith(TEXT(".RainCyd"))) continue;
		float Value = 0.0f;
		if (ReadSkyFloatProperty(Actor, TEXT("ScaleGlow"), Value))
			RainScaleGlow.Add(Value);
	}
	TestTrue(TEXT("canonical fixture retains all six authored rain glow values"),
		RainScaleGlow == Options.RainScaleGlow);

	FWoTFrontEndSkyPlan Plan;
	if (!TestTrue(TEXT("valid decoded weather fixture builds a sky plan"),
		WoTBuildFrontEndSkyPlan(World, *BSP, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("world keeps wall and ground provenance"),
		Plan.WorldMesh.TriangleSurfaces == TArray<uint32>({0, 3}));
	TestTrue(TEXT("sky keeps only its zone face provenance"),
		Plan.SkyMesh.TriangleSurfaces == TArray<uint32>({1}));
	TestFalse(TEXT("world does not draw the FakeBackdrop window"),
		Plan.WorldMesh.TriangleSurfaces.Contains(2));
	TestFalse(TEXT("sky does not draw the FakeBackdrop window"),
		Plan.SkyMesh.TriangleSurfaces.Contains(2));
	TestTrue(TEXT("world sections and index ranges are contiguous"),
		HasContiguousSections(Plan.WorldMesh));
	TestTrue(TEXT("sky sections and index ranges are contiguous"),
		HasContiguousSections(Plan.SkyMesh));
	TestTrue(TEXT("world indices are dense"),
		Plan.WorldMesh.Indices == TArray<uint32>({0, 1, 2, 3, 4, 5}));
	TestTrue(TEXT("sky indices are dense"),
		Plan.SkyMesh.Indices == TArray<uint32>({0, 1, 2}));

	const TArray<FVector2f> ExpectedSkyUV0 = {
		{0.2f, 0.3f}, {0.8f, 0.3f}, {0.5f, 0.9f}};
	const TArray<FVector2f> ExpectedSkyUV1 = {
		{0.11f, 0.12f}, {0.21f, 0.22f}, {0.31f, 0.32f}};
	TestTrue(TEXT("sky texture UVs remain exact"), Plan.SkyMesh.UV0 == ExpectedSkyUV0);
	TestTrue(TEXT("sky lightmap UVs remain exact"), Plan.SkyMesh.UV1 == ExpectedSkyUV1);
	TestTrue(TEXT("world texture UVs begin with the exact wall UVs"),
		Plan.WorldMesh.UV0.Num() >= 3
		&& Plan.WorldMesh.UV0[0] == FVector2f(0.125f, 0.25f)
		&& Plan.WorldMesh.UV0[1] == FVector2f(0.625f, 0.25f)
		&& Plan.WorldMesh.UV0[2] == FVector2f(0.125f, 0.75f));
	TestTrue(TEXT("both partitions retain the exact atlas bytes"),
		Plan.WorldMesh.LightmapBGRA == BSP->LightmapBGRA
		&& Plan.SkyMesh.LightmapBGRA == BSP->LightmapBGRA);
	if (TestEqual(TEXT("sky has one section style"), Plan.SkyStyles.Num(), 1)
		&& TestEqual(TEXT("world has one section style"), Plan.WorldStyles.Num(), 1))
	{
		TestEqual(TEXT("shared texture identity is preserved on the wall"),
			Plan.WorldStyles[0].TextureIdentity, World.Surfaces()[0].TextureIdentity);
		TestEqual(TEXT("shared texture identity is preserved on the sky"),
			Plan.SkyStyles[0].TextureIdentity, World.Surfaces()[1].TextureIdentity);
		TestTrue(TEXT("fixture proves sky and wall share a texture identity"),
			Plan.WorldStyles[0].TextureIdentity == Plan.SkyStyles[0].TextureIdentity);
		TestTrue(TEXT("sky pan is the exact authenticated v3 rate"),
			Plan.SkyStyles[0].PanUV == FVector2f(0.01f, -0.02f));
		TestTrue(TEXT("sky authored two-sided flag is retained"),
			Plan.SkyStyles[0].bTwoSided);
	}
	TestTrue(TEXT("sky origin is decoded from the winning actor"),
		Plan.Origin == FVector::ZeroVector);
	TestEqual(TEXT("sky yaw is decoded from UE1 rotation units"),
		Plan.Rotation.Yaw, 90.0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyV4EntryTest,
	"WoT.FrontEnd.Sky.AcceptsIdentityBearingV4Entry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyV4EntryTest::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bBSPV4 = true;
	Options.bSharedSkyTexture = true;
	FWoTRuntimeWorldResource World;
	TSharedPtr<const FWoTBSPPayloadData> BSP;
	FString Error;
	if (!TestTrue(TEXT("v4 entry fixture decodes through production paths"),
		LoadWeatherFixture(Fixture, Options, World, BSP, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("identity-bearing entry is v4"), BSP->FormatVersion,
		static_cast<uint16>(4));
	FWoTFrontEndSkyPlan Plan;
	if (!TestTrue(TEXT("v4 entry builds the canonical sky plan"),
		WoTBuildFrontEndSkyPlan(World, *BSP, Plan, Error)))
	{
		AddError(Error);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyGroupingStabilityTest,
	"WoT.FrontEnd.Sky.PreservesStableGroupingAndSignedZeroEquality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyGroupingStabilityTest::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bSharedSkyTexture = true;
	FWoTRuntimeWorldResource World;
	TSharedPtr<const FWoTBSPPayloadData> Decoded;
	FString Error;
	if (!TestTrue(TEXT("grouping fixture decodes"),
		LoadWeatherFixture(Fixture, Options, World, Decoded, Error)))
	{
		AddError(Error);
		return false;
	}

	FWoTBSPPayloadData DistinctPans = *Decoded;
	DistinctPans.EntrySurfaces[0].PanUV = FVector2f(0.25f, -0.5f);
	DistinctPans.EntrySurfaces[3].PanUV = FVector2f(-0.75f, 0.125f);
	FWoTFrontEndSkyPlan DistinctPlan;
	if (TestTrue(TEXT("distinct pans build a plan"),
		WoTBuildFrontEndSkyPlan(World, DistinctPans, DistinctPlan, Error)))
	{
		TestTrue(TEXT("grouping preserves source triangle order"),
			DistinctPlan.WorldMesh.TriangleSurfaces == TArray<uint32>({0, 3}));
		if (TestEqual(TEXT("distinct pans produce two first-seen groups"),
			DistinctPlan.WorldStyles.Num(), 2))
		{
			TestTrue(TEXT("the first group retains the first pan"),
				DistinctPlan.WorldStyles[0].PanUV == FVector2f(0.25f, -0.5f));
			TestTrue(TEXT("the second group retains the second pan"),
				DistinctPlan.WorldStyles[1].PanUV == FVector2f(-0.75f, 0.125f));
		}
	}

	FWoTBSPPayloadData SignedZeros = *Decoded;
	SignedZeros.EntrySurfaces[0].PanUV = FVector2f(0.0f, -0.0f);
	SignedZeros.EntrySurfaces[3].PanUV = FVector2f(-0.0f, 0.0f);
	FWoTFrontEndSkyPlan SignedZeroPlan;
	if (TestTrue(TEXT("signed-zero pans build a plan"),
		WoTBuildFrontEndSkyPlan(World, SignedZeros, SignedZeroPlan, Error)))
	{
		TestEqual(TEXT("signed zeros remain one equality group"),
			SignedZeroPlan.WorldStyles.Num(), 1);
		TestTrue(TEXT("signed-zero grouping preserves triangle order"),
			SignedZeroPlan.WorldMesh.TriangleSurfaces == TArray<uint32>({0, 3}));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyDerivedBoundsTest,
	"WoT.FrontEnd.Sky.RejectsDerivedGeometryOverRendererBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyDerivedBoundsTest::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bSharedSkyTexture = true;
	FWoTRuntimeWorldResource World;
	TSharedPtr<const FWoTBSPPayloadData> Decoded;
	FString Error;
	if (!TestTrue(TEXT("derived-bounds fixture decodes"),
		LoadWeatherFixture(Fixture, Options, World, Decoded, Error)))
	{
		AddError(Error);
		return false;
	}

	FWoTBSPPayloadData TooManySections = *Decoded;
	TooManySections.Indices.Reset();
	TooManySections.TriangleSurfaces.Reset();
	TooManySections.Sections.Reset();
	TooManySections.EntrySurfaces[0].PanUV = FVector2f(0.25f, 0.0f);
	TooManySections.EntrySurfaces[3].PanUV = FVector2f(-0.25f, 0.0f);
	for (int32 SectionIndex = 0; SectionIndex < 2048; ++SectionIndex)
	{
		const uint32 FirstIndex = static_cast<uint32>(TooManySections.Indices.Num());
		if (!AppendTriangleForSurface(*Decoded, 0, TooManySections)
			|| !AppendTriangleForSurface(*Decoded, 3, TooManySections))
		{
			AddError(TEXT("canonical fixture lacks a required world triangle"));
			return false;
		}
		AppendSection(TooManySections, FirstIndex);
	}
	{
		const uint32 FirstIndex = static_cast<uint32>(TooManySections.Indices.Num());
		if (!AppendTriangleForSurface(*Decoded, 0, TooManySections)
			|| !AppendTriangleForSurface(*Decoded, 1, TooManySections))
		{
			AddError(TEXT("canonical fixture lacks a required final triangle"));
			return false;
		}
		AppendSection(TooManySections, FirstIndex);
	}
	FWoTFrontEndSkyPlan SectionPlan;
	SectionPlan.WorldMesh.Positions.Add(FVector3f(1.0f));
	SectionPlan.WorldStyles.AddDefaulted();
	TestFalse(TEXT("more than 4096 derived sections are rejected"),
		WoTBuildFrontEndSkyPlan(World, TooManySections, SectionPlan, Error));
	TestTrue(TEXT("derived-section rejection identifies the preflight bound"),
		Error.Contains(TEXT("derived section"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("derived-section rejection publishes no partial output"),
		IsPlanOutputEmpty(SectionPlan));

	FWoTBSPPayloadData TooManyVertices = *Decoded;
	TooManyVertices.Indices.Reset();
	TooManyVertices.TriangleSurfaces.Reset();
	TooManyVertices.Sections.Reset();
	const uint32 FirstIndex = 0;
	TooManyVertices.Indices.Reserve(2000004);
	TooManyVertices.TriangleSurfaces.Reserve(666668);
	for (int32 TriangleIndex = 0; TriangleIndex < 666667; ++TriangleIndex)
	{
		if (!AppendTriangleForSurface(*Decoded, 0, TooManyVertices))
		{
			AddError(TEXT("canonical fixture lacks the repeated world triangle"));
			return false;
		}
	}
	if (!AppendTriangleForSurface(*Decoded, 1, TooManyVertices))
	{
		AddError(TEXT("canonical fixture lacks the retained sky triangle"));
		return false;
	}
	AppendSection(TooManyVertices, FirstIndex);
	FWoTFrontEndSkyPlan VertexPlan;
	VertexPlan.SkyMesh.Indices.Add(17);
	VertexPlan.SkyStyles.AddDefaulted();
	Error.Reset();
	TestFalse(TEXT("more than 2000000 expanded vertices are rejected"),
		WoTBuildFrontEndSkyPlan(World, TooManyVertices, VertexPlan, Error));
	TestTrue(TEXT("expanded-vertex rejection identifies the preflight bound"),
		Error.Contains(TEXT("expanded vertex"), ESearchCase::IgnoreCase));
	TestTrue(TEXT("expanded-vertex rejection publishes no partial output"),
		IsPlanOutputEmpty(VertexPlan));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyScaleTest,
	"WoT.FrontEnd.Sky.ScaleUsesFaceDistance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyScaleTest::RunTest(const FString&)
{
	TestEqual(TEXT("minimum shell scale"), WoTEntrySkyScale(10.f, 100.f), 12.f);
	TestEqual(TEXT("face bound, not vertex bound"),
		WoTEntrySkyScale(1000.f, 10.f), 250.f);
	TestEqual(TEXT("nonpositive world radius is rejected"),
		WoTEntrySkyScale(0.f, 10.f), 0.f);
	TestEqual(TEXT("nonpositive face distance is rejected"),
		WoTEntrySkyScale(10.f, 0.f), 0.f);
	TestEqual(TEXT("nonfinite input is rejected"),
		WoTEntrySkyScale(std::numeric_limits<float>::quiet_NaN(), 10.f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyGeometrySafetyTest,
	"WoT.FrontEnd.Sky.RejectsUnsafeShellGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyGeometrySafetyTest::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bSharedSkyTexture = true;
	FWoTRuntimeWorldResource World;
	TSharedPtr<const FWoTBSPPayloadData> Decoded;
	FString Error;
	if (!TestTrue(TEXT("geometry safety fixture decodes"),
		LoadWeatherFixture(Fixture, Options, World, Decoded, Error)))
	{
		AddError(Error);
		return false;
	}

	FWoTBSPPayloadData LargeFace = *Decoded;
	LargeFace.Positions[3] = FVector3f(-1000, -1000, 10);
	LargeFace.Positions[4] = FVector3f(1000, -1000, 10);
	LargeFace.Positions[5] = FVector3f(0, 1000, 10);
	FWoTFrontEndSkyPlan LargePlan;
	TestTrue(TEXT("a large face is measured at its interior, not its vertices"),
		WoTBuildFrontEndSkyPlan(World, LargeFace, LargePlan, Error));
	TestEqual(TEXT("large face produces the Ericson face-distance scale"),
		LargePlan.Scale, 36.0f);

	FWoTBSPPayloadData Reversed = LargeFace;
	Swap(Reversed.Indices[4], Reversed.Indices[5]);
	FWoTFrontEndSkyPlan ReversedPlan;
	if (TestTrue(TEXT("reversed winding remains valid"),
		WoTBuildFrontEndSkyPlan(World, Reversed, ReversedPlan, Error)))
	{
		TestTrue(TEXT("reversed winding order is preserved"),
			ReversedPlan.SkyMesh.Positions == TArray<FVector3f>({
				LargeFace.Positions[3], LargeFace.Positions[5], LargeFace.Positions[4]}));
	}

	FWoTBSPPayloadData OriginOnFace = LargeFace;
	OriginOnFace.Positions[3].Z = 0;
	OriginOnFace.Positions[4].Z = 0;
	OriginOnFace.Positions[5].Z = 0;
	FWoTFrontEndSkyPlan Rejected;
	TestFalse(TEXT("origin on a sky face is rejected"),
		WoTBuildFrontEndSkyPlan(World, OriginOnFace, Rejected, Error));

	FWoTBSPPayloadData EmptySky = *Decoded;
	EmptySky.TriangleSurfaces[1] = 0;
	TestFalse(TEXT("empty extracted sky is rejected"),
		WoTBuildFrontEndSkyPlan(World, EmptySky, Rejected, Error));

	FWoTBSPPayloadData Nonfinite = *Decoded;
	Nonfinite.Positions[3].X = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("nonfinite geometry is rejected"),
		WoTBuildFrontEndSkyPlan(World, Nonfinite, Rejected, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyRequiresV3Test,
	"WoT.FrontEnd.Sky.RequiresAuthenticatedV3Provenance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyRequiresV3Test::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"),
		EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	FWoTFrontEndWeatherFixtureOptions Options;
	Options.bLegacyV2 = true;
	Options.bSharedSkyTexture = true;
	FWoTRuntimeWorldResource World;
	TSharedPtr<const FWoTBSPPayloadData> BSP;
	FString Error;
	if (!TestTrue(TEXT("legacy fixture itself remains decodable"),
		LoadWeatherFixture(Fixture, Options, World, BSP, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("legacy fixture is version 2"), BSP->FormatVersion,
		static_cast<uint16>(2));
	FWoTFrontEndSkyPlan Plan;
	TestFalse(TEXT("sky staging rejects provenance-free v2"),
		WoTBuildFrontEndSkyPlan(World, *BSP, Plan, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFrontEndSkyRealEntryTest,
	"WoT.FrontEnd.Sky.RealRetailEntryMembership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTFrontEndSkyRealEntryTest::RunTest(const FString&)
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
	if (!TestTrue(TEXT("the genuine entry imports into an authenticated fixture"),
		WoTFrontEndTest_BuildRealWeatherStageFixture(Fixture, Error)))
	{
		AddError(Error);
		return false;
	}
	FWoTRuntimeWorldResource World;
	FWoTBootstrapStatus Loaded = World.LoadFirstUse({
		Fixture.Descriptor.WorldLogicalId, EWoTNeutralResourceKind::World});
	if (!TestFalse(TEXT("the genuine world decodes through the runtime decoder"),
		Loaded.IsError()))
	{
		if (Loaded.IsError()) AddError(MoveTemp(Loaded).GetError().ToString());
		return false;
	}
	TSharedPtr<const FWoTBSPPayloadData> BSP;
	if (!TestTrue(TEXT("the genuine v3 BSP decodes through authenticated production paths"),
		LoadSkyAuthenticatedBSP(Fixture.Descriptor.BspLogicalId, BSP, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("the genuine entry keeps dense pan provenance"),
		BSP->EntrySurfaces.Num(), World.Surfaces().Num())) return false;
	TestTrue(TEXT("actual surface 176 pins its zone-2 U pan"),
		BSP->EntrySurfaces.IsValidIndex(176)
		&& BSP->EntrySurfaces[176].PanUV == FVector2f(0.013671875f, 0.0f));
	TestTrue(TEXT("actual surface 186 pins its zone-2 V pan"),
		BSP->EntrySurfaces.IsValidIndex(186)
		&& BSP->EntrySurfaces[186].PanUV == FVector2f(0.0f, 0.02734375f));

	TSet<int32> SelectedSourceSurfaces;
	for (const FWoTRuntimeWorldNode& Node : World.Nodes())
	{
		if (Node.VertexCount >= 3 && (Node.Zones[0] == 2 || Node.Zones[1] == 2))
			SelectedSourceSurfaces.Add(Node.Surface);
	}
	TestEqual(TEXT("the genuine winner selects eleven source sky surfaces"),
		SelectedSourceSurfaces.Num(), 11);
	TSet<int32> FakeBackdropSurfaces;
	for (int32 SurfaceIndex = 0; SurfaceIndex < World.Surfaces().Num(); ++SurfaceIndex)
	{
		if ((World.Surfaces()[SurfaceIndex].Flags & 0x00000080u) != 0)
			FakeBackdropSurfaces.Add(SurfaceIndex);
	}
	TestEqual(TEXT("the genuine entry has thirteen source FakeBackdrop surfaces"),
		FakeBackdropSurfaces.Num(), 13);
	bool bTraversablePortalFromSky = false;
	for (const FWoTRuntimeWorldNode& Node : World.Nodes())
	{
		if (!World.Surfaces().IsValidIndex(Node.Surface)
			|| (World.Surfaces()[Node.Surface].Flags & 0x04000000u) == 0)
		{
			continue;
		}
		const uint8 Other = Node.Zones[0] == 2 ? Node.Zones[1]
			: Node.Zones[1] == 2 ? Node.Zones[0] : 0;
		bTraversablePortalFromSky = bTraversablePortalFromSky
			|| (Other != 0 && Other != 2);
	}
	TestFalse(TEXT("no traversable portal connects zone 2 to another nonzero zone"),
		bTraversablePortalFromSky);

	FWoTFrontEndSkyPlan Plan;
	if (!TestTrue(TEXT("the genuine entry builds the pure sky plan"),
		WoTBuildFrontEndSkyPlan(World, *BSP, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TSet<uint32> EmittedSkySurfaces;
	for (uint32 Surface : Plan.SkyMesh.TriangleSurfaces)
		EmittedSkySurfaces.Add(Surface);
	TestEqual(TEXT("all eleven selected sky surfaces survive extraction"),
		EmittedSkySurfaces.Num(), 11);
	for (uint32 Surface : EmittedSkySurfaces)
	{
		TestTrue(TEXT("every emitted sky surface belongs to the source selection"),
			SelectedSourceSurfaces.Contains(static_cast<int32>(Surface)));
		TestFalse(TEXT("no emitted sky surface is a FakeBackdrop window"),
			FakeBackdropSurfaces.Contains(static_cast<int32>(Surface)));
	}
	return !HasAnyErrors();
}

#endif
