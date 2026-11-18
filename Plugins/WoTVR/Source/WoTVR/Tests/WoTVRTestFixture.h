#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Camera/CameraTypes.h"
#include "InputCoreTypes.h"
#include "Misc/AutomationTest.h"
#include "WoTLocalPlayerHooks.h"
#include "WoTVRSettings.h"

class AActor;
class AGameModeBase;
class APawn;
class APlayerController;
class FSceneViewport;
class FWoTVRSyntheticSession;
class SViewport;
class UGameViewportClient;
class ULocalPlayer;
class UWoTVRPlayerSubsystem;
class UWorld;
struct FWorldContext;

// Shared fixtures for the WoT.VR specs. They run the plugin against the real
// game: real spec worlds, controllers, pawns and the real local-player hooks,
// with a synthetic session in place of OpenXR. The plugin links no game class,
// so game classes are reached by their script names and engine base types.
namespace WoTVRTest
{
	inline constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	/** One spec's plugin state: a synthetic session for OpenXR, the plugin
	 * active or not, the launch flags and known VR preferences, all restored
	 * when it ends and nothing written to disk. Declare it before any world, so
	 * the world (and its local player's rig) goes first. */
	struct FScope
	{
		TSharedRef<FWoTVRSyntheticSession> Session;

		explicit FScope(bool bActive = true, EWoTVRStartupMode Startup = EWoTVRStartupMode::Flat,
			TOptional<bool> bFlagVR = TOptional<bool>(), TOptional<bool> bFlagNoVR = TOptional<bool>());
		~FScope();

	private:
		bool bEnableVR;
		EWoTVRStartupMode StartupMode;
		EWoTVRPosture Posture;
		float StandingEyeHeightMeters;
		EWoTVRTurnMode TurnMode;
		float SnapTurnDegrees;
		float SmoothTurnDegreesPerSecond;
		FKey ModeSwitchKey;
		FKey RecentreKey;
		FKey TurnLeftKey;
		FKey TurnRightKey;
	};

	UWoTVRPlayerSubsystem* Rig(const ULocalPlayer* Player);

	/** A read-only hooks observer (no exclusive slot), as another optional
	 * consumer would be: it counts lifecycle events and can hold commands
	 * through the hooks. */
	struct FObserver
	{
		static const FName Owner;
		ULocalPlayer* Player = nullptr;
		TArray<EWoTLocalPlayerEvent> Events;

		bool Attach(ULocalPlayer& InPlayer);
		void Detach();
		int32 Count(EWoTLocalPlayerEvent Event) const;
		bool Command(EWoTLocalPlayerCommand Command, int32 Index = 0);
		~FObserver();
	};

	/** The final view the renderer receives for this local player. */
	FMinimalViewInfo FinalView(ULocalPlayer& Player);

	/** Keys through the player's real input, as a keyboard or pad delivers them. */
	void PressKey(APlayerController& PC, const FKey& Key);
	void ReleaseKey(APlayerController& PC, const FKey& Key);
	void SetAxis(APlayerController& PC, const FKey& Key, float Value);

	/** A game class by its script name (WoTGameMode -> /Script/WoT.WoTGameMode). */
	UClass* GameClass(const TCHAR* Name);
	/** Sets a float property by name on a game object (no game link). */
	bool SetFloatProperty(UObject* Object, const TCHAR* Name, float Value);
	bool SetIntProperty(UObject* Object, const TCHAR* Name, int32 Value);
	bool GetBoolProperty(const UObject* Object, const TCHAR* Name, bool& OutValue);
	/** Calls a game UFUNCTION by name with a parameter block. */
	bool CallFunction(UObject* Object, const TCHAR* Name, void* Params);

	/** A static box that blocks pawns, the camera and every other channel. */
	AActor* SpawnBlock(UWorld* World, const FVector& Center, const FVector& Extent);

	/** A spec world running a real game mode, with an input-ready viewport, a
	 * local player and its gameplay controller. Restart() spawns and possesses
	 * the pawn through the game mode, at a player start over a floor. */
	struct FMatchWorld
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameViewportClient* Viewport = nullptr;
		TSharedPtr<SViewport> Widget;
		TSharedPtr<FSceneViewport> Scene;
		ULocalPlayer* Player = nullptr;
		APlayerController* PC = nullptr;
		AGameModeBase* Game = nullptr;

		bool Init(FAutomationTestBase& Test, const TCHAR* GameModeName = TEXT("WoTGameMode"),
			bool bWithController = true);
		APawn* Restart(FAutomationTestBase& Test);
		void Frames(int32 Count);
		~FMatchWorld();
	};
}

#endif // WITH_AUTOMATION_TESTS
