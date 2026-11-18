#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "Input/Events.h"
#include "WoTGameViewportClient.generated.h"

/** Leave click activation capture to the viewport's mouse-down handling. */
UCLASS()
class WOT_API UWoTGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual void Activated(FViewport* InViewport,
		const FWindowActivateEvent& InActivateEvent) override;
	virtual bool IsInPermanentCapture() override;
	virtual void Tick(float DeltaTime) override;

private:
	bool SuppressClickActivation(FWindowActivateEvent::EActivationType ActivationType,
		bool bClickCausedActivation);
	bool bSuppressNextCaptureQuery = false;

	friend class FWoTViewportClickActivationSpec;
};
