#include "WoTVRSpecHMD.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRSyntheticSession.h"

#include "DefaultXRCamera.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "RenderCore.h"
#include "RenderingThread.h"
#include "StereoRendering.h"
#include "Widgets/SViewport.h"

FWoTVRSpecHMD::FWoTVRSpecHMD(const TSharedRef<FWoTVRSyntheticSession>& InSession)
	: FHeadMountedDisplayBase(nullptr)
	, Session(InSession)
{
}

TSharedPtr<FDefaultXRCamera, ESPMode::ThreadSafe> FWoTVRSpecHMD::GetDefaultCamera()
{
	// FXRTrackingSystemBase::GetXRCamera creates the engine's default camera.
	return StaticCastSharedPtr<FDefaultXRCamera>(GetXRCamera());
}

FName FWoTVRSpecHMD::GetSystemName() const
{
	static const FName Name(TEXT("WoTVRSpecHMD"));
	return Name;
}

int32 FWoTVRSpecHMD::GetXRSystemFlags() const
{
	return EXRSystemFlags::IsHeadMounted;
}

bool FWoTVRSpecHMD::EnumerateTrackedDevices(TArray<int32>& OutDevices, EXRTrackedDeviceType Type)
{
	if (Type == EXRTrackedDeviceType::Any || Type == EXRTrackedDeviceType::HeadMountedDisplay)
	{
		OutDevices.Add(IXRTrackingSystem::HMDDeviceId);
		return true;
	}
	return false;
}

bool FWoTVRSpecHMD::GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition)
{
	OutOrientation = FQuat::Identity;
	OutPosition = FVector::ZeroVector;
	FWoTVRHeadPose Pose;
	if (DeviceId != IXRTrackingSystem::HMDDeviceId || !Session->GetHeadPose(Pose))
	{
		return false;
	}
	OutOrientation = Pose.Orientation;
	OutPosition = (Pose.PositionMeters + (bLatePose ? LateDeltaMeters : FVector::ZeroVector)) * WorldToMeters;
	return true;
}

bool FWoTVRSpecHMD::IsHeadTrackingAllowed() const
{
	// FHeadMountedDisplayBase also asks the editor engine, which allows no head
	// tracking outside a VR preview; a game allows it while stereo is on.
	return IsStereoEnabled();
}

bool FWoTVRSpecHMD::OnStartGameFrame(FWorldContext& WorldContext)
{
	const UWorld* World = WorldContext.World();
	const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
	if (Settings && Settings->WorldToMeters > 0.0f)
	{
		WorldToMeters = Settings->WorldToMeters;
	}
	return false;
}

void FWoTVRSpecHMD::GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const
{
	OutHFOVInDegrees = FovRight - FovLeft;
	OutVFOVInDegrees = FovUp - FovDown;
}

bool FWoTVRSpecHMD::IsStereoEnabled() const
{
	return Session->IsStereoEnabled();
}

void FWoTVRSpecHMD::AdjustViewRect(int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const
{
	SizeX = SizeX / 2;
	if (ViewIndex == EStereoscopicEye::eSSE_RIGHT_EYE)
	{
		X += static_cast<int32>(SizeX);
	}
}

FMatrix FWoTVRSpecHMD::GetStereoProjectionMatrix(const int32 ViewIndex) const
{
	// An asymmetric per-eye frustum with an infinite far plane, the near plane
	// taken from the render thread's global as a headset runtime's is.
	const bool bRight = ViewIndex == EStereoscopicEye::eSSE_RIGHT_EYE;
	const double Left = FMath::Tan(FMath::DegreesToRadians(bRight ? -FovRight : FovLeft));
	const double Right = FMath::Tan(FMath::DegreesToRadians(bRight ? -FovLeft : FovRight));
	const double Up = FMath::Tan(FMath::DegreesToRadians(FovUp));
	const double Down = FMath::Tan(FMath::DegreesToRadians(FovDown));
	const double InvRL = 1.0 / (Right - Left);
	const double InvTB = 1.0 / (Up - Down);
	const float Near = GNearClippingPlane_RenderThread;
	return FMatrix(
		FPlane(2.0 * InvRL, 0.0, 0.0, 0.0),
		FPlane(0.0, 2.0 * InvTB, 0.0, 0.0),
		FPlane(-(Right + Left) * InvRL, -(Up + Down) * InvTB, 0.0, 1.0),
		FPlane(0.0, 0.0, Near, 0.0));
}

IStereoLayers* FWoTVRSpecHMD::GetStereoLayers()
{
	return GetSpecLayers();
}

FWoTVRSpecLayers* FWoTVRSpecHMD::GetSpecLayers()
{
	if (!SpecLayers.IsValid())
	{
		if (IStereoLayers* Default = FHeadMountedDisplayBase::GetStereoLayers())
		{
			SpecLayers = MakeUnique<FWoTVRSpecLayers>(*Default);
		}
	}
	return SpecLayers.Get();
}

int32 FWoTVRSpecLayers::TotalWrites() const
{
	int32 Total = 0;
	for (const TPair<uint32, int32>& Entry : Writes)
	{
		Total += Entry.Value;
	}
	return Total;
}

uint32 FWoTVRSpecLayers::CreateLayer(const FLayerDesc& InLayerDesc)
{
	++Creates;
	return Inner.CreateLayer(InLayerDesc);
}

void FWoTVRSpecLayers::DestroyLayer(uint32 LayerId)
{
	++Destroys;
	Inner.DestroyLayer(LayerId);
}

void FWoTVRSpecLayers::SetLayerDesc(uint32 LayerId, const FLayerDesc& InLayerDesc)
{
	++Writes.FindOrAdd(LayerId);
	Inner.SetLayerDesc(LayerId, InLayerDesc);
}

bool FWoTVRSpecLayers::GetLayerDesc(uint32 LayerId, FLayerDesc& OutLayerDesc)
{
	return Inner.GetLayerDesc(LayerId, OutLayerDesc);
}

const IStereoLayers::FLayerDesc* FWoTVRSpecLayers::FindLayerDesc(uint32 LayerId) const
{
	return Inner.FindLayerDesc(LayerId);
}

void FWoTVRSpecLayers::MarkTextureForUpdate(uint32 LayerId)
{
	Inner.MarkTextureForUpdate(LayerId);
}

namespace WoTVRTest
{
FStereoScope::FStereoScope(const TSharedRef<FWoTVRSyntheticSession>& Session, const TSharedPtr<SViewport>& InWidget)
	: Widget(InWidget)
{
	PreviousSystem = GEngine->XRSystem;
	PreviousStereo = GEngine->StereoRenderingDevice;
	PreviousNearClip = GNearClippingPlane;
	HMD = MakeShared<FWoTVRSpecHMD, ESPMode::ThreadSafe>(Session);
	GEngine->XRSystem = HMD;
	GEngine->StereoRenderingDevice = HMD;
	if (InWidget.IsValid())
	{
		bPreviousWidgetStereo = InWidget->IsStereoRenderingAllowed();
		InWidget->EnableStereoRendering(true);
	}
}

FStereoScope::~FStereoScope()
{
	if (const TSharedPtr<SViewport> Pinned = Widget.Pin())
	{
		Pinned->EnableStereoRendering(bPreviousWidgetStereo);
	}
	if (GNearClippingPlane != PreviousNearClip)
	{
		SetNearClipPlaneGlobals(PreviousNearClip);
	}
	FlushRenderingCommands();
	GEngine->StereoRenderingDevice = PreviousStereo;
	GEngine->XRSystem = PreviousSystem;
	HMD.Reset();
}
}

#endif // WITH_AUTOMATION_TESTS
