#pragma once

#include "CoreMinimal.h"
#include "WoTVRSession.h"

class IXRTrackingSystem;
class IOpenXRHMD;

/** The session backed by the engine's stock OpenXR plugin. With the XR gate
 * closed there is no XR system and it reports Unavailable; it never creates
 * one itself. */
class FWoTVROpenXRSession final : public IWoTVRSession
{
public:
	FWoTVROpenXRSession();
	virtual ~FWoTVROpenXRSession() override;

	virtual FName GetKind() const override;
	virtual EWoTVRSessionState GetState() const override;
	virtual bool GetHeadPose(FWoTVRHeadPose& OutPose) const override;
	virtual float GetInterpupillaryDistanceMeters() const override;
	virtual bool SetStereo(bool bEnable) override;
	virtual bool IsStereoEnabled() const override;
	virtual void PrepareTracking() override;
	virtual uint32 GetReferenceSpaceGeneration() const override { return ReferenceGeneration; }
	virtual uint32 GetInteractionProfileGeneration() const override { return ProfileGeneration; }
	virtual FWoTVRDiagnostics GetDiagnostics() const override;

private:
	/** The engine's XR system when it is OpenXR's, else null. */
	static IXRTrackingSystem* GetOpenXRSystem();
	static IOpenXRHMD* GetOpenXRHMD();

	uint32 ReferenceGeneration = 0;
	uint32 ProfileGeneration = 0;
	FDelegateHandle RecenterHandle;
	FDelegateHandle ProfileHandle;
};
