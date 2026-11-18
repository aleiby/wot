#include "WoTVRPresentation.h"

#include "WoTCanvasScale.h"
#include "WoTVRModule.h"
#include "WoTVRRig.h"

#include "Brushes/SlateColorBrush.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/UserInterfaceSettings.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "IStereoLayers.h"
#include "Misc/App.h"
#include "Rendering/DrawElements.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "Styling/SlateBrush.h"
#include "Styling/WidgetStyle.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SVirtualWindow.h"

// --- the pointer's ray --------------------------------------------------------

bool FWoTVRHeadGazeRay::GetRay(FVector& OutOriginMeters, FVector& OutDirection) const
{
	FWoTVRHeadPose Pose;
	if (!FWoTVRModule::IsAvailable() || !FWoTVRModule::Get().GetSession().GetHeadPose(Pose) || !Pose.bValid)
	{
		return false;
	}
	OutOriginMeters = Pose.PositionMeters;
	OutDirection = Pose.Orientation.GetForwardVector();
	return true;
}

// --- panel placement -----------------------------------------------------------

bool FWoTVRPanelPose::Intersect(const FVector& Origin, const FVector& Direction,
	const FIntPoint& Pixels, FVector2D& OutPixel, bool& bOutInside) const
{
	bOutInside = false;
	const FVector Normal = Orientation.GetForwardVector();
	const double Facing = FVector::DotProduct(Direction, Normal);
	if (Facing <= UE_KINDA_SMALL_NUMBER || Size.X <= 0.0 || Size.Y <= 0.0
		|| Pixels.X <= 0 || Pixels.Y <= 0)
	{
		return false;
	}
	const double Distance = FVector::DotProduct(Center - Origin, Normal) / Facing;
	if (Distance <= 0.0)
	{
		return false;
	}
	const FVector Local = Orientation.UnrotateVector(Origin + Direction * Distance - Center);
	const double U = 0.5 + Local.Y / Size.X;
	const double V = 0.5 - Local.Z / Size.Y;
	OutPixel = FVector2D(U * Pixels.X, V * Pixels.Y);
	bOutInside = U >= 0.0 && U <= 1.0 && V >= 0.0 && V <= 1.0;
	return true;
}

FVector FWoTVRPanelPose::PixelToTracking(const FVector2D& Pixel, const FIntPoint& Pixels) const
{
	const double U = Pixels.X > 0 ? Pixel.X / Pixels.X : 0.5;
	const double V = Pixels.Y > 0 ? Pixel.Y / Pixels.Y : 0.5;
	return Center + Orientation.RotateVector(FVector(0.0, (U - 0.5) * Size.X, (0.5 - V) * Size.Y));
}

FWoTVRPanelPose FWoTVRPanelPose::Facing(const FVector& Head, double Yaw, double Drop, double Distance,
	double Width, const FIntPoint& Pixels)
{
	FWoTVRPanelPose Pose;
	// Pitched down by Drop so the panel faces the head; never rolled.
	const FRotator Direction(-Drop, Yaw, 0.0);
	Pose.Orientation = Direction.Quaternion();
	Pose.Center = Head + Direction.Vector() * Distance;
	const double Aspect = Pixels.X > 0 ? static_cast<double>(Pixels.Y) / Pixels.X : 1.0;
	Pose.Size = FVector2D(Width, Width * Aspect);
	return Pose;
}

// --- stereo layers -------------------------------------------------------------

void FWoTVRStereoPanel::Update(IStereoLayers* Layers, UTexture* Texture, const FWoTVRPanelPose& InPose,
	float WorldToMeters, bool bVisible)
{
	Pose = InPose;
	if (Owner != Layers)
	{
		// Another device (or none): the old layer went with the old one.
		Owner = Layers;
		LayerId = 0;
		SentTexture.Reset();
		SentFlags = 0;
	}
	bShown = false;
	if (!Layers || (!bVisible && LayerId == 0))
	{
		return;
	}
	const bool bShow = bVisible && Texture != nullptr;
	if (!bShow && (SentFlags & IStereoLayers::LAYER_FLAG_HIDDEN) != 0)
	{
		// Hidden already: a hidden panel's place does not matter, and the
		// device hears nothing until it shows again.
		return;
	}
	// One texture for both eyes, locked to the tracking space the head pose is
	// in; the runtime composites it over the scene, crisp at any distance.
	IStereoLayers::FLayerDesc Desc;
	Desc.PositionType = IStereoLayers::TrackerLocked;
	Desc.Priority = Priority;
	Desc.Transform = FTransform(Pose.Orientation, Pose.Center * WorldToMeters);
	Desc.QuadSize = Pose.Size * WorldToMeters;
	Desc.TextureObj = Texture;
	// The surfaces are drawn every frame they show: the runtime copies the
	// latest each frame without being told.
	Desc.Flags = IStereoLayers::LAYER_FLAG_TEX_CONTINUOUS_UPDATE
		| (bShow ? 0u : static_cast<uint32>(IStereoLayers::LAYER_FLAG_HIDDEN));
	if (LayerId == 0)
	{
		LayerId = Layers->CreateLayer(Desc);
	}
	else if (Desc.Flags == SentFlags && SentTexture.Get() == Texture
		&& Desc.QuadSize.Equals(SentQuadSize, UE_KINDA_SMALL_NUMBER)
		&& Desc.Transform.GetRotation().Equals(SentTransform.GetRotation(), UE_KINDA_SMALL_NUMBER)
		&& Desc.Transform.GetTranslation().Equals(SentTransform.GetTranslation(),
			PositionToleranceMeters * WorldToMeters))
	{
		// Unchanged since the device last heard of it (a still or world-locked
		// panel, or a head-following one within the tracking's sub-millimetre
		// noise): each SetLayerDesc marks the runtime's layers for rebuilding
		// (FSimpleLayerManager::SetLayerDesc), so nothing is sent.
		bShown = bShow;
		return;
	}
	else
	{
		Layers->SetLayerDesc(LayerId, Desc);
	}
	// Compared against what was last sent, so small steps never add up unsent.
	SentTransform = Desc.Transform;
	SentQuadSize = Desc.QuadSize;
	SentTexture = Texture;
	SentFlags = Desc.Flags;
	bShown = bShow;
}

void FWoTVRStereoPanel::Hide(IStereoLayers* Layers)
{
	bShown = false;
	if (LayerId == 0 || !Layers || Layers != Owner || (SentFlags & IStereoLayers::LAYER_FLAG_HIDDEN) != 0)
	{
		return;
	}
	IStereoLayers::FLayerDesc Desc;
	if (Layers->GetLayerDesc(LayerId, Desc))
	{
		Desc.Flags |= IStereoLayers::LAYER_FLAG_HIDDEN;
		Layers->SetLayerDesc(LayerId, Desc);
		SentFlags = Desc.Flags;
	}
}

void FWoTVRStereoPanel::Destroy(IStereoLayers* Layers)
{
	if (LayerId != 0 && Layers && Layers == Owner)
	{
		Layers->DestroyLayer(LayerId);
	}
	LayerId = 0;
	Owner = nullptr;
	bShown = false;
	SentTexture.Reset();
	SentFlags = 0;
}

// --- render targets ---------------------------------------------------------

namespace
{
/** A render target for a surface, configured as the widget renderer's own
 * (FWidgetRenderer::CreateTargetFor): transparent, linear, bilinear. Without
 * rendering it is a texture of the right size with no resource. */
UTextureRenderTarget2D* WoTVRMakeTarget(const FIntPoint& Pixels, const TCHAR* Name)
{
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UTextureRenderTarget2D::StaticClass(), Name), RF_Transient);
	Target->Filter = TF_Bilinear;
	Target->ClearColor = FLinearColor::Transparent;
	Target->SRGB = true;
	Target->TargetGamma = 1.0f;
	if (FApp::CanEverRender() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
	{
		Target->InitCustomFormat(Pixels.X, Pixels.Y,
			FSlateApplication::Get().GetRenderer()->GetSlateRecommendedColorFormat(), /*bForceLinearGamma*/ true);
		Target->UpdateResourceImmediate(true);
	}
	else
	{
		Target->SizeX = Pixels.X;
		Target->SizeY = Pixels.Y;
	}
	return Target;
}

/** A render target with only a size, for a headless draw that is never flushed. */
class FWoTVRSizeOnlyTarget final : public FRenderTarget
{
public:
	explicit FWoTVRSizeOnlyTarget(const FIntPoint& InSize) : Size(InSize) {}
	virtual FIntPoint GetSizeXY() const override { return Size; }

private:
	FIntPoint Size;
};
}

FWoTVRCanvasSurface::FWoTVRCanvasSurface(const FIntPoint& InPixels, const TCHAR* Name)
	: Pixels(InPixels)
{
	Target = WoTVRMakeTarget(Pixels, Name);
	CanvasObject = NewObject<UCanvas>(GetTransientPackage(), NAME_None, RF_Transient);
}

void FWoTVRCanvasSurface::Draw(UWorld* World, TConstArrayView<FWoTHUDSection> Sections,
	const FIntPoint& Extent)
{
#if WITH_AUTOMATION_TESTS
	LastSections = TArray<FWoTHUDSection>(Sections);
#endif
	++DrawCount;
	// The arranged content fits whole-number scales; a surface smaller than it
	// at 1x draws it scaled down rather than cut off.
	LastFitScale = 1.0f;
	if (Extent.X > Pixels.X || Extent.Y > Pixels.Y)
	{
		LastFitScale = FMath::Min(static_cast<float>(Pixels.X) / FMath::Max(Extent.X, 1),
			static_cast<float>(Pixels.Y) / FMath::Max(Extent.Y, 1));
	}
	FTextureRenderTargetResource* Resource = FApp::CanEverRender() && Target
		? Target->GameThread_GetRenderTargetResource() : nullptr;
	FWoTVRSizeOnlyTarget SizeOnly(Pixels);
	FCanvas Canvas(Resource ? static_cast<FRenderTarget*>(Resource) : &SizeOnly, nullptr, World,
		World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel, FCanvas::CDM_DeferDrawing, 1.0f);
	if (Resource)
	{
		Canvas.Clear(FLinearColor::Transparent);
	}
	CanvasObject->Init(Pixels.X, Pixels.Y, nullptr, &Canvas);
	const bool bFit = LastFitScale < 1.0f;
	if (bFit)
	{
		Canvas.PushRelativeTransform(FScaleMatrix(FVector(LastFitScale, LastFitScale, 1.0f)));
	}
	{
		// The texture is cleared transparent and composited by its alpha: the
		// translucent elements (messages, the aimed player's name, the flash)
		// must add theirs, which the flat HUD's colour-only blend would not.
		// Text keeps its own shaders (FCanvas::SetWriteDestinationAlpha would
		// send every run through the alpha-only one).
		const FWoTCanvasAlphaScope AccumulateAlpha;
		FWoTHUDLayout::Draw(*CanvasObject, Sections);
	}
	if (bFit)
	{
		Canvas.PopTransform();
	}
	CanvasObject->Canvas = nullptr;
	if (Resource)
	{
		Canvas.Flush_GameThread(true);
		ENQUEUE_RENDER_COMMAND(WoTVRCanvasSurfaceResolve)(
			[Resource](FRHICommandListImmediate& RHICmdList)
			{
				Resource->FlushDeferredResourceUpdate(RHICmdList);
			});
	}
}

void FWoTVRCanvasSurface::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Target);
	Collector.AddReferencedObject(CanvasObject);
}

// --- Slate surfaces ----------------------------------------------------------

/** A ring and a dot where the UI pointer meets the surface; never hit. */
class SWoTVRReticle final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SWoTVRReticle) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		SetVisibility(EVisibility::HitTestInvisible);
	}

	void SetPoint(const TOptional<FVector2D>& InPoint)
	{
		if (Point != InPoint)
		{
			Point = InPoint;
			Invalidate(EInvalidateWidgetReason::Paint);
		}
	}

	TOptional<FVector2D> GetPoint() const { return Point; }

	virtual FVector2D ComputeDesiredSize(float) const override
	{
		return FVector2D::ZeroVector;
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry& AllottedGeometry, const FSlateRect&,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle&, bool) const override
	{
		if (!Point.IsSet())
		{
			return LayerId;
		}
		const FVector2f Centre(*Point);
		// A light ring with a dark outline reads on any menu art.
		const FLinearColor Light(1.0f, 0.95f, 0.8f, 0.95f);
		const FLinearColor Dark(0.0f, 0.0f, 0.0f, 0.7f);
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			const float Radius = Pass == 0 ? RingRadius + 1.5f : RingRadius;
			TArray<FVector2f> Ring;
			for (int32 Segment = 0; Segment <= RingSegments; ++Segment)
			{
				const float Angle = 2.0f * UE_PI * Segment / RingSegments;
				Ring.Add(Centre + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * Radius);
			}
			FSlateDrawElement::MakeLines(OutDrawElements, LayerId + 1 + Pass, AllottedGeometry.ToPaintGeometry(),
				MoveTemp(Ring), ESlateDrawEffect::None, Pass == 0 ? Dark : Light, true, Pass == 0 ? 4.0f : 2.0f);
		}
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 3,
			AllottedGeometry.ToPaintGeometry(FVector2f(DotSize, DotSize),
				FSlateLayoutTransform(Centre - FVector2f(DotSize * 0.5f, DotSize * 0.5f))),
			&DotBrush, ESlateDrawEffect::None, Light);
		return LayerId + 3;
	}

private:
	static constexpr float RingRadius = 14.0f;
	static constexpr int32 RingSegments = 24;
	static constexpr float DotSize = 4.0f;
	TOptional<FVector2D> Point;
	FSlateColorBrush DotBrush{FLinearColor::White};
};

FWoTVRSlateSurface::FWoTVRSlateSurface(const FIntPoint& InPixels, bool bWithReticle)
	: Pixels(InPixels)
	, Window(SNew(SVirtualWindow).Size(FVector2D(InPixels)))
{
	// The screens lay out as on a game viewport of this size: the same DPI
	// rule (UUserInterfaceSettings, by the viewport's size) scales them.
	if (const UUserInterfaceSettings* Settings = GetDefault<UUserInterfaceSettings>())
	{
		DPIScale = FMath::Max(Settings->GetDPIScaleBasedOnSize(Pixels), 0.01f);
	}
	TSharedRef<SOverlay> Root = SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SDPIScaler)
			.DPIScale(DPIScale)
			[
				SAssignNew(Screens, SOverlay)
			]
		];
	if (bWithReticle)
	{
		Root->AddSlot()
		[
			SAssignNew(Reticle, SWoTVRReticle)
		];
	}
	Window->SetContent(Root);
	Window->SetShouldResolveDeferred(true);
	SetRegistered(true);
	if (FApp::CanEverRender())
	{
		Renderer = MakeUnique<FWidgetRenderer>(/*bUseGammaCorrection*/ false, /*bInClearTarget*/ true);
	}
	Target = WoTVRMakeTarget(Pixels, TEXT("WoTVRSlateSurface"));
}

FWoTVRSlateSurface::~FWoTVRSlateSurface()
{
	if (Screens.IsValid())
	{
		Screens->ClearChildren();
	}
	SetRegistered(false);
	if (Renderer.IsValid())
	{
		// The renderer's draw buffers may still be in flight.
		FlushRenderingCommands();
		Renderer.Reset();
	}
}

void FWoTVRSlateSurface::SetRegistered(bool bInRegistered)
{
	if (bRegistered == bInRegistered || !FSlateApplication::IsInitialized())
	{
		return;
	}
	bRegistered = bInRegistered;
	if (bRegistered)
	{
		// Registered, Slate finds focus paths and windows inside it, as the
		// hooks' focus scope and the hit test need.
		FSlateApplication::Get().RegisterVirtualWindow(Window);
	}
	else
	{
		FSlateApplication::Get().UnregisterVirtualWindow(Window);
	}
}

void FWoTVRSlateSurface::Add(const TSharedRef<SWidget>& Screen, int32 ZOrder)
{
	Hosted.RemoveAll([](const TWeakPtr<SWidget>& Entry) { return !Entry.IsValid(); });
	Hosted.Add(Screen);
	Screens->AddSlot(ZOrder)[Screen];
}

bool FWoTVRSlateSurface::Remove(const TSharedRef<SWidget>& Screen)
{
	const int32 Removed = Hosted.RemoveAll([&Screen](const TWeakPtr<SWidget>& Entry)
	{
		return !Entry.IsValid() || Entry.HasSameObject(&Screen.Get());
	});
	if (Removed > 0)
	{
		Screens->RemoveSlot(Screen);
	}
	return Removed > 0;
}

void FWoTVRSlateSurface::Clear()
{
	Hosted.Reset();
	if (Screens.IsValid())
	{
		Screens->ClearChildren();
	}
}

bool FWoTVRSlateSurface::Contains(const SWidget& Screen) const
{
	return Hosted.ContainsByPredicate([&Screen](const TWeakPtr<SWidget>& Entry)
	{
		return Entry.HasSameObject(&Screen);
	});
}

int32 FWoTVRSlateSurface::Num() const
{
	int32 Count = 0;
	for (const TWeakPtr<SWidget>& Entry : Hosted)
	{
		Count += Entry.IsValid() ? 1 : 0;
	}
	return Count;
}

void FWoTVRSlateSurface::Paint(float DeltaSeconds)
{
	++PaintCount;
	if (Renderer.IsValid() && Target && Target->GameThread_GetRenderTargetResource())
	{
		Renderer->DrawWindow(Target, Window->GetHittestGrid(), Window, 1.0f, FVector2D(Pixels), DeltaSeconds);
		return;
	}
	// Headless: the layout, the widgets' ticks and the hit test, which is what
	// the pointer and focus use; no pixels.
	if (FSlateApplication::IsInitialized())
	{
		Window->SlatePrepass(1.0f);
		FSlateWindowElementList Elements(Window);
		Window->PaintWindow(FSlateApplication::Get().GetCurrentTime(), DeltaSeconds, Elements,
			FWidgetStyle(), true);
	}
}

void FWoTVRSlateSurface::SetReticle(const TOptional<FVector2D>& Pixel)
{
	if (Reticle.IsValid())
	{
		Reticle->SetPoint(Pixel);
	}
}

TOptional<FVector2D> FWoTVRSlateSurface::GetReticle() const
{
	return Reticle.IsValid() ? Reticle->GetPoint() : TOptional<FVector2D>();
}

void FWoTVRSlateSurface::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(Target);
}
