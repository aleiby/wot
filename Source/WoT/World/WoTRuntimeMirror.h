// Retail PF_Mirrored surfaces for the runtime BSP world (guarded and dev
// builds share this path).
//
// UE1's renderer: OccludeBsp meets a
// PF_Mirrored node, creates a child frame whose coords are the parent's
// mirrored through the node plane (Frame->Coords.MirrorByPlane) with that
// plane as its near clip (NearClip, applied to BSP polys
// and to mesh triangles), restricted to the surface's
// span. DrawFrame renders children first, clears Z, then
// draws the mirror surface over the child image with its own blend: a
// PF_Translucent mirror (every census surface; RenDev->ShinySurfaces is True
// for every hardware device in the shipped WoT.ini) is the single-TMU Glide
// translucent pair FB = M*(Tex + Dst*(1-Tex)) (ONE/ONE_MINUS_SRC_COLOR
// then the DST_COLOR/SRC_COLOR light-map pass) with PF_Occlude, so
// it writes depth; the water zone behind a mirrored portal is never entered
// (the mirror branch precedes the portal branch), so nothing shows through.
//
// Port: one USceneCaptureComponent2D per distinct mirror plane renders the
// scene from the viewer reflected through the plane (a proper rotation; the
// mirror image comes from projecting the surface point into that capture, see
// WoTMirrorProjectToCapture), clipped by the view's global clip plane (the
// NearClip equivalent for every primitive), into a display-space render
// target. The mirror sections leave the world mesh and draw on their own
// component with M_WoT_EntryTranslucent_Mirror (Tools/import/mirror_material.py):
// Emissive = M*(Tex + R*(1-Tex)), opaque, one-sided. Client presentation only;
// dedicated servers never stage mirrors.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "../WoTBSPPayload.h"
#include "WoTRuntimeMirror.generated.h"

class AWoTRuntimeBSPWorld;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneCaptureComponent2D;
class UStaticMesh;
class UStaticMeshComponent;
class UTextureRenderTarget2D;

/** Retail PF_Mirrored. */
inline constexpr uint32 WoTRuntimeMirrorPolyFlag = 0x08000000u;

/** The cooked mirror master and its runtime parameters. */
inline constexpr TCHAR WoTRuntimeMirrorMasterPath[] =
	TEXT("/Game/WoT/Materials/M_WoT_EntryTranslucent_Mirror.M_WoT_EntryTranslucent_Mirror");
inline constexpr TCHAR WoTRuntimeMirrorReflectionParameter[] = TEXT("Reflection");
inline constexpr TCHAR WoTRuntimeMirrorOriginParameter[] = TEXT("MirrorOrigin");
inline constexpr TCHAR WoTRuntimeMirrorAxisXParameter[] = TEXT("MirrorAxisX");
inline constexpr TCHAR WoTRuntimeMirrorAxisYParameter[] = TEXT("MirrorAxisY");
inline constexpr TCHAR WoTRuntimeMirrorAxisZParameter[] = TEXT("MirrorAxisZ");
inline constexpr TCHAR WoTRuntimeMirrorTanParameter[] = TEXT("MirrorTan");

/** Skip reasons (heartbeat tokens). Empty = staged, or nothing to stage. */
inline constexpr TCHAR WoTRuntimeMirrorSkipServer[] = TEXT("server");
inline constexpr TCHAR WoTRuntimeMirrorSkipForced[] = TEXT("forced");
inline constexpr TCHAR WoTRuntimeMirrorSkipClipPlane[] = TEXT("no-global-clip-plane");
inline constexpr TCHAR WoTRuntimeMirrorSkipMaster[] = TEXT("master-absent");
inline constexpr TCHAR WoTRuntimeMirrorSkipMesh[] = TEXT("mesh-failed");

/** True for the one composite the port reproduces: PF_Mirrored, PF_Translucent,
 * one-sided, drawn (every mirror on the 19 MP maps - Tools/convert/
 * presentation_census.py). Opaque (pure reflection), masked, modulated and
 * two-sided mirrors keep their non-mirror section material. */
WOT_API bool WoTRuntimeMirrorSectionEligible(uint32 SurfaceFlags);

/** One triangle of a staged BSP part (index of its first corner). */
struct FWoTRuntimeMirrorTriangle
{
	int32 Part = INDEX_NONE;
	int32 Section = INDEX_NONE;
	uint32 FirstIndex = 0;
};

/** One distinct mirror plane: n.P = Distance, n the surfaces' front normal. */
struct WOT_API FWoTRuntimeMirrorPlane
{
	FVector3f Normal = FVector3f::ZAxisVector;
	float Distance = 0.0f;
	FBox3f Bounds = FBox3f(ForceInit);
	TArray<FWoTRuntimeMirrorTriangle> Triangles;
};

/** Groups every triangle of the flagged sections by its plane (payload
 * normals are the flat surface normal; coplanar = normals within 1e-4 and
 * distances within 0.5 uu, both far inside BSP float noise and far outside
 * any two authored planes). Planes and triangles keep payload order. */
WOT_API void WoTRuntimeMirrorCollectPlanes(
	TConstArrayView<const FWoTBSPPayloadData*> Parts,
	TConstArrayView<TArray<bool>> MirrorSections,
	TArray<FWoTRuntimeMirrorPlane>& OutPlanes);

/** Compact render-only payload of one plane's triangles: one section per
 * contributing (part, section) pair in first-seen order (OutSources), no
 * collision. */
WOT_API bool WoTRuntimeMirrorPlanePayload(
	TConstArrayView<const FWoTBSPPayloadData*> Parts,
	const FWoTRuntimeMirrorPlane& Plane, FWoTBSPPayloadData& OutPayload,
	TArray<FIntPoint>& OutSources, FString& OutError);

/** The capture view that sees, through a mirror, what the viewer sees in it:
 * the viewer reflected through the plane, forward/up reflected (a proper
 * rotation, so the capture image is the mirror image flipped horizontally),
 * the main horizontal field of view, and the aspect of the target. */
struct WOT_API FWoTMirrorCaptureView
{
	bool bValid = false;
	FVector Origin = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector AxisX = FVector::XAxisVector;
	FVector AxisY = FVector::YAxisVector;
	FVector AxisZ = FVector::ZAxisVector;
	/** USceneCaptureComponent2D::FOVAngle (full horizontal, degrees). */
	float FovDegrees = 90.0f;
	/** The capture's own projection (SceneCaptureRendering.cpp
	 * BuildProjectionMatrix: tan(FOVAngle/2) horizontally, times H/W
	 * vertically). */
	double TanHalfFovX = 1.0;
	double TanHalfFovY = 1.0;
};

/** Invalid when the viewer is on or behind the one-sided mirror plane. */
WOT_API FWoTMirrorCaptureView WoTMirrorReflectView(const FVector& ViewOrigin,
	const FVector& ViewForward, const FVector& ViewUp, const FVector& PlaneNormal,
	double PlaneDistance, double TanHalfFovX, FIntPoint TargetSize);

/** C++ twin of the mirror master's projection HLSL: the capture-target UV of
 * a world point (u right, v down). */
WOT_API FVector2D WoTMirrorProjectToCapture(
	const FWoTMirrorCaptureView& View, const FVector& WorldPoint);

/** One staged mirror plane and its client-side render objects. */
USTRUCT()
struct FWoTRuntimeMirrorSlot
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Surface;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> Capture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Target;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	FVector Normal = FVector::ZAxisVector;
	double Distance = 0.0;
	FBox Bounds = FBox(ForceInit);
	uint32 TriangleCount = 0;
	uint32 CaptureCount = 0;
	FWoTMirrorCaptureView LastView;
};

/** Drives every mirror capture from the final camera of the frame (cameras
 * update just before TG_PostUpdateWork, LevelTick.cpp), so the capture is
 * queued for this frame's render and the reflection never lags the view. */
UCLASS(Transient, ClassGroup = WoT)
class WOT_API UWoTRuntimeMirrorDriver final : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoTRuntimeMirrorDriver();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	/** Aims, sizes and queues every mirror in view of one perspective main
	 * view (FSceneViewProjectionData terms); returns the captures queued. */
	int32 UpdateForView(const FVector& ViewOrigin,
		const FMatrix& ViewRotationMatrix, const FMatrix& ProjectionMatrix,
		FIntPoint ViewSize);

	UPROPERTY(Transient)
	TArray<FWoTRuntimeMirrorSlot> Mirrors;
};

/** One staged runtime BSP part as the mirror stage sees it (after the sky
 * stage decided which sections stay in the world). */
struct FWoTRuntimeMirrorPartInput
{
	const FWoTBSPPayloadData* Visible = nullptr;
	/** The world-slot material of every visible section. */
	TArray<UMaterialInstanceDynamic*> Materials;
	/** Per visible section; the stage clears the flag of every section it
	 * moves onto a mirror surface. */
	TArray<bool>* KeepInWorld = nullptr;
};

struct FWoTRuntimeMirrorStageStats
{
	uint32 PlaneCount = 0;
	uint32 SectionCount = 0;
	uint32 TriangleCount = 0;
	/** PF_Mirrored sections of a composite the port does not reproduce. */
	uint32 IneligibleSectionCount = 0;
	FString SkipReason;
};

/** Automation seams; production passes the defaults. */
struct FWoTRuntimeMirrorOptions
{
	/** Stand-in master when the generated one is absent from a test tree. */
	UMaterialInterface* MasterOverride = nullptr;
	bool bForceSkip = false;
	/** Treat r.AllowGlobalClipPlane as enabled. */
	bool bAssumeGlobalClipPlane = false;
};

/** Stage every mirror plane of the runtime BSP world: surface component,
 * capture, render target and mirror materials, plus the driver. Any skip
 * leaves every section in the world with its non-mirror material. */
WOT_API void WoTStageRuntimeMirrors(AWoTRuntimeBSPWorld& RuntimeWorld,
	TArrayView<FWoTRuntimeMirrorPartInput> Parts,
	const FWoTRuntimeMirrorOptions& Options, FWoTRuntimeMirrorStageStats& OutStats);
