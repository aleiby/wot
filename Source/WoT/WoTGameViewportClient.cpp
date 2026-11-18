#include "WoTGameViewportClient.h"

#include "Slate/SceneViewport.h"
#include "WoTLocalPlayerHooks.h"
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

void UWoTGameViewportClient::AddViewportWidgetContent(TSharedRef<SWidget> ViewportContent,
	const int32 ZOrder)
{
	if (!FWoTLocalPlayerHooks::HostUIContent(*this, ViewportContent, ZOrder))
	{
		AddOverlayContent(ViewportContent, ZOrder);
	}
}

void UWoTGameViewportClient::RemoveViewportWidgetContent(TSharedRef<SWidget> ViewportContent)
{
	if (!FWoTLocalPlayerHooks::UnhostUIContent(*this, ViewportContent))
	{
		RemoveOverlayContent(ViewportContent);
	}
}

TArray<TPair<TSharedRef<SWidget>, int32>> UWoTGameViewportClient::GetOverlayContent() const
{
	TArray<TPair<TSharedRef<SWidget>, int32>> Screens;
	for (const TPair<TWeakPtr<SWidget>, int32>& Entry : OverlayScreens)
	{
		if (const TSharedPtr<SWidget> Screen = Entry.Key.Pin())
		{
			Screens.Emplace(Screen.ToSharedRef(), Entry.Value);
		}
	}
	// The overlay's own order: by z-order, then as added.
	Screens.StableSort([](const TPair<TSharedRef<SWidget>, int32>& A,
		const TPair<TSharedRef<SWidget>, int32>& B)
	{
		return A.Value < B.Value;
	});
	return Screens;
}

void UWoTGameViewportClient::AddOverlayContent(TSharedRef<SWidget> Screen, int32 ZOrder)
{
	OverlayScreens.RemoveAll([](const TPair<TWeakPtr<SWidget>, int32>& Entry)
	{
		return !Entry.Key.IsValid();
	});
	OverlayScreens.Emplace(Screen, ZOrder);
	Super::AddViewportWidgetContent(Screen, ZOrder);
}

void UWoTGameViewportClient::RemoveOverlayContent(TSharedRef<SWidget> Screen)
{
	const int32 Index = OverlayScreens.IndexOfByPredicate(
		[&Screen](const TPair<TWeakPtr<SWidget>, int32>& Entry)
		{
			return Entry.Key.HasSameObject(&Screen.Get());
		});
	if (Index != INDEX_NONE)
	{
		OverlayScreens.RemoveAt(Index);
	}
	Super::RemoveViewportWidgetContent(Screen);
}
