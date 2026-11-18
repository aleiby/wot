#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTVRTestFixture.h"
#include "WoTVRSpecHMD.h"
#include "Tests/WoTHUDStreamFixture.h"
#include "Tests/WoTLocalPlayerHooksTestFixture.h"
#include "WoTCanvasScale.h"
#include "WoTHUD.h"
#include "WoTLocalPlayerHooks.h"
#include "WoTPlayerController.h"
#include "WoTVRPlayerSubsystem.h"
#include "WoTVRPresentation.h"
#include "WoTVRRig.h"
#include "WoTVRSyntheticSession.h"
#include "WoTVRTuning.h"

#include "Camera/PlayerCameraManager.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/LocalPlayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "IStereoLayers.h"
#include "Input/Events.h"
#include "SceneTypes.h"
#include "UnrealClient.h"

// The HUD in the headset (design section 10, H5): while VR runs the plugin owns
// the HUD destination, draws each update's snapshot on canvases of its own in
// the Compact and Reading layouts, once per frame, and shows them on stereo
// layers: the HUD panel soft-attached below the forward view, the board
// world-locked in front of it. A switch to flat gives the flat Canvas HUD
// back, drawing exactly what it drew before (the H5 goldens).

namespace WoTVRHUDTest
{
/** Two arrangements are the same sections with the same commands. */
bool SameSections(TConstArrayView<FWoTHUDSection> A, TConstArrayView<FWoTHUDSection> B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		const FWoTHUDSection& Left = A[Index];
		const FWoTHUDSection& Right = B[Index];
		if (Left.Section != Right.Section || Left.Path != Right.Path || Left.BoardScale != Right.BoardScale
			|| Left.Commands.Num() != Right.Commands.Num())
		{
			return false;
		}
		for (int32 Command = 0; Command < Left.Commands.Num(); ++Command)
		{
			const FWoTHUDDrawCmd& L = Left.Commands[Command];
			const FWoTHUDDrawCmd& R = Right.Commands[Command];
			if (L.TexId != R.TexId || L.Text != R.Text || L.FontId != R.FontId || L.X != R.X || L.Y != R.Y
				|| L.Scale != R.Scale || L.Style != R.Style || !L.Tint.Equals(R.Tint)
				|| L.TextAlignment != R.TextAlignment || L.bCenterX != R.bCenterX
				|| L.Font.Get() != R.Font.Get() || L.Texture != R.Texture)
			{
				return false;
			}
		}
	}
	return true;
}

bool HasSection(TConstArrayView<FWoTHUDSection> Sections, EWoTHUDSection Part)
{
	return Sections.ContainsByPredicate([Part](const FWoTHUDSection& Section) { return Section.Section == Part; });
}

/** A tuning value for one spec, restored afterwards. */
struct FCVar
{
	IConsoleVariable* Variable = nullptr;
	float Previous = 0.0f;
	FCVar(const TCHAR* Name, float Value)
		: Variable(IConsoleManager::Get().FindConsoleVariable(Name))
	{
		if (Variable)
		{
			Previous = Variable->GetFloat();
			Variable->Set(Value, ECVF_SetByCode);
		}
	}
	~FCVar()
	{
		if (Variable)
		{
			Variable->Set(Previous, ECVF_SetByCode);
		}
	}
};

/** The layer the panel shows, as the stereo device holds it. */
bool LayerOf(FWoTVRSpecHMD& HMD, const FWoTVRStereoPanel& Panel, IStereoLayers::FLayerDesc& Out)
{
	IStereoLayers* Layers = HMD.GetStereoLayers();
	return Layers && Panel.GetLayerId() != 0 && Layers->GetLayerDesc(Panel.GetLayerId(), Out);
}

bool IsHidden(const IStereoLayers::FLayerDesc& Desc)
{
	return (Desc.Flags & IStereoLayers::LAYER_FLAG_HIDDEN) != 0;
}

/** A render target with only a size: the deferred canvas is never flushed. */
class FSizeOnlyTarget final : public FRenderTarget
{
public:
	explicit FSizeOnlyTarget(const FIntPoint& InSize) : Size(InSize) {}
	virtual FIntPoint GetSizeXY() const override { return Size; }

private:
	FIntPoint Size;
};

/** One submission the HUD made, and the blend a translucent tile was given at
 * that moment (the icon line names the style; the tile's blend is this). */
struct FSubmission
{
	FString Line;
	ESimpleElementBlendMode TranslucentTile = SE_BLEND_Opaque;
};

/** One frame of the real HUD on a Width x Height canvas, as the viewport draws
 * it (the world ticks, then DrawHUD), recording every submission: the flat
 * Canvas's, and a destination's that draws inside the HUD's update. */
TArray<FSubmission> DrawObserved(WoTHUDHookTest::FScene& S, int32 Width, int32 Height)
{
	S.F.Frames(1);
	TArray<FSubmission> Seen;
	FSizeOnlyTarget Target(FIntPoint(Width, Height));
	FCanvas Canvas(&Target, nullptr, S.F.World, GMaxRHIFeatureLevel, FCanvas::CDM_DeferDrawing, 1.0f);
	UCanvas* CanvasObject = NewObject<UCanvas>(GetTransientPackage());
	CanvasObject->Init(Width, Height, nullptr, &Canvas);
	FWoTCanvasScale::SubmissionObserverForTesting() = [&Seen](const FString& Line)
	{
		Seen.Add({Line, FWoTCanvasScale::BlendModeFor(EWoTHUDStyle::Translucent)});
	};
	S.HUD->SetCanvas(CanvasObject, CanvasObject);
	S.HUD->DrawHUD();
	S.HUD->SetCanvas(nullptr, nullptr);
	FWoTCanvasScale::SubmissionObserverForTesting() = nullptr;
	CanvasObject->Canvas = nullptr;
	return Seen;
}

/** A text line's blend (its "blend=" field), or -1 for an icon line. */
int32 TextBlend(const FString& Line)
{
	const int32 At = Line.Find(TEXT(" blend="), ESearchCase::CaseSensitive);
	return Line.StartsWith(TEXT("text ")) && At != INDEX_NONE ? FCString::Atoi(*Line.Mid(At + 7)) : -1;
}

/** An icon line drawn in the translucent style. */
bool IsTranslucentIcon(const FString& Line)
{
	return Line.StartsWith(TEXT("icon ")) && Line.Contains(FString::Printf(TEXT(" style=%d "),
		static_cast<int32>(EWoTHUDStyle::Translucent)));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRHUDDestinationSpec, "WoT.VR.HUD.DestinationReplacesFlat", WoTVRTest::Flags)
bool FWoTVRHUDDestinationSpec::RunTest(const FString&)
{
	using namespace WoTVRHUDTest;
	WoTVRTest::FScope Scope;
	WoTHUDHookTest::FScene S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	S.AddMessages();
	S.ShowWarningAndFlash();
	TestFalse(TEXT("flat: the flat HUD draws on its Canvas"), S.DrawFrame().IsEmpty());

	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestTrue(TEXT("the headset HUD attaches with VR"), Presentation && Presentation->IsAttached())) return false;
	TestTrue(TEXT("the plugin owns the HUD destination"), FWoTLocalPlayerHooks::OwnsHUDDestination(S.F.PC));
	const FWoTVRCanvasSurface* Canvas = Presentation->GetHUDSurface();
	if (!TestNotNull(TEXT("the plugin's canvas"), Canvas)) return false;
	const FIntPoint Pixels = Canvas->GetPixels();
	for (int32 Frame = 0; Frame < 5; ++Frame)
	{
		const FString Label = FString::Printf(TEXT("frame %d"), Frame);
		const int32 Draws = Canvas->GetDrawCount();
		const int32 Updates = Presentation->GetHUDUpdates();
		const TArray<FString> Lines = S.DrawFrame();
		TestFalse(Label + TEXT(": the flat Canvas HUD is not drawn"), Lines.ContainsByPredicate(
			[](const FString& Line) { return WoTHUDHookTest::LandedOnCanvas(Line, 640, 480); }));
		TestTrue(Label + TEXT(": the plugin's canvas is drawn"), Lines.ContainsByPredicate(
			[&Pixels](const FString& Line) { return WoTHUDHookTest::LandedOnCanvas(Line, Pixels.X, Pixels.Y); }));
		TestEqual(Label + TEXT(": one update"), Presentation->GetHUDUpdates() - Updates, 1);
		TestEqual(Label + TEXT(": the canvas draws once"), Canvas->GetDrawCount() - Draws, 1);
		const FWoTHUDSnapshot* Latest = FWoTLocalPlayerHooks::LatestHUD(*S.F.Player);
		if (!TestNotNull(Label + TEXT(": the update's snapshot"), Latest)) return false;
		FWoTHUDLayoutOptions Options;
		Options.Layout = EWoTHUDLayout::Compact;
		Options.Size = Pixels;
		const FWoTHUDLayoutResult Arranged = FWoTHUDLayout::Arrange(*Latest, Options);
		TestTrue(Label + TEXT(": exactly the snapshot's sections, arranged compact"),
			SameSections(Canvas->GetLastSections(), Arranged.Sections));
		if (Frame == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("WoT.VR.HUD: compact extent with every queue full %dx%d on a %dx%d panel"),
				Arranged.Extent.X, Arranged.Extent.Y, Pixels.X, Pixels.Y);
		}
		TestTrue(Label + TEXT(": the compact layout fits its panel"),
			Arranged.Extent.X <= Pixels.X && Arranged.Extent.Y <= Pixels.Y && Canvas->GetLastFitScale() == 1.0f);
		TestTrue(Label + TEXT(": the warning survives"), HasSection(Canvas->GetLastSections(), EWoTHUDSection::Warning));
		TestTrue(Label + TEXT(": the groups show"), HasSection(Canvas->GetLastSections(), EWoTHUDSection::Hands));
		TestFalse(Label + TEXT(": no reticle on the panel"), HasSection(Canvas->GetLastSections(), EWoTHUDSection::Reticle));
		// A second draw in the frame (another canvas) updates nothing again.
		WoTHUDHookTest::DrawHUDOnce(*S.HUD, 640, 480);
		TestEqual(Label + TEXT(": still once"), Canvas->GetDrawCount() - Draws, 1);
		if (Frame == 0)
		{
			TestTrue(Label + TEXT(": the flash survives"), HasSection(Canvas->GetLastSections(), EWoTHUDSection::Flash));
		}
	}

	// The HUD panel's layer: one texture for both eyes, locked to tracking
	// space, about 1.2 m away and 35 degrees wide, below the forward view.
	IStereoLayers::FLayerDesc Desc;
	if (TestTrue(TEXT("the HUD panel is a stereo layer"), LayerOf(*Stereo.HMD, Presentation->GetHUDPanel(), Desc)))
	{
		TestTrue(TEXT("the panel shows"), !IsHidden(Desc) && Presentation->GetHUDPanel().IsShown());
		TestTrue(TEXT("the HUD's canvas is its one texture"), Desc.TextureObj.Get() == Canvas->GetTarget()
			&& !Desc.LeftTextureObj.IsValid());
		TestEqual(TEXT("tracker-locked"), static_cast<int32>(Desc.PositionType),
			static_cast<int32>(IStereoLayers::TrackerLocked));
		const double Scale = Stereo.HMD->GetWorldToMetersScale();
		const double Width = 2.0 * 1.2 * FMath::Tan(FMath::DegreesToRadians(17.5));
		TestEqual(TEXT("35 degrees wide at 1.2 m"), Desc.QuadSize.X, Width * Scale, 0.01);
		const FVector Head = Scope.Session->Pose.PositionMeters * Scale;
		TestEqual(TEXT("1.2 m from the head"), FVector::Distance(Desc.Transform.GetLocation(), Head), 1.2 * Scale, 0.01);
		TestTrue(TEXT("below the forward view"), Desc.Transform.GetLocation().Z < Head.Z - 0.3 * Scale);
	}

	// The board: the Reading layout on its own world-locked panel while it
	// shows, every row reachable a page at a time.
	if (!S.AddScoreboardPlayers(*this, 15)) return false;
	S.SetScoreboardShown(true);
	S.Draw();
	S.Draw();
	const FWoTVRCanvasSurface* Reading = Presentation->GetReadingSurface();
	if (!TestNotNull(TEXT("the board's canvas"), Reading)) return false;
	TestTrue(TEXT("the board shows on the reading panel"), Presentation->IsReadingShown()
		&& Presentation->GetReadingPanel().IsShown());
	const FWoTHUDSnapshot* Latest = FWoTLocalPlayerHooks::LatestHUD(*S.F.Player);
	if (!TestNotNull(TEXT("the board's snapshot"), Latest)) return false;
	FWoTHUDLayoutOptions Options;
	Options.Layout = EWoTHUDLayout::Reading;
	Options.Size = Reading->GetPixels();
	const FWoTHUDLayoutResult Board = FWoTHUDLayout::Arrange(*Latest, Options);
	TestTrue(TEXT("the reading layout fits its panel"),
		Board.Extent.X <= Reading->GetPixels().X && Board.Extent.Y <= Reading->GetPixels().Y);
	TestTrue(TEXT("the reading canvas draws the board's first page"), SameSections(Reading->GetLastSections(), Board.Sections));
	TestEqual(TEXT("the panel pages the board as the layout does"), Presentation->GetReadingPageCount(), Board.PageCount);
	TestTrue(TEXT("sixteen rows take more than one page on the reading panel"), Board.PageCount > 1);
	TestFalse(TEXT("the compact panel carries no board"), HasSection(Canvas->GetLastSections(), EWoTHUDSection::Scoreboard));
	if (LayerOf(*Stereo.HMD, Presentation->GetReadingPanel(), Desc))
	{
		TestTrue(TEXT("the board's canvas is its layer's texture"), Desc.TextureObj.Get() == Reading->GetTarget());
		TestTrue(TEXT("the board draws over the HUD panel"), Desc.Priority > Presentation->GetHUDPanel().GetPriority());
	}
	S.SetScoreboardShown(false);
	S.Draw();
	S.Draw();
	TestFalse(TEXT("released, the board's panel hides"), Presentation->GetReadingPanel().IsShown());

	// Switching to flat gives the flat Canvas HUD back at its next draw.
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	TestNull(TEXT("the headset HUD is gone"), Rig->GetPresentation());
	TestFalse(TEXT("the HUD destination is free"), FWoTLocalPlayerHooks::OwnsHUDDestination(S.F.PC));
	const TArray<FString> Restored = S.DrawFrame();
	TestTrue(TEXT("the flat HUD draws again"), Restored.ContainsByPredicate(
		[](const FString& Line) { return WoTHUDHookTest::LandedOnCanvas(Line, 640, 480); }));
	const FWoTHUDSnapshot* Flat = FWoTLocalPlayerHooks::LatestHUD(*S.F.Player);
	if (TestNotNull(TEXT("the flat HUD's snapshot"), Flat))
	{
		TestEqual(TEXT("exactly its snapshot"), WoTHUDHookTest::DrawSectionsOnce(S.F.World,
			Flat->Sections, 640, 480), Restored);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRHUDVisibilitySpec, "WoT.VR.HUD.VisibilityEffectsStayInTheView", WoTVRTest::Flags)
bool FWoTVRHUDVisibilitySpec::RunTest(const FString&)
{
	// The visibility effects are the scene's, not the HUD's: a flash's fog (the
	// underwater and zone tints add to it) and the darkening scale stay in the
	// view both eyes are rendered from while the headset owns the HUD.
	WoTVRTest::FScope Scope;
	WoTHUDHookTest::FScene S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	S.Draw();
	TestTrue(TEXT("the headset owns the HUD"), FWoTLocalPlayerHooks::OwnsHUDDestination(S.F.PC));
	APlayerCameraManager* Camera = S.F.PC->PlayerCameraManager;
	if (!TestNotNull(TEXT("camera"), Camera)) return false;
	S.F.PC->CurrentViewFlashFog = FVector(0.5f, 0.25f, 0.0f);
	S.F.PC->CurrentViewFlashScale = 0.6f;
	S.Draw();
	const FVector Fog = S.F.PC->CurrentViewFlashFog;
	const FMinimalViewInfo View = WoTVRTest::FinalView(*S.F.Player);
	TestFalse(TEXT("the flash is still fading"), Fog.IsNearlyZero(1e-3));
	TestTrue(TEXT("the view keeps the flash's fog"), View.PostProcessSettings.bOverride_ColorOffset
		&& FVector(View.PostProcessSettings.ColorOffset).Equals(Fog, 1e-4));
	TestTrue(TEXT("the view keeps the darkening"), Camera->bEnableColorScaling
		&& Camera->DesiredColorScale.Equals(FVector(FMath::Max(0.0f, S.F.PC->CurrentViewFlashScale)), 1e-4));
	TestTrue(TEXT("the view is the head's"), Rig->HasEnteredVR());
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRHUDGoldenSpec, "WoT.VR.HUD.FlatRestoredExactly", WoTVRTest::Flags)
bool FWoTVRHUDGoldenSpec::RunTest(const FString&)
{
	// The ten recorded flat states (the H5 goldens), each frame first drawn in
	// VR (the update goes to the headset panel, the flat Canvas draws nothing),
	// then switched to flat and drawn again in the same frame: the flat stream
	// must be byte-identical to the recording, frame by frame.
	WoTVRTest::FScope Scope;
	int32 HeadsetFrames = 0;
	int32 FlatLeaks = 0;
	int32 Leftovers = 0;
	WoTHUDHookTest::RunRecordedStates(*this, [this, &Scope, &HeadsetFrames, &FlatLeaks, &Leftovers](
		WoTHUDHookTest::FScene& S)
	{
		UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
		if (!TestNotNull(TEXT("rig"), Rig))
		{
			return false;
		}
		TSharedPtr<WoTVRTest::FStereoScope> Stereo = MakeShared<WoTVRTest::FStereoScope>(Scope.Session, S.F.Widget);
		S.OnTeardown = [Stereo, Rig]() mutable
		{
			if (IsValid(Rig))
			{
				Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec teardown"));
			}
			Stereo.Reset();
		};
		S.BeforeDraw = [Rig, &HeadsetFrames, &FlatLeaks, &Leftovers](WoTHUDHookTest::FScene& Scene, int32 Width, int32 Height)
		{
			Rig->RequestMode(EWoTVRMode::VR, TEXT("spec"));
			const FWoTVRPresentation* Presentation = Rig->GetPresentation();
			if (Presentation && Presentation->IsAttached())
			{
				++HeadsetFrames;
			}
			const TArray<FString> Headset = WoTHUDHookTest::DrawHUDOnce(*Scene.HUD, Width, Height);
			FlatLeaks += Headset.FilterByPredicate([Width, Height](const FString& Line)
			{
				return WoTHUDHookTest::LandedOnCanvas(Line, Width, Height);
			}).Num();
			Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec"));
			Leftovers += FWoTLocalPlayerHooks::OwnsHUDDestination(Scene.F.PC) || Rig->GetPresentation() ? 1 : 0;
		};
		return true;
	});
	TestEqual(TEXT("every recorded frame went to the headset first"), HeadsetFrames, 44);
	TestEqual(TEXT("the flat Canvas HUD drew nothing while VR owned the HUD"), FlatLeaks, 0);
	TestEqual(TEXT("each switch to flat gave the HUD back"), Leftovers, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRHUDPlacementSpec, "WoT.VR.HUD.PanelPlacementAndFollow", WoTVRTest::Flags)
bool FWoTVRHUDPlacementSpec::RunTest(const FString&)
{
	using namespace WoTVRHUDTest;
	WoTVRTest::FScope Scope;
	WoTHUDHookTest::FScene S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	const FVector Head(0.1, -0.2, 0.05);
	Scope.Session->SetHead(Head, FRotator(0.0, 0.0, 0.0));
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset HUD"), Presentation)) return false;
	S.Draw();
	S.Draw();
	auto HUDPose = [Presentation]() { return Presentation->GetHUDPanel().GetPose(); };
	auto PanelYaw = [](const FWoTVRPanelPose& Pose) { return Pose.Orientation.Rotator().Yaw; };
	const FWoTVRPanelPose Start = HUDPose();
	TestEqual(TEXT("the HUD panel: 1.2 m from the head"), FVector::Distance(Start.Center, Head), 1.2, 1e-3);
	TestEqual(TEXT("30 degrees below the level view"), Start.Orientation.Rotator().Pitch, -30.0, 1e-3);
	TestEqual(TEXT("35 degrees wide"), Start.Size.X, 2.0 * 1.2 * FMath::Tan(FMath::DegreesToRadians(17.5)), 1e-4);
	TestEqual(TEXT("4:3, the HUD surface's aspect"), Start.Size.Y * 4.0, Start.Size.X * 3.0, 1e-4);

	// No head roll reaches the panel.
	Scope.Session->SetHead(Head, FRotator(0.0, 0.0, 25.0));
	S.Draw();
	TestEqual(TEXT("head roll leaves the panel level"), HUDPose().Orientation.Rotator().Roll, 0.0, 1e-3);
	// Within the dead zone the panel holds still.
	Scope.Session->SetHead(Head, FRotator(10.0, 15.0, 0.0));
	for (int32 Frame = 0; Frame < 30; ++Frame)
	{
		S.Wait(1);
	}
	TestEqual(TEXT("within the dead zone the panel stays"), PanelYaw(HUDPose()), 0.0, 1e-3);
	TestEqual(TEXT("head pitch does not move the panel"), HUDPose().Orientation.Rotator().Pitch, -30.0, 1e-3);
	// Past it the panel follows gently, then settles on the head's yaw.
	Scope.Session->SetHead(Head, FRotator(0.0, 40.0, 0.0));
	S.Wait(1);
	const double FirstStep = PanelYaw(HUDPose());
	TestTrue(TEXT("past the dead zone it begins to follow"), FirstStep > 0.5);
	TestTrue(TEXT("gently: not all the way at once"), FirstStep < 20.0);
	S.Wait(120);
	TestEqual(TEXT("it settles on the head's yaw"), PanelYaw(HUDPose()), 40.0, 1.0);
	TestEqual(TEXT("still 1.2 m away"), FVector::Distance(HUDPose().Center, Head), 1.2, 1e-3);
	// The panel follows the head's position at once (soft-attached).
	const FVector Moved = Head + FVector(0.3, 0.1, -0.1);
	Scope.Session->SetHead(Moved, FRotator(0.0, 40.0, 0.0));
	S.Wait(1);
	TestEqual(TEXT("it comes with the head"), FVector::Distance(HUDPose().Center, Moved), 1.2, 1e-3);

	// The distance, width and drop are tunable.
	{
		FCVar Distance(TEXT("wot.vr.HUDDistance"), 2.0f);
		FCVar Width(TEXT("wot.vr.HUDWidthDegrees"), 50.0f);
		FCVar Drop(TEXT("wot.vr.HUDDropDegrees"), 10.0f);
		S.Wait(1);
		TestEqual(TEXT("wot.vr.HUDDistance"), FVector::Distance(HUDPose().Center, Moved), 2.0, 1e-3);
		TestEqual(TEXT("wot.vr.HUDWidthDegrees"), HUDPose().Size.X, 2.0 * 2.0 * FMath::Tan(FMath::DegreesToRadians(25.0)), 1e-4);
		TestEqual(TEXT("wot.vr.HUDDropDegrees"), HUDPose().Orientation.Rotator().Pitch, -10.0, 1e-3);
	}

	// The board is world-locked where the view was when it began to show.
	if (!S.AddScoreboardPlayers(*this, 15)) return false;
	S.SetScoreboardShown(true);
	S.Draw();
	S.Draw();
	const FWoTVRPanelPose Board = Presentation->GetReadingPanel().GetPose();
	TestEqual(TEXT("the board: about 1.5 m in front of the view"), FVector::Distance(Board.Center, Moved), 1.5, 1e-3);
	TestEqual(TEXT("where the head looked"), PanelYaw(Board), 40.0, 1e-3);
	Scope.Session->SetHead(Moved, FRotator(0.0, 100.0, 0.0));
	S.Draw();
	S.Draw();
	TestTrue(TEXT("the board stays where it was as the head turns"),
		Presentation->GetReadingPanel().GetPose().Center.Equals(Board.Center, 1e-4));
	// Recentre moves it in front of the view.
	TestTrue(TEXT("recentre"), Rig->Recentre(TEXT("spec")));
	S.Draw();
	TestEqual(TEXT("recentred, the board is in front of the view again"),
		PanelYaw(Presentation->GetReadingPanel().GetPose()), 100.0, 1e-3);
	TestEqual(TEXT("recentred, the HUD panel faces the head"), PanelYaw(HUDPose()), 100.0, 1.0);
	S.SetScoreboardShown(false);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRHUDPagingSpec, "WoT.VR.HUD.BoardPaging", WoTVRTest::Flags)
bool FWoTVRHUDPagingSpec::RunTest(const FString&)
{
	WoTVRTest::FScope Scope;
	WoTHUDHookTest::FScene S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset HUD"), Presentation)) return false;
	FSlateApplication& Slate = FSlateApplication::Get();
	const int32 User = FWoTLocalPlayerHooks::UISlateUser(*S.F.Player);
	auto Press = [&Slate, User](const FKey& Key)
	{
		const FKeyEvent Event(Key, FModifierKeysState(), User, false, 0, 0);
		const bool bHandled = Slate.ProcessKeyDownEvent(Event);
		Slate.ProcessKeyUpEvent(Event);
		return bHandled;
	};
	auto Wheel = [&Slate, User](float Delta)
	{
		const FPointerEvent Event(User, FSlateApplication::CursorPointerIndex, FVector2D::ZeroVector,
			FVector2D::ZeroVector, TSet<FKey>(), EKeys::MouseWheelAxis, Delta, FModifierKeysState());
		return Slate.ProcessMouseWheelOrGestureEvent(Event, nullptr);
	};
	S.Draw();
	const FName Selected = FWoTLocalPlayerHooks::LatestHUD(*S.F.Player)->Artifact;
	TestFalse(TEXT("without the board the bumpers are the game's"), Press(EKeys::Gamepad_RightShoulder)
		&& Presentation->GetReadingPage() != 0);

	if (!S.AddScoreboardPlayers(*this, 15)) return false;
	S.SetScoreboardShown(true);
	S.Draw();
	S.Draw();
	const int32 Pages = Presentation->GetReadingPageCount();
	if (!TestTrue(TEXT("the sixteen-row board pages"), Pages > 1)) return false;
	TestEqual(TEXT("it opens on the first page"), Presentation->GetReadingPage(), 0);
	TestTrue(TEXT("the right bumper turns the page"), Press(EKeys::Gamepad_RightShoulder));
	S.Draw();
	TestEqual(TEXT("the second page shows"), Presentation->GetReadingPage(), 1);
	TestTrue(TEXT("the drawn rows are the second page's"), Presentation->GetReadingSurface()->GetLastSections().ContainsByPredicate(
		[Pages](const FWoTHUDSection& Section)
		{
			return Section.Commands.ContainsByPredicate([Pages](const FWoTHUDDrawCmd& Command)
			{
				return Command.Text == FString::Printf(TEXT("%d / %d"), 2, Pages);
			});
		}));
	TestTrue(TEXT("the wheel turns it back (scroll up selects the previous item)"), Wheel(1.0f));
	S.Draw();
	TestEqual(TEXT("the first page again"), Presentation->GetReadingPage(), 0);
	TestTrue(TEXT("the left bumper at the first page stays there"), Press(EKeys::Gamepad_LeftShoulder));
	S.Draw();
	TestEqual(TEXT("still the first page"), Presentation->GetReadingPage(), 0);
	TestTrue(TEXT("the selection is unchanged while the board takes the paging keys"),
		FWoTLocalPlayerHooks::LatestHUD(*S.F.Player)->Artifact == Selected);

	// Released, the keys are the game's again.
	S.SetScoreboardShown(false);
	S.Draw();
	S.Draw();
	TestFalse(TEXT("the board is gone"), Presentation->IsReadingShown());
	TestFalse(TEXT("the presentation lets the bumper through"), Presentation->HandleKey(
		FKeyEvent(EKeys::Gamepad_RightShoulder, FModifierKeysState(), User, false, 0, 0), true));
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixTranslucentAlphaSpec, "WoT.VR.HeadsetFixes.TranslucentHUDWritesAlpha",
	WoTVRTest::Flags)
bool FWoTVRFixTranslucentAlphaSpec::RunTest(const FString&)
{
	// The HUD panel's texture is cleared transparent and the runtime composites
	// it by its alpha. The flat HUD's translucent blend writes colour only
	// (SE_BLEND_Translucent), which would leave every translucent element there
	// at alpha 0, unseen: the messages, the aimed player's name and the flash.
	// On the plugin's canvas, and only there, they accumulate alpha.
	using namespace WoTVRHUDTest;
	WoTVRTest::FScope Scope;
	WoTHUDHookTest::FScene S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	S.AddMessages();
	S.ShowWarningAndFlash();
	const int32 Translucent = static_cast<int32>(SE_BLEND_Translucent);
	const int32 PanelBlend = static_cast<int32>(SE_BLEND_AlphaComposite);
	auto IsMessage = [](const FString& Line) { return Line.Contains(TEXT("\"Left one\"")); };

	// Flat: the translucent style keeps its colour-only blend.
	const TArray<FSubmission> Flat = DrawObserved(S, 640, 480);
	const FSubmission* FlatMessage = Flat.FindByPredicate([&IsMessage](const FSubmission& Seen) { return IsMessage(Seen.Line); });
	if (TestNotNull(TEXT("flat: a message line is drawn"), FlatMessage))
	{
		TestEqual(TEXT("flat: a message keeps SE_BLEND_Translucent"), TextBlend(FlatMessage->Line), Translucent);
	}
	const FSubmission* FlatFlash = Flat.FindByPredicate([](const FSubmission& Seen) { return IsTranslucentIcon(Seen.Line); });
	if (TestNotNull(TEXT("flat: the flash is drawn"), FlatFlash))
	{
		TestEqual(TEXT("flat: the flash tile keeps SE_BLEND_Translucent"), static_cast<int32>(FlatFlash->TranslucentTile),
			Translucent);
	}
	TestFalse(TEXT("flat: no alpha scope is open"), FWoTCanvasAlphaScope::IsOpen());

	// VR: everything the plugin's canvas draws accumulates alpha.
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset HUD"), Presentation)) return false;
	const FIntPoint Pixels = Presentation->GetHUDSurface()->GetPixels();
	// While the headset owns the HUD the flat Canvas draws nothing: every
	// submission in the frame is the plugin's canvas's.
	const TArray<FSubmission> Panel = DrawObserved(S, 640, 480);
	TestFalse(TEXT("VR: nothing on the flat Canvas"), Panel.ContainsByPredicate([](const FSubmission& Seen)
	{
		return WoTHUDHookTest::LandedOnCanvas(Seen.Line, 640, 480);
	}));
	TestTrue(TEXT("VR: the plugin's canvas is drawn"), Panel.ContainsByPredicate([&Pixels](const FSubmission& Seen)
	{
		return WoTHUDHookTest::LandedOnCanvas(Seen.Line, Pixels.X, Pixels.Y);
	}));
	int32 TranslucentTexts = 0;
	for (const FSubmission& Seen : Panel)
	{
		const int32 Blend = TextBlend(Seen.Line);
		if (Blend == Translucent)
		{
			AddError(FString::Printf(TEXT("a text run on the panel uses SE_BLEND_Translucent: %s"), *Seen.Line));
		}
		TranslucentTexts += Blend == PanelBlend ? 1 : 0;
	}
	const FSubmission* Message = Panel.FindByPredicate([&IsMessage](const FSubmission& Seen) { return IsMessage(Seen.Line); });
	if (TestNotNull(TEXT("VR: the message is on the panel"), Message))
	{
		TestEqual(TEXT("VR: a message writes straight colour and alpha (SE_BLEND_AlphaComposite)"), TextBlend(Message->Line), PanelBlend);
	}
	TestTrue(TEXT("VR: every message line accumulates alpha"), TranslucentTexts >= 4);
	const FSubmission* Flash = Panel.FindByPredicate([](const FSubmission& Seen) { return IsTranslucentIcon(Seen.Line); });
	if (TestNotNull(TEXT("VR: the flash is on the panel"), Flash))
	{
		TestEqual(TEXT("VR: the flash tile writes straight colour and alpha (SE_BLEND_AlphaComposite)"),
			static_cast<int32>(Flash->TranslucentTile), PanelBlend);
	}
	TestFalse(TEXT("VR: the scope closes with the panel's draw"), FWoTCanvasAlphaScope::IsOpen());
	// The texture is not readable without rendering (-nullrhi gives it no
	// resource): the blend each element is submitted with is what is checked.
	AddInfo(TEXT("Pixels are not readable headless; the panel's submissions' blend modes are asserted instead."));

	// Flat again: the colour-only blend, exactly as before.
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	const TArray<FSubmission> Restored = DrawObserved(S, 640, 480);
	const FSubmission* RestoredMessage = Restored.FindByPredicate([&IsMessage](const FSubmission& Seen) { return IsMessage(Seen.Line); });
	if (TestNotNull(TEXT("flat again: the message is drawn"), RestoredMessage))
	{
		TestEqual(TEXT("flat again: SE_BLEND_Translucent"), TextBlend(RestoredMessage->Line), Translucent);
	}
	TestEqual(TEXT("flat again: a translucent tile gets SE_BLEND_Translucent"),
		static_cast<int32>(FWoTCanvasScale::BlendModeFor(EWoTHUDStyle::Translucent)), Translucent);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixLayerWritesSpec, "WoT.VR.HeadsetFixes.LayersWrittenOnlyOnChange",
	WoTVRTest::Flags)
bool FWoTVRFixLayerWritesSpec::RunTest(const FString&)
{
	// Each SetLayerDesc marks the runtime's layers for rebuilding: a still
	// panel, a world-locked one and a hidden one tell the device nothing.
	using namespace WoTVRHUDTest;
	WoTVRTest::FScope Scope;
	WoTHUDHookTest::FScene S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	FWoTVRSpecLayers* Layers = Stereo.HMD->GetSpecLayers();
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig) || !TestNotNull(TEXT("the spec display's layers"), Layers)) return false;
	const FVector Head(0.0, 0.0, 0.0);
	Scope.Session->SetHead(Head, FRotator::ZeroRotator);
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset HUD"), Presentation)) return false;
	S.AddMessages();
	S.Draw();
	S.Draw();
	S.Draw();
	const uint32 HUDLayer = Presentation->GetHUDPanel().GetLayerId();
	if (!TestTrue(TEXT("the HUD panel shows"), HUDLayer != 0 && Presentation->GetHUDPanel().IsShown())) return false;

	// Still head, still panel: nothing is written, frame after frame; the
	// board, menu and card panels never shown have no layer at all.
	int32 Before = Layers->TotalWrites();
	for (int32 Frame = 0; Frame < 6; ++Frame)
	{
		S.Draw();
	}
	TestEqual(TEXT("a still HUD panel is not rewritten each frame"), Layers->TotalWrites(), Before);
	TestEqual(TEXT("panels never shown have no layer"), Layers->Creates, 1);

	// The head turns within the dead zone: the soft-attached panel holds still.
	Scope.Session->SetHead(Head, FRotator(5.0, 10.0, 0.0));
	S.Draw();
	S.Draw();
	TestEqual(TEXT("within the dead zone the HUD panel is not rewritten"), Layers->WritesTo(HUDLayer), 0);
	// The head moves: the panel comes with it, one write per frame it moves.
	Scope.Session->SetHead(Head + FVector(0.1, 0.0, 0.0), FRotator(5.0, 10.0, 0.0));
	S.Draw();
	TestEqual(TEXT("a moved HUD panel is written once"), Layers->WritesTo(HUDLayer), 1);
	S.Draw();
	TestEqual(TEXT("and not again while it stays"), Layers->WritesTo(HUDLayer), 1);
	// Tracking noise (a fifth of a millimetre) is not sent; steps that add up
	// past half a millimetre from what was sent are.
	Scope.Session->SetHead(Head + FVector(0.1002, 0.0, 0.0), FRotator(5.0, 10.0, 0.0));
	S.Draw();
	TestEqual(TEXT("a sub-millimetre jitter is not sent"), Layers->WritesTo(HUDLayer), 1);
	Scope.Session->SetHead(Head + FVector(0.1004, 0.0, 0.0), FRotator(5.0, 10.0, 0.0));
	S.Draw();
	TestEqual(TEXT("still within half a millimetre of what was sent"), Layers->WritesTo(HUDLayer), 1);
	Scope.Session->SetHead(Head + FVector(0.1006, 0.0, 0.0), FRotator(5.0, 10.0, 0.0));
	S.Draw();
	TestEqual(TEXT("small steps that add up past it are sent"), Layers->WritesTo(HUDLayer), 2);

	// The board: shown once, world-locked (never rewritten as the head turns),
	// hidden once, and then left alone.
	if (!S.AddScoreboardPlayers(*this, 3)) return false;
	S.SetScoreboardShown(true);
	S.Draw();
	S.Draw();
	const uint32 BoardLayer = Presentation->GetReadingPanel().GetLayerId();
	if (!TestTrue(TEXT("the board shows"), BoardLayer != 0 && Presentation->GetReadingPanel().IsShown())) return false;
	const int32 BoardWrites = Layers->WritesTo(BoardLayer);
	Scope.Session->SetHead(Head + FVector(0.1, 0.0, 0.0), FRotator(0.0, 15.0, 0.0));
	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		S.Draw();
	}
	TestEqual(TEXT("the world-locked board is not rewritten"), Layers->WritesTo(BoardLayer), BoardWrites);
	S.SetScoreboardShown(false);
	S.Draw();
	S.Draw();
	TestFalse(TEXT("the board hides"), Presentation->GetReadingPanel().IsShown());
	TestEqual(TEXT("hiding it is one write"), Layers->WritesTo(BoardLayer), BoardWrites + 1);
	for (int32 Frame = 0; Frame < 4; ++Frame)
	{
		S.Draw();
	}
	TestEqual(TEXT("a hidden panel is not written again"), Layers->WritesTo(BoardLayer), BoardWrites + 1);
	IStereoLayers::FLayerDesc Desc;
	TestTrue(TEXT("the device holds it hidden"), Layers->GetLayerDesc(BoardLayer, Desc) && IsHidden(Desc));
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTVRFixHUDStopsSpec, "WoT.VR.HeadsetFixes.PanelsHideWhenTheHUDStops",
	WoTVRTest::Flags)
bool FWoTVRFixHUDStopsSpec::RunTest(const FString&)
{
	// The HUD updates once for every frame it draws. When it stops drawing (the
	// HUD hidden, no HUD actor), no snapshot arrives and the panels must hide
	// rather than hold their last frame.
	using namespace WoTVRHUDTest;
	WoTVRTest::FScope Scope;
	WoTHUDHookTest::FScene S;
	if (!S.Init(*this)) return false;
	WoTVRTest::FStereoScope Stereo(Scope.Session, S.F.Widget);
	UWoTVRPlayerSubsystem* Rig = WoTVRTest::Rig(S.F.Player);
	if (!TestNotNull(TEXT("rig"), Rig)) return false;
	if (!TestTrue(TEXT("switch to VR"), Rig->RequestMode(EWoTVRMode::VR, TEXT("spec")))) return false;
	FWoTVRPresentation* Presentation = Rig->GetPresentation();
	if (!TestNotNull(TEXT("the headset HUD"), Presentation)) return false;
	S.AddMessages();
	if (!S.AddScoreboardPlayers(*this, 3)) return false;
	S.SetScoreboardShown(true);
	S.Draw();
	S.Draw();
	S.Draw();
	TestTrue(TEXT("drawing: the HUD panel shows"), Presentation->GetHUDPanel().IsShown());
	TestTrue(TEXT("drawing: the board shows"), Presentation->GetReadingPanel().IsShown());

	// Frames pass without a HUD draw. The first still has the last frame's
	// snapshot; from the next one there is none.
	S.Wait(1);
	TestTrue(TEXT("the last snapshot still shows"), Presentation->GetHUDPanel().IsShown());
	S.Wait(1);
	TestFalse(TEXT("no snapshot: the HUD panel hides"), Presentation->GetHUDPanel().IsShown());
	TestFalse(TEXT("no snapshot: the board hides"), Presentation->GetReadingPanel().IsShown()
		|| Presentation->IsReadingShown());
	IStereoLayers::FLayerDesc Desc;
	TestTrue(TEXT("the device holds the HUD panel hidden"), LayerOf(*Stereo.HMD, Presentation->GetHUDPanel(), Desc)
		&& IsHidden(Desc));
	S.Wait(5);
	TestFalse(TEXT("still hidden while nothing draws"), Presentation->GetHUDPanel().IsShown());

	// It draws again: the panels come back with the next snapshot.
	S.Draw();
	S.Draw();
	TestTrue(TEXT("drawing again: the HUD panel shows"), Presentation->GetHUDPanel().IsShown());
	TestTrue(TEXT("drawing again: the board shows"), Presentation->GetReadingPanel().IsShown());
	S.SetScoreboardShown(false);
	TestTrue(TEXT("switch to flat"), Rig->RequestMode(EWoTVRMode::Flat, TEXT("spec")));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
