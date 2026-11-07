#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../Settings/WoTGameSettings.h"
#include "../WoTGameInstance.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTCharacter.h"
#include "../WoTPlayerController.h"
#include "../WoTHUD.h"
#include "../WoTBloodFX.h"
#include "../WoTCreatureChunk.h"
#include "../WoTVertexAnimComponent.h"
#include "../Payload/WoTPayloadActivation.h"
#include "WoTTestWorld.h"
#include "WoTLaunchEngineHooks.h"

#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerState.h"
#include "Components/BoxComponent.h"
#include "Engine/StaticMeshActor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include <limits>

namespace
{
constexpr EAutomationTestFlags SettingsFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

TArray<uint8> Utf8(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	return Bytes;
}

void StripVersionSevenPresentationRecords(FString& Text)
{
	for (const TCHAR* Key : {
		TEXT("GameViewBob="), TEXT("GameLookSpring="), TEXT("GameJoystick="),
		TEXT("GameAlwaysMouseLook="),
		TEXT("GameAutoSlopeLook="), TEXT("GamePlayerID="),
		TEXT("GameCrosshair="), TEXT("GameGoreDetail="),
		// and the version-10 controller records
		TEXT("GameControllerLookSensitivity="), TEXT("GameControllerInvertY="),
		TEXT("GameControllerDeadZone=")})
	{
		const int32 Start = Text.Find(Key, ESearchCase::CaseSensitive);
		if (Start != INDEX_NONE)
		{
			const int32 End = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive,
				ESearchDir::FromStart, Start);
			Text.RemoveAt(Start, End == INDEX_NONE ? Text.Len() - Start : End - Start + 1);
		}
	}
}

FWoTBootstrapStatus AdoptDefaults(UWoTPreferencesSubsystem& Preferences)
{
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
		Utf8("Schema=EarlyUI-v1\nUIScale=1.25\nLocale=fr-FR\n"),
		FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
	if (Parsed.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Parsed).GetError());
	}
	return Preferences.AdoptAuthenticatedEarlyUI(MoveTemp(Parsed).GetValue());
}

AStaticMeshActor* P1C_SpawnStairSlab(UWorld* World, const FVector& Location,
	const FVector& Extent)
{
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
		Location, FRotator::ZeroRotator, Spawn);
	if (!Actor) return nullptr;
	UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
	Box->SetupAttachment(Actor->GetRootComponent());
	Box->SetBoxExtent(Extent);
	Box->SetMobility(EComponentMobility::Static);
	Box->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Box->SetCollisionObjectType(ECC_WorldStatic);
	Box->SetCollisionResponseToAllChannels(ECR_Block);
	Box->RegisterComponent();
	return Actor;
}

struct FSettingsFileFixture
{
	FString PreviousCulture = FInternationalization::Get().GetCurrentCulture()->GetName();
	float PreviousScale = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetApplicationScale() : 1.0f;
	float PreviousVolume = FApp::GetVolumeMultiplier();
	FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("Automation"), FString(TEXT("RealSettings-")) + FGuid::NewGuid().ToString());
	FString Leaf = FPaths::Combine(Directory, TEXT("WoTUserPreferences"));
	FSettingsFileFixture()
	{
		IFileManager::Get().MakeDirectory(*Directory, true);
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(Leaf);
		ResetWoTPayloadActivationForTesting();
	}
	~FSettingsFileFixture()
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPreferencesSubsystem::SetPreferencePathForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
		FApp::SetVolumeMultiplier(PreviousVolume);
		FInternationalization::Get().SetCurrentCulture(PreviousCulture);
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetApplicationScale(PreviousScale);
		}
	}
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGameSettingsValidationSpec,
	"WoT.Preferences.GameSettings.Validation", SettingsFlags)

bool FWoTGameSettingsValidationSpec::RunTest(const FString&)
{
	FWoTGameSettings Settings = FWoTGameSettings::Defaults();
	FString Error;
	TestTrue(TEXT("closed defaults validate"), Settings.Validate(Error));
	TestTrue(TEXT("VSync defaults on to prevent first-run tearing"),
		Settings.bVSync);
	TestEqual(TEXT("every managed action has a binding"), Settings.Bindings.Num(), 53);
	TestEqual(TEXT("retail brightness defaults to 0.6"), Settings.Brightness, 0.6f);
	TestEqual(TEXT("retail PlayerPawn Bob defaults to 0.016"),
		Settings.ViewBob, 0.016f);
	TestEqual(TEXT("retail crosshair defaults to 13"), Settings.Crosshair, uint8(13));
	TestEqual(TEXT("retail gore detail defaults to 2"),
		Settings.GoreDetailLevel, uint8(2));
	TestFalse(TEXT("retail look spring defaults off"), Settings.bLookSpring);
	TestTrue(TEXT("retail always-mouse-look defaults on"), Settings.bAlwaysMouseLook);
	// Retail's UseJoystick defaults off; controller support turns it on (PORT_NOTES).
	TestTrue(TEXT("the joystick preference defaults on"),
		Settings.bJoystickEnabled);
	TestTrue(TEXT("retail Tab Type binding is managed"),
		Settings.FindBinding(EWoTManagedBinding::Type)->Primary == EKeys::Tab);
	TestTrue(TEXT("retail T Talk binding is managed"),
		Settings.FindBinding(EWoTManagedBinding::Talk)->Primary == EKeys::T);
	TestTrue(TEXT("retail Y TeamTalk binding is managed"),
		Settings.FindBinding(EWoTManagedBinding::TeamTalk)->Primary == EKeys::Y);
	const FWoTManagedKeyBinding* Drop = Settings.FindBinding(EWoTManagedBinding::Drop);
	TestTrue(TEXT("retail Ctrl/Backslash Drop bindings are managed"), Drop
		&& Drop->Primary == EKeys::LeftControl && Drop->Secondary == EKeys::Backslash);
	TestTrue(TEXT("retail Alt DropTainted binding is managed"),
		Settings.FindBinding(EWoTManagedBinding::DropTainted)->Primary == EKeys::LeftAlt);
	const FWoTManagedKeyBinding* Scores = Settings.FindBinding(EWoTManagedBinding::ShowScores);
	TestTrue(TEXT("secondary score key exists"), Scores && Scores->Secondary == EKeys::MiddleMouseButton);
	const FWoTManagedKeyBinding* Forward = Settings.FindBinding(EWoTManagedBinding::MoveForward);
	TestTrue(TEXT("retail cursor key supplements forward movement"),
		Forward && Forward->Primary == EKeys::W && Forward->Secondary == EKeys::Up);
	const FWoTManagedKeyBinding* HandSix = Settings.FindBinding(EWoTManagedBinding::SelectHand6);
	TestTrue(TEXT("retail Q key supplements hand index five"),
		HandSix && HandSix->Secondary == EKeys::Q);
	TestTrue(TEXT("F1 help is managed"),
		Settings.FindBinding(EWoTManagedBinding::ShowHelp)->Primary == EKeys::F1);
	TestTrue(TEXT("F10 citadel editor is managed"),
		Settings.FindBinding(EWoTManagedBinding::ServerEditCitadel)->Primary == EKeys::F10);
	TestTrue(TEXT("F9 screenshot is managed"),
		Settings.FindBinding(EWoTManagedBinding::SShot)->Primary == EKeys::F9);
	TestTrue(TEXT("F12 fullscreen is managed"),
		Settings.FindBinding(EWoTManagedBinding::ToggleFullscreen)->Primary == EKeys::F12);
	const FWoTManagedKeyBinding* Walking =
		Settings.FindBinding(EWoTManagedBinding::Walking);
	TestTrue(TEXT("retail Shift Walking covers both shift keys"), Walking
		&& Walking->Primary == EKeys::LeftShift
		&& Walking->Secondary == EKeys::RightShift);
	const TPair<EWoTManagedBinding, FKey> InputParityDefaults[] = {
		{EWoTManagedBinding::PlayTaunt1, EKeys::G},
		{EWoTManagedBinding::PlayTaunt2, EKeys::H},
		{EWoTManagedBinding::PlayTaunt3, EKeys::J},
		{EWoTManagedBinding::PlayTaunt4, EKeys::K},
		{EWoTManagedBinding::PlayTaunt5, EKeys::L},
		{EWoTManagedBinding::PlayTaunt6, EKeys::Semicolon},
		{EWoTManagedBinding::PlayTauntRandom, EKeys::R},
		{EWoTManagedBinding::Look, EKeys::M},
		{EWoTManagedBinding::LookToggle, EKeys::CapsLock},
		{EWoTManagedBinding::FreeLook, EKeys::Insert},
		{EWoTManagedBinding::CenterView, EKeys::End},
		{EWoTManagedBinding::Strafe, EKeys::Apostrophe},
		{EWoTManagedBinding::Brightness, EKeys::F11},
		{EWoTManagedBinding::Pause, EKeys::Pause},
	};
	for (const TPair<EWoTManagedBinding, FKey>& Expected : InputParityDefaults)
	{
		const FWoTManagedKeyBinding* Binding = Settings.FindBinding(Expected.Key);
		TestTrue(FString::Printf(TEXT("input-parity default %s"),
			FWoTGameSettings::ActionName(Expected.Key)),
			Binding && Binding->Primary == Expected.Value);
	}
	Settings.MouseSensitivity = 10.01f;
	TestFalse(TEXT("out-of-range mouse sensitivity rejected"), Settings.Validate(Error));
	Settings = FWoTGameSettings::Defaults();
	Settings.Brightness = 1.0f;
	TestFalse(TEXT("brightness outside the retail cycle rejected"), Settings.Validate(Error));
	Settings = FWoTGameSettings::Defaults();
	Settings.MasterVolume = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("nonfinite master volume rejected"), Settings.Validate(Error));
	Settings = FWoTGameSettings::Defaults();
	Settings.FrameRateLimit = 29;
	TestFalse(TEXT("frame cap below 30 rejected"), Settings.Validate(Error));
	Settings = FWoTGameSettings::Defaults();
	Settings.Width = 639;
	TestFalse(TEXT("resolution below EarlyUI production bound rejected"), Settings.Validate(Error));
	Settings = FWoTGameSettings::Defaults();
	if (FWoTManagedKeyBinding* Jump = Settings.FindBinding(EWoTManagedBinding::Jump))
	{
		Jump->Primary = EKeys::W;
	}
	TestFalse(TEXT("conflicting managed key rejected"), Settings.Validate(Error));
	Settings = FWoTGameSettings::Defaults();
	if (FWoTManagedKeyBinding* Jump = Settings.FindBinding(EWoTManagedBinding::Jump))
	{
		Jump->Primary = FKey();
	}
	TestFalse(TEXT("primary binding cannot be empty"), Settings.Validate(Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGameSettingsCodecSpec,
	"WoT.Preferences.GameSettings.Codec", SettingsFlags)

bool FWoTGameSettingsCodecSpec::RunTest(const FString&)
{
	FWoTGameSettings Out;
	FString Error;
	const TArray<uint8> Old = Utf8("Schema=EarlyUI-v1\nSelectedFaction=Hound\n");
	TestTrue(TEXT("old file supplies settings defaults"),
		FWoTGameSettingsCodec::Parse(Old, Out, Error));
	TestTrue(TEXT("old file defaults are complete"), Out == FWoTGameSettings::Defaults());
	FWoTGameSettings Edited = FWoTGameSettings::Defaults();
	Edited.MouseSensitivity = 2.0f;
	Edited.MasterVolume = 0.35f;
	if (FWoTManagedKeyBinding* Scores = Edited.FindBinding(EWoTManagedBinding::ShowScores))
	{
		Scores->Primary = EKeys::F5;
	}
	const FString Encoded = FWoTGameSettingsCodec::Serialize(Edited);
	TestTrue(TEXT("new saves use game settings version 10"),
		Encoded.StartsWith(TEXT("GameSettingsVersion=10\n")));
	const FTCHARToUTF8 Bytes(*Encoded);
	TArray<uint8> Full = Old;
	Full.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
	TestTrue(TEXT("typed records roundtrip"),
		FWoTGameSettingsCodec::Parse(Full, Out, Error));
	TestTrue(TEXT("roundtrip preserves both binding slots and numeric fields"), Out == Edited);
	FString VersionOneText = Encoded;
	StripVersionSevenPresentationRecords(VersionOneText);
	VersionOneText.ReplaceInline(TEXT("GameSettingsVersion=10"),
		TEXT("GameSettingsVersion=1"));
	VersionOneText.ReplaceInline(TEXT("GameVSync=1"), TEXT("GameVSync=0"));
	VersionOneText.ReplaceInline(TEXT("GameMouseSensitivity=2"),
		TEXT("GameMouseSensitivity=3"));
	const FTCHARToUTF8 VersionOneUtf8(*VersionOneText);
	TArray<uint8> VersionOne = Old;
	VersionOne.Append(reinterpret_cast<const uint8*>(VersionOneUtf8.Get()),
		VersionOneUtf8.Length());
	TestTrue(TEXT("version 1 settings migrate"),
		FWoTGameSettingsCodec::Parse(VersionOne, Out, Error));
	TestTrue(TEXT("version 1 migrates the unreachable VSync setting to on"),
		Out.bVSync);
	TestEqual(TEXT("version 1 preserves unrelated settings"),
		Out.MouseSensitivity, 3.0f);
	// Version 2 wrote the old 23-action catalog.  New actions must be filled
	// from their retail defaults without discarding any saved legacy binding.
	FString VersionTwoText = Encoded;
	StripVersionSevenPresentationRecords(VersionTwoText);
	VersionTwoText.ReplaceInline(TEXT("GameSettingsVersion=10"),
		TEXT("GameSettingsVersion=2"));
	// Authentic v2 records predate the newly introduced secondary aliases.
	VersionTwoText.ReplaceInline(TEXT("GameBinding.MoveForward=W,Up"),
		TEXT("GameBinding.MoveForward=W,-"));
	VersionTwoText.ReplaceInline(TEXT("GameBinding.MoveBackward=S,Down"),
		TEXT("GameBinding.MoveBackward=S,F8"));
	VersionTwoText.ReplaceInline(TEXT("GameBinding.MoveRight=D,Right"),
		TEXT("GameBinding.MoveRight=D,-"));
	VersionTwoText.ReplaceInline(TEXT("GameBinding.MoveLeft=A,Left"),
		TEXT("GameBinding.MoveLeft=A,-"));
	VersionTwoText.ReplaceInline(TEXT("GameBinding.SelectHand6=Six,Q"),
		TEXT("GameBinding.SelectHand6=Six,-"));
	VersionTwoText.ReplaceInline(TEXT("GameBinding.SelectHand7=Seven,E"),
		TEXT("GameBinding.SelectHand7=Seven,-"));
	VersionTwoText.ReplaceInline(TEXT("GameBinding.SelectHand8=Eight,Z"),
		TEXT("GameBinding.SelectHand8=Eight,-"));
	VersionTwoText.ReplaceInline(TEXT("GameBinding.SelectHand9=Nine,X"),
		TEXT("GameBinding.SelectHand9=Nine,-"));
	for (const TCHAR* NewIdentity : {
		TEXT("NextHand"), TEXT("PrevHand"), TEXT("TurnLeft"), TEXT("TurnRight"),
		TEXT("LookUp"), TEXT("LookDown"), TEXT("SShot"),
		TEXT("ToggleFullscreen"), TEXT("ServerEditCitadel"), TEXT("ShowHelp"),
		TEXT("Type"), TEXT("Talk"), TEXT("TeamTalk"),
		TEXT("Drop"), TEXT("DropTainted"),
		TEXT("Walking"), TEXT("Look"), TEXT("LookToggle"), TEXT("FreeLook"),
		TEXT("CenterView"), TEXT("Strafe"),
		TEXT("PlayTaunt1"), TEXT("PlayTaunt2"), TEXT("PlayTaunt3"),
		TEXT("PlayTaunt4"), TEXT("PlayTaunt5"), TEXT("PlayTaunt6"),
		TEXT("PlayTauntRandom"), TEXT("Brightness"), TEXT("Pause")})
	{
		const FString Prefix = FString::Printf(TEXT("GameBinding.%s="), NewIdentity);
		int32 Start = VersionTwoText.Find(Prefix, ESearchCase::CaseSensitive);
		if (Start != INDEX_NONE)
		{
			const int32 End = VersionTwoText.Find(TEXT("\n"), ESearchCase::CaseSensitive,
				ESearchDir::FromStart, Start);
			VersionTwoText.RemoveAt(Start, End - Start + 1);
		}
	}
	VersionTwoText.ReplaceInline(TEXT("GameBrightness=0.600000024\n"), TEXT(""));
	VersionTwoText.ReplaceInline(TEXT("GameBrightness=0.6\n"), TEXT(""));
	const FTCHARToUTF8 VersionTwoUtf8(*VersionTwoText);
	TArray<uint8> VersionTwo = Old;
	VersionTwo.Append(reinterpret_cast<const uint8*>(VersionTwoUtf8.Get()),
		VersionTwoUtf8.Length());
	TestTrue(TEXT("version 2 settings migrate with new retail defaults"),
		FWoTGameSettingsCodec::Parse(VersionTwo, Out, Error));
	TestTrue(TEXT("version 2 preserves the saved scoreboard key"),
		Out.FindBinding(EWoTManagedBinding::ShowScores)->Primary == EKeys::F5);
	TestTrue(TEXT("version 2 fills F1 help from the new default"),
		Out.FindBinding(EWoTManagedBinding::ShowHelp)->Primary == EKeys::F1);
	TestTrue(TEXT("version 2 fills the new cursor movement secondary"),
		Out.FindBinding(EWoTManagedBinding::MoveForward)->Secondary == EKeys::Up);
	TestTrue(TEXT("version 2 fills the new Q hand secondary"),
		Out.FindBinding(EWoTManagedBinding::SelectHand6)->Secondary == EKeys::Q);
	TestTrue(TEXT("version 2 preserves an existing custom secondary"),
		Out.FindBinding(EWoTManagedBinding::MoveBackward)->Secondary == EKeys::F8);
	// Version 3 wrote the complete pre-chat 33-action catalog. Chat actions must
	// migrate from retail defaults while every saved v3 choice is retained.
	FString VersionThreeText = Encoded;
	StripVersionSevenPresentationRecords(VersionThreeText);
	VersionThreeText.ReplaceInline(TEXT("GameSettingsVersion=10"),
		TEXT("GameSettingsVersion=3"));
	for (const TCHAR* ChatIdentity : {
		TEXT("Type"), TEXT("Talk"), TEXT("TeamTalk"),
		TEXT("Drop"), TEXT("DropTainted"),
		TEXT("Walking"), TEXT("Look"), TEXT("LookToggle"), TEXT("FreeLook"),
		TEXT("CenterView"), TEXT("Strafe"),
		TEXT("PlayTaunt1"), TEXT("PlayTaunt2"), TEXT("PlayTaunt3"),
		TEXT("PlayTaunt4"), TEXT("PlayTaunt5"), TEXT("PlayTaunt6"),
		TEXT("PlayTauntRandom"), TEXT("Brightness"), TEXT("Pause")})
	{
		const FString Prefix = FString::Printf(TEXT("GameBinding.%s="), ChatIdentity);
		const int32 Start = VersionThreeText.Find(Prefix, ESearchCase::CaseSensitive);
		if (Start != INDEX_NONE)
		{
			const int32 End = VersionThreeText.Find(TEXT("\n"),
				ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			VersionThreeText.RemoveAt(Start, End - Start + 1);
		}
	}
	VersionThreeText.ReplaceInline(TEXT("GameBrightness=0.600000024\n"), TEXT(""));
	VersionThreeText.ReplaceInline(TEXT("GameBrightness=0.6\n"), TEXT(""));
	const FTCHARToUTF8 VersionThreeUtf8(*VersionThreeText);
	TArray<uint8> VersionThree = Old;
	VersionThree.Append(reinterpret_cast<const uint8*>(VersionThreeUtf8.Get()),
		VersionThreeUtf8.Length());
	TestTrue(TEXT("version 3 settings migrate with retail chat defaults"),
		FWoTGameSettingsCodec::Parse(VersionThree, Out, Error));
	TestTrue(TEXT("version 3 preserves the saved scoreboard key"),
		Out.FindBinding(EWoTManagedBinding::ShowScores)->Primary == EKeys::F5);
	TestTrue(TEXT("version 3 fills the retail Talk key"),
		Out.FindBinding(EWoTManagedBinding::Talk)->Primary == EKeys::T);
	// Tab/T/Y were free in v3 and therefore could be either slot of an existing
	// action. Migration must retain every such user choice instead of rejecting
	// the record and causing the preferences owner to reset all game settings.
	for (const TPair<FKey, EWoTManagedBinding>& Collision : {
		TPair<FKey, EWoTManagedBinding>(EKeys::Tab, EWoTManagedBinding::Type),
		TPair<FKey, EWoTManagedBinding>(EKeys::T, EWoTManagedBinding::Talk),
		TPair<FKey, EWoTManagedBinding>(EKeys::Y, EWoTManagedBinding::TeamTalk)})
	{
		for (const bool bPrimaryCollision : {true, false})
		{
			FString CollisionText = VersionThreeText;
			const FString PreservedRecord = bPrimaryCollision
				? FString::Printf(TEXT("GameBinding.ShowScores=%s,MiddleMouseButton"),
					*Collision.Key.ToString())
				: FString::Printf(TEXT("GameBinding.ShowScores=F5,%s"),
					*Collision.Key.ToString());
			CollisionText.ReplaceInline(
				TEXT("GameBinding.ShowScores=F5,MiddleMouseButton"),
				*PreservedRecord);
			const FTCHARToUTF8 CollisionUtf8(*CollisionText);
			TArray<uint8> CollisionBytes = Old;
			CollisionBytes.Append(
				reinterpret_cast<const uint8*>(CollisionUtf8.Get()),
				CollisionUtf8.Length());
			const FString Slot = bPrimaryCollision ? TEXT("primary") : TEXT("secondary");
			TestTrue(FString::Printf(TEXT("v3 %s %s collision migrates"),
				*Collision.Key.ToString(), *Slot),
				FWoTGameSettingsCodec::Parse(CollisionBytes, Out, Error));
			const FWoTManagedKeyBinding* Scores =
				Out.FindBinding(EWoTManagedBinding::ShowScores);
			TestTrue(FString::Printf(TEXT("v3 %s %s assignment is preserved"),
				*Collision.Key.ToString(), *Slot), Scores
				&& (bPrimaryCollision ? Scores->Primary : Scores->Secondary)
					== Collision.Key);
			const FWoTManagedKeyBinding* NewChat =
				Out.FindBinding(Collision.Value);
			TestTrue(FString::Printf(
				TEXT("colliding new %s default stays unbound"),
				*Collision.Key.ToString()),
				NewChat && !NewChat->Primary.IsValid());
			TestEqual(TEXT("collision migration preserves numeric settings"),
				Out.MouseSensitivity, 2.0f);
		}
	}
	// Version 4 wrote the complete pre-drop 36-action catalog. The two new
	// records fill from retail defaults, but any key already owned by v4 stays
	// with its saved action and leaves the colliding new slot visibly unbound.
	FString VersionFourText = Encoded;
	StripVersionSevenPresentationRecords(VersionFourText);
	VersionFourText.ReplaceInline(TEXT("GameSettingsVersion=10"),
		TEXT("GameSettingsVersion=4"));
	for (const TCHAR* DropIdentity : {
		TEXT("Drop"), TEXT("DropTainted"),
		TEXT("Walking"), TEXT("Look"), TEXT("LookToggle"), TEXT("FreeLook"),
		TEXT("CenterView"), TEXT("Strafe"),
		TEXT("PlayTaunt1"), TEXT("PlayTaunt2"), TEXT("PlayTaunt3"),
		TEXT("PlayTaunt4"), TEXT("PlayTaunt5"), TEXT("PlayTaunt6"),
		TEXT("PlayTauntRandom"), TEXT("Brightness"), TEXT("Pause")})
	{
		const FString Prefix = FString::Printf(TEXT("GameBinding.%s="), DropIdentity);
		const int32 Start = VersionFourText.Find(Prefix, ESearchCase::CaseSensitive);
		const int32 End = VersionFourText.Find(TEXT("\n"),
			ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		VersionFourText.RemoveAt(Start, End - Start + 1);
	}
	VersionFourText.ReplaceInline(TEXT("GameBrightness=0.600000024\n"), TEXT(""));
	VersionFourText.ReplaceInline(TEXT("GameBrightness=0.6\n"), TEXT(""));
	{
		const FTCHARToUTF8 VersionFourUtf8(*VersionFourText);
		TArray<uint8> VersionFour = Old;
		VersionFour.Append(reinterpret_cast<const uint8*>(VersionFourUtf8.Get()),
			VersionFourUtf8.Length());
		TestTrue(TEXT("version 4 fills retail drop defaults"),
			FWoTGameSettingsCodec::Parse(VersionFour, Out, Error));
		const FWoTManagedKeyBinding* MigratedDrop =
			Out.FindBinding(EWoTManagedBinding::Drop);
		TestTrue(TEXT("v4 migration fills Ctrl/Backslash Drop"), MigratedDrop
			&& MigratedDrop->Primary == EKeys::LeftControl
			&& MigratedDrop->Secondary == EKeys::Backslash);
		TestTrue(TEXT("v4 migration fills Alt DropTainted"),
			Out.FindBinding(EWoTManagedBinding::DropTainted)->Primary == EKeys::LeftAlt);
	}
	for (const FKey& CollisionKey : {
		EKeys::LeftControl, EKeys::Backslash, EKeys::LeftAlt})
	{
		FString CollisionText = VersionFourText;
		CollisionText.ReplaceInline(
			TEXT("GameBinding.ShowScores=F5,MiddleMouseButton"),
			*FString::Printf(TEXT("GameBinding.ShowScores=%s,MiddleMouseButton"),
				*CollisionKey.ToString()));
		const FTCHARToUTF8 CollisionUtf8(*CollisionText);
		TArray<uint8> CollisionBytes = Old;
		CollisionBytes.Append(reinterpret_cast<const uint8*>(CollisionUtf8.Get()),
			CollisionUtf8.Length());
		TestTrue(FString::Printf(TEXT("v4 %s collision migrates"),
			*CollisionKey.ToString()),
			FWoTGameSettingsCodec::Parse(CollisionBytes, Out, Error));
		TestTrue(TEXT("old binding retains a colliding drop key"),
			Out.FindBinding(EWoTManagedBinding::ShowScores)->Primary == CollisionKey);
		const FWoTManagedKeyBinding* Migrated = Out.FindBinding(
			CollisionKey == EKeys::LeftAlt
				? EWoTManagedBinding::DropTainted : EWoTManagedBinding::Drop);
		TestTrue(TEXT("colliding new drop slot remains visibly unbound"), Migrated
			&& (CollisionKey == EKeys::Backslash
				? !Migrated->Secondary.IsValid() : !Migrated->Primary.IsValid()));
	}
	// Version 5 wrote the complete pre-input-parity 38-action catalog and no
	// brightness field. The new records use retail defaults unless an old action
	// already owns one of those keys.
	FString VersionFiveText = Encoded;
	StripVersionSevenPresentationRecords(VersionFiveText);
	VersionFiveText.ReplaceInline(TEXT("GameSettingsVersion=10"),
		TEXT("GameSettingsVersion=5"));
	for (const TCHAR* NewIdentity : {
		TEXT("Walking"), TEXT("PlayTaunt1"), TEXT("PlayTaunt2"), TEXT("PlayTaunt3"),
		TEXT("PlayTaunt4"), TEXT("PlayTaunt5"), TEXT("PlayTaunt6"), TEXT("PlayTauntRandom"),
		TEXT("Look"), TEXT("LookToggle"), TEXT("FreeLook"), TEXT("CenterView"),
		TEXT("Strafe"), TEXT("Brightness"), TEXT("Pause")})
	{
		const FString Prefix = FString::Printf(TEXT("GameBinding.%s="), NewIdentity);
		const int32 Start = VersionFiveText.Find(Prefix, ESearchCase::CaseSensitive);
		const int32 End = VersionFiveText.Find(TEXT("\n"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Start);
		VersionFiveText.RemoveAt(Start, End - Start + 1);
	}
	{
		const int32 Start = VersionFiveText.Find(
			TEXT("GameBrightness="), ESearchCase::CaseSensitive);
		const int32 End = VersionFiveText.Find(TEXT("\n"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Start);
		VersionFiveText.RemoveAt(Start, End - Start + 1);
	}
	{
		const FTCHARToUTF8 VersionFiveUtf8(*VersionFiveText);
		TArray<uint8> VersionFive = Old;
		VersionFive.Append(reinterpret_cast<const uint8*>(VersionFiveUtf8.Get()),
			VersionFiveUtf8.Length());
		TestTrue(TEXT("version 5 fills input-parity defaults"),
			FWoTGameSettingsCodec::Parse(VersionFive, Out, Error));
		TestEqual(TEXT("version 5 fills brightness 0.6"), Out.Brightness, 0.6f);
		TestTrue(TEXT("version 5 fills Walking"),
			Out.FindBinding(EWoTManagedBinding::Walking)->Primary == EKeys::LeftShift);
		TestTrue(TEXT("version 5 fills random taunt"),
			Out.FindBinding(EWoTManagedBinding::PlayTauntRandom)->Primary == EKeys::R);
	}
	{
		FString CollisionText = VersionFiveText;
		CollisionText.ReplaceInline(
			TEXT("GameBinding.ShowScores=F5,MiddleMouseButton"),
			TEXT("GameBinding.ShowScores=G,MiddleMouseButton"));
		const FTCHARToUTF8 CollisionUtf8(*CollisionText);
		TArray<uint8> CollisionBytes = Old;
		CollisionBytes.Append(reinterpret_cast<const uint8*>(CollisionUtf8.Get()),
			CollisionUtf8.Length());
		TestTrue(TEXT("version 5 key collision migrates"),
			FWoTGameSettingsCodec::Parse(CollisionBytes, Out, Error));
		TestTrue(TEXT("version 5 preserves the old G assignment"),
			Out.FindBinding(EWoTManagedBinding::ShowScores)->Primary == EKeys::G);
		TestFalse(TEXT("colliding Taunt1 default remains visible and unbound"),
			Out.FindBinding(EWoTManagedBinding::PlayTaunt1)->Primary.IsValid());
	}
	FString VersionSixText = Encoded;
	StripVersionSevenPresentationRecords(VersionSixText);
	VersionSixText.ReplaceInline(TEXT("GameSettingsVersion=10"),
		TEXT("GameSettingsVersion=6"));
	{
		const FTCHARToUTF8 VersionSixUtf8(*VersionSixText);
		TArray<uint8> VersionSix = Old;
		VersionSix.Append(reinterpret_cast<const uint8*>(VersionSixUtf8.Get()),
			VersionSixUtf8.Length());
		TestTrue(TEXT("version 6 fills presentation preference defaults"),
			FWoTGameSettingsCodec::Parse(VersionSix, Out, Error));
		TestEqual(TEXT("version 6 fills retail Bob"), Out.ViewBob, 0.016f);
		TestEqual(TEXT("version 6 fills retail crosshair"), Out.Crosshair, uint8(13));
		TestEqual(TEXT("version 6 fills retail gore detail"),
			Out.GoreDetailLevel, uint8(2));
		TestFalse(TEXT("version 6 fills disabled look spring"), Out.bLookSpring);
		TestTrue(TEXT("version 6 migrates the joystick on"), Out.bJoystickEnabled);
	}
	FString MalformedLegacy = VersionTwoText;
	MalformedLegacy.ReplaceInline(TEXT("GameBinding.NextItem=MouseScrollDown,-"),
		TEXT("GameBinding.ShowHelp=H,-"));
	const FTCHARToUTF8 MalformedLegacyUtf8(*MalformedLegacy);
	TArray<uint8> MalformedLegacyBytes = Old;
	MalformedLegacyBytes.Append(
		reinterpret_cast<const uint8*>(MalformedLegacyUtf8.Get()),
		MalformedLegacyUtf8.Length());
	TestFalse(TEXT("legacy migration rejects a new identity substituted for an old one"),
		FWoTGameSettingsCodec::Parse(MalformedLegacyBytes, Out, Error));
	Out = Edited;
	TArray<uint8> Duplicate = Full;
	const TArray<uint8> DuplicateTail = Utf8("GameMouseSensitivity=1\n");
	Duplicate.Append(DuplicateTail);
	TestFalse(TEXT("duplicate setting rejected"),
		FWoTGameSettingsCodec::Parse(Duplicate, Out, Error));
	FString NonfiniteText = Encoded;
	NonfiniteText.ReplaceInline(TEXT("GameMouseSensitivity=2"), TEXT("GameMouseSensitivity=nan"));
	FTCHARToUTF8 NonfiniteUtf8(*NonfiniteText);
	TArray<uint8> Nonfinite = Old;
	Nonfinite.Append(reinterpret_cast<const uint8*>(NonfiniteUtf8.Get()), NonfiniteUtf8.Length());
	TestFalse(TEXT("nonfinite setting rejected"),
		FWoTGameSettingsCodec::Parse(Nonfinite, Out, Error));
	TArray<uint8> Unknown = Full;
	const TArray<uint8> UnknownTail = Utf8("GameBinding.FutureAction=F1,-\n");
	Unknown.Append(UnknownTail);
	TestFalse(TEXT("unknown binding identity rejected"),
		FWoTGameSettingsCodec::Parse(Unknown, Out, Error));
	TestTrue(TEXT("failed parse leaves previous output untouched"), Out == Edited);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGameSettingsOwnerSpec,
	"WoT.Preferences.GameSettings.Owner", SettingsFlags)

bool FWoTGameSettingsOwnerSpec::RunTest(const FString&)
{
	FSettingsFileFixture File;
	UWoTGameInstance* Instance = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Owner = NewObject<UWoTPreferencesSubsystem>(Instance);
	TestFalse(TEXT("adopt authenticated EarlyUI"), AdoptDefaults(*Owner).IsError());
	FWoTGameSettings Draft = FWoTGameSettings::Defaults();
	Draft.MouseSensitivity = 2.0f;
	TestTrue(TEXT("save dormant before activation"), Owner->SaveGameSettings(Draft).IsError());
	TestTrue(TEXT("dormant save preserves defaults"),
		Owner->GetGameSettings() == FWoTGameSettings::Defaults());
	UPlayerInput* Input = NewObject<UPlayerInput>();
	const TArray<FInputActionKeyMapping> BeforeDormantMappings = Input->ActionMappings;
	TestTrue(TEXT("input application dormant before activation"),
		Owner->ApplyGameSettingsToInput(*Input).IsError());
	TestTrue(TEXT("dormant input application has no mapping effects"),
		Input->ActionMappings == BeforeDormantMappings);
	const TArray<uint8> OldFile = Utf8("Schema=EarlyUI-v1\nSelectedFaction=Hound\n");
	TestTrue(TEXT("write old preference format fixture"),
		FFileHelper::SaveArrayToFile(OldFile, *File.Leaf));
	TestFalse(TEXT("publish bounded test activation"),
		PublishWoTPayloadBootstrapActivationForTesting().IsError());
	TestFalse(TEXT("load old or missing settings"),
		Owner->ApplyFullPreferencesAfterCacheActivation().IsError());
	TestTrue(TEXT("old file receives complete game defaults"),
		Owner->GetGameSettings() == FWoTGameSettings::Defaults());
	TestEqual(TEXT("old faction record survives migration"),
		static_cast<int32>(Owner->GetPreferences().SelectedFaction),
		static_cast<int32>(EWoTPlayerFaction::Hound));
	TestFalse(TEXT("persist faction through owner"),
		Owner->SetSelectedFaction(EWoTPlayerFaction::Hound, true).IsError());
	TestFalse(TEXT("save settings through same owner"), Owner->SaveGameSettings(Draft).IsError());
	TestTrue(TEXT("settings become current after durable save"), Owner->GetGameSettings() == Draft);
	TArray<uint8> Bytes;
	TestTrue(TEXT("single preference file exists"), FFileHelper::LoadFileToArray(Bytes, *File.Leaf));
	const auto Early = ParseEarlyUIV1(Bytes, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
	TestFalse(TEXT("authenticated EarlyUI survives save"), Early.IsError());
	if (!Early.IsError())
	{
		TestTrue(TEXT("EarlyUI is immutable"), Early.GetValue().Get() == Owner->GetPreferences().EarlyUI);
	}
	const FUTF8ToTCHAR FileText(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	TestTrue(TEXT("faction record survives settings save"),
		FString(FileText.Length(), FileText.Get()).Contains(TEXT("SelectedFaction=Hound")));
	FWoTGameSettings Reloaded;
	FString Error;
	TestTrue(TEXT("persisted settings parse"), FWoTGameSettingsCodec::Parse(Bytes, Reloaded, Error));
	TestTrue(TEXT("persisted values roundtrip"), Reloaded == Draft);
	const FWoTGameSettings BeforeFailure = Owner->GetGameSettings();
	const FString Blocker = FPaths::Combine(File.Directory, TEXT("blocker"));
	TestTrue(TEXT("create deterministic storage blocker"), FFileHelper::SaveStringToFile(TEXT("x"), *Blocker));
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(FPaths::Combine(Blocker, TEXT("settings")));
	Draft.MasterVolume = 0.4f;
	TestTrue(TEXT("durability error reported"), Owner->SaveGameSettings(Draft).IsError());
	TestTrue(TEXT("failed save keeps committed settings"), Owner->GetGameSettings() == BeforeFailure);
	TestEqual(TEXT("failed save does not edit caller draft"), Draft.MasterVolume, 0.4f);
	UWoTPreferencesSubsystem::SetPreferencePathForTesting(File.Leaf);
	const TArray<uint8> BadTail = Utf8("GameMouseSensitivity=3\n");
	Bytes.Append(BadTail);
	TestTrue(TEXT("write malformed saved record fixture"), FFileHelper::SaveArrayToFile(Bytes, *File.Leaf));
	UWoTPreferencesSubsystem* ReloadOwner = NewObject<UWoTPreferencesSubsystem>(Instance);
	TestFalse(TEXT("adopt second immutable bootstrap"), AdoptDefaults(*ReloadOwner).IsError());
	TestFalse(TEXT("malformed saved settings fall back without fatal startup"),
		ReloadOwner->ApplyFullPreferencesAfterCacheActivation().IsError());
	TestTrue(TEXT("malformed settings load atomically as defaults"),
		ReloadOwner->GetGameSettings() == FWoTGameSettings::Defaults());
	TestEqual(TEXT("independent faction remains preserved"),
		static_cast<int32>(ReloadOwner->GetPreferences().SelectedFaction),
		static_cast<int32>(EWoTPlayerFaction::Hound));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGameSettingsInputSpec,
	"WoT.Preferences.GameSettings.Input", SettingsFlags)

bool FWoTGameSettingsInputSpec::RunTest(const FString&)
{
	FSettingsFileFixture File;
	UWoTGameInstance* Instance = NewObject<UWoTGameInstance>();
	UWoTPreferencesSubsystem* Owner = NewObject<UWoTPreferencesSubsystem>(Instance);
	TestFalse(TEXT("adopt EarlyUI"), AdoptDefaults(*Owner).IsError());
	TestFalse(TEXT("activate"),
		PublishWoTPayloadBootstrapActivationForTesting().IsError());
	TestFalse(TEXT("load"), Owner->ApplyFullPreferencesAfterCacheActivation().IsError());
	UPlayerInput* DefaultInput = NewObject<UPlayerInput>();
	TestFalse(TEXT("apply original default sensitivity"),
		Owner->ApplyGameSettingsToInput(*DefaultInput).IsError());
	TestTrue(TEXT("1x sensitivity preserves shipped 0.07"),
		FMath::IsNearlyEqual(DefaultInput->GetMouseSensitivityX(), 0.07f, 0.0001f));
	FWoTGameSettings Settings = FWoTGameSettings::Defaults();
	Settings.MouseSensitivity = 2.0f;
	Settings.bInvertMouseY = true;
	if (FWoTManagedKeyBinding* Scores = Settings.FindBinding(EWoTManagedBinding::ShowScores))
	{
		Scores->Primary = EKeys::F5;
	}
	TestFalse(TEXT("save bindings"), Owner->SaveGameSettings(Settings).IsError());
	UPlayerInput* Input = NewObject<UPlayerInput>();
	TestFalse(TEXT("apply to local input"), Owner->ApplyGameSettingsToInput(*Input).IsError());
	TestFalse(TEXT("repeat application"), Owner->ApplyGameSettingsToInput(*Input).IsError());
	int32 PrimaryCount = 0;
	int32 SecondaryCount = 0;
	for (const FInputActionKeyMapping& Mapping : Input->ActionMappings)
	{
		if (Mapping.ActionName == TEXT("ShowScores") && Mapping.Key == EKeys::F5) { ++PrimaryCount; }
		if (Mapping.ActionName == TEXT("ShowScores") && Mapping.Key == EKeys::MiddleMouseButton) { ++SecondaryCount; }
	}
	TestEqual(TEXT("edited primary applied once"), PrimaryCount, 1);
	TestEqual(TEXT("existing secondary retained once"), SecondaryCount, 1);
	TestTrue(TEXT("mouse Y inverted"), Input->GetInvertAxisKey(EKeys::MouseY));
	TestTrue(TEXT("2x sensitivity scales shipped 0.07"),
		FMath::IsNearlyEqual(Input->GetMouseSensitivityX(), 0.14f, 0.0001f));
	UPlayerInput* LaterInput = NewObject<UPlayerInput>();
	TestFalse(TEXT("new local input after activation accepts saved settings"),
		Owner->ApplyGameSettingsToInput(*LaterInput).IsError());
	TestTrue(TEXT("new input has saved mouse sensitivity"),
		FMath::IsNearlyEqual(LaterInput->GetMouseSensitivityX(), 0.14f, 0.0001f));
	TestTrue(TEXT("new input has saved secondary binding"),
		LaterInput->ActionMappings.ContainsByPredicate([](const FInputActionKeyMapping& Mapping)
		{
			return Mapping.ActionName == TEXT("ShowScores")
				&& Mapping.Key == EKeys::MiddleMouseButton;
		}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGameSettingsProjectionSpec,
	"WoT.Preferences.GameSettings.Projection", SettingsFlags)

bool FWoTGameSettingsProjectionSpec::RunTest(const FString&)
{
	const float PreviousVolume = FApp::GetVolumeMultiplier();
	const float PreviousMaster = FWoTGameSettingsAdapter::GetAppliedMasterVolume();
	struct FProjection
	{
		FWoTGameSettings Settings;
		bool bResolution = false;
		bool bFrameRate = false;
	};
	TArray<FProjection> Projected;
	FWoTGameSettingsAdapter::SetDisplayProjectionForTesting(
		[&Projected](const FWoTGameSettings& Settings,
			bool bResolution, bool bFrameRate)
		{
			Projected.Add({Settings, bResolution, bFrameRate});
		});
	ON_SCOPE_EXIT
	{
		FWoTGameSettingsAdapter::SetDisplayProjectionForTesting({});
		FApp::SetVolumeMultiplier(PreviousVolume);
		FWoTGameSettings Restore = FWoTGameSettings::Defaults();
		Restore.MasterVolume = PreviousMaster;
		FWoTGameSettingsAdapter::ApplyAudio(Restore);
	};
	const FWoTGameSettings Confirmed = FWoTGameSettings::Defaults();
	FWoTGameSettings Draft = Confirmed;
	Draft.Width = 1920;
	Draft.Height = 1080;
	Draft.WindowMode = EWoTWindowMode::WindowedFullscreen;
	Draft.bVSync = true;
	Draft.GraphicsQuality = EWoTGraphicsQuality::Low;
	Draft.FrameRateLimit = 60;
	Draft.MasterVolume = 0.35f;
	FString Error;
	TestTrue(TEXT("bounded video preview projects typed draft"),
		FWoTGameSettingsAdapter::PreviewVideo(Draft, Error));
	TestEqual(TEXT("preview projects once"), Projected.Num(), 1);
	if (Projected.Num() == 1)
	{
		TestTrue(TEXT("preview includes mode, quality, vsync and cap"),
			Projected[0].Settings == Draft
			&& Projected[0].bResolution && Projected[0].bFrameRate);
	}
	FWoTGameSettingsAdapter::RevertVideo(Confirmed);
	TestEqual(TEXT("rollback projects confirmed state"), Projected.Num(), 2);
	if (Projected.Num() == 2)
	{
		TestTrue(TEXT("rollback restores all display fields"),
			Projected[1].Settings == Confirmed
			&& Projected[1].bResolution && Projected[1].bFrameRate);
	}
	FWoTGameSettingsAdapter::ApplyAudio(Draft);
	TestTrue(TEXT("master volume reaches the audio devices' main submix"),
		FMath::IsNearlyEqual(FWoTGameSettingsAdapter::GetAppliedMasterVolume(), 0.35f));
	Draft.Width = 1;
	TestFalse(TEXT("invalid video preview rejected"),
		FWoTGameSettingsAdapter::PreviewVideo(Draft, Error));
	TestEqual(TEXT("invalid preview has no projection"), Projected.Num(), 2);
	Draft.Width = 1920;
	FWoTGameSettingsAdapter::ApplyVideoOnStartup(Draft);
	TestEqual(TEXT("startup applies saved video without overrides"), Projected.Num(), 3);
	if (Projected.Num() == 3)
	{
		TestTrue(TEXT("ordinary startup projects resolution and saved frame cap"),
			Projected[2].bResolution && Projected[2].bFrameRate);
	}
	{
		const FString LiveCommandLine = FCommandLine::Get();
		ON_SCOPE_EXIT { FCommandLine::Set(*LiveCommandLine); };
		FCommandLine::Set(*(LiveCommandLine
			+ TEXT(" -windowed -ForceRes -ResX=1280 -ResY=720")));
		TestTrue(TEXT("review display overrides detected"),
			FWoTGameSettingsAdapter::HasReviewDisplayOverrides());
		FWoTGameSettingsAdapter::ApplyVideoOnStartup(Draft);
		TestEqual(TEXT("review overrides still project saved VSync and quality"),
			Projected.Num(), 4);
		if (Projected.Num() == 4)
		{
			TestTrue(TEXT("override startup projection retains VSync and quality"),
				Projected[3].Settings.bVSync
				&& Projected[3].Settings.GraphicsQuality == EWoTGraphicsQuality::Low);
			TestFalse(TEXT("review launcher owns resolution and window mode"),
				Projected[3].bResolution);
			TestTrue(TEXT("saved frame cap still applies without t.MaxFPS"),
				Projected[3].bFrameRate);
		}
		FCommandLine::Set(*(LiveCommandLine
			+ TEXT(" -windowed -ForceRes -ResX=1280 -ResY=720")
			+ TEXT(" -ExecCmds=\"t.MaxFPS 60\"")));
		FWoTGameSettingsAdapter::ApplyVideoOnStartup(Draft);
		TestEqual(TEXT("t.MaxFPS launch still projects saved VSync and quality"),
			Projected.Num(), 5);
		if (Projected.Num() == 5)
		{
			TestFalse(TEXT("t.MaxFPS command owns the startup frame cap"),
				Projected[4].bFrameRate);
		}
		FCommandLine::Set(*(LiveCommandLine
			+ TEXT(" -ExecCmds=\"SomeCommand t.MaxFPS 60\"")));
		TestFalse(TEXT("embedded t.MaxFPS text is not an executable command"),
			FWoTGameSettingsAdapter::
				HasCommandLineFrameRateOverrideForTesting(true));
		FCommandLine::Set(*(LiveCommandLine
			+ TEXT(" -ExecCmds=\"t.MaxFPS nope\"")));
		TestFalse(TEXT("t.MaxFPS requires a valid numeric argument"),
			FWoTGameSettingsAdapter::
				HasCommandLineFrameRateOverrideForTesting(true));
		FCommandLine::Set(*(LiveCommandLine
			+ TEXT(" -ExecCmds=\"stat fps,t.MaxFPS 75\"")));
		TestTrue(TEXT("parsed ExecCmds recognizes an exact t.MaxFPS command"),
			FWoTGameSettingsAdapter::
				HasCommandLineFrameRateOverrideForTesting(true));
		TestFalse(TEXT("shipping-style builds ignore non-executable ExecCmds"),
			FWoTGameSettingsAdapter::
				HasCommandLineFrameRateOverrideForTesting(false));
		TestTrue(TEXT("preview still projects under overrides"),
			FWoTGameSettingsAdapter::PreviewVideo(Draft, Error));
		TestEqual(TEXT("explicit preview projected"), Projected.Num(), 6);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP1CSettingsOptionsCodecRuntimeSpec,
	"WoT.Parity.P1C.SettingsOptions.CodecRuntime", SettingsFlags)

bool FWoTP1CSettingsOptionsCodecRuntimeSpec::RunTest(const FString&)
{
	FWoTGameSettings Settings = FWoTGameSettings::Defaults();
	TestFalse(TEXT("Auto Slope Look keeps the retail disabled default"),
		Settings.bAutoSlopeLook);
	TestTrue(TEXT("Player ID keeps the retail enabled default"),
		Settings.bPlayerIDEnabled);
	Settings.bAutoSlopeLook = true;
	Settings.bAlwaysMouseLook = false;
	Settings.bPlayerIDEnabled = false;
	const FString Encoded = FWoTGameSettingsCodec::Serialize(Settings);
	TestTrue(TEXT("the current codec persists Auto Slope Look"),
		Encoded.Contains(TEXT("GameAutoSlopeLook=1\n")));
	TestTrue(TEXT("the current codec persists Player ID"),
		Encoded.Contains(TEXT("GamePlayerID=0\n")));
	FWoTGameSettings Decoded;
	FString Error;
	const FTCHARToUTF8 EncodedUtf8(*Encoded);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(EncodedUtf8.Get()),
		EncodedUtf8.Length());
	TestTrue(TEXT("the current preference record parses"),
		FWoTGameSettingsCodec::Parse(Bytes, Decoded, Error));
	TestTrue(TEXT("the two new preferences roundtrip"),
		Decoded.bAutoSlopeLook && !Decoded.bPlayerIDEnabled);
	FWoTGameSettings Invalid = FWoTGameSettings::Defaults();
	Invalid.bAutoSlopeLook = true;
	Invalid.bAlwaysMouseLook = true;
	TestFalse(TEXT("Auto Slope Look and Always Mouse Look are mutually exclusive"),
		Invalid.Validate(Error));

	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	ON_SCOPE_EXIT { UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false); };
	FWoTEarlyUIAuthority& EarlyUIAuthority = GetEarlyUIAuthority();
	EarlyUIAuthority.ResetForTesting();
	ON_SCOPE_EXIT { EarlyUIAuthority.ResetForTesting(); };
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> EarlyUI = ParseEarlyUIV1(
		Utf8("Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"),
		FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
	if (!TestFalse(TEXT("runtime fixture EarlyUI parses"), EarlyUI.IsError())
		|| !TestFalse(TEXT("runtime fixture EarlyUI publishes"),
			EarlyUIAuthority.PublishOnce(MoveTemp(EarlyUI).GetValue()).IsError()))
	{
		return false;
	}
	const auto ApplyEarlyUI = [](uint16, uint16, EWoTWindowMode)
		-> FWoTBootstrapStatus { return FWoTBootstrapStatus::Success(); };
	const FWoTEarlyUINativeProjectionSink ProjectionSink{ApplyEarlyUI};
	FString ProjectionError;
	if (!TestTrue(TEXT("runtime fixture applies native EarlyUI projection"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			ProjectionSink, &ProjectionError)))
	{
		AddError(ProjectionError);
		return false;
	}

	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	UWoTPreferencesSubsystem* Preferences = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("runtime preferences"), Preferences)) return false;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
		FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	AWoTPlayerController* TargetController =
		World->SpawnActor<AWoTPlayerController>();
	AWoTCharacter* Target = World->SpawnActor<AWoTCharacter>(
		FVector(400, 0, 100), FRotator::ZeroRotator, Spawn);
	if (!Controller || !Pawn || !TargetController || !Target) return false;
	Controller->SetAsLocalPlayerController();
	Controller->InitPlayerState();
	Controller->Possess(Pawn);
	Controller->ClientSetHUD(AWoTHUD::StaticClass());
	TargetController->InitPlayerState();
	TargetController->Possess(Target);
	TargetController->PlayerState->SetPlayerName(TEXT("Aviendha"));
	Controller->SetControlRotation(FRotator::ZeroRotator);
	AWoTHUD* HUD = Cast<AWoTHUD>(Controller->GetHUD());
	if (!TestNotNull(TEXT("production HUD"), HUD)) return false;
	Preferences->SetGameSettingsForTesting(Settings);
	HUD->UpdatePlayerID(nullptr, 0.0f);
	TestTrue(TEXT("disabled Player ID suppresses and clears the aimed-at label"),
		HUD->IdentifyTarget.IsEmpty() && HUD->IdentifyFadeTime == 0.0f);
	Settings.bPlayerIDEnabled = true;
	Preferences->SetGameSettingsForTesting(Settings);
	HUD->UpdatePlayerID(nullptr, 0.0f);
	TestEqual(TEXT("enabled Player ID restores the real aimed-at player trace"),
		HUD->IdentifyTarget, FString(TEXT("Aviendha")));

	Settings.bPlayerIDEnabled = false;
	Preferences->SetGameSettingsForTesting(Settings);
	Target->Destroy();
	P1C_SpawnStairSlab(World, FVector(108, 0, 54), FVector(52, 90, 50));
	P1C_SpawnStairSlab(World, FVector(220, 0, 64), FVector(58, 90, 60));
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Controller->SetControlRotation(FRotator::ZeroRotator);
	Pawn->Tick(0.05f);
	TestTrue(TEXT("walking toward rising steps applies local slope pitch"),
		Controller->GetControlRotation().Pitch > 0.1f);
	Controller->SetControlRotation(FRotator::ZeroRotator);
	Pawn->AddPitchInputForTest(1.0f);
	Pawn->Tick(0.05f);
	TestEqual(TEXT("explicit look input suppresses slope pitch for its frame"),
		static_cast<float>(Controller->GetControlRotation().Pitch), 0.0f, 0.01f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGameSettingsEffectsSpec,
	"WoT.Preferences.GameSettings.Effects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWoTGameSettingsEffectsSpec::RunTest(const FString&)
{
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	ON_SCOPE_EXIT { UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false); };
	FWoTEarlyUIAuthority& EarlyUIAuthority = GetEarlyUIAuthority();
	EarlyUIAuthority.ResetForTesting();
	ON_SCOPE_EXIT { EarlyUIAuthority.ResetForTesting(); };
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> EarlyUI = ParseEarlyUIV1(
		Utf8("Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"),
		FWoTEarlyUILimits::Production(), FWoTInventoryLocaleSet::Production());
	if (!TestFalse(TEXT("effect fixture EarlyUI parses"), EarlyUI.IsError())
		|| !TestFalse(TEXT("effect fixture EarlyUI publishes"),
			EarlyUIAuthority.PublishOnce(MoveTemp(EarlyUI).GetValue()).IsError()))
	{
		return false;
	}
	const auto ApplyEarlyUI = [](uint16, uint16, EWoTWindowMode)
		-> FWoTBootstrapStatus
	{
		return FWoTBootstrapStatus::Success();
	};
	const FWoTEarlyUINativeProjectionSink ProjectionSink{ApplyEarlyUI};
	FString ProjectionError;
	if (!TestTrue(TEXT("effect fixture applies native EarlyUI projection"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			ProjectionSink, &ProjectionError)))
	{
		AddError(ProjectionError);
		return false;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	ON_SCOPE_EXIT { WoTTest::DestroySpecWorld(World); };
	UWoTPreferencesSubsystem* Preferences = World && World->GetGameInstance()
		? World->GetGameInstance()->GetSubsystem<UWoTPreferencesSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("preferences subsystem"), Preferences))
	{
		return false;
	}

	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWoTPlayerController* Controller = World->SpawnActor<AWoTPlayerController>(
		FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
	AWoTCharacter* Pawn = World->SpawnActor<AWoTCharacter>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	Controller->SetAsLocalPlayerController();
	Controller->Possess(Pawn);
	Controller->ClientSetHUD(AWoTHUD::StaticClass());
	Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	Pawn->GetCharacterMovement()->Velocity = FVector(300, 0, 0);

	FWoTGameSettings Settings = FWoTGameSettings::Defaults();
	Settings.ViewBob = 0.0f;
	Settings.Crosshair = 4;
	Settings.bJoystickEnabled = false;
	Preferences->SetGameSettingsForTesting(Settings);
	Controller->TickRetailCameraForTest(0.05f);
	TestTrue(TEXT("zero ViewBob suppresses the production camera bob"),
		Controller->RetailWalkBobForTest().IsNearlyZero());
	Settings.ViewBob = 0.032f;
	Preferences->SetGameSettingsForTesting(Settings);
	Controller->TickRetailCameraForTest(0.05f);
	TestFalse(TEXT("nonzero ViewBob drives the production camera bob"),
		Controller->RetailWalkBobForTest().IsNearlyZero());
	if (AWoTHUD* HUD = Cast<AWoTHUD>(Controller->GetHUD()))
	{
		TestEqual(TEXT("Crosshair setting reaches the live HUD"), HUD->Crosshair, 4);
	}
	else
	{
		AddError(TEXT("production HUD was not created"));
	}
	TestFalse(TEXT("disabled joystick is rejected by the production input gate"),
		Controller->IsJoystickInputEnabledForTest());
	Settings.bJoystickEnabled = true;
	Preferences->SetGameSettingsForTesting(Settings);
	TestTrue(TEXT("enabled joystick passes the production input gate"),
		Controller->IsJoystickInputEnabledForTest());

	Settings.bLookSpring = true;
	Settings.bAlwaysMouseLook = true;
	Preferences->SetGameSettingsForTesting(Settings);
	Controller->SetControlRotation(FRotator(20, 0, 0));
	Pawn->Tick(0.05f);
	TestEqual(TEXT("AlwaysMouseLook suppresses idle LookSpring"),
		static_cast<float>(Controller->GetControlRotation().Pitch), 20.0f, 0.01f);
	Settings.bAlwaysMouseLook = false;
	Preferences->SetGameSettingsForTesting(Settings);
	Pawn->Tick(0.05f);
	TestTrue(TEXT("LookSpring centers idle pitch when AlwaysMouseLook is off"),
		Controller->GetControlRotation().Pitch < 20.0f);
	Controller->SetControlRotation(FRotator(20, 0, 0));
	Pawn->AddPitchInputForTest(1.0f);
	const float ExplicitPitch = Controller->GetControlRotation().Pitch;
	Pawn->Tick(0.05f);
	TestEqual(TEXT("explicit look input suppresses LookSpring for its frame"),
		static_cast<float>(Controller->GetControlRotation().Pitch), ExplicitPitch, 0.01f);

	AWoTCharacter* Replacement = World->SpawnActor<AWoTCharacter>(
		FVector(1200, 800, 5000), FRotator::ZeroRotator, Spawn);
	Controller->Possess(Replacement);
	TestTrue(TEXT("pawn identity reset seeds the new camera location"),
		Controller->RetailPreviousPawnLocationForTest().Equals(
			Replacement->GetActorLocation()));
	TestEqual(TEXT("pawn identity reset clears the old movement mode"),
		static_cast<int32>(Controller->RetailPreviousMovementModeForTest()),
		static_cast<int32>(MOVE_None));
	const float StandingEye = Replacement->GetRetailStandingBaseEyeHeight();
	UWoTVertexAnimComponent* SwimAnim =
		Cast<UWoTVertexAnimComponent>(Replacement->GetMesh());
	if (TestNotNull(TEXT("production swimming vertex-animation channel"), SwimAnim))
	{
		FWoTAnimSequence Swim;
		Swim.Name = TEXT("Swim");
		Swim.NumFrames = 2;
		Swim.Rate = 15.0f;
		SwimAnim->Sequences.Add(Swim);
	}
	Replacement->GetCharacterMovement()->SetMovementMode(MOVE_Swimming);
	// Keep this unit world from applying dry-volume swimming physics while the
	// real vertex-animation component advances its production tween.
	Replacement->GetCharacterMovement()->SetComponentTickEnabled(false);
	TestEqual(TEXT("swim entry starts at 0.8 standing eye height"),
		Replacement->BaseEyeHeight, 0.8f * StandingEye, 0.01f);
	const float SwimTween = Replacement->GetLastSwimmingTweenSecondsForTest();
	TestEqual(TEXT("swim uses the production animation-table tween time"),
		SwimTween, 0.2f, 0.001f);
	if (SwimAnim)
	{
		WoTTest::TickWorld(World, SwimTween - 0.01f, SwimTween - 0.01f);
	}
	TestEqual(TEXT("swim eye remains 0.8 before the animation event"),
		Replacement->BaseEyeHeight, 0.8f * StandingEye, 0.01f);
	if (SwimAnim)
	{
		WoTTest::TickWorld(World, 0.01f, 0.01f);
	}
	TestEqual(TEXT("swim AnimEnd starts PlaySwimming at 0.5 eye height"),
		Replacement->BaseEyeHeight, 0.5f * StandingEye, 0.01f);
	Replacement->GetCharacterMovement()->SetComponentTickEnabled(true);
	Controller->TickRetailCameraForTest(0.1f);
	TestEqual(TEXT("camera consumes the live swimming BaseEyeHeight"),
		Controller->RetailEyeHeightForTest(), Replacement->BaseEyeHeight, 0.01f);

	const FWoTBloodConfig* Blood = WoTBloodFX::Find(TEXT("BloodDamageRed1"));
	if (TestNotNull(TEXT("authenticated blood config"), Blood))
	{
		Settings.GoreDetailLevel = Blood->GoreLevel > 0 ? Blood->GoreLevel - 1 : 0;
		Preferences->SetGameSettingsForTesting(Settings);
		AWoTParticleSprayer* LiveBlood = WoTBloodFX::SpawnLocal(*World,
			TEXT("BloodDamageRed1"), FVector::ZeroVector, FRotator::ZeroRotator);
		if (TestNotNull(TEXT("blood sprayer always exists on a renderer"), LiveBlood)
			&& Blood->GoreLevel > 0)
		{
			LiveBlood->Tick(0.0f);
			TestFalse(TEXT("lowering gore hides the same live sprayer"),
				LiveBlood->IsLocalGoreVisibleForTesting());
		}
		Settings.GoreDetailLevel = Blood->GoreLevel;
		Preferences->SetGameSettingsForTesting(Settings);
		if (LiveBlood)
		{
			LiveBlood->Tick(0.0f);
			TestTrue(TEXT("raising gore shows that same simulated sprayer"),
				LiveBlood->IsLocalGoreVisibleForTesting());
		}
	}
	AWoTCreatureChunk* Chunk = World->SpawnActor<AWoTCreatureChunk>(
		FVector(0, 0, 200), FRotator::ZeroRotator, Spawn);
	Chunk->PresentationGoreLevel = 3;
	Settings.GoreDetailLevel = 0;
	Preferences->SetGameSettingsForTesting(Settings);
	Chunk->Tick(0.0f);
	TestFalse(TEXT("chunk above the local gore level is hidden"),
		Chunk->Mesh->IsVisible());
	Settings.GoreDetailLevel = 3;
	Preferences->SetGameSettingsForTesting(Settings);
	Chunk->Tick(0.0f);
	TestTrue(TEXT("chunk at the local gore level is visible"),
		Chunk->Mesh->IsVisible());

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTGameSettingsControllerFieldsSpec,
	"WoT.Preferences.GameSettings.ControllerFields", SettingsFlags)

bool FWoTGameSettingsControllerFieldsSpec::RunTest(const FString&)
{
	const FWoTGameSettings Defaults = FWoTGameSettings::Defaults();
	TestTrue(TEXT("new installs have the joystick on"), Defaults.bJoystickEnabled);
	TestEqual(TEXT("default look sensitivity"), Defaults.ControllerLookSensitivity, 1.0f);
	TestFalse(TEXT("default invert Y"), Defaults.bControllerInvertY);
	TestEqual(TEXT("default dead zone"), Defaults.ControllerDeadZone, 0.20f);

	FWoTGameSettings Edited = Defaults;
	Edited.ControllerLookSensitivity = 2.0f;
	Edited.bControllerInvertY = true;
	Edited.ControllerDeadZone = 0.3f;
	Edited.bJoystickEnabled = false;
	const FString Encoded = FWoTGameSettingsCodec::Serialize(Edited);
	TestTrue(TEXT("controller records are written"),
		Encoded.Contains(TEXT("GameControllerLookSensitivity=2\n"))
		&& Encoded.Contains(TEXT("GameControllerInvertY=1\n"))
		&& Encoded.Contains(TEXT("GameControllerDeadZone=0.300000012\n")));
	const FTCHARToUTF8 Bytes(*Encoded);
	TArray<uint8> Full = Utf8("Schema=EarlyUI-v1\n");
	Full.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
	FWoTGameSettings Out;
	FString Error;
	TestTrue(TEXT("version 10 roundtrips"), FWoTGameSettingsCodec::Parse(Full, Out, Error));
	TestTrue(TEXT("roundtrip keeps controller fields and an explicit joystick off"), Out == Edited);

	FString VersionNine = Encoded;
	VersionNine.ReplaceInline(TEXT("GameSettingsVersion=10"), TEXT("GameSettingsVersion=9"));
	for (const TCHAR* Key : {TEXT("GameControllerLookSensitivity="),
		TEXT("GameControllerInvertY="), TEXT("GameControllerDeadZone=")})
	{
		const int32 Start = VersionNine.Find(Key, ESearchCase::CaseSensitive);
		const int32 End = VersionNine.Find(TEXT("\n"), ESearchCase::CaseSensitive,
			ESearchDir::FromStart, Start);
		VersionNine.RemoveAt(Start, End - Start + 1);
	}
	const FTCHARToUTF8 NineBytes(*VersionNine);
	TArray<uint8> Nine = Utf8("Schema=EarlyUI-v1\n");
	Nine.Append(reinterpret_cast<const uint8*>(NineBytes.Get()), NineBytes.Length());
	TestTrue(TEXT("version 9 still parses"), FWoTGameSettingsCodec::Parse(Nine, Out, Error));
	TestTrue(TEXT("version 9 migrates the joystick on once"), Out.bJoystickEnabled);
	TestEqual(TEXT("version 9 fills the default sensitivity"), Out.ControllerLookSensitivity, 1.0f);
	TestEqual(TEXT("version 9 fills the default dead zone"), Out.ControllerDeadZone, 0.20f);

	FString NineWithController = VersionNine + TEXT("GameControllerInvertY=1\n");
	const FTCHARToUTF8 BadBytes(*NineWithController);
	TArray<uint8> Bad = Utf8("Schema=EarlyUI-v1\n");
	Bad.Append(reinterpret_cast<const uint8*>(BadBytes.Get()), BadBytes.Length());
	TestFalse(TEXT("a version 9 record set cannot carry controller records"),
		FWoTGameSettingsCodec::Parse(Bad, Out, Error));

	FWoTGameSettings Range = Defaults;
	Range.ControllerDeadZone = 0.4f;
	TestFalse(TEXT("a dead zone above 0.35 is refused"), Range.Validate(Error));
	Range.ControllerDeadZone = 0.05f;
	TestTrue(TEXT("a dead zone of 0.05 is accepted"), Range.Validate(Error));
	Range.ControllerLookSensitivity = NAN;
	TestFalse(TEXT("a non-finite sensitivity is refused"), Range.Validate(Error));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
