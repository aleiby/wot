#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

class AActor;
class APlayerController;
class SWidget;
class UWorld;

// The game's real screens, built the way the H6 hosting specs build them
// (placeholder labels and the engine's own fonts), for the optional WoTVR
// plugin's specs: the screen widgets are the game module's own and are not
// exported, so these hand them over as plain Slate widgets.
namespace WoTUIScreensTest
{
	/** The front end's root, on its main menu. */
	WOT_API TSharedRef<SWidget> MakeFrontEnd();
	/** Ticks the front end at Size and runs Paint after each pass, so it
	 * builds its current screen the way the UI specs drive it. */
	WOT_API void SettleFrontEnd(const TSharedRef<SWidget>& FrontEnd, const FVector2D& Size,
		TFunctionRef<void()> Paint);
	/** The server browser (the main menu's first entry). */
	WOT_API void OpenServerBrowser(const TSharedRef<SWidget>& FrontEnd);
	/** Class and team choice (the player setup). */
	WOT_API void OpenPlayerSetup(const TSharedRef<SWidget>& FrontEnd);
	WOT_API TSharedPtr<SWidget> AddressField(const TSharedRef<SWidget>& FrontEnd);
	WOT_API TSharedPtr<SWidget> GamePasswordField(const TSharedRef<SWidget>& FrontEnd);
	WOT_API TSharedPtr<SWidget> PlayerNameField(const TSharedRef<SWidget>& FrontEnd);
	WOT_API FString PendingPlayerName(const TSharedRef<SWidget>& FrontEnd);

	/** The in-game menu of a gameplay controller, and the row it focuses first. */
	WOT_API TSharedRef<SWidget> MakeInGameMenu(APlayerController& Controller);
	WOT_API TSharedPtr<SWidget> InGameMenuFocus(const TSharedRef<SWidget>& Menu);
	/** The selected item's information window for an artifact spawned in World. */
	WOT_API TSharedRef<SWidget> MakeItemInfo(APlayerController& Controller);
	/** The help window, as the controller opens it for a screen of this size;
	 * OnClose runs when it asks to close (B, Escape, its close keys). */
	WOT_API TSharedRef<SWidget> MakeHelp(const FVector2D& PhysicalSize,
		TFunction<void()> OnClose = TFunction<void()>());
	/** The help window's whole-number pixel scale (1 or 2). */
	WOT_API int32 HelpPixelScale(const TSharedRef<SWidget>& Help);
	/** The online sharing notice: passive, over any screen. */
	WOT_API TSharedRef<SWidget> MakeNotice();

	/** Opens the controller's real chat line, as the gamepad's chat button
	 * does; null when it did not open. */
	WOT_API TSharedPtr<SWidget> OpenControllerChat(APlayerController& Controller);
	WOT_API void CloseChat(APlayerController& Controller);
	/** The chat line's UI DPI scale, as it laid itself out last. */
	WOT_API float ChatDpiScale(const TSharedRef<SWidget>& Chat);
}

#endif // WITH_AUTOMATION_TESTS
