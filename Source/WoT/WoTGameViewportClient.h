#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "Input/Events.h"
#include "WoTGameViewportClient.generated.h"

/** Leave click activation capture to the viewport's mouse-down handling.
 * Screens go through a local player's UI host when one is attached (H6,
 * FWoTLocalPlayerHooks), and onto the viewport as always otherwise. */
UCLASS()
class WOT_API UWoTGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()

public:
	virtual void Activated(FViewport* InViewport,
		const FWindowActivateEvent& InActivateEvent) override;
	virtual bool IsInPermanentCapture() override;
	virtual void Tick(float DeltaTime) override;
	virtual void AddViewportWidgetContent(TSharedRef<SWidget> ViewportContent,
		const int32 ZOrder = 0) override;
	virtual void RemoveViewportWidgetContent(TSharedRef<SWidget> ViewportContent) override;

	/** H6: the screens on the viewport itself, bottom first, with their
	 * z-orders. */
	TArray<TPair<TSharedRef<SWidget>, int32>> GetOverlayContent() const;
	/** H6: a screen straight onto the viewport, or off it, past any UI host. */
	void AddOverlayContent(TSharedRef<SWidget> Screen, int32 ZOrder);
	void RemoveOverlayContent(TSharedRef<SWidget> Screen);

private:
	bool SuppressClickActivation(FWindowActivateEvent::EActivationType ActivationType,
		bool bClickCausedActivation);
	bool bSuppressNextCaptureQuery = false;
	/** The screens on the viewport's overlay and their z-orders, as added. */
	TArray<TPair<TWeakPtr<SWidget>, int32>> OverlayScreens;

	friend class FWoTViewportClickActivationSpec;
};
