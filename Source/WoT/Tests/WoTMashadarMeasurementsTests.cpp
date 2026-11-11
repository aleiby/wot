// Real retail data exercises both the imported asset and guarded runtime paths.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "WoTPayloadActivationTestSupport.h"
#include "../Assets/WoTRuntimeAssets.h"
#include "../Mesh/WoTRuntimeVertexMesh.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeProceduralResource.h"
#include "../Projectiles/WoTMashadarSegment.h"
#include "../Projectiles/WoTMashadarGuide.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTFractalData.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTVertexAnimComponent.h"
#include "../WoTMeshFX.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "WoTRetailReader/FontProcedural.h"
#include "WoTRetailReader/MeshAnimation.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/TextureSound.h"

namespace
{
class FMashadarMeasurementWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView Id, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("measurement object exceeds bound")));
		auto& Object = Objects.AddDefaulted_GetRef();
		Object.LogicalId = FString(Id);
		Object.Schema = FString(Schema);
		Object.Bytes.Append(Bytes.GetData(), Bytes.Num());
		return FWoTBootstrapStatus::Success();
	}
	TArray<FWoTPayloadActivationTestObject> Objects;
};

class FMashadarMeasurementProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
		{ return FWoTBootstrapStatus::Success(); }
	bool IsCancellationRequested() const override { return false; }
};

uint32 MashReadU32(const TArray<uint8>& Bytes, int32 Offset)
{
	return uint32(Bytes[Offset]) | uint32(Bytes[Offset+1]) << 8
		| uint32(Bytes[Offset+2]) << 16 | uint32(Bytes[Offset+3]) << 24;
}

struct FMashRetailFrames
{
	FString Name;
	float DrawScale = 0;
	TArray<TArray<FVector>> Frames;
};

// Independent packed-vertex oracle. The native reader authenticates the retail
// package and emits the original integers; do not use runtime BlendPose to
// construct its own expected positions. MashSeg/Tip have zero Origin/RotOrigin.
bool ReadMashRetail(FAutomationTestBase& Test,
	const FMashadarMeasurementWriter& Writer, FMashRetailFrames& Out)
{
	const FString MeshId = TEXT("cache/mesh/WoTPawns__") + Out.Name;
	const FString AnimId = TEXT("cache/animation/WoTPawns__") + Out.Name;
	const auto* Mesh = Writer.Objects.FindByPredicate([&MeshId](const auto& Object)
		{ return Object.LogicalId == MeshId; });
	const auto* Anim = Writer.Objects.FindByPredicate([&AnimId](const auto& Object)
		{ return Object.LogicalId == AnimId; });
	if (!Test.TestNotNull(TEXT("real mesh object"), Mesh)
		|| !Test.TestNotNull(TEXT("real animation object"), Anim)) return false;
	const int32 M = 12 + MashReadU32(Mesh->Bytes, 8);
	const int32 A = 12 + MashReadU32(Anim->Bytes, 8);
	const int32 Count = MashReadU32(Anim->Bytes, A + 12);
	const int32 FrameCount = MashReadU32(Anim->Bytes, A + 16);
	Test.TestEqual(TEXT("retail frame count"), FrameCount, 10);
	Test.TestEqual(TEXT("retail source vertex count"), Count, Out.Name == TEXT("MashTip") ? 17 : 15);
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Test.TestEqual(TEXT("retail origin is zero"), MashReadU32(Mesh->Bytes, M+44+Axis*4), 0u);
		Test.TestEqual(TEXT("retail rotation origin is zero"), MashReadU32(Mesh->Bytes, M+68+Axis*4), 0u);
	}
	for (int32 Frame = 0; Frame < FrameCount; ++Frame)
	{
		auto& Positions = Out.Frames.AddDefaulted_GetRef();
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			const uint32 Packed = MashReadU32(Anim->Bytes, A+28+(Frame*Count+Vertex)*4);
			const int32 X = int32(Packed & 2047) - ((Packed & 1024) ? 2048 : 0);
			const int32 Y = int32((Packed >> 11) & 2047) - ((Packed & (1u << 21)) ? 2048 : 0);
			const int32 Z = int32(Packed >> 22) - ((Packed & (1u << 31)) ? 1024 : 0);
			Positions.Add(FVector(X * 0.2, Y * 0.2, Z * 0.4));
		}
	}
	return true;
}

FBox MashBounds(const TArray<FVector>& Positions, const FTransform& Transform)
{
	FBox Box(ForceInit);
	for (const FVector& Position : Positions) Box += Transform.TransformPosition(Position);
	return Box;
}

void LogMashBounds(FAutomationTestBase& Test, const FString& Label,
	const TArray<FVector>& Positions, const FTransform& Transform)
{
	const FBox Box = MashBounds(Positions, Transform);
	Test.AddInfo(FString::Printf(TEXT("MEASURE %s count=%d min=(%.6f,%.6f,%.6f) max=(%.6f,%.6f,%.6f)"),
		*Label, Positions.Num(), Box.Min.X, Box.Min.Y, Box.Min.Z, Box.Max.X, Box.Max.Y, Box.Max.Z));
}

AWoTMashadarSegment* SpawnMash(UWorld* World, bool bHead)
{
	auto* Segment = World->SpawnActorDeferred<AWoTMashadarSegment>(
		AWoTMashadarSegment::StaticClass(), FTransform::Identity);
	Segment->bIsHead = bHead;
	Segment->FinishSpawning(FTransform::Identity);
	Segment->Tick(0); // bind the actual display mesh and quad renderer
	return Segment;
}

void MeasureMash(FAutomationTestBase& Test, UWorld* World,
	const FMashRetailFrames& Retail, bool bRuntime)
{
	const bool bHead = Retail.Name == TEXT("MashTip");
	AWoTMashadarSegment* Segment = SpawnMash(World, bHead);
	UWoTVertexAnimComponent* Mesh = Segment->DisplayMesh;
	if (bRuntime)
	{
#if !WOT_GUARDED_BOOTSTRAP
		Test.TestFalse(TEXT("ordinary Development resolver stays disabled with an active retail payload"),
			FWoTRuntimeVertexMesh::ResolveAndLoad(Retail.Name).IsValid());
#endif
		// SetVertexMesh's guarded branch uses this resolver and this binder.
		// The source objects above were imported from the actual retail package.
		auto Data = FWoTRuntimeVertexMesh::ResolveAndLoadForTesting(Retail.Name);
		if (!Test.TestTrue(TEXT("real retail runtime bind"), Data.IsValid()
			&& Mesh->BindRuntimeVertexMeshForTesting(Data))) return;
	}
	const FString Label = Retail.Name + (bRuntime ? TEXT(" runtime") : TEXT(" asset"));
	// Independently decoded retail MyTex2 is 128x128 on both readers.
	Test.TestEqual(Label + TEXT(" retail sprite dimensions"), Segment->GetParticleTextureSize(), FIntPoint(128,128));
	UTexture* InitialTexture = nullptr;
	Segment->GetParticleMaterial()->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), InitialTexture);
	Test.TestNotNull(Label + TEXT(" MyTex2 actually resolved"), InitialTexture);
	if (bRuntime && InitialTexture)
		Test.TestTrue(TEXT("guarded texture is a runtime procedural placeholder"),
			InitialTexture->GetName().StartsWith(TEXT("WoTProc_")));
	Test.TestFalse(Label + TEXT(" hidden driver"), Mesh->IsVisible());
	Test.TestTrue(Label + TEXT(" ticking driver"), Mesh->IsComponentTickEnabled());
	// UnMesh.cpp:433,574: bParticles uses DrawScale=1 for mesh positions.
	// Actor DrawScale affects only DrawIcon's size (UnMeshRn.cpp:443-444).
	Test.TestTrue(Label + TEXT(" bParticles cloud ignores actor DrawScale"),
		Mesh->GetComponentScale().Equals(FVector::OneVector, 1.e-4));
	TArray<int32> Mapping;
	TArray<FVector> Initial;
	TArray<FVector> Pose;
	for (int32 Frame = 0; Frame < Retail.Frames.Num(); ++Frame)
	{
		Mesh->SetFrameAndStop(TEXT("All"), Frame / 10.0f);
		Mesh->GetCurrentPoseVertexPositions(Pose);
		Segment->Tick(0);
		if (Frame == 0)
		{
			Initial = Pose;
			LogMashBounds(Test, Label + TEXT(" frame0 pose"), Pose, Mesh->GetComponentTransform());
			for (const FVector& Position : Pose)
			{
				const int32 Index = Retail.Frames[0].IndexOfByPredicate([&Position](const FVector& Expected)
					{ return Position.Equals(Expected, 0.001); });
				Mapping.Add(Index);
			}
			Test.TestFalse(Label + TEXT(" positions match retail space/scale"), Mapping.Contains(INDEX_NONE));
			Test.TestEqual(Label + TEXT(" pose count"), Pose.Num(), bHead && !bRuntime ? 16 : Retail.Frames[0].Num());
		}
		bool bPositionsMatch = Pose.Num() == Mapping.Num();
		for (int32 Vertex = 0; Vertex < Pose.Num() && Vertex < Mapping.Num(); ++Vertex)
			bPositionsMatch &= Mapping[Vertex] != INDEX_NONE
				&& Pose[Vertex].Equals(Retail.Frames[Frame][Mapping[Vertex]], 0.001);
		Test.TestTrue(Label + FString::Printf(TEXT(" frame %d pose follows retail"), Frame), bPositionsMatch);
		TArray<FVector> Centers;
		int32 VisibleCount = 0;
		for (int32 Index = 0; Index < Segment->ParticleQuads->GetInstanceCount(); ++Index)
		{
			FTransform Instance;
			Segment->ParticleQuads->GetInstanceTransform(Index, Instance, true);
			if (Instance.GetScale3D().X < 0.001) continue;
			++VisibleCount;
			Centers.Add(Instance.GetLocation());
		}
		if (Frame == 0) LogMashBounds(Test, Label + TEXT(" frame0 quads"), Centers, FTransform::Identity);
		Test.TestEqual(Label + TEXT(" exactly one quad per retail source vertex"), VisibleCount, Retail.Frames[Frame].Num());
		for (const FVector& Expected : Retail.Frames[Frame])
		{
			const FVector WorldExpected = Segment->GetActorTransform().TransformPositionNoScale(Expected);
			Test.TestTrue(Label + TEXT(" quad at every retail vertex"), Centers.ContainsByPredicate(
				[&WorldExpected](const FVector& Center) { return Center.Equals(WorldExpected, 0.001); }));
		}
	}
	Mesh->SetFrameAndStop(TEXT("All"), 0);
	Mesh->LoopAnim(TEXT("All"), 0.02f); // midpoint of retail's 0.01..0.03 range
	Mesh->GetCurrentPoseVertexPositions(Initial);
	auto* Fractals = World->GetSubsystem<UWoTFractalSubsystem>();
	Fractals->SetRenderGateBypassForTesting(true);
	const FName MyTex2(TEXT("WoTPawns.Mashadar.MyTex2"));
	const int32 StepsBefore = Fractals->GetStepCount(MyTex2);
	WoTTest::TickWorld(World, 120, 1.0f/60.0f);
	Mesh->GetCurrentPoseVertexPositions(Pose);
	const float FrameNow = Mesh->GetAnimFrame() * 10;
	const int32 FrameA = FMath::FloorToInt(FrameNow);
	const float Alpha = FrameNow - FrameA;
	float MinMotion = MAX_flt;
	float MaxMotion = 0;
	for (int32 Vertex = 0; Vertex < Pose.Num() && Vertex < Mapping.Num(); ++Vertex)
	{
		const float Motion = Mesh->GetComponentTransform().TransformVector(Pose[Vertex]-Initial[Vertex]).Size();
		MinMotion = FMath::Min(MinMotion, Motion);
		MaxMotion = FMath::Max(MaxMotion, Motion);
		if (Mapping[Vertex] == INDEX_NONE) continue;
		const FVector Expected = FMath::Lerp(Retail.Frames[FrameA][Mapping[Vertex]],
			Retail.Frames[(FrameA+1)%10][Mapping[Vertex]], Alpha);
		Test.TestTrue(Label + TEXT(" hidden tick publishes interpolated retail pose"), Pose[Vertex].Equals(Expected, 0.002));
		Test.AddInfo(FString::Printf(TEXT("MOTION %s vertex=%d distance2s=%.6f"), *Label, Mapping[Vertex], Motion));
	}
	Test.TestTrue(Label + TEXT(" all vertices animate"), MinMotion > 0);
	TArray<FVector> LiveCenters;
	for (int32 Index = 0; Index < Segment->ParticleQuads->GetInstanceCount(); ++Index)
	{
		FTransform Instance;
		Segment->ParticleQuads->GetInstanceTransform(Index, Instance, true);
		if (Instance.GetScale3D().X < 0.001) continue;
		LiveCenters.Add(Instance.GetLocation());
		Test.TestTrue(Label + TEXT(" quad size is DrawScale times retail USize/VSize"),
			Instance.GetScale3D().Equals(FVector(Retail.DrawScale*128/100, Retail.DrawScale*128/100, 1), 1.e-4));
	}
	for (int32 Vertex = 0; Vertex < Retail.Frames[0].Num(); ++Vertex)
	{
		const FVector Expected = Segment->GetActorTransform().TransformPositionNoScale(FMath::Lerp(
			Retail.Frames[FrameA][Vertex], Retail.Frames[(FrameA+1)%10][Vertex], Alpha));
		Test.TestTrue(Label + TEXT(" renderer consumes this tick's complete animated pose"),
			LiveCenters.ContainsByPredicate([&Expected](const FVector& Center) { return Center.Equals(Expected, 0.002); }));
	}
	UTexture* AnimatedTexture = nullptr;
	Segment->GetParticleMaterial()->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), AnimatedTexture);
	Test.TestTrue(Label + TEXT(" material samples live WetTexture"),
		AnimatedTexture && AnimatedTexture == Fractals->FindLiveTexture(MyTex2));
	Test.TestTrue(Label + TEXT(" WetTexture advances"), Fractals->GetStepCount(MyTex2) > StepsBefore);
	Test.AddInfo(FString::Printf(TEXT("MEASURE %s 2s frame=%.6f motion=[%.6f,%.6f] texture=%dx%d"),
		*Label, FrameNow, MinMotion, MaxMotion, Segment->GetParticleTextureSize().X, Segment->GetParticleTextureSize().Y));
	LogMashBounds(Test, Label + TEXT(" 2s pose"), Pose, Mesh->GetComponentTransform());
	LogMashBounds(Test, Label + TEXT(" 2s quads"), LiveCenters, FTransform::Identity);
	Mesh->SetFrameAndStop(TEXT("All"), 0);
	Segment->Tick(0);
	FTransform BeforeFade;
	Segment->ParticleQuads->GetInstanceTransform(0, BeforeFade, true);
	Segment->FadeAway();
	WoTTest::TickWorld(World, 1);
	FTransform AfterFade;
	Segment->ParticleQuads->GetInstanceTransform(0, AfterFade, true);
	Test.TestTrue(Label + TEXT(" fade enlarges icons"), AfterFade.GetScale3D().X > BeforeFade.GetScale3D().X);
	Test.TestTrue(Label + TEXT(" fade does not enlarge vertex cloud"),
		AfterFade.GetLocation().Equals(BeforeFade.GetLocation(), 1.e-4));
	Segment->Destroy();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMashadarRealRetailMeasurements,
	"WoT.Mashadar.RealRetailMeasurements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTMashadarRealRetailMeasurements::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	auto Contract = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("retail contract loads"), Contract.IsError())) return false;
	Contract.GetValue().Facts.RemoveAll([](const FWoTRequiredFact& Fact)
	{
		return Fact.SourcePath != TEXT("System/WoTPawns.u")
			|| !((Fact.Domain == EWoTFactDomain::Mesh || Fact.Domain == EWoTFactDomain::Animation)
				? (Fact.SourceExport == TEXT("WoTPawns.MashSeg") || Fact.SourceExport == TEXT("WoTPawns.MashTip"))
				: (Fact.Domain == EWoTFactDomain::Texture || Fact.Domain == EWoTFactDomain::Procedural));
	});
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("real WoTPawns package loads"), FFileHelper::LoadFileToArray(Bytes,
		*WoTReferencePath(TEXT("original/System/WoTPawns.u"))))) return false;
	FMashadarMeasurementWriter Writer;
	FMashadarMeasurementProgress Progress;
	auto Meshes = ImportMeshAnimationPackageBytesForTesting(Bytes, TEXT("System/WoTPawns.u"), Contract.GetValue(), Writer, Progress);
	auto Textures = ImportTexturePackageBytesForTesting(Bytes, TEXT("System/WoTPawns.u"), Contract.GetValue(), Writer, Progress);
	auto Procedurals = ImportFontProceduralPackageBytesForTesting(Bytes, TEXT("System/WoTPawns.u"), Contract.GetValue(), Writer, Progress);
	if (!TestFalse(TEXT("retail meshes import"), Meshes.IsError())
		|| !TestFalse(TEXT("retail textures import"), Textures.IsError())
		|| !TestFalse(TEXT("retail procedurals import"), Procedurals.IsError())) return false;
	FMashRetailFrames Retail[2];
	Retail[0].Name = TEXT("MashSeg"); Retail[0].DrawScale = 0.3f;
	Retail[1].Name = TEXT("MashTip"); Retail[1].DrawScale = 0.2f;
	for (auto& Item : Retail)
	{
		if (!ReadMashRetail(*this, Writer, Item)) return false;
		LogMashBounds(*this, Item.Name + TEXT(" retail bParticles frame0"), Item.Frames[0], FTransform::Identity);
		for (int32 Vertex = 0; Vertex < Item.Frames[0].Num(); ++Vertex)
		{
			double Excursion = 0;
			double Travel = 0;
			for (int32 Frame = 0; Frame < Item.Frames.Num(); ++Frame)
			{
				Excursion = FMath::Max(Excursion, FVector::Distance(Item.Frames[0][Vertex], Item.Frames[Frame][Vertex]));
				Travel += FVector::Distance(Item.Frames[Frame][Vertex], Item.Frames[(Frame+1)%Item.Frames.Num()][Vertex]);
			}
			AddInfo(FString::Printf(TEXT("RETAIL %s vertex=%d excursion10=%.6f travel10=%.6f"), *Item.Name, Vertex, Excursion, Travel));
		}
	}
	UWorld* AssetWorld = WoTTest::CreateSpecWorld();
	for (const auto& Item : Retail) MeasureMash(*this, AssetWorld, Item, false);
	WoTTest::DestroySpecWorld(AssetWorld);
	const FString CacheRoot = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()
		/ (TEXT("MashadarMeasurements_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)) / TEXT("ContentCache"));
	FString Error;
	if (!TestTrue(TEXT("real retail generation publishes"),
		WoTPayloadActivationTest_PublishObjects(CacheRoot, Writer.Objects, Error)))
	{
		AddError(Error);
		return false;
	}
	ResetWoTPayloadActivationForTesting();
	UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, true);
	UWoTRuntimeAssets::SetPayloadTextureResolutionForTesting(true);
	ON_SCOPE_EXIT
	{
		UWoTRuntimeAssets::SetPayloadTextureResolutionForTesting(false);
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
		ResetWoTPayloadActivationForTesting();
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(CacheRoot), false, true);
	};
	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bActivatePayload = false;
	Options.BeforeInitializeStandalone = [&CacheRoot](UGameInstance* Instance)
		{ CastChecked<UWoTGameInstance>(Instance)->SetPayloadCacheDir(CacheRoot, false); };
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	UWorld* RuntimeWorld = WoTTest::CreateSpecWorld(Options);
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(RuntimeWorld); };
	if (!TestTrue(TEXT("real payload activates"), WoTPayloadActivationTest_ActivatePublished(
		RuntimeWorld->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>(), CacheRoot, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("runtime world ready"), PublishWoTBootstrapWorldReady().IsError());
	FWoTRuntimeProceduralResource Procedural;
	if (!TestFalse(TEXT("authenticated retail MyTex2 loads"), Procedural.LoadFirstUse(
		{TEXT("cache/procedural/WoTPawns__MyTex2"), EWoTNeutralResourceKind::Procedural}).IsError())) return false;
	FWoTFractalTextureRecord Record;
	TestTrue(TEXT("retail wet record decodes"), Procedural.BuildRecord(Record));
	TestTrue(TEXT("MyTex2 is WetTexture"), Record.FractalClass == EWoTFractalClass::Wet);
	TestEqual(TEXT("retail MyTex2 USize"), Record.Width, 128);
	TestEqual(TEXT("retail MyTex2 VSize"), Record.Height, 128);
	AddInfo(FString::Printf(TEXT("MEASURE retail MyTex2=%dx%d source=%s waveamp=%d drops=%d"),
		Record.Width, Record.Height, *Record.SourceTextureRef, Record.WaveAmp, Record.Drops.Num()));
	auto* Fractals = RuntimeWorld->GetSubsystem<UWoTFractalSubsystem>();
	Fractals->ActivateGuardedCatalogForTesting();
	Fractals->SetRenderGateBypassForTesting(true);
	for (const auto& Item : Retail) MeasureMash(*this, RuntimeWorld, Item, true);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTShieldRealRetailScalePaths,
	"WoT.ShieldParticles.RealRetailScalePaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTShieldRealRetailScalePaths::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	auto Contract = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("retail contract loads"), Contract.IsError())) return false;
	Contract.GetValue().Facts.RemoveAll([](const FWoTRequiredFact& Fact)
	{
		return Fact.SourcePath != TEXT("System/Angreal.u")
			|| Fact.SourceExport != TEXT("Angreal.ShieldParticleMesh")
			|| (Fact.Domain != EWoTFactDomain::Mesh && Fact.Domain != EWoTFactDomain::Animation);
	});
	TArray<uint8> Bytes;
	if (!TestTrue(TEXT("real shield package loads"), FFileHelper::LoadFileToArray(Bytes,
		*WoTReferencePath(TEXT("original/System/Angreal.u"))))) return false;
	FMashadarMeasurementWriter Writer;
	FMashadarMeasurementProgress Progress;
	auto Imported = ImportMeshAnimationPackageBytesForTesting(Bytes, TEXT("System/Angreal.u"),
		Contract.GetValue(), Writer, Progress);
	if (!TestFalse(TEXT("real shield imports"), Imported.IsError())) return false;
	const auto* MeshObject = Writer.Objects.FindByPredicate([](const auto& Object)
		{ return Object.LogicalId == TEXT("cache/mesh/Angreal__ShieldParticleMesh"); });
	const auto* AnimObject = Writer.Objects.FindByPredicate([](const auto& Object)
		{ return Object.LogicalId == TEXT("cache/animation/Angreal__ShieldParticleMesh"); });
	if (!TestNotNull(TEXT("retail shield mesh"), MeshObject)
		|| !TestNotNull(TEXT("retail shield animation"), AnimObject)) return false;
	const int32 M = 12 + MashReadU32(MeshObject->Bytes, 8);
	const int32 A = 12 + MashReadU32(AnimObject->Bytes, 8);
	const int32 Count = MashReadU32(AnimObject->Bytes, A + 12);
	FVector RetailScale;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		TestEqual(TEXT("shield origin is zero"), MashReadU32(MeshObject->Bytes, M + 44 + Axis*4), 0u);
		TestEqual(TEXT("shield rotation origin is zero"), MashReadU32(MeshObject->Bytes, M + 68 + Axis*4), 0u);
		const uint32 ScaleBits = MashReadU32(MeshObject->Bytes, M + 56 + Axis*4);
		float AxisScale;
		FMemory::Memcpy(&AxisScale, &ScaleBits, sizeof(AxisScale));
		RetailScale[Axis] = AxisScale;
	}
	TArray<FVector> RetailPositions;
	for (int32 Vertex = 0; Vertex < Count; ++Vertex)
	{
		const uint32 Packed = MashReadU32(AnimObject->Bytes, A + 28 + Vertex*4);
		const int32 X = int32(Packed & 2047) - ((Packed & 1024) ? 2048 : 0);
		const int32 Y = int32((Packed >> 11) & 2047) - ((Packed & (1u << 21)) ? 2048 : 0);
		const int32 Z = int32(Packed >> 22) - ((Packed & (1u << 31)) ? 1024 : 0);
		RetailPositions.Add(FVector(X, Y, Z) * RetailScale);
	}
	const FBox RetailBounds = MashBounds(RetailPositions, FTransform::Identity);
	LogMashBounds(*this, TEXT("Shield retail frame0"), RetailPositions, FTransform::Identity);
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	const auto CheckCloud = [this, &RetailBounds, &RetailPositions, Count](AWoTMeshEffect* Effect,
		float IconScale, const FString& Label)
	{
		TArray<FVector> Centers;
		TestEqual(Label + TEXT(" quad count"), Effect->ParticleQuads->GetInstanceCount(), Count);
		const UTexture2D* Texture = Cast<UTexture2D>(Effect->GetBoundTextureForTesting());
		if (!TestNotNull(Label + TEXT(" authored texture"), Texture)) return;
		FIntPoint Dims(Texture->GetSurfaceWidth(), Texture->GetSurfaceHeight());
#if WITH_EDITORONLY_DATA
		if (Texture->Source.IsValid())
			Dims = FIntPoint(Texture->Source.GetSizeX(), Texture->Source.GetSizeY());
#endif
		TestTrue(Label + TEXT(" nonzero texture size"), Dims.X > 0 && Dims.Y > 0);
		for (int32 Index = 0; Index < Effect->ParticleQuads->GetInstanceCount(); ++Index)
		{
			FTransform Instance;
			if (!TestTrue(Label + TEXT(" instance readable"),
				Effect->ParticleQuads->GetInstanceTransform(Index, Instance, true))) continue;
			Centers.Add(Effect->GetActorTransform().InverseTransformPositionNoScale(Instance.GetLocation()));
			TestTrue(Label + TEXT(" icon size = DrawScale times texture size"),
				Instance.GetScale3D().Equals(FVector(Dims.X * IconScale / 100.0,
					Dims.Y * IconScale / 100.0, 1), 1.e-4));
		}
		const FBox Actual = MashBounds(Centers, FTransform::Identity);
		TestTrue(Label + TEXT(" particle-centre bounds equal retail at scale 1"),
			Actual.Min.Equals(RetailBounds.Min, 0.002) && Actual.Max.Equals(RetailBounds.Max, 0.002));
		for (const FVector& Expected : RetailPositions)
			TestTrue(Label + TEXT(" retail vertex has a quad"), Centers.ContainsByPredicate(
				[&Expected](const FVector& Center) { return Center.Equals(Expected, 0.002); }));
		LogMashBounds(*this, Label, Centers, FTransform::Identity);
	};
	for (const TCHAR* Config : {TEXT("AirShieldVisual"), TEXT("EarthShieldVisual"),
		TEXT("FireShieldVisual"), TEXT("SpiritShieldVisual"), TEXT("WaterShieldVisual")})
	{
		AWoTMeshEffect* Effect = WoTMeshFX::SpawnLocal(*World, Config, FVector(0, 0, 100));
		if (!TestNotNull(TEXT("real shield spawned"), Effect)) return false;
		Effect->SetLifeSpan(0);
		Effect->Mesh->SetFrameAndStop(TEXT("All"), 0);
		Effect->Tick(0);
		CheckCloud(Effect, 0.25f, FString(Config) + TEXT(" spawn"));
		Effect->SetEffectDrawScale(0.5f);
		Effect->Tick(0);
		CheckCloud(Effect, 0.5f, FString(Config) + TEXT(" setter"));
		// Retail shields do not grow. Exercise the shared grow path with the
		// real shield row and mesh, changing only its deterministic growth rate.
		FWoTMeshEffectRow Growing = *WoTMeshFX::Find(Config);
		Growing.MinGrowRate = Growing.MaxGrowRate = 0.5f;
		Effect->Configure(Config, Growing, nullptr);
		Effect->Mesh->SetFrameAndStop(TEXT("All"), 0);
		Effect->Tick(0.1f);
		CheckCloud(Effect, Growing.DrawScale + 0.05f, FString(Config) + TEXT(" growth"));
		Effect->Destroy();
	}
	// The same spawn/setter/growth operations must retain ordinary mesh scale.
	const FWoTMeshEffectRow* SolidRow = WoTMeshFX::Find(TEXT("LSRing"));
	if (!TestNotNull(TEXT("non-particle row"), SolidRow)) return false;
	AWoTMeshEffect* Solid = WoTMeshFX::SpawnLocal(*World, TEXT("LSRing"), FVector::ZeroVector);
	if (!TestNotNull(TEXT("non-particle effect"), Solid)) return false;
	TestFalse(TEXT("control uses solid mesh"), Solid->bParticles);
	TestTrue(TEXT("solid spawn retains DrawScale"), Solid->Mesh->GetRelativeScale3D().Equals(FVector(SolidRow->DrawScale)));
	Solid->SetEffectDrawScale(0.5f);
	TestTrue(TEXT("solid setter retains DrawScale"), Solid->Mesh->GetRelativeScale3D().Equals(FVector(0.5)));
	FWoTMeshEffectRow GrowingSolid = *SolidRow;
	GrowingSolid.MinGrowRate = GrowingSolid.MaxGrowRate = 0.5f;
	Solid->Configure(TEXT("LSRing"), GrowingSolid, nullptr);
	Solid->Tick(0.1f);
	TestTrue(TEXT("solid growth retains DrawScale"),
		Solid->Mesh->GetRelativeScale3D().Equals(FVector(GrowingSolid.DrawScale + 0.05f), 1.e-4));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMashadarChainMeasurements,
	"WoT.Mashadar.ChainCountAndSpacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTMashadarChainMeasurements::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	const FVector Lair(0,0,100);
	const FTransform At(Lair);
	auto* Guide = World->SpawnActorDeferred<AWoTMashadarGuide>(
		AWoTMashadarGuide::StaticClass(), At);
	Guide->SetRole(ROLE_SimulatedProxy);
	Guide->InitialGuideLocation = Lair;
	Guide->InitialGuideRotation = FRotator::ZeroRotator;
	Guide->HidingPlaceLocation = Lair;
	Guide->bActive = true;
	Guide->ChainPhase = EWoTMashadarChainPhase::Extending;
	Guide->Destination = Guide;
	Guide->ClientSetDestinationCounter = 1;
	Guide->FinishSpawning(At);
	TestEqual(TEXT("retail server segment threshold"), Guide->MaxNumSegments, 100);
	TestFalse(TEXT("active master visible"), Guide->GetMasterSegment()->IsHidden());
	Guide->SetActorLocation(Lair + FVector(420,0,0));
	WoTTest::TickWorld(World, 1);
	int32 BodyCount = 0;
	AWoTIKTrailer* Previous = Guide->GetMasterSegment();
	TestTrue(TEXT("head joint is 48 units from guide"), FMath::IsNearlyEqual(
		FVector::Distance(Guide->GetActorLocation(), Previous->GetActorLocation()), 48.0, 0.001));
	for (AWoTIKTrailer* Joint = Previous->ChildTrailer; Joint; Joint = Joint->ChildTrailer)
	{
		TestTrue(TEXT("body joints spaced 42 units"), FMath::IsNearlyEqual(
			FVector::Distance(Previous->GetActorLocation(), Joint->GetActorLocation()), 42.0, 0.001));
		Previous = Joint;
		++BodyCount;
	}
	TestEqual(TEXT("420-unit reach catches up eight body joints in one tick"), BodyCount, 8);
	AddInfo(FString::Printf(TEXT("MEASURE chain reach=420 head=1 bodies=%d headDist=%.1f bodyDist=%.1f max=%d"),
		BodyCount, Guide->HeadDist, Guide->BodyDist, Guide->MaxNumSegments));
	++Guide->ClientDeactivateCounter;
	Guide->OnRep_ClientDeactivateCounter();
	Guide->SetActorLocation(Lair);
	WoTTest::TickWorld(World, 1);
	TestNull(TEXT("home removes body chain"), Guide->GetMasterSegment()->ChildTrailer.Get());
	TestTrue(TEXT("resting master is hidden"), Guide->GetMasterSegment()->IsHidden());
	return !HasAnyErrors();
}

#endif
