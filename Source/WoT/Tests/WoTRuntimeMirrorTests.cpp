// Retail PF_Mirrored surfaces on the runtime BSP world (WoTRuntimeMirror.h).
// The real-retail census rides WoT.World.Sky.RealRetailCensus
// (WoTRuntimeMapProjectionTests.cpp); these specs pin the stage, the
// reflected capture geometry and the skip-not-fail contract on synthetic data.
#include "Misc/AutomationTest.h"

#include "../World/WoTRuntimeBSPRenderer.h"
#include "../World/WoTRuntimeMirror.h"
#include "../World/WoTWorldMaterializer.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "RHIShaderPlatform.h"
#include "Misc/App.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Math/InverseRotationMatrix.h"
#include "Math/PerspectiveMatrix.h"
#include "Math/RandomStream.h"
#include "Math/RotationMatrix.h"
#include "UObject/Package.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR

namespace
{
constexpr EAutomationTestFlags WoTMirrorSpecFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

constexpr uint32 WoTMirrorSpecTranslucent = 0x00000004u;
constexpr uint32 WoTMirrorSpecTwoSided = 0x00000100u;

UWorld* WoTMirrorSpecWorld(const TCHAR* Leaf)
{
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/WoTRuntimeMirror_%s_%s"), Leaf,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetFlags(RF_Transient);
	return UWorld::CreateWorld(EWorldType::Game, false,
		MakeUniqueObjectName(Package, UWorld::StaticClass(), TEXT("World")),
		Package, true, ERHIFeatureLevel::Num, nullptr, true);
}

void WoTMirrorSpecDestroyWorld(UWorld* World)
{
	if (World == nullptr) return;
	World->RemoveFromRoot();
	if (World->bIsWorldInitialized)
	{
		World->DestroyWorld(false);
	}
	else
	{
		World->ClearFlags(RF_Standalone);
		World->MarkAsGarbage();
	}
}

/** Appends one +Z-facing triangle at height Z as its own section. */
void WoTMirrorSpecAddSection(FWoTBSPPayloadData& Part, const TCHAR* Package,
	const TCHAR* Name, const TCHAR* Suffix, uint32 Flags,
	TConstArrayView<float> Heights)
{
	FWoTBSPPayloadSection& Section = Part.Sections.AddDefaulted_GetRef();
	Section.MaterialSlot = FString::Printf(TEXT("%s__%s%s"), Package, Name, Suffix);
	Section.CanonicalPackage = Package;
	Section.CanonicalName = Name;
	Section.SourceTextureIdentity = FString::Printf(TEXT("%s.%s"), Package, Name);
	Section.SurfaceFlags = Flags;
	Section.bCollisionEnabled = !Part.bPortal;
	Section.FirstIndex = static_cast<uint32>(Part.Indices.Num());
	Section.MinVertex = static_cast<uint32>(Part.Positions.Num());
	for (const float Z : Heights)
	{
		const uint32 First = static_cast<uint32>(Part.Positions.Num());
		Part.Positions.Append({FVector3f(0, 0, Z), FVector3f(100, 0, Z),
			FVector3f(0, 100, Z)});
		Part.Normals.Append({FVector3f::ZAxisVector, FVector3f::ZAxisVector,
			FVector3f::ZAxisVector});
		Part.UV0.Append({FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
		Part.UV1.Append({FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)});
		Part.Indices.Append({First, First + 1, First + 2});
		Part.BoundsMin = Part.BoundsMin.ComponentMin(FVector3f(0, 0, Z));
		Part.BoundsMax = Part.BoundsMax.ComponentMax(FVector3f(100, 100, Z));
	}
	Section.IndexCount = static_cast<uint32>(Part.Indices.Num()) - Section.FirstIndex;
	Section.MaxVertex = static_cast<uint32>(Part.Positions.Num()) - 1;
}

/** One main part: a plain floor, a mirrored pond sheet on two planes (z=100
 * twice, z=300), a mirrored glass tile coplanar with the first pond plane,
 * and an opaque mirror (a composite the port does not reproduce). */
FWoTBSPPayloadData WoTMirrorSpecPart()
{
	FWoTBSPPayloadData Part;
	Part.FormatVersion = 4;
	Part.bPortal = false;
	Part.BoundsMin = FVector3f(0, 0, 0);
	Part.BoundsMax = FVector3f(100, 100, 0);
	const float Floor[] = {0.0f};
	const float Pond[] = {100.0f, 100.0f, 300.0f};
	const float Glass[] = {100.0f};
	const float Opaque[] = {500.0f};
	WoTMirrorSpecAddSection(Part, TEXT("TestT"), TEXT("Floor"), TEXT(""), 0u, Floor);
	WoTMirrorSpecAddSection(Part, TEXT("TestT"), TEXT("Pond"), TEXT("_T"),
		WoTRuntimeMirrorPolyFlag | WoTMirrorSpecTranslucent, Pond);
	WoTMirrorSpecAddSection(Part, TEXT("TestT"), TEXT("Glass"), TEXT("_T"),
		WoTRuntimeMirrorPolyFlag | WoTMirrorSpecTranslucent, Glass);
	WoTMirrorSpecAddSection(Part, TEXT("TestT"), TEXT("Chrome"), TEXT(""),
		WoTRuntimeMirrorPolyFlag, Opaque);
	return Part;
}

FWoTWorldDescriptor WoTMirrorSpecDescriptor()
{
	FWoTWorldDescriptor Descriptor;
	Descriptor.MapIdentity = TEXT("SyntheticMirrors");
	FWoTBSPPartDescriptor& Main = Descriptor.BSPParts.AddDefaulted_GetRef();
	Main.PartIdentity = Descriptor.MapIdentity;
	Main.Role = EWoTBSPPartRole::Main;
	return Descriptor;
}

TArray<FWoTRuntimeBSPTestConverterResource> WoTMirrorSpecResources()
{
	return {
		{false, TEXT("TestT__Floor"), TEXT("TestT.Floor"), true},
		{false, TEXT("TestT__Pond"), TEXT("TestT.Pond"), true},
		{false, TEXT("TestT__Glass"), TEXT("TestT.Glass"), true},
		{false, TEXT("TestT__Chrome"), TEXT("TestT.Chrome"), true},
	};
}

/** UE's game view terms for a camera (FSceneView view space X right, Y up,
 * Z forward; reversed-Z infinite perspective). */
void WoTMirrorSpecView(const FRotator& Rotation, float HalfFovDegrees,
	FIntPoint Size, FMatrix& OutViewRotation, FMatrix& OutProjection)
{
	OutViewRotation = FInverseRotationMatrix(Rotation) * FMatrix(
		FPlane(0, 0, 1, 0), FPlane(1, 0, 0, 0), FPlane(0, 1, 0, 0), FPlane(0, 0, 0, 1));
	const float HalfFov = FMath::DegreesToRadians(HalfFovDegrees);
	OutProjection = FReversedZPerspectiveMatrix(HalfFov, HalfFov, 1.0f,
		static_cast<float>(Size.X) / static_cast<float>(Size.Y), 10.0f, 10.0f);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMirrorEligibilitySpec,
	"WoT.World.RuntimeMirror.Eligibility", WoTMirrorSpecFlags)

bool FWoTRuntimeMirrorEligibilitySpec::RunTest(const FString&)
{
	// The census flags (Tools/convert/presentation_census.py): the pond
	// portal, the glass, the marble and the stone floor tiles.
	for (const uint32 Flags : {0x0C00000Cu, 0x0800000Cu, 0x08000024u, 0x08000004u})
		TestTrue(FString::Printf(TEXT("0x%08x is a translucent mirror"), Flags),
			WoTRuntimeMirrorSectionEligible(Flags));
	TestFalse(TEXT("an unmirrored translucent sheet"),
		WoTRuntimeMirrorSectionEligible(WoTMirrorSpecTranslucent));
	TestFalse(TEXT("an opaque mirror (pure reflection) is not reproduced"),
		WoTRuntimeMirrorSectionEligible(WoTRuntimeMirrorPolyFlag));
	TestFalse(TEXT("a two-sided mirror is not reproduced"),
		WoTRuntimeMirrorSectionEligible(
			WoTRuntimeMirrorPolyFlag | WoTMirrorSpecTranslucent | WoTMirrorSpecTwoSided));
	TestFalse(TEXT("an invisible mirror draws nothing here"),
		WoTRuntimeMirrorSectionEligible(
			WoTRuntimeMirrorPolyFlag | WoTMirrorSpecTranslucent | 0x1u));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMirrorPlaneSpec,
	"WoT.World.RuntimeMirror.OnePlanePerDistinctPlane", WoTMirrorSpecFlags)

bool FWoTRuntimeMirrorPlaneSpec::RunTest(const FString&)
{
	const FWoTBSPPayloadData Part = WoTMirrorSpecPart();
	TArray<const FWoTBSPPayloadData*> Parts = {&Part};
	TArray<TArray<bool>> Flags;
	TArray<bool>& PartFlags = Flags.AddDefaulted_GetRef();
	for (const FWoTBSPPayloadSection& Section : Part.Sections)
		PartFlags.Add(WoTRuntimeMirrorSectionEligible(Section.SurfaceFlags));
	TArray<FWoTRuntimeMirrorPlane> Planes;
	WoTRuntimeMirrorCollectPlanes(Parts, Flags, Planes);
	if (!TestEqual(TEXT("two distinct mirror planes"), Planes.Num(), 2)) return false;
	TestTrue(TEXT("first plane is z=100"), FMath::IsNearlyEqual(Planes[0].Distance, 100.0f)
		&& Planes[0].Normal.Equals(FVector3f::ZAxisVector));
	TestEqual(TEXT("the coplanar pond and glass triangles share one capture"),
		Planes[0].Triangles.Num(), 3);
	TestTrue(TEXT("second plane is z=300"), FMath::IsNearlyEqual(Planes[1].Distance, 300.0f));
	TestEqual(TEXT("z=300 has one triangle"), Planes[1].Triangles.Num(), 1);

	FWoTBSPPayloadData Payload;
	TArray<FIntPoint> Sources;
	FString Error;
	if (TestTrue(TEXT("the z=100 plane builds a render payload"),
		WoTRuntimeMirrorPlanePayload(Parts, Planes[0], Payload, Sources, Error)))
	{
		TestEqual(TEXT("one section per contributing source"), Payload.Sections.Num(), 2);
		TestTrue(TEXT("sources are the pond then the glass"),
			Sources == TArray<FIntPoint>{FIntPoint(0, 1), FIntPoint(0, 2)});
		TestTrue(TEXT("render-only payload"), Payload.bPortal
			&& !Payload.Sections[0].bCollisionEnabled && !Payload.Sections[1].bCollisionEnabled);
		TestEqual(TEXT("nine compact vertices"), Payload.Positions.Num(), 9);
		TestNotEqual(TEXT("slot names are unique"), Payload.Sections[0].MaterialSlot,
			Payload.Sections[1].MaterialSlot);
		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage());
		FString MeshError;
		TestTrue(TEXT("the payload builds a render mesh"), Mesh != nullptr
			&& WoTRuntimeBSPBuildRenderMesh(*Mesh, Payload, MeshError));
		if (!MeshError.IsEmpty()) AddError(MeshError);
	}
	else AddError(Error);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMirrorReflectionSpec,
	"WoT.World.RuntimeMirror.ReflectedCapture", WoTMirrorSpecFlags)

bool FWoTRuntimeMirrorReflectionSpec::RunTest(const FString&)
{
	// A plane point seen by the main view at (u, v) is seen by the reflected
	// capture at (1-u, v): the capture is the mirror image flipped left/right
	// (proper rotation), so the projected lookup reproduces the reflection.
	FRandomStream Random(1999);
	int32 Checked = 0;
	for (int32 Case = 0; Case < 200; ++Case)
	{
		const FVector Normal = Random.GetUnitVector();
		const double Distance = Random.FRandRange(-3000.0, 3000.0);
		FVector Eye(Random.FRandRange(-2000.0, 2000.0),
			Random.FRandRange(-2000.0, 2000.0), Random.FRandRange(-2000.0, 2000.0));
		const double Height = FVector::DotProduct(Normal, Eye) - Distance;
		if (Height < 1.0) Eye += (1.0 - Height + Random.FRandRange(0.0, 500.0)) * Normal;
		const FRotator Rotation(Random.FRandRange(-80.0, 80.0),
			Random.FRandRange(-180.0, 180.0), 0.0);
		const FRotationMatrix Axes(Rotation);
		const FVector Forward = Axes.GetScaledAxis(EAxis::X);
		const FVector Right = Axes.GetScaledAxis(EAxis::Y);
		const FVector Up = Axes.GetScaledAxis(EAxis::Z);
		const double TanX = FMath::Tan(FMath::DegreesToRadians(Random.FRandRange(60.0, 110.0)) * 0.5);
		const FIntPoint Size(1600, 1200);
		const FWoTMirrorCaptureView View = WoTMirrorReflectView(
			Eye, Forward, Up, Normal, Distance, TanX, Size);
		if (!TestTrue(TEXT("a viewer in front of the plane reflects"), View.bValid)) continue;
		const double TanY = View.TanHalfFovX * Size.Y / Size.X;
		for (int32 Sample = 0; Sample < 8; ++Sample)
		{
			const FVector Ray = (Forward + Random.FRandRange(-0.9, 0.9) * View.TanHalfFovX * Right
				+ Random.FRandRange(-0.9, 0.9) * TanY * Up).GetSafeNormal();
			const double Denominator = FVector::DotProduct(Ray, Normal);
			if (Denominator > -1e-3) continue;
			const FVector Point = Eye + Ray
				* ((Distance - FVector::DotProduct(Normal, Eye)) / Denominator);
			const FVector Delta = Point - Eye;
			const double Depth = FVector::DotProduct(Delta, Forward);
			const double MainU = 0.5 + 0.5 * FVector::DotProduct(Delta, Right)
				/ (Depth * View.TanHalfFovX);
			const double MainV = 0.5 - 0.5 * FVector::DotProduct(Delta, Up) / (Depth * TanY);
			const FVector2D Capture = WoTMirrorProjectToCapture(View, Point);
			TestTrue(TEXT("capture u mirrors the main u"),
				FMath::IsNearlyEqual(Capture.X, 1.0 - MainU, 1e-6));
			TestTrue(TEXT("capture v equals the main v"),
				FMath::IsNearlyEqual(Capture.Y, MainV, 1e-6));
			++Checked;
		}
		TestTrue(TEXT("the reflected origin mirrors the eye"), View.Origin.Equals(
			Eye - 2.0 * (FVector::DotProduct(Normal, Eye) - Distance) * Normal, 1e-6));
		TestTrue(TEXT("the capture projection is rebuilt from its float FOVAngle"),
			FMath::IsNearlyEqual(View.TanHalfFovX,
				FMath::Tan(static_cast<double>(View.FovDegrees) * UE_DOUBLE_PI / 360.0), 1e-12)
			&& FMath::IsNearlyEqual(View.TanHalfFovX, TanX, 1e-6));
	}
	TestTrue(FString::Printf(TEXT("the identity was exercised (%d samples)"), Checked),
		Checked > 100);
	const FWoTMirrorCaptureView Behind = WoTMirrorReflectView(FVector(0, 0, 50),
		FVector::XAxisVector, FVector::ZAxisVector, FVector::ZAxisVector, 100.0,
		1.0, FIntPoint(800, 600));
	TestFalse(TEXT("a viewer behind the one-sided mirror does not capture"), Behind.bValid);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMirrorStageSpec,
	"WoT.World.RuntimeMirror.StagedPerPlane", WoTMirrorSpecFlags)

bool FWoTRuntimeMirrorStageSpec::RunTest(const FString&)
{
	UMaterialInterface* StandIn = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_EntryTranslucent.M_WoT_EntryTranslucent"));
	if (StandIn == nullptr)
	{
		AddInfo(TEXT("the Entry translucent master is absent; staged-mirror spec skipped"));
		return true;
	}
	const FWoTWorldDescriptor Descriptor = WoTMirrorSpecDescriptor();
	const TArray<FWoTBSPPayloadData> Parts = {WoTMirrorSpecPart()};
	FWoTRuntimeMirrorOptions Options;
	Options.MasterOverride = StandIn;
	Options.bAssumeGlobalClipPlane = true;
	UWorld* World = WoTMirrorSpecWorld(TEXT("Staged"));
	AWoTRuntimeBSPWorld* Owner = nullptr;
	FWoTRuntimeMirrorStageStats Stats;
	FString Error;
	if (!TestTrue(TEXT("the mirrored world materializes"),
		WoTWorldBuildRuntimeBSPWithMirrorsForTesting(*World, Descriptor, Parts, Owner,
			Error, WoTMirrorSpecResources(), Options, Stats)) || Owner == nullptr)
	{
		AddError(Error);
		WoTMirrorSpecDestroyWorld(World);
		return false;
	}
	TestTrue(TEXT("nothing skipped"), Stats.SkipReason.IsEmpty());
	TestEqual(TEXT("two planes"), Stats.PlaneCount, 2u);
	TestEqual(TEXT("the pond and glass sections moved"), Stats.SectionCount, 2u);
	TestEqual(TEXT("four mirror triangles"), Stats.TriangleCount, 4u);
	TestEqual(TEXT("the opaque mirror is counted, not staged"),
		Stats.IneligibleSectionCount, 1u);
	if (TestEqual(TEXT("one world geometry part"), Owner->GeometryComponents.Num(), 1))
		TestEqual(TEXT("the world mesh keeps the floor and the opaque mirror"),
			Owner->GeometryComponents[0]->GetNumMaterials(), 2);
	UWoTRuntimeMirrorDriver* Driver = Owner->MirrorDriver;
	if (!TestNotNull(TEXT("mirror driver"), Driver)
		|| !TestEqual(TEXT("one mirror per plane"), Driver->Mirrors.Num(), 2))
	{
		WoTMirrorSpecDestroyWorld(World);
		return false;
	}
	TestTrue(TEXT("the driver ticks after the cameras"),
		Driver->PrimaryComponentTick.TickGroup == TG_PostUpdateWork);
	UTexture* SharedLightmap = Owner->RuntimeTextures.IsValidIndex(1)
		? Owner->RuntimeTextures[1] : nullptr;
	for (int32 Index = 0; Index < Driver->Mirrors.Num(); ++Index)
	{
		const FWoTRuntimeMirrorSlot& Mirror = Driver->Mirrors[Index];
		const FString Label = FString::Printf(TEXT("mirror %d "), Index);
		if (!TestNotNull(Label + TEXT("surface"), Mirror.Surface.Get())
			|| !TestNotNull(Label + TEXT("capture"), Mirror.Capture.Get())
			|| !TestNotNull(Label + TEXT("target"), Mirror.Target.Get())) continue;
		TestEqual(Label + TEXT("surface owned by the runtime world"),
			Mirror.Surface->GetOwner(), static_cast<AActor*>(Owner));
		TestTrue(Label + TEXT("surface has no collision"),
			Mirror.Surface->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
		TestEqual(Label + TEXT("materials per source section"),
			Mirror.Materials.Num(), Mirror.Surface->GetNumMaterials());
		for (UMaterialInstanceDynamic* Material : Mirror.Materials)
		{
			UTexture* Lightmap = nullptr;
			Material->GetTextureParameterValue(
				FHashedMaterialParameterInfo(TEXT("Lightmap")), Lightmap);
			TestEqual(Label + TEXT("keeps the world section's light map"),
				Lightmap, SharedLightmap);
			TestEqual(Label + TEXT("parents on the mirror master"),
				Material->Parent.Get(), StandIn);
		}
		USceneCaptureComponent2D* Capture = Mirror.Capture;
		TestTrue(Label + TEXT("captures display-space scene colour on demand"),
			Capture->CaptureSource == SCS_SceneColorHDR && !Capture->bCaptureEveryFrame
			&& !Capture->bCaptureOnMovement && !Capture->bAlwaysPersistRenderingState);
		TestTrue(Label + TEXT("clips at the mirror plane, keeping the viewer side"),
			Capture->bEnableClipPlane
			&& Capture->ClipPlaneNormal.Equals(FVector::ZAxisVector)
			&& FMath::IsNearlyEqual(Capture->ClipPlaneBase.Z, Mirror.Distance));
		TestTrue(Label + TEXT("hides its own surface"),
			Capture->HiddenComponents.Contains(
				TWeakObjectPtr<UPrimitiveComponent>(Mirror.Surface.Get())));
		TestEqual(Label + TEXT("writes its target"),
			Capture->TextureTarget.Get(), Mirror.Target.Get());
		TestTrue(Label + TEXT("raw 8-bit target"),
			Mirror.Target->bForceLinearGamma
			&& Mirror.Target->GetFormat() == PF_B8G8R8A8);
	}

	// Views: looking straight down from above both planes queues both; from
	// between them only the lower (the upper one is seen from behind); looking
	// up, neither is in view.
	const FIntPoint Size(800, 600);
	FMatrix ViewRotation, Projection;
	WoTMirrorSpecView(FRotator(-89.0, 0.0, 0.0), 45.0f, Size, ViewRotation, Projection);
	TestEqual(TEXT("above both planes"), Driver->UpdateForView(
		FVector(50, 50, 400), ViewRotation, Projection, Size), 2);
	const FWoTRuntimeMirrorSlot& Lower = Driver->Mirrors[0];
	TestTrue(TEXT("the lower capture sits at the reflected eye"),
		Lower.LastView.Origin.Equals(FVector(50, 50, -200), 1e-3));
	TestTrue(TEXT("the capture uses the main horizontal FOV"),
		FMath::IsNearlyEqual(Lower.Capture->FOVAngle, 90.0f, 1e-3f));
	TestTrue(TEXT("the capture follows the main near plane"),
		Lower.Capture->bOverride_CustomNearClippingPlane
		&& FMath::IsNearlyEqual(Lower.Capture->CustomNearClippingPlane, 10.0f));
	TestTrue(TEXT("the target follows the main view size"),
		Lower.Target->SizeX == Size.X && Lower.Target->SizeY == Size.Y);
	TestEqual(TEXT("between the planes"), Driver->UpdateForView(
		FVector(50, 50, 200), ViewRotation, Projection, Size), 1);
	WoTMirrorSpecView(FRotator(89.0, 0.0, 0.0), 45.0f, Size, ViewRotation, Projection);
	TestEqual(TEXT("looking away"), Driver->UpdateForView(
		FVector(50, 50, 400), ViewRotation, Projection, Size), 0);
	WoTMirrorSpecDestroyWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeMirrorSkipSpec,
	"WoT.World.RuntimeMirror.SkipNotFail", WoTMirrorSpecFlags)

bool FWoTRuntimeMirrorSkipSpec::RunTest(const FString&)
{
	const FWoTWorldDescriptor Descriptor = WoTMirrorSpecDescriptor();
	const TArray<FWoTBSPPayloadData> Parts = {WoTMirrorSpecPart()};
	{
		FWoTRuntimeMirrorOptions Options;
		Options.bForceSkip = true;
		UWorld* World = WoTMirrorSpecWorld(TEXT("Forced"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		FWoTRuntimeMirrorStageStats Stats;
		FString Error;
		if (TestTrue(TEXT("a skipped mirror stage still materializes the world"),
			WoTWorldBuildRuntimeBSPWithMirrorsForTesting(*World, Descriptor, Parts,
				Owner, Error, WoTMirrorSpecResources(), Options, Stats)) && Owner != nullptr)
		{
			TestEqual(TEXT("reason"), Stats.SkipReason, FString(WoTRuntimeMirrorSkipForced));
			TestNull(TEXT("no driver"), Owner->MirrorDriver.Get());
			TestEqual(TEXT("nothing staged"), Stats.PlaneCount, 0u);
			if (TestEqual(TEXT("one world part"), Owner->GeometryComponents.Num(), 1))
				TestEqual(TEXT("every section stays in the world"),
					Owner->GeometryComponents[0]->GetNumMaterials(), 4);
		}
		else AddError(Error);
		WoTMirrorSpecDestroyWorld(World);
	}
	{
		// Production seams: whatever this tree provides (generated master,
		// r.AllowGlobalClipPlane), the world builds and a skip keeps it whole.
		UWorld* World = WoTMirrorSpecWorld(TEXT("Production"));
		AWoTRuntimeBSPWorld* Owner = nullptr;
		FWoTRuntimeMirrorStageStats Stats;
		FString Error;
		if (TestTrue(TEXT("the production stage materializes the world"),
			WoTWorldBuildRuntimeBSPWithMirrorsForTesting(*World, Descriptor, Parts,
				Owner, Error, WoTMirrorSpecResources(), FWoTRuntimeMirrorOptions(), Stats))
			&& Owner != nullptr)
		{
			AddInfo(FString::Printf(TEXT("production mirror stage: planes=%u skip=%s"),
				Stats.PlaneCount, Stats.SkipReason.IsEmpty() ? TEXT("-") : *Stats.SkipReason));
			if (!Stats.SkipReason.IsEmpty())
			{
				TestTrue(TEXT("a production skip names its cause"),
					Stats.SkipReason == WoTRuntimeMirrorSkipMaster
					|| Stats.SkipReason == WoTRuntimeMirrorSkipClipPlane);
				TestNull(TEXT("no driver after a skip"), Owner->MirrorDriver.Get());
				if (Owner->GeometryComponents.Num() == 1)
					TestEqual(TEXT("a skip keeps every section in the world"),
						Owner->GeometryComponents[0]->GetNumMaterials(), 4);
			}
			else if (TestNotNull(TEXT("staged driver"), Owner->MirrorDriver.Get()))
			{
				// The generated master: its Reflection/view parameters bind.
				const FWoTRuntimeMirrorSlot& Mirror = Owner->MirrorDriver->Mirrors[0];
				UTexture* Reflection = nullptr;
				TestTrue(TEXT("the master exposes Reflection"),
					Mirror.Materials.Num() > 0 && Mirror.Materials[0]->GetTextureParameterValue(
						FHashedMaterialParameterInfo(WoTRuntimeMirrorReflectionParameter),
						Reflection));
				TestEqual(TEXT("Reflection is the plane's target"),
					Reflection, static_cast<UTexture*>(Mirror.Target.Get()));
				const FIntPoint Size(800, 600);
				FMatrix ViewRotation, Projection;
				WoTMirrorSpecView(FRotator(-89.0, 0.0, 0.0), 45.0f, Size,
					ViewRotation, Projection);
				Owner->MirrorDriver->UpdateForView(
					FVector(50, 50, 400), ViewRotation, Projection, Size);
				FLinearColor Origin;
				TestTrue(TEXT("the master exposes MirrorOrigin"),
					Mirror.Materials[0]->GetVectorParameterValue(
						FHashedMaterialParameterInfo(WoTRuntimeMirrorOriginParameter), Origin));
				TestTrue(TEXT("MirrorOrigin is the reflected eye"),
					FVector(Origin.R, Origin.G, Origin.B).Equals(
						Mirror.LastView.Origin, 1e-2));
			}
		}
		else AddError(Error);
		WoTMirrorSpecDestroyWorld(World);
	}
	return !HasAnyErrors();
}

// The generated masters this integration regenerated or added compile for the
// running shader platform: the mirror master, the world family and the mover
// masters that carry the G45 term (their graphs were saved by -nullrhi
// commandlets, which never compile). A renderer is required; -nullrhi runs only
// confirm the assets load.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGeneratedMastersCompileSpec,
	"WoT.World.RuntimeMirror.GeneratedMastersCompile", WoTMirrorSpecFlags)

bool FWoTGeneratedMastersCompileSpec::RunTest(const FString&)
{
	static const TCHAR* const Masters[] = {
		TEXT("M_WoT_EntryTranslucent_Mirror"),
		TEXT("M_WoT_World"), TEXT("M_WoT_WorldMasked"), TEXT("M_WoT_WorldTranslucent"),
		TEXT("M_WoT_WorldModulated"), TEXT("M_WoT_WorldT_SceneBlend"),
		TEXT("M_WoT_WorldWave"), TEXT("M_WoT_WorldWaveMasked"),
		TEXT("M_WoT_WorldWaveTranslucent"), TEXT("M_WoT_WorldWaveModulated"),
		TEXT("M_WoT_WorldWaveT_SceneBlend"),
		TEXT("M_WoT_EntryWorld"), TEXT("M_WoT_EntryWorld_TwoSided"),
		TEXT("M_WoT_EntryMasked"), TEXT("M_WoT_EntryMasked_TwoSided"),
		TEXT("M_WoT_EntryTranslucent"), TEXT("M_WoT_EntryTranslucent_TwoSided"),
		TEXT("M_WoT_EntryModulated"), TEXT("M_WoT_EntryModulated_TwoSided"),
		TEXT("M_WoT_EntryTranslucent_SceneBlend"),
		TEXT("M_WoT_EntrySkyModulate"), TEXT("M_WoT_EntrySkyAdd"),
		TEXT("M_WoT_MoverLit"), TEXT("M_WoT_MoverLitMasked"),
	};
	const bool bRenders = FApp::CanEverRender();
	if (!bRenders)
	{
		AddInfo(TEXT("no renderer (-nullrhi): shader compilation is exercised by a rendering run"));
	}
	for (const TCHAR* Name : Masters)
	{
		UMaterial* Material = LoadObject<UMaterial>(nullptr,
			*FString::Printf(TEXT("/Game/WoT/Materials/%s.%s"), Name, Name));
		if (Material == nullptr)
		{
			// Generated content: a tree without it is a pipeline-state skip,
			// not a compile failure (WoTStageRuntimeMirrors skips the same way).
			AddWarning(FString::Printf(TEXT("%s is not generated in this tree"), Name));
			continue;
		}
		if (!bRenders) continue;
		FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
		if (!TestNotNull(FString::Printf(TEXT("%s has a material resource"), Name), Resource))
			continue;
		Resource->FinishCompilation();
		TestTrue(FString::Printf(TEXT("%s compiled"), Name),
			Resource->IsCompilationFinished());
		TestTrue(FString::Printf(TEXT("%s compiles without errors: %s"), Name,
			*FString::Join(Resource->GetCompileErrors(), TEXT(" | "))),
			Resource->GetCompileErrors().IsEmpty());
		TestNotNull(FString::Printf(TEXT("%s has a game-thread shader map"), Name),
			Resource->GetGameThreadShaderMap());
	}
	return !HasAnyErrors();
}

#endif
