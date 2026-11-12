#include "WoTGameViewportClient.h"

#include "Slate/SceneViewport.h"
#include "WoTWindowActivation.h"

void UWoTGameViewportClient::Activated(FViewport* InViewport,
	const FWindowActivateEvent& InActivateEvent)
{
	Super::Activated(InViewport, InActivateEvent);
#if PLATFORM_MAC
	if (SuppressClickActivation(InActivateEvent.GetActivationType(),
		WoTWasWindowActivatedByClick()) && InViewport)
	{
		// SceneViewport reads both capture flags immediately after Activated.
		static_cast<FSceneViewport*>(InViewport)->SetPlayInEditorGetsMouseControl(false);
	}
#endif
}

bool UWoTGameViewportClient::SuppressClickActivation(
	FWindowActivateEvent::EActivationType ActivationType, bool bClickCausedActivation)
{
	bSuppressNextCaptureQuery = ActivationType == FWindowActivateEvent::EA_Activate
		&& bClickCausedActivation;
	return bSuppressNextCaptureQuery;
}

bool UWoTGameViewportClient::IsInPermanentCapture()
{
	if (bSuppressNextCaptureQuery)
	{
		bSuppressNextCaptureQuery = false;
		return false;
	}
	return Super::IsInPermanentCapture();
}

void UWoTGameViewportClient::Tick(float DeltaTime)
{
	bSuppressNextCaptureQuery = false;
	Super::Tick(DeltaTime);
}
