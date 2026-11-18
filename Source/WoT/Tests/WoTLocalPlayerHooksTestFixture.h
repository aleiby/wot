#pragma once

#include "CoreMinimal.h"

#if WITH_AUTOMATION_TESTS

#include "Engine/EngineTypes.h"
#include "GameFramework/GameModeBase.h"
#include "Templates/SubclassOf.h"

class AActor;
class APlayerController;
class AWoTCharacter;
class AWoTPlayerController;
class FAutomationTestBase;
class FWoTFrontEndSceneMaterializer;
class FSceneViewport;
class SViewport;
class UCameraComponent;
class UGameViewportClient;
class UInputComponent;
class ULocalPlayer;
class UWorld;
struct FInputActionBinding;
struct FWorldContext;

// Shared fixtures for the WoT.VR.Hooks specs: real spec worlds, a real local
// player behind an input-ready game viewport, and the startup order that
// creates the real preferences subsystem. Exported for the optional WoTVR
// plugin's specs, which drive the hooks through the same worlds.
namespace WoTLocalPlayerHooksTest
{
	/** Publishes a validated early-UI record, as startup does, so the next spec
	 * world creates the real preferences subsystem. Declare it before the world
	 * so the world is destroyed first. */
	struct WOT_API FPreferencesScope
	{
		bool Init(FAutomationTestBase& Test);
		~FPreferencesScope();

	private:
		bool bActive = false;
	};

	/** A spec world with a game viewport, a local player, a gameplay controller
	 * (local from BeginPlay, as the game mode spawns it) and a possessed
	 * character. */
	struct WOT_API FPlayerWorld
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameViewportClient* Viewport = nullptr;
		TSharedPtr<SViewport> Widget;
		TSharedPtr<FSceneViewport> Scene;
		ULocalPlayer* Player = nullptr;
		AWoTPlayerController* PC = nullptr;
		AWoTCharacter* Pawn = nullptr;

		bool Init(FAutomationTestBase& Test, const FVector& PawnLocation = FVector(0, 0, 500),
			EMovementMode Mode = MOVE_Flying, TSubclassOf<AGameModeBase> GameMode = nullptr);
		void Frames(int32 Count);
		/** A static box that blocks pawns (floors, walls). */
		AActor* SpawnBlock(const FVector& Center, const FVector& Extent);
		/** Execute the first action binding with this name and edge on the
		 * controller's or the pawn's input component, as the key would. */
		bool Binding(FName Name, EInputEvent Edge = IE_Pressed);
		/** The first action binding with this name and edge, or null. */
		const FInputActionBinding* FindBinding(FName Name, EInputEvent Edge = IE_Pressed) const;
		/** Open or close an onboarding window's UI-only input through the
		 * controller's own overlay seam. False when the seam is not installed. */
		bool SetOverlayInput(bool bOpen);
		/** Whether the game viewport ignores input (a window holds UI-only input). */
		bool ViewportIgnoresInput() const;
		~FPlayerWorld();
	};

	/** The front end's real scene, committed by the scene materializer from its
	 * synthetic test content (FWoTFrontEndSceneMaterializer::StageForTesting),
	 * with a controller's view on it: the real scene camera at an authored eye.
	 * Shut down with this object; declare it after the world so it goes first. */
	struct WOT_API FFrontEndScene
	{
		FFrontEndScene();
		~FFrontEndScene();
		FFrontEndScene(const FFrontEndScene&) = delete;
		FFrontEndScene& operator=(const FFrontEndScene&) = delete;

		bool Commit(FAutomationTestBase& Test, UWorld& World, APlayerController& ViewOwner,
			const FVector& Eye, const FRotator& Rotation);

		/** The scene's root, the view owner's view target after the commit. */
		AActor* Root = nullptr;
		/** The scene's own view camera. */
		UCameraComponent* Camera = nullptr;

	private:
		TUniquePtr<FWoTFrontEndSceneMaterializer> Materializer;
	};
}

#endif // WITH_AUTOMATION_TESTS
