// Slice 2e: headless state/layout coverage for the editor HUD shell. Canvas execution
// remains a PIE gate; these specs exercise the exact state and draw-command inputs.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Assets/WoTRuntimeAssets.h"
#include "../Inventory/WoTHandSet.h"
#include "../Payload/WoTNeutralResources.h"
#include "../WoTBattleGameMode.h"
#include "../WoTCharacter.h"
#include "../WoTHUD.h"
#include "../WoTPlayerController.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

namespace
{
	UWorld* EditorHudSpecCreateWorld(bool bBattleMode)
	{
		return bBattleMode
			? WoTTest::CreateSpecWorld(AWoTBattleGameMode::StaticClass())
			: WoTTest::CreateSpecWorld();
	}

	FActorSpawnParameters EditorHudSpecAlwaysSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	AWoTCharacter* EditorHudSpecSpawnCharacter(UWorld* World, float X)
	{
		return World->SpawnActor<AWoTCharacter>(FVector(X, 0.0f, 100.0f),
			FRotator::ZeroRotator, EditorHudSpecAlwaysSpawnParams());
	}

	AWoTHUD* EditorHudSpecSpawnLocalPlayer(UWorld* World,
		AWoTPlayerController*& OutController, AWoTCharacter*& OutCharacter)
	{
		OutController = World->SpawnActor<AWoTPlayerController>(
			EditorHudSpecAlwaysSpawnParams());
		OutCharacter = EditorHudSpecSpawnCharacter(World, 0.0f);
		if (!IsValid(OutController) || !IsValid(OutCharacter))
		{
			return nullptr;
		}
		OutController->SetAsLocalPlayerController();
		OutController->Possess(OutCharacter);
		OutController->ClientSetHUD(AWoTHUD::StaticClass());
		return Cast<AWoTHUD>(OutController->GetHUD());
	}

	void EditorHudSpecSetEditState(AWoTPlayerController* Controller, uint8 Team,
		bool bAuthorityGrant)
	{
		if (!IsValid(Controller))
		{
			return;
		}
		Controller->EditSessionTeam = bAuthorityGrant ? Team : 255;
		Controller->OwnerEditSessionTeam = Team;
		Controller->OnRep_OwnerEditSessionTeam();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorHudModeSpec,
	"WoT.Citadel.EditorHud.ModeReflectsOwnerState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorHudModeSpec::RunTest(const FString&)
{
	UWorld* World = EditorHudSpecCreateWorld(false);
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Character = nullptr;
	AWoTHUD* HUD = EditorHudSpecSpawnLocalPlayer(World, Controller, Character);
	if (!TestNotNull(TEXT("local AWoTHUD exists"), HUD))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	TestEqual(TEXT("HUD starts normal"), static_cast<int32>(HUD->GetHUDMode()),
		static_cast<int32>(EWoTHUDMode::Normal));
	EditorHudSpecSetEditState(Controller, 2, false);
	TestTrue(TEXT("owner edit mirror selects editor HUD mode"), HUD->IsEditorMode());
	TestTrue(TEXT("editor mode uses CitadelEditorHandSet"),
		Character->CurrentHandSet == Character->CitadelEditorHandSet);

	const TArray<FWoTHUDDrawCmd> Hands = AWoTHUD::LayoutHands(
		Character->CurrentHandSet, 800, 600);
	const TArray<FWoTHUDDrawCmd> CurrentHand = AWoTHUD::LayoutCurrentHand(
		Character->CurrentHandSet, 800, 600);
	TestTrue(TEXT("editor palette uses normal hand layout commands"), Hands.Num() > 0);
	TestTrue(TEXT("selected editor hand uses current-hand layout commands"),
		CurrentHand.Num() > 0);

	Controller->OwnerEditSessionTeam = 255;
	Controller->OnRep_OwnerEditSessionTeam();
	TestEqual(TEXT("owner exit mirror restores normal HUD mode"),
		static_cast<int32>(HUD->GetHUDMode()),
		static_cast<int32>(EWoTHUDMode::Normal));
	TestTrue(TEXT("owner exit restores AngrealHandSet"),
		Character->CurrentHandSet == Character->AngrealHandSet);

	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorHudPossessionRestoreSpec,
	"WoT.Citadel.EditorHud.PossessionRestoresNormal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorHudPossessionRestoreSpec::RunTest(const FString&)
{
	UWorld* World = EditorHudSpecCreateWorld(false);
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* FirstCharacter = nullptr;
	AWoTHUD* HUD = EditorHudSpecSpawnLocalPlayer(World, Controller, FirstCharacter);
	AWoTCharacter* Replacement = EditorHudSpecSpawnCharacter(World, 200.0f);
	if (!IsValid(Controller) || !IsValid(HUD) || !IsValid(Replacement))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	EditorHudSpecSetEditState(Controller, 1, true);
	TestTrue(TEXT("precondition: editor HUD active"), HUD->IsEditorMode());
	Controller->Possess(Replacement);
	TestFalse(TEXT("possession ends authority edit grant"),
		Controller->IsEditingCitadel());
	TestFalse(TEXT("possession clears owner edit mirror"),
		Controller->IsOwnerEditingCitadel());
	TestEqual(TEXT("possession restores normal HUD"),
		static_cast<int32>(HUD->GetHUDMode()),
		static_cast<int32>(EWoTHUDMode::Normal));
	TestTrue(TEXT("replacement pawn retains normal hands"),
		Replacement->CurrentHandSet == Replacement->AngrealHandSet);

	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorHudPhaseRestoreSpec,
	"WoT.Citadel.EditorHud.MatchPhaseRestoresNormal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorHudPhaseRestoreSpec::RunTest(const FString&)
{
	UWorld* World = EditorHudSpecCreateWorld(true);
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Character = nullptr;
	AWoTHUD* HUD = EditorHudSpecSpawnLocalPlayer(World, Controller, Character);
	AWoTBattleGameMode* BattleMode = World->GetAuthGameMode<AWoTBattleGameMode>();
	if (!IsValid(Controller) || !IsValid(HUD) || !IsValid(BattleMode))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}

	EditorHudSpecSetEditState(Controller, 0, true);
	TestTrue(TEXT("precondition: editor HUD active"), HUD->IsEditorMode());
	BattleMode->EnableTeleporters();
	TestFalse(TEXT("phase edge ends authority edit grant"),
		Controller->IsEditingCitadel());
	TestFalse(TEXT("phase edge clears owner edit mirror"),
		Controller->IsOwnerEditingCitadel());
	TestEqual(TEXT("phase edge restores normal HUD"),
		static_cast<int32>(HUD->GetHUDMode()),
		static_cast<int32>(EWoTHUDMode::Normal));

	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorHudCursorSpec,
	"WoT.Citadel.EditorHud.CursorGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorHudCursorSpec::RunTest(const FString&)
{
	const FWoTEditorCursorGeometry Even = AWoTHUD::EditorCursorGeometry(800, 600);
	TestEqual(TEXT("cursor width"), Even.Size.X, 16);
	TestEqual(TEXT("cursor height"), Even.Size.Y, 16);
	TestEqual(TEXT("cursor hotspot X"), Even.Hotspot.X, 7);
	TestEqual(TEXT("cursor hotspot Y"), Even.Hotspot.Y, 7);
	TestEqual(TEXT("even viewport cursor point X"), static_cast<float>(Even.CursorPoint.X), 400.0f);
	TestEqual(TEXT("even viewport cursor point Y"), static_cast<float>(Even.CursorPoint.Y), 300.0f);
	TestEqual(TEXT("hotspot places even cursor X"), static_cast<float>(Even.TopLeft.X), 393.0f);
	TestEqual(TEXT("hotspot places even cursor Y"), static_cast<float>(Even.TopLeft.Y), 293.0f);

	const FWoTEditorCursorGeometry Odd = AWoTHUD::EditorCursorGeometry(801, 601);
	TestEqual(TEXT("odd viewport fixed integer center X"), static_cast<float>(Odd.CursorPoint.X), 400.0f);
	TestEqual(TEXT("odd viewport fixed integer center Y"), static_cast<float>(Odd.CursorPoint.Y), 300.0f);
	const TArray<FWoTHUDDrawCmd> Commands = AWoTHUD::LayoutEditorCursor(801, 601);
	TestEqual(TEXT("one editor cursor command"), Commands.Num(), 1);
	if (Commands.Num() == 1)
	{
		TestEqual(TEXT("editor cursor texture"), Commands[0].TexId,
			FName(TEXT("UI_CECursor")));
		TestEqual(TEXT("editor cursor X"), Commands[0].X, 393.0f);
		TestEqual(TEXT("editor cursor Y"), Commands[0].Y, 293.0f);
		TestEqual(TEXT("editor cursor is masked"),
			static_cast<int32>(Commands[0].Style),
			static_cast<int32>(EWoTHUDStyle::Masked));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorHudHealthGateSpec,
	"WoT.Citadel.EditorHud.HealthSuppressed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorHudHealthGateSpec::RunTest(const FString&)
{
	TestTrue(TEXT("normal mode permits DrawHealth"),
		AWoTHUD::ShouldDrawHealth(EWoTHUDMode::Normal));
	TestFalse(TEXT("editor mode gates DrawHealth before command generation"),
		AWoTHUD::ShouldDrawHealth(EWoTHUDMode::CitadelEditor));
	return true;
}

// T6-2 packaged runs recorded assets-resolved failed_textures=20 (host) and
// 90-180 (client), all /Game/WoT/UI/I_S* (I_SSeal, I_STrolloc, I_SMyrddraal,
// I_SAlarm). Cause: ExecuteHandCommands resolved every command's TexId through
// the HUD-registry route FIRST, even when the command carried its item's
// authored StatusIcon (Texture'WoTTraps.UI.I_SAlarm' -> /Game/WoT/Textures/
// WoTTraps/I_SAlarm). The TexId is only that asset's leaf, and the registry
// route /Game/WoT/UI/I_SAlarm names no retail identity. Each draw command now
// resolves exactly one route: the bound item texture, else the registry id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCitadelEditorHudIconRouteSpec,
	"WoT.Citadel.EditorHud.HandIconsResolveRetailIdentities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCitadelEditorHudIconRouteSpec::RunTest(const FString&)
{
	UWorld* World = EditorHudSpecCreateWorld(false);
	AWoTPlayerController* Controller = nullptr;
	AWoTCharacter* Character = nullptr;
	AWoTHUD* HUD = EditorHudSpecSpawnLocalPlayer(World, Controller, Character);
	if (!TestNotNull(TEXT("local AWoTHUD exists"), HUD))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	EditorHudSpecSetEditState(Controller, 0, false);
	TestTrue(TEXT("editor mode uses CitadelEditorHandSet"),
		Character->CurrentHandSet == Character->CitadelEditorHandSet);

	// The editor palette's authored icons exactly as the production cache
	// contract names them: CE_SEALS (SealInventory.uc StatusIcon=
	// Texture'WOT.UI.I_SSeal') and the seven trap cards (<Trap>Inventory.uc
	// StatusIcon=Texture'WoTTraps.UI.I_S<X>').
	TMap<FString, FString> Expected;
	Expected.Add(TEXT("I_SSeal"), TEXT("cache/texture/WOT__I_SSeal"));
	for (const TCHAR* TrapLeaf : {TEXT("I_SAlarm"), TEXT("I_SSpear"), TEXT("I_SFire"),
		TEXT("I_SWall"), TEXT("I_SPit"), TEXT("I_SStaircase"), TEXT("I_SPortcullis")})
	{
		Expected.Add(TrapLeaf, FString(TEXT("cache/texture/WoTTraps__")) + TrapLeaf);
	}
	TArray<FWoTNeutralResourceId> Registry;
	for (const TPair<FString, FString>& Row : Expected)
	{
		Registry.Add({Row.Value, EWoTNeutralResourceKind::Texture});
	}

	TSet<FString> SeenIcons;
	for (const FWoTHUDDrawCmd& Command : AWoTHUD::LayoutHands(
		Character->CurrentHandSet, 800, 600))
	{
		if (Command.Texture.IsNull())
		{
			continue;
		}
		const FString Leaf = Command.TexId.ToString();
		SeenIcons.Add(Leaf);
		const FString* WantedIdentity = Expected.Find(Leaf);
		if (!TestNotNull(Leaf + TEXT(" is a pinned editor palette icon"), WantedIdentity))
		{
			continue;
		}
		TestEqual(Leaf + TEXT(" resolves its bound item texture only"),
			AWoTHUD::CommandTexturePath(Command), Command.Texture.ToSoftObjectPath());
		FString Identity;
		FString Failure;
		TestTrue(Leaf + TEXT(" item texture is a retail cache identity"),
			UWoTRuntimeAssets::ResolveCacheId(
				AWoTHUD::CommandTexturePath(Command).ToString(),
				EWoTRuntimeAssetKind::Texture, Registry, Identity, &Failure));
		TestEqual(Leaf + TEXT(" maps to its retail identity"), Identity, *WantedIdentity);
		FString Ignored;
		TestFalse(Leaf + TEXT(" leaf is NOT a HUD-registry id (the old eager route)"),
			UWoTRuntimeAssets::ResolveCacheId(
				AWoTHUD::RuntimeHUDTexturePath(Command.TexId).ToString(),
				EWoTRuntimeAssetKind::Texture, Registry, Ignored));
	}
	TestTrue(TEXT("the seal card draws its authored icon (T6-2: I_SSeal)"),
		SeenIcons.Contains(TEXT("I_SSeal")));
	TestTrue(TEXT("the alarm trap card draws its authored icon (T6-2: I_SAlarm)"),
		SeenIcons.Contains(TEXT("I_SAlarm")));

	FWoTHUDDrawCmd Notch;
	Notch.TexId = FName(TEXT("I_Notch"));
	TestEqual(TEXT("unbound commands keep the HUD-registry route"),
		AWoTHUD::CommandTexturePath(Notch).ToString(),
		FString(TEXT("/Game/WoT/UI/I_Notch.I_Notch")));
	TestTrue(TEXT("an icon-less command resolves nothing (no None lookup)"),
		AWoTHUD::CommandTexturePath(FWoTHUDDrawCmd()).IsNull());

	WoTTest::TickWorld(World, 1);
	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
