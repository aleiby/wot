#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "UObject/GCObject.h"
#include "WoTHUDSnapshot.h"
#include "WoTVRSession.h"

class FWidgetRenderer;
class FWoTVRUIInputProcessor;
class IStereoLayers;
class SOverlay;
class SVirtualWindow;
class SWidget;
class SWoTVRReticle;
class UCanvas;
class ULocalPlayer;
class UTexture;
class UTextureRenderTarget2D;
class UWorld;
struct FAnalogInputEvent;
struct FKeyEvent;
struct FPointerEvent;
enum class EWoTUIContent : uint8;
enum class EWoTUIPointerAction : uint8;

// The HUD and the game's screens in the headset (design section 10), through
// the game module's hooks H5 (the HUD destination) and H6 (the UI host). The
// HUD snapshot is drawn on canvases of the plugin's own and the screens are
// hosted on offscreen Slate surfaces; each texture is shown, once for both
// eyes, on a stereo layer: a quad the runtime composites over the scene,
// locked to tracking space. Everything here is local presentation: no
// gameplay input, timing or authority changes.

/** Where a UI pointer aims from, in tracking space (metres). The head's gaze
 * today; a tracked controller's laser can replace it (SetRaySource). */
class IWoTVRRaySource
{
public:
	virtual ~IWoTVRRaySource() = default;
	/** The ray this frame; false when there is none (the source is not tracked). */
	virtual bool GetRay(FVector& OutOriginMeters, FVector& OutDirection) const = 0;
	virtual const TCHAR* GetName() const = 0;
};

/** The head-gaze ray: from the tracked head along its forward direction. */
class FWoTVRHeadGazeRay final : public IWoTVRRaySource
{
public:
	virtual bool GetRay(FVector& OutOriginMeters, FVector& OutDirection) const override;
	virtual const TCHAR* GetName() const override { return TEXT("head gaze"); }
};

/** A panel's place in tracking space: its centre (metres), its orientation (X
 * away from the viewer, Y to the viewer's right, Z up) and its size (metres). */
struct FWoTVRPanelPose
{
	FVector Center = FVector::ZeroVector;
	FQuat Orientation = FQuat::Identity;
	FVector2D Size = FVector2D::ZeroVector;

	/** Where a ray meets the panel's plane, in pixels of a surface Pixels in
	 * size (the panel's top left is 0,0). False when the ray runs along the
	 * plane or away from it; bOutInside: the point is on the panel. */
	bool Intersect(const FVector& Origin, const FVector& Direction, const FIntPoint& Pixels,
		FVector2D& OutPixel, bool& bOutInside) const;
	/** A surface pixel's place on the panel, in tracking space. */
	FVector PixelToTracking(const FVector2D& Pixel, const FIntPoint& Pixels) const;
	/** The panel facing a head at Head (no roll, whatever the head's), centred
	 * Distance metres away along the yaw Yaw and Drop degrees below level;
	 * Width metres wide, as tall as the surface's aspect makes it. */
	static FWoTVRPanelPose Facing(const FVector& Head, double Yaw, double Drop, double Distance,
		double Width, const FIntPoint& Pixels);
};

/** One panel shown as a stereo layer, from one texture for both eyes. The
 * device hears of the layer only when its description changes (each change
 * marks the runtime's layers for rebuilding); the texture's content reaches
 * the runtime every frame through the continuous-update flag. */
class FWoTVRStereoPanel
{
public:
	explicit FWoTVRStereoPanel(int32 InPriority) : Priority(InPriority) {}
	/** A move smaller than this (metres) since the layer's last description
	 * is not sent: a head-following panel then stays quiet while the head
	 * holds still. */
	static constexpr double PositionToleranceMeters = 0.0005;
	/** Shows the texture at Pose (tracking space, metres), or hides the layer. */
	void Update(IStereoLayers* Layers, UTexture* Texture, const FWoTVRPanelPose& InPose,
		float WorldToMeters, bool bVisible);
	/** Hides the layer, keeping it on the device for the next Update. */
	void Hide(IStereoLayers* Layers);
	/** Removes the layer from the device that has it (when it still exists). */
	void Destroy(IStereoLayers* Layers);
	uint32 GetLayerId() const { return LayerId; }
	bool IsShown() const { return bShown; }
	int32 GetPriority() const { return Priority; }
	const FWoTVRPanelPose& GetPose() const { return Pose; }

private:
	/** The device the layer was made on: compared, never dereferenced, once
	 * another device has replaced it. */
	const IStereoLayers* Owner = nullptr;
	uint32 LayerId = 0;
	int32 Priority = 0;
	bool bShown = false;
	FWoTVRPanelPose Pose;
	/** What the device last received for the layer. */
	FTransform SentTransform = FTransform::Identity;
	FVector2D SentQuadSize = FVector2D::ZeroVector;
	TWeakObjectPtr<UTexture> SentTexture;
	uint32 SentFlags = 0;
};

/** A render target the HUD's arranged sections are drawn on, through
 * FWoTHUDLayout::Draw, as the flat HUD draws its own Canvas. */
class FWoTVRCanvasSurface
{
public:
	FWoTVRCanvasSurface(const FIntPoint& InPixels, const TCHAR* Name);
	/** Clears the texture and draws the sections, scaled down to fit when
	 * their extent is larger than the surface. The HUD's translucent elements
	 * accumulate alpha there (FWoTCanvasAlphaScope): the runtime composites the
	 * texture by its alpha, and a colour-only translucent blend would leave
	 * them at the cleared alpha of zero, unseen. Without rendering (a headless
	 * run) the same draw goes to a canvas that is never flushed. */
	void Draw(UWorld* World, TConstArrayView<FWoTHUDSection> Sections, const FIntPoint& Extent);
	UTextureRenderTarget2D* GetTarget() const { return Target; }
	const FIntPoint& GetPixels() const { return Pixels; }
	int32 GetDrawCount() const { return DrawCount; }
#if WITH_AUTOMATION_TESTS
	/** The sections the last draw received (specs only). */
	const TArray<FWoTHUDSection>& GetLastSections() const { return LastSections; }
#endif
	float GetLastFitScale() const { return LastFitScale; }
	void AddReferencedObjects(FReferenceCollector& Collector);

private:
	FIntPoint Pixels;
	TObjectPtr<UTextureRenderTarget2D> Target;
	TObjectPtr<UCanvas> CanvasObject;
	int32 DrawCount = 0;
#if WITH_AUTOMATION_TESTS
	TArray<FWoTHUDSection> LastSections;
#endif
	float LastFitScale = 1.0f;
};

/** An offscreen Slate surface: a registered virtual window the hosted screens
 * live in, laid out at Pixels under the game viewport's DPI rule for that
 * size, painted each frame into a render target with the widget renderer
 * (which also fills the window's hit test). A reticle can mark the pointer. */
class FWoTVRSlateSurface
{
public:
	FWoTVRSlateSurface(const FIntPoint& InPixels, bool bWithReticle);
	~FWoTVRSlateSurface();
	FWoTVRSlateSurface(const FWoTVRSlateSurface&) = delete;
	FWoTVRSlateSurface& operator=(const FWoTVRSlateSurface&) = delete;

	/** Registers the window with Slate (while its presentation is attached) or
	 * unregisters it (detached, keeping the window, renderer and texture). */
	void SetRegistered(bool bInRegistered);
	bool IsRegistered() const { return bRegistered; }
	void Add(const TSharedRef<SWidget>& Screen, int32 ZOrder);
	bool Remove(const TSharedRef<SWidget>& Screen);
	/** Lets go of every screen at once. */
	void Clear();
	bool Contains(const SWidget& Screen) const;
	int32 Num() const;
	/** Lays out and paints the window: into the texture when the engine
	 * renders, else only its layout, ticks and hit test (a headless run). */
	void Paint(float DeltaSeconds);
	void SetReticle(const TOptional<FVector2D>& Pixel);
	TOptional<FVector2D> GetReticle() const;
	TSharedRef<SVirtualWindow> GetWindow() const { return Window; }
	const FIntPoint& GetPixels() const { return Pixels; }
	float GetDPIScale() const { return DPIScale; }
	UTextureRenderTarget2D* GetTarget() const { return Target; }
	int32 GetPaintCount() const { return PaintCount; }
	void AddReferencedObjects(FReferenceCollector& Collector);

private:
	FIntPoint Pixels;
	float DPIScale = 1.0f;
	TSharedRef<SVirtualWindow> Window;
	TSharedPtr<SOverlay> Screens;
	TSharedPtr<SWoTVRReticle> Reticle;
	TArray<TWeakPtr<SWidget>> Hosted;
	TUniquePtr<FWidgetRenderer> Renderer;
	TObjectPtr<UTextureRenderTarget2D> Target;
	int32 PaintCount = 0;
	bool bRegistered = false;
};

/**
 * One local player's headset HUD and menus. Attached while VR runs with a
 * usable session (UWoTVRPlayerSubsystem decides), it owns the player's HUD
 * destination and UI host in the local-player hooks:
 * - the HUD snapshot, in the Compact layout, on a panel soft-attached below
 *   the forward view, and the scoreboard (Reading layout, paged) on a
 *   world-locked panel while it shows;
 * - every game screen on an offscreen surface shown world-locked in front of
 *   the view, passive cards on a surface beside the HUD panel;
 * - a pointer from a ray source (head gaze) with a reticle; confirm on the
 *   gamepad's A, Enter or the left mouse button; D-pad and stick navigation and
 *   B through the hooks' routes as the player's one Slate user, only while the
 *   game's input is blocked by a window or text entry runs.
 * Detaching gives the HUD back to the flat Canvas and every screen back to the
 * viewport with its focus. Its surfaces (render targets, offscreen windows,
 * the widget renderer and the stereo layers) outlive a detach, hidden and
 * unpainted, so travel, tracking loss and the runtime's dashboard attach
 * again without rebuilding them; Release frees them, on the switch to flat
 * and at shutdown.
 */
class FWoTVRPresentation final : public FGCObject, public TSharedFromThis<FWoTVRPresentation>
{
public:
	/** The presentation's owner name in the local-player hooks. */
	static const FName Owner;
	/** Surface sizes in pixels. The HUD panel is 4:3, room for the Compact
	 * layout with every message queue full (its content sits top left, the
	 * rest stays transparent); the board 16:9, the scoreboard's 640-unit
	 * design at 2x with a page of rows at a time; menus 4:3 like the game's
	 * screens. */
	static constexpr int32 HUDPixelsX = 1024;
	static constexpr int32 HUDPixelsY = 768;
	static constexpr int32 ReadingPixelsX = 1280;
	static constexpr int32 ReadingPixelsY = 720;
	static constexpr int32 MenuPixelsX = 1600;
	static constexpr int32 MenuPixelsY = 1200;
	static constexpr int32 CardPixelsX = 800;
	static constexpr int32 CardPixelsY = 600;
	/** Stereo layer priorities: higher draws over lower. */
	static constexpr int32 HUDPriority = 10;
	static constexpr int32 CardPriority = 11;
	static constexpr int32 ReadingPriority = 20;
	static constexpr int32 MenuPriority = 30;

	explicit FWoTVRPresentation(ULocalPlayer& InPlayer);
	virtual ~FWoTVRPresentation() override;

	/** Takes the HUD destination and the UI host, with the head at Head,
	 * making the surfaces on the first attach and reusing them after. False
	 * (nothing attached, nothing kept) without stereo layers to show them on,
	 * or when another consumer holds either slot. */
	bool Attach(const FWoTVRHeadPose& Head);
	/** Gives the HUD back to the flat Canvas and the screens back to the
	 * viewport with their focus, releasing anything held. The surfaces stay:
	 * the layers hidden, nothing painted. */
	void Detach();
	/** Detaches, then frees the surfaces, the renderer and the layers. */
	void Release();
	bool IsAttached() const { return bAttached; }
	/** Whether the surfaces exist (attached, or kept through a detach). */
	bool HasSurfaces() const { return HUD.IsValid(); }
	/** Once per frame while attached, also while the world is paused: places
	 * the panels for the head, points, and paints the Slate surfaces. */
	void Tick(float DeltaSeconds, const FWoTVRHeadPose& Head);
	/** Recentre: the HUD panel faces the head again, and open menus and boards
	 * move in front of the view. */
	void Recentre(const FWoTVRHeadPose& Head);

	void SetRaySource(TUniquePtr<IWoTVRRaySource> Source);
	const IWoTVRRaySource& GetRaySource() const { return *RaySource; }

	// What the specs and the status command read.
	const FWoTVRCanvasSurface* GetHUDSurface() const { return HUD.Get(); }
	const FWoTVRCanvasSurface* GetReadingSurface() const { return Reading.Get(); }
	const FWoTVRSlateSurface* GetMenuSurface() const { return Menu.Get(); }
	const FWoTVRSlateSurface* GetCardSurface() const { return Cards.Get(); }
	const FWoTVRStereoPanel& GetHUDPanel() const { return HUDPanel; }
	const FWoTVRStereoPanel& GetCardPanel() const { return CardPanel; }
	const FWoTVRStereoPanel& GetReadingPanel() const { return ReadingPanel; }
	const FWoTVRStereoPanel& GetMenuPanel() const { return MenuPanel; }
	int32 GetHUDUpdates() const { return HUDUpdates; }
	int32 GetReadingPage() const { return ReadingPage; }
	int32 GetReadingPageCount() const { return ReadingPageCount; }
	bool IsReadingShown() const { return bReadingWanted; }
	/** Where the pointer meets the menu surface, in its pixels, when it does. */
	TOptional<FVector2D> GetPointer() const { return bPointerOnMenu ? TOptional<FVector2D>(PointerPixel) : TOptional<FVector2D>(); }
	/** The menu surface takes the confirm, back and navigation input now. */
	bool IsMenuInputActive() const;
	/** Confirm presses the focused control rather than the one at the
	 * reticle: after D-pad or stick navigation, until the gaze moves onto a
	 * different control. */
	bool IsFocusPreferred() const { return bPreferFocus; }
	FString Describe() const;

	// From the input processor (Slate's events before any widget sees them).
	bool HandleKey(const FKeyEvent& Event, bool bPressed);
	bool HandleAnalog(const FAnalogInputEvent& Event);
	bool HandleMouseButton(const FPointerEvent& Event, bool bPressed, bool bDoubleClick);
	bool HandleWheel(const FPointerEvent& Event);

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FWoTVRPresentation"); }

private:
	enum class ERoute : uint8
	{
		/** The press went nowhere (a mouse button with no menu under the
		 * pointer, a paging key): its release goes nowhere too. */
		Dropped,
		/** Through the hooks' key route. */
		Key,
		/** As the pointer's button. */
		Pointer
	};

	void PresentHUD(const FWoTHUDSnapshot& Snapshot);
	bool ShowScreen(const TSharedRef<SWidget>& Screen, int32 ZOrder, EWoTUIContent Kind);
	void HideScreen(const TSharedRef<SWidget>& Screen);
	void HandleDetachedByGame();
	/** Detached: the layers hide and the windows leave Slate; nothing is freed. */
	void Suspend();
	void ReleaseSurfaces();
	void UpdateHUDFollow(const FWoTVRHeadPose& Head, float DeltaSeconds);
	FWoTVRPanelPose PlaceInFront(const FWoTVRHeadPose& Head, float Distance, float Width,
		const FIntPoint& Pixels) const;
	void UpdatePointer();
	bool RoutePointer(EWoTUIPointerAction Action);
	/** The gaze is off the surface and no press is held: the pointer leaves
	 * every control it hovered there (EWoTUIPointerAction::Leave). */
	void LeavePanel();
	bool RouteKey(const FKey& Key, bool bPressed, bool bRepeat);
	void ReleaseRoute(const FKey& Key, ERoute Route);
	bool IsPagingKey(const FKey& Key, int32& OutDirection) const;
	void PageBy(int32 Direction);
	/** A press the pointer holds (from A, Enter or the left mouse button). */
	bool IsPointerPressHeld() const;
	/** The control under the pointer on the menu surface: the deepest widget
	 * the hit test finds there that is interactive or takes focus (never the
	 * window), or null. */
	TSharedPtr<SWidget> ControlUnderPointer() const;
	/** The player navigated: confirm prefers the focused control from here. */
	void NoteNavigation();
	/** Ends the focus preference once the gaze has moved onto another control. */
	void UpdateFocusPreference();
	UWorld* GetWorld() const;

	TWeakObjectPtr<ULocalPlayer> Player;
	bool bAttached = false;
	bool bDetaching = false;
	TUniquePtr<FWoTVRCanvasSurface> HUD;
	TUniquePtr<FWoTVRCanvasSurface> Reading;
	TUniquePtr<FWoTVRSlateSurface> Menu;
	TUniquePtr<FWoTVRSlateSurface> Cards;
	FWoTVRStereoPanel HUDPanel{HUDPriority};
	FWoTVRStereoPanel CardPanel{CardPriority};
	FWoTVRStereoPanel ReadingPanel{ReadingPriority};
	FWoTVRStereoPanel MenuPanel{MenuPriority};
	FWoTVRPanelPose ReadingPose;
	FWoTVRPanelPose MenuPose;
	double HUDYaw = 0.0;
	bool bHUDFollowing = false;
	bool bHUDWanted = false;
	bool bReadingWanted = false;
	/** A HUD snapshot arrived since the last tick: without one the HUD stopped
	 * drawing (no HUD, or the HUD hidden), and its panels hide. */
	bool bSnapshotSinceTick = false;
	bool bPlaceReading = false;
	bool bPlaceMenu = false;
	int32 ReadingPage = 0;
	int32 ReadingPageCount = 1;
	int32 HUDUpdates = 0;
	FWoTVRHeadPose LastHead;
	TUniquePtr<IWoTVRRaySource> RaySource;
	FVector2D PointerPixel = FVector2D::ZeroVector;
	bool bPointerOnMenu = false;
	TOptional<FVector2D> LastRoutedMove;
	/** The pointer was routed onto the menu surface since its last leave, so
	 * what it hovered there hears a leave once the gaze is off the surface. */
	bool bPointerHovering = false;
	/** Confirm presses the focused control (IsFocusPreferred). */
	bool bPreferFocus = false;
	/** The control under the gaze, and the gaze's direction, when the player
	 * last navigated. */
	TWeakPtr<SWidget> NavigationControl;
	TOptional<FVector> NavigationGaze;
	TMap<FKey, ERoute> Held;
	TSharedPtr<FWoTVRUIInputProcessor> Processor;
	/** Set while the presentation routes an event, which comes back through
	 * the processor: routed events pass. */
	bool bRouting = false;
};
