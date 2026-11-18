#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "WoTLocalPlayerHooksTestFixture.h"
#include "../WoTHUDSnapshot.h"

class AWoTCharacter;
class AWoTHUD;
class FAutomationTestBase;
class UWorld;

// The flat HUD's canvas output, item by item, for representative states (H5).
// The streams were recorded from the HUD before its update, snapshot, layout
// and draw were separated; with no consumer attached, or with a consumer
// attached and detached around the draws, they must not change. Exported for
// the optional WoTVR plugin's specs, which replay the same states with VR
// switched on and off around every frame.
namespace WoTHUDHookTest
{
	/** Spawning that never fails on overlap. */
	WOT_API FActorSpawnParameters SpawnParams();
	/** Runs the real DrawHUD once on a Width x Height canvas, headless, and
	 * returns every tile and text run submitted during the draw (the flat HUD's,
	 * and any destination's that draws inside it), each with the canvas size it
	 * landed on. */
	WOT_API TArray<FString> DrawHUDOnce(AWoTHUD& HUD, int32 Width, int32 Height);
	/** Draws arranged sections on a canvas of the caller's own, as a destination
	 * does, and returns what was submitted. */
	WOT_API TArray<FString> DrawSectionsOnce(UWorld* World, TConstArrayView<FWoTHUDSection> Sections,
		int32 Width, int32 Height);
	/** How many commands of a snapshot section draw this text. */
	WOT_API int32 SectionTexts(const FWoTHUDSnapshot& Snapshot, EWoTHUDSection Part, const FString& Text);
	/** Whether a submission line from DrawHUDOnce landed on a canvas of this size. */
	WOT_API bool LandedOnCanvas(const FString& Line, int32 Width, int32 Height);

	/** A real local player's flat HUD: a body with a health icon, three
	 * artifacts in its hands and two status icons. */
	struct WOT_API FScene
	{
		WoTLocalPlayerHooksTest::FPlayerWorld F;
		AWoTHUD* HUD = nullptr;
		AWoTCharacter* Body = nullptr;
		int32 FrameIndex = 0;
		TArray<FString> Stream;
		/** Runs in each recorded frame after its tick and before its flat draw. */
		TFunction<void(FScene&, int32 /*Width*/, int32 /*Height*/)> BeforeDraw;
		/** Runs first when the scene ends, before its world goes. */
		TFunction<void()> OnTeardown;

		/** The world (the Arena rules unless Mode says otherwise), the body, its
		 * HUD and its hands. */
		bool Init(FAutomationTestBase& Test, TSubclassOf<AGameModeBase> Mode = nullptr);
		/** Every queue the HUD draws: hand, left, right, centre, subtitle and
		 * generic messages, and two progress lines. */
		void AddMessages();
		/** The warning icon and a flash texture (the slow-frame warning). */
		void ShowWarningAndFlash();
		/** Count more named players on the scoreboard, and a name for the
		 * local one: with 15, the full sixteen-player board. */
		bool AddScoreboardPlayers(FAutomationTestBase& Test, int32 Count);
		/** The scoreboard held (or released) by the local player. */
		void SetScoreboardShown(bool bShown);
		/** One frame: the world ticks once, then the HUD draws. */
		void Draw(int32 Width = 640, int32 Height = 480);
		/** Draw, returning this frame's submissions. */
		TArray<FString> DrawFrame(int32 Width = 640, int32 Height = 480);
		/** Advance time without drawing (Count ticks of 1/60 s). */
		void Wait(int32 Count);
		~FScene();
	};

	/** Compares a recorded stream with its golden; on a mismatch the stream is
	 * saved under the automation directory and the spec fails. */
	WOT_API void CheckStream(FAutomationTestBase& Test, const FString& State, const TArray<FString>& Lines);

	/** The ten recorded states, each in a fresh scene, checked against the
	 * goldens. Prepare runs on each scene right after it is built. */
	WOT_API bool RunRecordedStates(FAutomationTestBase& Test,
		TFunction<bool(FScene&)> Prepare = TFunction<bool(FScene&)>());
}

#endif // WITH_AUTOMATION_TESTS
