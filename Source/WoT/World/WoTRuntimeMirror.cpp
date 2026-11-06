#include "WoTRuntimeMirror.h"

#include "WoTRuntimeBSPRenderer.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "ConvexVolume.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Math/TranslationMatrix.h"
#include "SceneView.h"
#include "UnrealClient.h"

namespace
{
constexpr uint32 WoTMirrorPFInvisible = 0x00000001u;
constexpr uint32 WoTMirrorPFTranslucent = 0x00000004u;
constexpr uint32 WoTMirrorPFFakeBackdrop = 0x00000080u;
constexpr uint32 WoTMirrorPFTwoSided = 0x00000100u;

// Coplanarity: BSP surface normals of one authored plane agree to float
// noise, and two distinct authored mirror planes differ by whole units.
constexpr float WoTMirrorNormalDot = 0.9999f;
constexpr float WoTMirrorDistanceTolerance = 0.5f;

// The render target starts tiny and follows the main view size once the
// mirror is first in view (no full-size allocation for an unseen mirror).
constexpr int32 WoTMirrorInitialTargetSize = 16;

bool WoTMirrorTrianglePlane(const FWoTBSPPayloadData& Part, uint32 FirstIndex,
	FVector3f& OutNormal, float& OutDistance)
{
	if (static_cast<int64>(FirstIndex) + 2 >= Part.Indices.Num()) return false;
	const uint32 Corner = Part.Indices[static_cast<int32>(FirstIndex)];
	if (!Part.Positions.IsValidIndex(static_cast<int32>(Corner))
		|| !Part.Normals.IsValidIndex(static_cast<int32>(Corner))) return false;
	OutNormal = Part.Normals[static_cast<int32>(Corner)].GetSafeNormal();
	if (OutNormal.IsNearlyZero()) return false;
	OutDistance = FVector3f::DotProduct(
		OutNormal, Part.Positions[static_cast<int32>(Corner)]);
	return true;
}

void WoTMirrorCopySectionParameters(UMaterialInstanceDynamic& Source,
	UMaterialInstanceDynamic& Mirror)
{
	for (const TCHAR* TextureName : {TEXT("Tex"), TEXT("Lightmap")})
	{
		UTexture* Value = nullptr;
		if (Source.GetTextureParameterValue(
			FHashedMaterialParameterInfo(TextureName), Value) && Value != nullptr)
			Mirror.SetTextureParameterValue(TextureName, Value);
	}
	for (const TCHAR* ScalarName : {TEXT("PanU"), TEXT("PanV"),
		TEXT("WoTSceneTime"), TEXT("DynLightEnable")})
	{
		float Value = 0.0f;
		if (Source.GetScalarParameterValue(
			FHashedMaterialParameterInfo(ScalarName), Value))
			Mirror.SetScalarParameterValue(ScalarName, Value);
	}
}

void WoTMirrorApplyView(TConstArrayView<TObjectPtr<UMaterialInstanceDynamic>> Materials,
	const FWoTMirrorCaptureView& View)
{
	auto AsColor = [](const FVector& Value)
	{
		return FLinearColor(static_cast<float>(Value.X),
			static_cast<float>(Value.Y), static_cast<float>(Value.Z), 0.0f);
	};
	const FLinearColor Tangents(static_cast<float>(View.TanHalfFovX),
		static_cast<float>(View.TanHalfFovY), 0.0f, 0.0f);
	for (UMaterialInstanceDynamic* Material : Materials)
	{
		if (!IsValid(Material)) continue;
		Material->SetVectorParameterValue(
			WoTRuntimeMirrorOriginParameter, AsColor(View.Origin));
		Material->SetVectorParameterValue(
			WoTRuntimeMirrorAxisXParameter, AsColor(View.AxisX));
		Material->SetVectorParameterValue(
			WoTRuntimeMirrorAxisYParameter, AsColor(View.AxisY));
		Material->SetVectorParameterValue(
			WoTRuntimeMirrorAxisZParameter, AsColor(View.AxisZ));
		Material->SetVectorParameterValue(WoTRuntimeMirrorTanParameter, Tangents);
	}
}
}

bool WoTRuntimeMirrorSectionEligible(const uint32 SurfaceFlags)
{
	return (SurfaceFlags & WoTRuntimeMirrorPolyFlag) != 0
		&& (SurfaceFlags & WoTMirrorPFTranslucent) != 0
		&& (SurfaceFlags & (WoTMirrorPFTwoSided | WoTMirrorPFInvisible
			| WoTMirrorPFFakeBackdrop)) == 0;
}

void WoTRuntimeMirrorCollectPlanes(
	TConstArrayView<const FWoTBSPPayloadData*> Parts,
	TConstArrayView<TArray<bool>> MirrorSections,
	TArray<FWoTRuntimeMirrorPlane>& OutPlanes)
{
	OutPlanes.Reset();
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTBSPPayloadData* Part = Parts[PartIndex];
		if (Part == nullptr || !MirrorSections.IsValidIndex(PartIndex)) continue;
		const TArray<bool>& Flags = MirrorSections[PartIndex];
		for (int32 SectionIndex = 0; SectionIndex < Part->Sections.Num(); ++SectionIndex)
		{
			if (!Flags.IsValidIndex(SectionIndex) || !Flags[SectionIndex]) continue;
			const FWoTBSPPayloadSection& Section = Part->Sections[SectionIndex];
			const uint64 End = static_cast<uint64>(Section.FirstIndex) + Section.IndexCount;
			if (End > static_cast<uint64>(Part->Indices.Num())) continue;
			for (uint32 First = Section.FirstIndex; static_cast<uint64>(First) + 3 <= End;
				First += 3)
			{
				FVector3f Normal;
				float Distance = 0.0f;
				if (!WoTMirrorTrianglePlane(*Part, First, Normal, Distance)) continue;
				FWoTRuntimeMirrorPlane* Plane = OutPlanes.FindByPredicate(
					[&Normal, Distance](const FWoTRuntimeMirrorPlane& Candidate)
					{
						return FVector3f::DotProduct(Candidate.Normal, Normal)
								>= WoTMirrorNormalDot
							&& FMath::Abs(Candidate.Distance - Distance)
								<= WoTMirrorDistanceTolerance;
					});
				if (Plane == nullptr)
				{
					Plane = &OutPlanes.AddDefaulted_GetRef();
					Plane->Normal = Normal;
					Plane->Distance = Distance;
				}
				FWoTRuntimeMirrorTriangle& Triangle = Plane->Triangles.AddDefaulted_GetRef();
				Triangle.Part = PartIndex;
				Triangle.Section = SectionIndex;
				Triangle.FirstIndex = First;
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 Vertex = static_cast<int32>(
						Part->Indices[static_cast<int32>(First) + Corner]);
					if (Part->Positions.IsValidIndex(Vertex))
						Plane->Bounds += Part->Positions[Vertex];
				}
			}
		}
	}
}

bool WoTRuntimeMirrorPlanePayload(
	TConstArrayView<const FWoTBSPPayloadData*> Parts,
	const FWoTRuntimeMirrorPlane& Plane, FWoTBSPPayloadData& OutPayload,
	TArray<FIntPoint>& OutSources, FString& OutError)
{
	OutPayload = FWoTBSPPayloadData();
	OutSources.Reset();
	// Render-only: the portal role carries no collision (the complete part
	// keeps cooking the collision of these triangles).
	OutPayload.FormatVersion = 4;
	OutPayload.bPortal = true;
	for (const FWoTRuntimeMirrorTriangle& Triangle : Plane.Triangles)
		OutSources.AddUnique(FIntPoint(Triangle.Part, Triangle.Section));
	for (const FIntPoint& Source : OutSources)
	{
		const FWoTBSPPayloadData* Part = Parts.IsValidIndex(Source.X)
			? Parts[Source.X] : nullptr;
		if (Part == nullptr || !Part->Sections.IsValidIndex(Source.Y))
		{
			OutError = TEXT("a mirror triangle names a missing BSP section");
			return false;
		}
		FWoTBSPPayloadSection& Added =
			OutPayload.Sections.Add_GetRef(Part->Sections[Source.Y]);
		// Unique slot names: the fast build binds polygon groups to material
		// slots by name, and two sources may share a converter slot.
		Added.MaterialSlot += FString::Printf(
			TEXT("__Mirror%02d"), OutPayload.Sections.Num() - 1);
		Added.FirstIndex = static_cast<uint32>(OutPayload.Indices.Num());
		Added.IndexCount = 0;
		Added.MinVertex = static_cast<uint32>(OutPayload.Positions.Num());
		Added.bCollisionEnabled = false;
		for (const FWoTRuntimeMirrorTriangle& Triangle : Plane.Triangles)
		{
			if (Triangle.Part != Source.X || Triangle.Section != Source.Y) continue;
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 IndexAt = static_cast<int32>(Triangle.FirstIndex) + Corner;
				const int32 Vertex = Part->Indices.IsValidIndex(IndexAt)
					? static_cast<int32>(Part->Indices[IndexAt]) : INDEX_NONE;
				if (!Part->Positions.IsValidIndex(Vertex)
					|| !Part->Normals.IsValidIndex(Vertex)
					|| !Part->UV0.IsValidIndex(Vertex) || !Part->UV1.IsValidIndex(Vertex))
				{
					OutError = TEXT("a mirror triangle references a missing vertex");
					return false;
				}
				OutPayload.Indices.Add(static_cast<uint32>(OutPayload.Positions.Num()));
				OutPayload.Positions.Add(Part->Positions[Vertex]);
				OutPayload.Normals.Add(Part->Normals[Vertex]);
				OutPayload.UV0.Add(Part->UV0[Vertex]);
				OutPayload.UV1.Add(Part->UV1[Vertex]);
			}
			Added.IndexCount += 3;
		}
		if (Added.IndexCount == 0)
		{
			OutError = TEXT("a mirror section contributed no triangle");
			return false;
		}
		Added.MaxVertex = static_cast<uint32>(OutPayload.Positions.Num() - 1);
	}
	if (OutPayload.Positions.IsEmpty())
	{
		OutError = TEXT("a mirror plane has no triangle");
		return false;
	}
	OutPayload.BoundsMin = OutPayload.BoundsMax = OutPayload.Positions[0];
	for (const FVector3f& Position : OutPayload.Positions)
	{
		OutPayload.BoundsMin = OutPayload.BoundsMin.ComponentMin(Position);
		OutPayload.BoundsMax = OutPayload.BoundsMax.ComponentMax(Position);
	}
	return true;
}

FWoTMirrorCaptureView WoTMirrorReflectView(const FVector& ViewOrigin,
	const FVector& ViewForward, const FVector& ViewUp, const FVector& PlaneNormal,
	const double PlaneDistance, const double TanHalfFovX, const FIntPoint TargetSize)
{
	FWoTMirrorCaptureView Out;
	const FVector Normal = PlaneNormal.GetSafeNormal();
	const double Height = FVector::DotProduct(Normal, ViewOrigin) - PlaneDistance;
	if (Normal.IsZero() || !(Height > UE_KINDA_SMALL_NUMBER)
		|| !(TanHalfFovX > 0.0) || TargetSize.X <= 0 || TargetSize.Y <= 0)
	{
		return Out;
	}
	// Frame->Coords.MirrorByPlane: the reflected viewpoint and axes. The
	// reflected (forward, up) pair spans a proper rotation whose right axis is
	// the NEGATED reflected right - the capture image is the mirror image
	// flipped left/right, which the projected lookup undoes exactly.
	Out.Origin = ViewOrigin - 2.0 * Height * Normal;
	const FVector Forward =
		ViewForward - 2.0 * FVector::DotProduct(ViewForward, Normal) * Normal;
	const FVector Up = ViewUp - 2.0 * FVector::DotProduct(ViewUp, Normal) * Normal;
	Out.Rotation = FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat();
	Out.AxisX = Out.Rotation.GetAxisX();
	Out.AxisY = Out.Rotation.GetAxisY();
	Out.AxisZ = Out.Rotation.GetAxisZ();
	// The capture stores FOVAngle as a float and rebuilds its projection from
	// it (SceneCaptureRendering.cpp: tan(FOVAngle*PI/360), Y scaled by W/H).
	Out.FovDegrees = static_cast<float>(
		FMath::RadiansToDegrees(2.0 * FMath::Atan(TanHalfFovX)));
	Out.TanHalfFovX = FMath::Tan(static_cast<double>(Out.FovDegrees) * UE_DOUBLE_PI / 360.0);
	Out.TanHalfFovY = Out.TanHalfFovX * static_cast<double>(TargetSize.Y)
		/ static_cast<double>(TargetSize.X);
	Out.bValid = true;
	return Out;
}

FVector2D WoTMirrorProjectToCapture(
	const FWoTMirrorCaptureView& View, const FVector& WorldPoint)
{
	const FVector Delta = WorldPoint - View.Origin;
	const double Depth = FMath::Max(FVector::DotProduct(Delta, View.AxisX), 1e-4);
	return FVector2D(
		0.5 + 0.5 * FVector::DotProduct(Delta, View.AxisY) / (Depth * View.TanHalfFovX),
		0.5 - 0.5 * FVector::DotProduct(Delta, View.AxisZ) / (Depth * View.TanHalfFovY));
}

UWoTRuntimeMirrorDriver::UWoTRuntimeMirrorDriver()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.bTickEvenWhenPaused = true;
	// Player cameras update after every actor tick group and before
	// TG_PostUpdateWork (LevelTick.cpp "Update cameras last").
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bAutoActivate = true;
}

void UWoTRuntimeMirrorDriver::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UWorld* World = GetWorld();
	if (World == nullptr || Mirrors.IsEmpty() || GEngine == nullptr
		|| World->GetNetMode() == NM_DedicatedServer) return;
	// One viewer: each plane owns one capture/target/material set, so the
	// mirrors follow the first local player. Retail and the port have exactly
	// one local player (nothing creates another); split-screen would need a
	// capture set per view (PORT_NOTES "Mirror surfaces").
	APlayerController* Controller = GEngine->GetFirstLocalPlayerController(World);
	ULocalPlayer* Player = Controller != nullptr ? Controller->GetLocalPlayer() : nullptr;
	FViewport* Viewport = Player != nullptr && Player->ViewportClient != nullptr
		? Player->ViewportClient->Viewport : nullptr;
	FSceneViewProjectionData Projection;
	if (Viewport == nullptr || !Player->GetProjectionData(Viewport, Projection)
		|| !Projection.IsValidViewRectangle()) return;
	UpdateForView(Projection.ViewOrigin, Projection.ViewRotationMatrix,
		Projection.ProjectionMatrix, Projection.GetConstrainedViewRect().Size());
}

int32 UWoTRuntimeMirrorDriver::UpdateForView(const FVector& ViewOrigin,
	const FMatrix& ViewRotationMatrix, const FMatrix& ProjectionMatrix,
	const FIntPoint ViewSize)
{
	if (ViewSize.X <= 0 || ViewSize.Y <= 0 || ProjectionMatrix.M[3][3] >= 1.0f
		|| ProjectionMatrix.M[0][0] <= 0.0) return 0;
	// FSceneView view space: X right, Y up, Z forward (world axes = columns).
	const FVector Forward = ViewRotationMatrix.GetColumn(2);
	const FVector Up = ViewRotationMatrix.GetColumn(1);
	const double TanHalfFovX = 1.0 / ProjectionMatrix.M[0][0];
	const float NearPlane =
		FSceneViewProjectionData::GetNearPlaneFromProjectionMatrix(ProjectionMatrix);
	FConvexVolume Frustum;
	GetViewFrustumBounds(Frustum,
		FTranslationMatrix(-ViewOrigin) * ViewRotationMatrix * ProjectionMatrix, true);
	int32 Queued = 0;
	for (FWoTRuntimeMirrorSlot& Mirror : Mirrors)
	{
		if (!IsValid(Mirror.Capture) || !IsValid(Mirror.Target)
			|| !Mirror.Bounds.IsValid
			|| !Frustum.IntersectBox(Mirror.Bounds.GetCenter(), Mirror.Bounds.GetExtent()))
			continue;
		const FWoTMirrorCaptureView View = WoTMirrorReflectView(ViewOrigin, Forward,
			Up, Mirror.Normal, Mirror.Distance, TanHalfFovX, ViewSize);
		// Behind (or on) the one-sided mirror its surface is culled anyway.
		if (!View.bValid) continue;
		if (Mirror.Target->SizeX != ViewSize.X || Mirror.Target->SizeY != ViewSize.Y)
			Mirror.Target->ResizeTarget(static_cast<uint32>(ViewSize.X),
				static_cast<uint32>(ViewSize.Y));
		Mirror.Capture->SetWorldLocationAndRotation(View.Origin, View.Rotation);
		Mirror.Capture->FOVAngle = View.FovDegrees;
		Mirror.Capture->bOverride_CustomNearClippingPlane = true;
		Mirror.Capture->CustomNearClippingPlane = NearPlane;
		WoTMirrorApplyView(Mirror.Materials, View);
		Mirror.Capture->CaptureSceneDeferred();
		Mirror.LastView = View;
		++Mirror.CaptureCount;
		++Queued;
	}
	return Queued;
}

void WoTStageRuntimeMirrors(AWoTRuntimeBSPWorld& RuntimeWorld,
	TArrayView<FWoTRuntimeMirrorPartInput> Parts,
	const FWoTRuntimeMirrorOptions& Options, FWoTRuntimeMirrorStageStats& OutStats)
{
	OutStats = FWoTRuntimeMirrorStageStats();
	TArray<const FWoTBSPPayloadData*> Payloads;
	TArray<TArray<bool>> Eligible;
	Payloads.SetNum(Parts.Num());
	Eligible.SetNum(Parts.Num());
	uint32 EligibleSections = 0;
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		const FWoTRuntimeMirrorPartInput& Input = Parts[PartIndex];
		Payloads[PartIndex] = Input.Visible;
		if (Input.Visible == nullptr || Input.KeepInWorld == nullptr) continue;
		Eligible[PartIndex].Init(false, Input.Visible->Sections.Num());
		for (int32 SectionIndex = 0; SectionIndex < Input.Visible->Sections.Num();
			++SectionIndex)
		{
			const uint32 Flags = Input.Visible->Sections[SectionIndex].SurfaceFlags;
			// A section the sky box took stays there (no mirror lies in a sky
			// zone on the 19 maps).
			if ((Flags & WoTRuntimeMirrorPolyFlag) == 0
				|| !Input.KeepInWorld->IsValidIndex(SectionIndex)
				|| !(*Input.KeepInWorld)[SectionIndex]) continue;
			if (!WoTRuntimeMirrorSectionEligible(Flags)
				|| !Input.Materials.IsValidIndex(SectionIndex)
				|| Input.Materials[SectionIndex] == nullptr)
			{
				++OutStats.IneligibleSectionCount;
				continue;
			}
			Eligible[PartIndex][SectionIndex] = true;
			++EligibleSections;
		}
	}
	if (EligibleSections == 0) return;

	UWorld* World = RuntimeWorld.GetWorld();
	if (IsRunningDedicatedServer()
		|| (World != nullptr && World->GetNetMode() == NM_DedicatedServer))
	{
		OutStats.SkipReason = WoTRuntimeMirrorSkipServer;
		return;
	}
	if (Options.bForceSkip)
	{
		OutStats.SkipReason = WoTRuntimeMirrorSkipForced;
		return;
	}
	// The capture's clip plane (the retail NearClip) is compiled into the
	// vertex shaders only with r.AllowGlobalClipPlane (Common.ush
	// USE_GLOBAL_CLIP_PLANE); without it a reflection would show what lies
	// behind the mirror, so keep the plain sheet instead.
	const IConsoleVariable* ClipPlaneSupport =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.AllowGlobalClipPlane"));
	if (!Options.bAssumeGlobalClipPlane
		&& (ClipPlaneSupport == nullptr || ClipPlaneSupport->GetInt() == 0))
	{
		OutStats.SkipReason = WoTRuntimeMirrorSkipClipPlane;
		return;
	}
	UMaterialInterface* Master = Options.MasterOverride != nullptr
		? Options.MasterOverride
		: LoadObject<UMaterialInterface>(nullptr, WoTRuntimeMirrorMasterPath);
	if (Master == nullptr)
	{
		OutStats.SkipReason = WoTRuntimeMirrorSkipMaster;
		return;
	}

	TArray<FWoTRuntimeMirrorPlane> Planes;
	WoTRuntimeMirrorCollectPlanes(Payloads, Eligible, Planes);
	// Phase one (fallible): every plane's render mesh. Nothing is attached to
	// the world until all of them built.
	TArray<FWoTBSPPayloadData> PlanePayloads;
	TArray<TArray<FIntPoint>> PlaneSources;
	TArray<UStaticMesh*> PlaneMeshes;
	PlanePayloads.SetNum(Planes.Num());
	PlaneSources.SetNum(Planes.Num());
	for (int32 PlaneIndex = 0; PlaneIndex < Planes.Num(); ++PlaneIndex)
	{
		FString MeshError;
		UStaticMesh* Mesh = NewObject<UStaticMesh>(&RuntimeWorld,
			FName(*FString::Printf(TEXT("RuntimeMirrorMesh_%02d"), PlaneIndex)),
			RF_Transient);
		if (Mesh == nullptr
			|| !WoTRuntimeMirrorPlanePayload(Payloads, Planes[PlaneIndex],
				PlanePayloads[PlaneIndex], PlaneSources[PlaneIndex], MeshError)
			|| !WoTRuntimeBSPBuildRenderMesh(*Mesh, PlanePayloads[PlaneIndex], MeshError))
		{
			OutStats.SkipReason = WoTRuntimeMirrorSkipMesh;
			return;
		}
		PlaneMeshes.Add(Mesh);
	}

	// Phase two: allocate every object, then attach. An allocation failure
	// leaves the world exactly as the sky stage left it.
	struct FStagedObjects
	{
		UTextureRenderTarget2D* Target = nullptr;
		UStaticMeshComponent* Surface = nullptr;
		USceneCaptureComponent2D* Capture = nullptr;
	};
	TArray<FStagedObjects> Staged;
	Staged.SetNum(Planes.Num());
	UWoTRuntimeMirrorDriver* Driver = NewObject<UWoTRuntimeMirrorDriver>(
		&RuntimeWorld, TEXT("RuntimeMirrorDriver"), RF_Transient);
	bool bAllocated = Driver != nullptr;
	for (int32 PlaneIndex = 0; bAllocated && PlaneIndex < Planes.Num(); ++PlaneIndex)
	{
		FStagedObjects& Objects = Staged[PlaneIndex];
		Objects.Target = NewObject<UTextureRenderTarget2D>(&RuntimeWorld,
			FName(*FString::Printf(TEXT("RuntimeMirrorTarget_%02d"), PlaneIndex)),
			RF_Transient);
		Objects.Surface = NewObject<UStaticMeshComponent>(&RuntimeWorld,
			FName(*FString::Printf(TEXT("RuntimeMirrorSurface_%02d"), PlaneIndex)),
			RF_Transient);
		Objects.Capture = NewObject<USceneCaptureComponent2D>(&RuntimeWorld,
			FName(*FString::Printf(TEXT("RuntimeMirrorCapture_%02d"), PlaneIndex)),
			RF_Transient);
		bAllocated = Objects.Target != nullptr && Objects.Surface != nullptr
			&& Objects.Capture != nullptr;
	}
	if (!bAllocated)
	{
		OutStats.SkipReason = WoTRuntimeMirrorSkipMesh;
		return;
	}
	RuntimeWorld.AddInstanceComponent(Driver);
	RuntimeWorld.AddOwnedComponent(Driver);
	RuntimeWorld.MirrorDriver = Driver;
	USceneComponent* Root = RuntimeWorld.GetRootComponent();
	for (int32 PlaneIndex = 0; PlaneIndex < Planes.Num(); ++PlaneIndex)
	{
		const FWoTRuntimeMirrorPlane& Plane = Planes[PlaneIndex];
		const FStagedObjects& Objects = Staged[PlaneIndex];
		FWoTRuntimeMirrorSlot& Slot = Driver->Mirrors.AddDefaulted_GetRef();
		Slot.Mesh = PlaneMeshes[PlaneIndex];
		Slot.Normal = FVector(Plane.Normal);
		Slot.Distance = static_cast<double>(Plane.Distance);
		Slot.Bounds = FBox(FVector(Plane.Bounds.Min), FVector(Plane.Bounds.Max));
		Slot.TriangleCount = static_cast<uint32>(Plane.Triangles.Num());
		RuntimeWorld.RuntimeMeshes.Add(Slot.Mesh);

		// Display-space scene colour sampled raw (the world masters'
		// GAMMA_SPACE convention); 8 bits per channel and clamped, like the
		// retail frame buffer the child frame was drawn into.
		UTextureRenderTarget2D* Target = Objects.Target;
		Target->ClearColor = FLinearColor::Black;
		Target->bAutoGenerateMips = false;
		Target->AddressX = TA_Clamp;
		Target->AddressY = TA_Clamp;
		Target->InitCustomFormat(WoTMirrorInitialTargetSize, WoTMirrorInitialTargetSize,
			PF_B8G8R8A8, true);
		Slot.Target = Target;

		UStaticMeshComponent* Surface = Objects.Surface;
		RuntimeWorld.AddInstanceComponent(Surface);
		RuntimeWorld.AddOwnedComponent(Surface);
		Surface->SetupAttachment(Root);
		Surface->SetMobility(EComponentMobility::Static);
		Surface->SetStaticMesh(Slot.Mesh);
		Surface->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Surface->SetGenerateOverlapEvents(false);
		Surface->SetCastShadow(false);
		Slot.Surface = Surface;

		const TArray<FIntPoint>& Sources = PlaneSources[PlaneIndex];
		for (int32 SectionIndex = 0; SectionIndex < Sources.Num(); ++SectionIndex)
		{
			const FIntPoint Source = Sources[SectionIndex];
			UMaterialInstanceDynamic* WorldMaterial =
				Parts[Source.X].Materials[Source.Y];
			UMaterialInstanceDynamic* MirrorMaterial =
				UMaterialInstanceDynamic::Create(Master, &RuntimeWorld);
			if (MirrorMaterial == nullptr || WorldMaterial == nullptr) continue;
			WoTMirrorCopySectionParameters(*WorldMaterial, *MirrorMaterial);
			MirrorMaterial->SetTextureParameterValue(
				WoTRuntimeMirrorReflectionParameter, Target);
			Surface->SetMaterial(SectionIndex, MirrorMaterial);
			Slot.Materials.Add(MirrorMaterial);
			RuntimeWorld.RuntimeMaterials.Add(MirrorMaterial);
			if (RuntimeWorld.SceneTimeMaterials.Contains(WorldMaterial))
				RuntimeWorld.SceneTimeMaterials.Add(MirrorMaterial);
		}

		USceneCaptureComponent2D* Capture = Objects.Capture;
		RuntimeWorld.AddInstanceComponent(Capture);
		RuntimeWorld.AddOwnedComponent(Capture);
		Capture->SetupAttachment(Root);
		Capture->SetMobility(EComponentMobility::Movable);
		// Queued by the driver only on frames the mirror is in view.
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		// No view state: a stateless view renders at PreExposure 1
		// (PostProcessEyeAdaptation.cpp FViewInfo::UpdatePreExposure), so the
		// target holds the unscaled display-space scene colour the master
		// composites; and stale occlusion history from a view that jumps with
		// the camera would pop reflected geometry.
		Capture->bAlwaysPersistRenderingState = false;
		Capture->CaptureSource = SCS_SceneColorHDR;
		Capture->TextureTarget = Target;
		// UE1 NearClip = the mirror plane, keeping the viewer's side.
		Capture->bEnableClipPlane = true;
		Capture->ClipPlaneBase = Slot.Normal * Slot.Distance;
		Capture->ClipPlaneNormal = Slot.Normal;
		// The surface itself lies on the clip plane and samples this target.
		Capture->HideComponent(Surface);
		Slot.Capture = Capture;

		OutStats.TriangleCount += Slot.TriangleCount;
	}
	for (int32 PartIndex = 0; PartIndex < Parts.Num(); ++PartIndex)
	{
		for (int32 SectionIndex = 0; SectionIndex < Eligible[PartIndex].Num(); ++SectionIndex)
		{
			if (!Eligible[PartIndex][SectionIndex]) continue;
			(*Parts[PartIndex].KeepInWorld)[SectionIndex] = false;
			++OutStats.SectionCount;
		}
	}
	OutStats.PlaneCount = static_cast<uint32>(Planes.Num());
}
