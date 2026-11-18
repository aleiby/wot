#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "HeadMountedDisplayBase.h"
#include "IStereoLayers.h"

class FDefaultXRCamera;
class FWoTVRSyntheticSession;
class SViewport;

/** The spec display's stereo layers: the engine's default layers
 * (FHeadMountedDisplayBase::GetStereoLayers) behind a counter of what the
 * device is told, so specs can see which layer changes reach it. */
class FWoTVRSpecLayers final : public IStereoLayers
{
public:
	explicit FWoTVRSpecLayers(IStereoLayers& InInner) : Inner(InInner) {}

	int32 Creates = 0;
	int32 Destroys = 0;
	/** SetLayerDesc calls, by layer. */
	TMap<uint32, int32> Writes;

	int32 WritesTo(uint32 LayerId) const { return Writes.FindRef(LayerId); }
	int32 TotalWrites() const;

	virtual uint32 CreateLayer(const FLayerDesc& InLayerDesc) override;
	virtual void DestroyLayer(uint32 LayerId) override;
	virtual void SetLayerDesc(uint32 LayerId, const FLayerDesc& InLayerDesc) override;
	virtual bool GetLayerDesc(uint32 LayerId, FLayerDesc& OutLayerDesc) override;
	virtual const FLayerDesc* FindLayerDesc(uint32 LayerId) const override;
	virtual void MarkTextureForUpdate(uint32 LayerId) override;
	virtual void PushLayerState(bool bPreserve = false) override { Inner.PushLayerState(bPreserve); }
	virtual void PopLayerState() override { Inner.PopLayerState(); }
	virtual bool SupportsLayerState() override { return Inner.SupportsLayerState(); }
	virtual void HideBackgroundLayer() override { Inner.HideBackgroundLayer(); }
	virtual void ShowBackgroundLayer() override { Inner.ShowBackgroundLayer(); }
	virtual bool IsBackgroundLayerVisible() const override { return Inner.IsBackgroundLayerVisible(); }

private:
	IStereoLayers& Inner;
};

/**
 * A stand-in head-mounted display for the stereo-pipeline specs. It derives
 * from the engine's FHeadMountedDisplayBase, as the stock OpenXR HMD does, so
 * the engine's own XR camera (FDefaultXRCamera, created by
 * FXRTrackingSystemBase::GetXRCamera) computes every eye: the stereo offset on
 * the game thread (ULocalPlayer::GetProjectionData), the scene view's base pose
 * (SetupView) and the render-thread late update (PreRenderView_RenderThread).
 *
 * Its head pose is the synthetic session's, at the engine's world scale, so the
 * rig and the engine see the same head. Like OpenXR it supports a late
 * projection update, builds each eye's projection with the render thread's
 * global near plane (FOpenXRHMD::GetStereoProjectionMatrix) and takes its world
 * scale from the ticking world (FOpenXRHMD::OnStartGameFrame).
 */
class FWoTVRSpecHMD final : public FHeadMountedDisplayBase,
	public TSharedFromThis<FWoTVRSpecHMD, ESPMode::ThreadSafe>
{
public:
	explicit FWoTVRSpecHMD(const TSharedRef<FWoTVRSyntheticSession>& InSession);

	/** While set (the late update a spec runs), the pose is the late one: the
	 * game thread's plus LateDeltaMeters. */
	bool bLatePose = false;
	FVector LateDeltaMeters = FVector::ZeroVector;
	float WorldToMeters = 100.0f;
	/** Each eye's asymmetric field of view in degrees (left, right, up, down),
	 * mirrored for the right eye. */
	float FovLeft = -52.0f;
	float FovRight = 42.0f;
	float FovUp = 44.0f;
	float FovDown = -50.0f;

	/** The engine's XR camera this display created. */
	TSharedPtr<FDefaultXRCamera, ESPMode::ThreadSafe> GetDefaultCamera();

	// IXRSystemIdentifier
	virtual FName GetSystemName() const override;
	// IXRTrackingSystem
	virtual int32 GetXRSystemFlags() const override;
	virtual bool DoesSupportPositionalTracking() const override { return true; }
	virtual bool DoesSupportLateProjectionUpdate() const override { return true; }
	virtual bool EnumerateTrackedDevices(TArray<int32>& OutDevices,
		EXRTrackedDeviceType Type = EXRTrackedDeviceType::Any) override;
	virtual bool GetCurrentPose(int32 DeviceId, FQuat& OutOrientation, FVector& OutPosition) override;
	virtual float GetWorldToMetersScale() const override { return WorldToMeters; }
	virtual void ResetOrientationAndPosition(float Yaw = 0.0f) override {}
	virtual bool IsHeadTrackingAllowed() const override;
	virtual bool OnStartGameFrame(FWorldContext& WorldContext) override;
	virtual IHeadMountedDisplay* GetHMDDevice() override { return this; }
	virtual TSharedPtr<IStereoRendering, ESPMode::ThreadSafe> GetStereoRenderingDevice() override
	{
		return AsShared();
	}
	// IHeadMountedDisplay
	virtual bool IsHMDConnected() override { return true; }
	virtual bool IsHMDEnabled() const override { return true; }
	virtual void EnableHMD(bool bEnable = true) override {}
	virtual bool GetHMDMonitorInfo(MonitorInfo& OutInfo) override { return false; }
	virtual void GetFieldOfView(float& OutHFOVInDegrees, float& OutVFOVInDegrees) const override;
	virtual void SetInterpupillaryDistance(float NewInterpupillaryDistance) override {}
	virtual float GetInterpupillaryDistance() const override { return 0.064f; }
	virtual bool IsChromaAbCorrectionEnabled() const override { return false; }
	// IStereoRendering
	virtual bool IsStereoEnabled() const override;
	virtual bool EnableStereo(bool bStereo = true) override { return true; }
	virtual void AdjustViewRect(int32 ViewIndex, int32& X, int32& Y, uint32& SizeX, uint32& SizeY) const override;
	virtual FMatrix GetStereoProjectionMatrix(const int32 ViewIndex) const override;
	virtual IStereoLayers* GetStereoLayers() override;

	/** The counting layers GetStereoLayers gives. */
	FWoTVRSpecLayers* GetSpecLayers();

private:
	TSharedRef<FWoTVRSyntheticSession> Session;
	TUniquePtr<FWoTVRSpecLayers> SpecLayers;
};

namespace WoTVRTest
{
	/** Installs a spec display as the engine's XR system and stereo device for
	 * one scope and lets the viewport render stereo, as the game's own viewport
	 * does (UGameEngine::CreateGameViewportWidget). Everything is restored when
	 * the scope ends; declare it after the world so it ends first. */
	struct FStereoScope
	{
		TSharedPtr<FWoTVRSpecHMD, ESPMode::ThreadSafe> HMD;

		FStereoScope(const TSharedRef<FWoTVRSyntheticSession>& Session, const TSharedPtr<SViewport>& InWidget);
		~FStereoScope();

	private:
		TSharedPtr<class IXRTrackingSystem, ESPMode::ThreadSafe> PreviousSystem;
		TSharedPtr<class IStereoRendering, ESPMode::ThreadSafe> PreviousStereo;
		TWeakPtr<SViewport> Widget;
		bool bPreviousWidgetStereo = false;
		float PreviousNearClip = 0.0f;
	};
}

#endif // WITH_AUTOMATION_TESTS
