#pragma once

#include "CoreMinimal.h"

struct FSlateDynamicImageBrush;
class APlayerController;

/**
 * The setup (bootstrap) screen's backdrop: a blurred screenshot of the main
 * menu scene instead of plain black.
 *
 * The first run has no game content yet, so it draws a generated dark
 * gradient. Once the front end has shown its main-menu scene, a downscaled,
 * blurred and brightness-normalized capture of that scene is saved beside the user's
 * preferences and used from then on. The capture is made on the player's
 * machine from their own retail copy and never shipped in a package.
 */
namespace WoTSetupBackdrop
{
// Kept at the capture size: a 320x180 backdrop stretched 4x read as too
// blurry.
constexpr int32 Width = 640;
constexpr int32 Height = 360;

/** Box-downsample Pixels (Size) to Width x Height, blur, and normalize its
 * mean luminance to the menu screen's own (~14/255; gain clamped 0.5-3).
 * Empty
 * when Pixels does not hold Size.X * Size.Y colors. */
TArray<FColor> FromCapture(TConstArrayView<FColor> Pixels, FIntPoint Size);
/** The generated first-run backdrop: slate-to-black gradient, vignette. */
TArray<FColor> Fallback();

/** <preference dir>/WoTSetupBackdrop.png (UWoTPreferencesSubsystem::SideFilePath). */
bool Path(FString& OutPath);
/** Writes a Width x Height backdrop as PNG, replacing any previous one. */
bool Save(TConstArrayView<FColor> Backdrop, const FString& InPath);
/** Reads a saved backdrop; false when missing, oversized, malformed, or not
 * exactly Width x Height. */
bool Load(const FString& InPath, TArray<FColor>& OutBackdrop);
/** The saved backdrop when it loads, else the generated one. */
TArray<FColor> Current();
/** True when the saved backdrop at InPath is missing, does not load (a
 * corrupt or foreign file is repaired by the next capture), or predates the
 * running executable (a new install or update recaptures once). */
bool IsStale(const FString& InPath);
/** The width and height a PNG header declares, read before any decoding;
 * false for anything that is not a PNG. */
bool ReadPngSize(TConstArrayView<uint8> Bytes, FIntPoint& OutSize);

/** A stretchable Slate brush for a Width x Height backdrop, or null when no
 * Slate renderer can host it (headless runs). */
TSharedPtr<FSlateDynamicImageBrush> MakeBrush(TConstArrayView<FColor> Backdrop);

/** If the saved backdrop is stale, render the controller's current camera
 * view once into a small render target (a scene capture: the scene only, never
 * the menu over it), read it on the next tick, and save its backdrop. At most
 * one capture is pending at a time. Never reads the game viewport and never
 * involves the engine's screenshot request. */
void CaptureIfStale(APlayerController* Controller);
/** Drops a pending capture and its scene-capture component. */
void CancelPendingCapture();
}
