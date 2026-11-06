#include "WoTGameSettings.h"
#include "WoTDisplayRampSubsystem.h"

#include "AudioDevice.h"
#include "Sound/SoundSubmix.h"
#include "AudioDeviceManager.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ParseExecCommands.h"
#include "Scalability.h"

namespace
{
struct FBindingDefinition
{
	EWoTManagedBinding Id;
	const TCHAR* Name;
	FKey Primary;
	FKey Secondary;
	const TCHAR* AxisName;
	float AxisScale;
};

const FBindingDefinition BindingDefinitions[] = {
	{EWoTManagedBinding::MoveForward, TEXT("MoveForward"), EKeys::W, EKeys::Up, TEXT("MoveForward"), 1},
	{EWoTManagedBinding::MoveBackward, TEXT("MoveBackward"), EKeys::S, EKeys::Down, TEXT("MoveForward"), -1},
	{EWoTManagedBinding::MoveRight, TEXT("MoveRight"), EKeys::D, EKeys::Right, TEXT("MoveRight"), 1},
	{EWoTManagedBinding::MoveLeft, TEXT("MoveLeft"), EKeys::A, EKeys::Left, TEXT("MoveRight"), -1},
	{EWoTManagedBinding::TurnLeft, TEXT("TurnLeft"), EKeys::Comma, FKey(), TEXT("Turn"), -1},
	{EWoTManagedBinding::TurnRight, TEXT("TurnRight"), EKeys::Period, FKey(), TEXT("Turn"), 1},
	{EWoTManagedBinding::LookUp, TEXT("LookUp"), EKeys::Delete, FKey(), TEXT("LookUp"), 1},
	{EWoTManagedBinding::LookDown, TEXT("LookDown"), EKeys::PageDown, FKey(), TEXT("LookUp"), -1},
	{EWoTManagedBinding::Jump, TEXT("Jump"), EKeys::SpaceBar, FKey(), nullptr, 0},
	{EWoTManagedBinding::Fire, TEXT("Fire"), EKeys::LeftMouseButton, FKey(), nullptr, 0},
	{EWoTManagedBinding::NextItem, TEXT("NextItem"), EKeys::MouseScrollDown, FKey(), nullptr, 0},
	{EWoTManagedBinding::PrevItem, TEXT("PrevItem"), EKeys::MouseScrollUp, FKey(), nullptr, 0},
	{EWoTManagedBinding::ShowScores, TEXT("ShowScores"), EKeys::F4, EKeys::MiddleMouseButton, nullptr, 0},
	{EWoTManagedBinding::InGameMenu, TEXT("InGameMenu"), EKeys::Escape, FKey(), nullptr, 0},
	{EWoTManagedBinding::Type, TEXT("Type"), EKeys::Tab, FKey(), nullptr, 0},
	{EWoTManagedBinding::Talk, TEXT("Talk"), EKeys::T, FKey(), nullptr, 0},
	{EWoTManagedBinding::TeamTalk, TEXT("TeamTalk"), EKeys::Y, FKey(), nullptr, 0},
	{EWoTManagedBinding::ShowInventoryInfo, TEXT("ShowInventoryInfo"), EKeys::F2, FKey(), nullptr, 0},
	{EWoTManagedBinding::ViewUp, TEXT("ViewUp"), EKeys::Equals, FKey(), nullptr, 0},
	{EWoTManagedBinding::ViewDown, TEXT("ViewDown"), EKeys::Hyphen, FKey(), nullptr, 0},
	{EWoTManagedBinding::NextHand, TEXT("NextHand"), EKeys::RightBracket, FKey(), nullptr, 0},
	{EWoTManagedBinding::PrevHand, TEXT("PrevHand"), EKeys::LeftBracket, FKey(), nullptr, 0},
	{EWoTManagedBinding::Drop, TEXT("Drop"), EKeys::LeftControl, EKeys::Backslash, nullptr, 0},
	{EWoTManagedBinding::DropTainted, TEXT("DropTainted"), EKeys::LeftAlt, FKey(), nullptr, 0},
	{EWoTManagedBinding::SShot, TEXT("SShot"), EKeys::F9, FKey(), nullptr, 0},
	{EWoTManagedBinding::ToggleFullscreen, TEXT("ToggleFullscreen"), EKeys::F12, FKey(), nullptr, 0},
	{EWoTManagedBinding::ServerEditCitadel, TEXT("ServerEditCitadel"), EKeys::F10, FKey(), nullptr, 0},
	{EWoTManagedBinding::ShowHelp, TEXT("ShowHelp"), EKeys::F1, FKey(), nullptr, 0},
	{EWoTManagedBinding::Walking, TEXT("Walking"), EKeys::LeftShift, EKeys::RightShift, nullptr, 0},
	{EWoTManagedBinding::Look, TEXT("Look"), EKeys::M, FKey(), nullptr, 0},
	{EWoTManagedBinding::LookToggle, TEXT("LookToggle"), EKeys::CapsLock, FKey(), nullptr, 0},
	{EWoTManagedBinding::FreeLook, TEXT("FreeLook"), EKeys::Insert, FKey(), nullptr, 0},
	{EWoTManagedBinding::CenterView, TEXT("CenterView"), EKeys::End, FKey(), nullptr, 0},
	{EWoTManagedBinding::Strafe, TEXT("Strafe"), EKeys::Apostrophe, FKey(), nullptr, 0},
	{EWoTManagedBinding::PlayTaunt1, TEXT("PlayTaunt1"), EKeys::G, FKey(), nullptr, 0},
	{EWoTManagedBinding::PlayTaunt2, TEXT("PlayTaunt2"), EKeys::H, FKey(), nullptr, 0},
	{EWoTManagedBinding::PlayTaunt3, TEXT("PlayTaunt3"), EKeys::J, FKey(), nullptr, 0},
	{EWoTManagedBinding::PlayTaunt4, TEXT("PlayTaunt4"), EKeys::K, FKey(), nullptr, 0},
	{EWoTManagedBinding::PlayTaunt5, TEXT("PlayTaunt5"), EKeys::L, FKey(), nullptr, 0},
	{EWoTManagedBinding::PlayTaunt6, TEXT("PlayTaunt6"), EKeys::Semicolon, FKey(), nullptr, 0},
	{EWoTManagedBinding::PlayTauntRandom, TEXT("PlayTauntRandom"), EKeys::R, FKey(), nullptr, 0},
	{EWoTManagedBinding::Brightness, TEXT("Brightness"), EKeys::F11, FKey(), nullptr, 0},
	{EWoTManagedBinding::Pause, TEXT("Pause"), EKeys::Pause, FKey(), nullptr, 0},
	{EWoTManagedBinding::SelectHand1, TEXT("SelectHand1"), EKeys::One, FKey(), nullptr, 0},
	{EWoTManagedBinding::SelectHand2, TEXT("SelectHand2"), EKeys::Two, FKey(), nullptr, 0},
	{EWoTManagedBinding::SelectHand3, TEXT("SelectHand3"), EKeys::Three, FKey(), nullptr, 0},
	{EWoTManagedBinding::SelectHand4, TEXT("SelectHand4"), EKeys::Four, FKey(), nullptr, 0},
	{EWoTManagedBinding::SelectHand5, TEXT("SelectHand5"), EKeys::Five, FKey(), nullptr, 0},
	{EWoTManagedBinding::SelectHand6, TEXT("SelectHand6"), EKeys::Six, EKeys::Q, nullptr, 0},
	{EWoTManagedBinding::SelectHand7, TEXT("SelectHand7"), EKeys::Seven, EKeys::E, nullptr, 0},
	{EWoTManagedBinding::SelectHand8, TEXT("SelectHand8"), EKeys::Eight, EKeys::Z, nullptr, 0},
	{EWoTManagedBinding::SelectHand9, TEXT("SelectHand9"), EKeys::Nine, EKeys::X, nullptr, 0},
	{EWoTManagedBinding::SelectHand0, TEXT("SelectHand0"), EKeys::Zero, FKey(), nullptr, 0},
};
constexpr int32 BindingCount = UE_ARRAY_COUNT(BindingDefinitions);
constexpr int32 LegacyBindingCount = 23;
constexpr int32 Version3BindingCount = 33;
constexpr int32 Version4BindingCount = 36;
constexpr int32 Version5BindingCount = 38;
const EWoTManagedBinding Version6Bindings[] = {
	EWoTManagedBinding::Walking, EWoTManagedBinding::Look,
	EWoTManagedBinding::LookToggle, EWoTManagedBinding::FreeLook,
	EWoTManagedBinding::CenterView, EWoTManagedBinding::Strafe,
	EWoTManagedBinding::PlayTaunt1, EWoTManagedBinding::PlayTaunt2,
	EWoTManagedBinding::PlayTaunt3, EWoTManagedBinding::PlayTaunt4,
	EWoTManagedBinding::PlayTaunt5, EWoTManagedBinding::PlayTaunt6,
	EWoTManagedBinding::PlayTauntRandom, EWoTManagedBinding::Brightness,
	EWoTManagedBinding::Pause,
};
static_assert(UE_ARRAY_COUNT(Version6Bindings) == 15);
const EWoTManagedBinding LegacyBindings[] = {
	EWoTManagedBinding::MoveForward, EWoTManagedBinding::MoveBackward,
	EWoTManagedBinding::MoveRight, EWoTManagedBinding::MoveLeft,
	EWoTManagedBinding::Jump, EWoTManagedBinding::Fire,
	EWoTManagedBinding::NextItem, EWoTManagedBinding::PrevItem,
	EWoTManagedBinding::ShowScores, EWoTManagedBinding::InGameMenu,
	EWoTManagedBinding::ShowInventoryInfo, EWoTManagedBinding::ViewUp,
	EWoTManagedBinding::ViewDown, EWoTManagedBinding::SelectHand1,
	EWoTManagedBinding::SelectHand2, EWoTManagedBinding::SelectHand3,
	EWoTManagedBinding::SelectHand4, EWoTManagedBinding::SelectHand5,
	EWoTManagedBinding::SelectHand6, EWoTManagedBinding::SelectHand7,
	EWoTManagedBinding::SelectHand8, EWoTManagedBinding::SelectHand9,
	EWoTManagedBinding::SelectHand0,
};
static_assert(UE_ARRAY_COUNT(LegacyBindings) == LegacyBindingCount);

const FBindingDefinition* Definition(EWoTManagedBinding Id)
{
	for (const FBindingDefinition& Candidate : BindingDefinitions)
	{
		if (Candidate.Id == Id) { return &Candidate; }
	}
	return nullptr;
}

bool IsChatBinding(EWoTManagedBinding Id)
{
	return Id == EWoTManagedBinding::Type
		|| Id == EWoTManagedBinding::Talk
		|| Id == EWoTManagedBinding::TeamTalk;
}

bool IsVersion6Binding(EWoTManagedBinding Id)
{
	for (const EWoTManagedBinding Added : Version6Bindings)
	{
		if (Id == Added) { return true; }
	}
	return false;
}

bool IsMigrationUnboundBinding(EWoTManagedBinding Id)
{
	if (IsChatBinding(Id) || Id == EWoTManagedBinding::Drop
		|| Id == EWoTManagedBinding::DropTainted)
	{
		return true;
	}
	return IsVersion6Binding(Id);
}

bool IsEditableKey(const FKey& Key)
{
	return Key.IsValid() && Key.IsDigital() && Key.IsBindableToActions()
		&& !Key.IsGamepadKey() && !Key.IsTouch() && !Key.IsGesture()
		&& !Key.IsVirtual() && !Key.IsDeprecated()
		&& (Key.GetMenuCategory() == EKeys::NAME_KeyboardCategory
			|| Key.GetMenuCategory() == EKeys::NAME_MouseCategory);
}

bool ParseKey(const FString& Text, FKey& Out)
{
	if (Text.Len() > 64) { return false; }
	if (Text == TEXT("-")) { Out = FKey(); return true; }
	TArray<FKey> Keys;
	EKeys::GetAllKeys(Keys);
	for (const FKey& Key : Keys)
	{
		if (IsEditableKey(Key) && Key.GetFName().ToString().Equals(Text, ESearchCase::CaseSensitive))
		{
			Out = Key;
			return true;
		}
	}
	return false;
}

bool ParseUnsigned(const FString& Text, uint32& Out)
{
	if (Text.IsEmpty() || Text.Len() > 10) { return false; }
	uint64 Value = 0;
	for (TCHAR C : Text)
	{
		if (C < TEXT('0') || C > TEXT('9')) { return false; }
		Value = Value * 10 + static_cast<uint32>(C - TEXT('0'));
		if (Value > MAX_uint32) { return false; }
	}
	Out = static_cast<uint32>(Value);
	return true;
}

bool ParseScalar(const FString& Text, float& Out)
{
	if (Text.IsEmpty() || Text.Len() > 32) { return false; }
	for (TCHAR C : Text)
	{
		if ((C < TEXT('0') || C > TEXT('9')) && C != TEXT('.')
			&& C != TEXT('e') && C != TEXT('E') && C != TEXT('+') && C != TEXT('-'))
		{
			return false;
		}
	}
	return LexTryParseString(Out, *Text) && FMath::IsFinite(Out);
}

const TCHAR* ModeName(EWoTWindowMode Mode)
{
	switch (Mode)
	{
	case EWoTWindowMode::Windowed: return TEXT("Windowed");
	case EWoTWindowMode::WindowedFullscreen: return TEXT("WindowedFullscreen");
	case EWoTWindowMode::Fullscreen: return TEXT("Fullscreen");
	default: return TEXT("");
	}
}

EWindowMode::Type NativeMode(EWoTWindowMode Mode)
{
	switch (Mode)
	{
	case EWoTWindowMode::WindowedFullscreen: return EWindowMode::WindowedFullscreen;
	case EWoTWindowMode::Fullscreen: return EWindowMode::Fullscreen;
	default: return EWindowMode::Windowed;
	}
}

#if WITH_AUTOMATION_TESTS
FWoTGameSettingsAdapter::FDisplayProjection DisplayProjectionForTesting;
FWoTGameSettingsAdapter::FBrightnessProjection BrightnessProjectionForTesting;
TWeakObjectPtr<UWorld> BrightnessWorldForTesting;
#endif

bool HasCommandLineFrameRateOverride(bool bExecCmdsCanRun)
{
	if (!bExecCmdsCanRun)
	{
		return false;
	}
	for (const FString& Command :
		ParseExecCommands::ParseExecCmdsFromCommandLine(TEXT("ExecCmds")))
	{
		const TCHAR* Cursor = *Command;
		const FString CommandName = FParse::Token(Cursor, false);
		if (!CommandName.Equals(TEXT("t.MaxFPS"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString NumberToken = FParse::Token(Cursor, false);
		double Number = 0.0;
		if (!NumberToken.IsEmpty()
			&& LexTryParseString(Number, *NumberToken)
			&& FMath::IsFinite(Number))
		{
			return true;
		}
	}
	return false;
}

bool HasCommandLineFrameRateOverride()
{
#if !(UE_BUILD_SHIPPING) || ENABLE_PGO_PROFILE
	return HasCommandLineFrameRateOverride(/*bExecCmdsCanRun*/ true);
#else
	return false;
#endif
}

void ProjectVideo(const FWoTGameSettings& Settings, bool bStartup,
	bool bApplyResolutionAndWindowMode = true,
	bool bApplyFrameRateLimit = true)
{
#if WITH_AUTOMATION_TESTS
	if (DisplayProjectionForTesting)
	{
		DisplayProjectionForTesting(
			Settings, bApplyResolutionAndWindowMode, bApplyFrameRateLimit);
		return;
	}
#endif
	if (GIsEditor || IsRunningCommandlet()) { return; }
	FWoTGameSettingsAdapter::ApplyBrightness(Settings.Brightness);
	if (GEngine)
	{
		if (IConsoleVariable* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync")))
		{
			VSync->Set(Settings.bVSync ? 1 : 0, ECVF_SetByGameSetting);
		}
		if (bApplyFrameRateLimit)
		{
			GEngine->SetMaxFPS(Settings.FrameRateLimit);
		}
		Scalability::FQualityLevels Quality;
		Quality.SetFromSingleQualityLevel(static_cast<int32>(Settings.GraphicsQuality));
		Scalability::SetQualityLevels(Quality);
	}
	if (bApplyResolutionAndWindowMode)
	{
		UGameUserSettings::RequestResolutionChange(Settings.Width, Settings.Height,
			NativeMode(Settings.WindowMode), bStartup);
	}
}
} // namespace

TConstArrayView<EWoTManagedBinding> FWoTGameSettings::ManagedActions()
{
	static const EWoTManagedBinding Order[] = {
		EWoTManagedBinding::Fire, EWoTManagedBinding::Jump,
		EWoTManagedBinding::MoveForward, EWoTManagedBinding::MoveBackward,
		EWoTManagedBinding::MoveLeft, EWoTManagedBinding::MoveRight,
		EWoTManagedBinding::TurnLeft, EWoTManagedBinding::TurnRight,
		EWoTManagedBinding::NextHand, EWoTManagedBinding::PrevHand,
		EWoTManagedBinding::Drop, EWoTManagedBinding::DropTainted,
		EWoTManagedBinding::Look, EWoTManagedBinding::LookToggle,
		EWoTManagedBinding::LookUp, EWoTManagedBinding::LookDown,
		EWoTManagedBinding::CenterView, EWoTManagedBinding::Walking,
		EWoTManagedBinding::Strafe, EWoTManagedBinding::FreeLook,
		EWoTManagedBinding::NextItem, EWoTManagedBinding::PrevItem,
		EWoTManagedBinding::ViewUp, EWoTManagedBinding::ViewDown,
		EWoTManagedBinding::ShowScores, EWoTManagedBinding::ShowInventoryInfo,
		EWoTManagedBinding::InGameMenu,
		EWoTManagedBinding::Type, EWoTManagedBinding::Talk,
		EWoTManagedBinding::TeamTalk, EWoTManagedBinding::SShot,
		EWoTManagedBinding::ToggleFullscreen, EWoTManagedBinding::ServerEditCitadel,
		EWoTManagedBinding::ShowHelp,
		EWoTManagedBinding::PlayTaunt1, EWoTManagedBinding::PlayTaunt2,
		EWoTManagedBinding::PlayTaunt3, EWoTManagedBinding::PlayTaunt4,
		EWoTManagedBinding::PlayTaunt5, EWoTManagedBinding::PlayTaunt6,
		EWoTManagedBinding::PlayTauntRandom,
		EWoTManagedBinding::Brightness, EWoTManagedBinding::Pause,
		EWoTManagedBinding::SelectHand1, EWoTManagedBinding::SelectHand2,
		EWoTManagedBinding::SelectHand3, EWoTManagedBinding::SelectHand4,
		EWoTManagedBinding::SelectHand5, EWoTManagedBinding::SelectHand6,
		EWoTManagedBinding::SelectHand7, EWoTManagedBinding::SelectHand8,
		EWoTManagedBinding::SelectHand9, EWoTManagedBinding::SelectHand0,
	};
	static_assert(UE_ARRAY_COUNT(Order) == BindingCount, "managed order must cover the catalog");
	return MakeArrayView(Order);
}

const TCHAR* FWoTGameSettings::ActionName(EWoTManagedBinding Action)
{
	const FBindingDefinition* Def = Definition(Action);
	return Def ? Def->Name : TEXT("");
}

bool FWoTGameSettings::IsEditableKey(const FKey& Key)
{
	return ::IsEditableKey(Key);
}

FWoTGameSettings FWoTGameSettings::Defaults()
{
	FWoTGameSettings Settings;
	for (const FBindingDefinition& Entry : BindingDefinitions)
	{
		Settings.Bindings.Add({Entry.Id, Entry.Primary, Entry.Secondary});
	}
	return Settings;
}

bool FWoTGameSettings::Validate(FString& OutError) const
{
	OutError.Reset();
	const FWoTEarlyUILimits Limits = FWoTEarlyUILimits::Production();
	if (!FMath::IsFinite(MouseSensitivity) || MouseSensitivity < 0.1f || MouseSensitivity > 10.0f)
	{ OutError = TEXT("Mouse sensitivity must be between 0.1 and 10."); return false; }
	if (!FMath::IsFinite(Brightness) || Brightness < 0.0f || Brightness > 0.9f)
	{ OutError = TEXT("Brightness must be between 0 and 0.9."); return false; }
	if (!FMath::IsFinite(MasterVolume) || MasterVolume < 0.0f || MasterVolume > 1.0f)
	{ OutError = TEXT("Master volume must be between 0 and 1."); return false; }
	if (!FMath::IsFinite(ViewBob) || ViewBob < 0.0f || ViewBob > 0.032f)
	{ OutError = TEXT("View bob must be between 0 and 0.032."); return false; }
	if (bAlwaysMouseLook && bAutoSlopeLook)
	{ OutError = TEXT("Always Mouse Look and Auto Slope Look cannot both be enabled."); return false; }
	if (Crosshair > 20 || GoreDetailLevel > 3)
	{ OutError = TEXT("Retail presentation preference is outside supported limits."); return false; }
	if (Width < Limits.MinWidth || Width > Limits.MaxWidth
		|| Height < Limits.MinHeight || Height > Limits.MaxHeight)
	{ OutError = TEXT("Resolution is outside supported display limits."); return false; }
	if (ModeName(WindowMode)[0] == 0
		|| static_cast<uint8>(GraphicsQuality) > static_cast<uint8>(EWoTGraphicsQuality::Epic)
		|| (FrameRateLimit != 0 && (FrameRateLimit < 30 || FrameRateLimit > 240)))
	{ OutError = TEXT("Display setting is outside supported limits."); return false; }
	if (Bindings.Num() != BindingCount)
	{ OutError = TEXT("The managed binding catalog is incomplete."); return false; }
	TSet<FKey> Used;
	TSet<EWoTManagedBinding> Ids;
	for (const FWoTManagedKeyBinding& Binding : Bindings)
	{
		if (!Definition(Binding.Action) || Ids.Contains(Binding.Action))
		{ OutError = TEXT("An unknown or duplicate action was supplied."); return false; }
		Ids.Add(Binding.Action);
		const bool bUnboundMigratedAction = IsMigrationUnboundBinding(Binding.Action)
			&& !Binding.Primary.IsValid();
		if ((!bUnboundMigratedAction && !IsEditableKey(Binding.Primary))
			|| (Binding.Secondary.IsValid() && !IsEditableKey(Binding.Secondary)))
		{ OutError = TEXT("Choose a keyboard or mouse button for this action."); return false; }
		for (const FKey& Key : {Binding.Primary, Binding.Secondary})
		{
			if (!Key.IsValid()) { continue; }
			if (Used.Contains(Key))
			{ OutError = FString::Printf(TEXT("%s is already bound."), *Key.ToString()); return false; }
			if (Key == EKeys::Escape && Binding.Action != EWoTManagedBinding::InGameMenu)
			{ OutError = TEXT("Escape is reserved for the game menu."); return false; }
			Used.Add(Key);
		}
	}
	const FWoTManagedKeyBinding* Menu = FindBinding(EWoTManagedBinding::InGameMenu);
	if (!Menu || (Menu->Primary != EKeys::Escape && Menu->Secondary != EKeys::Escape))
	{ OutError = TEXT("Escape must remain bound to the game menu."); return false; }
	return true;
}

bool FWoTGameSettings::operator==(const FWoTGameSettings& Other) const
{
	return MouseSensitivity == Other.MouseSensitivity && Brightness == Other.Brightness
		&& bInvertMouseY == Other.bInvertMouseY && Width == Other.Width
		&& Height == Other.Height && WindowMode == Other.WindowMode
		&& bVSync == Other.bVSync && GraphicsQuality == Other.GraphicsQuality
		&& FrameRateLimit == Other.FrameRateLimit
		&& MasterVolume == Other.MasterVolume
		&& bAlwaysMouseLook == Other.bAlwaysMouseLook
		&& bAutoSlopeLook == Other.bAutoSlopeLook
		&& bPlayerIDEnabled == Other.bPlayerIDEnabled
		&& bLookSpring == Other.bLookSpring
		&& Crosshair == Other.Crosshair && ViewBob == Other.ViewBob
		&& bJoystickEnabled == Other.bJoystickEnabled
		&& GoreDetailLevel == Other.GoreDetailLevel && Bindings == Other.Bindings;
}

FWoTManagedKeyBinding* FWoTGameSettings::FindBinding(EWoTManagedBinding Action)
{
	return Bindings.FindByPredicate([Action](const FWoTManagedKeyBinding& B) { return B.Action == Action; });
}

const FWoTManagedKeyBinding* FWoTGameSettings::FindBinding(EWoTManagedBinding Action) const
{
	return Bindings.FindByPredicate([Action](const FWoTManagedKeyBinding& B) { return B.Action == Action; });
}

void FWoTGameSettings::ApplyToInput(UPlayerInput& Input) const
{
	Input.ActionMappings.RemoveAll([](const FInputActionKeyMapping& Mapping)
	{
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			if (!Entry.AxisName && Mapping.ActionName == Entry.Name) { return true; }
		}
		return false;
	});
	Input.AxisMappings.RemoveAll([](const FInputAxisKeyMapping& Mapping)
	{
		if (!Mapping.Key.IsDigital()) { return false; }
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			if (Entry.AxisName && Mapping.AxisName == Entry.AxisName) { return true; }
		}
		return false;
	});
	for (const FBindingDefinition& Entry : BindingDefinitions)
	{
		const FWoTManagedKeyBinding* Binding = FindBinding(Entry.Id);
		if (!Binding) { continue; }
		for (const FKey& Key : {Binding->Primary, Binding->Secondary})
		{
			if (!Key.IsValid()) { continue; }
			if (Entry.AxisName)
			{
				Input.AxisMappings.Add(FInputAxisKeyMapping(Entry.AxisName, Key, Entry.AxisScale));
			}
			else
			{
				Input.ActionMappings.Add(FInputActionKeyMapping(Entry.Name, Key));
			}
		}
	}
	// Rebuild first: the engine clears its cached axis properties here.
	Input.ForceRebuildingKeyMaps(false);
	for (const FKey& Key : {EKeys::MouseX, EKeys::MouseY})
	{
		FInputAxisProperties Properties;
		if (!Input.GetAxisProperties(Key, Properties))
		{
			FInputAxisConfigEntry& Entry = Input.AxisConfig.AddDefaulted_GetRef();
			Entry.AxisKeyName = Key.GetFName();
			Properties.DeadZone = 0.0f;
			Properties.Exponent = 1.0f;
		}
		Properties.Sensitivity = 0.07f * MouseSensitivity;
		Properties.bInvert = Key == EKeys::MouseY && bInvertMouseY;
		Input.SetAxisProperties(Key, Properties);
	}
}

FString FWoTGameSettingsCodec::Serialize(const FWoTGameSettings& Settings)
{
	FString Text = FString::Printf(
		TEXT("GameSettingsVersion=9\nGameMouseSensitivity=%.9g\nGameBrightness=%.9g\nGameInvertMouseY=%d\n")
		TEXT("GameWidth=%u\nGameHeight=%u\nGameWindowMode=%s\nGameVSync=%d\n")
		TEXT("GameGraphicsQuality=%u\nGameFrameRateLimit=%u\nGameMasterVolume=%.9g\n")
		TEXT("GameAlwaysMouseLook=%d\nGameAutoSlopeLook=%d\nGamePlayerID=%d\n")
		TEXT("GameLookSpring=%d\nGameCrosshair=%u\nGameViewBob=%.9g\nGameJoystick=%d\nGameGoreDetail=%u\n"),
		Settings.MouseSensitivity, Settings.Brightness, Settings.bInvertMouseY ? 1 : 0,
		Settings.Width, Settings.Height, ModeName(Settings.WindowMode),
		Settings.bVSync ? 1 : 0, static_cast<uint32>(Settings.GraphicsQuality),
		Settings.FrameRateLimit, Settings.MasterVolume,
		Settings.bAlwaysMouseLook ? 1 : 0, Settings.bAutoSlopeLook ? 1 : 0,
		Settings.bPlayerIDEnabled ? 1 : 0, Settings.bLookSpring ? 1 : 0,
		Settings.Crosshair, Settings.ViewBob,
		Settings.bJoystickEnabled ? 1 : 0, Settings.GoreDetailLevel);
	for (const FBindingDefinition& Entry : BindingDefinitions)
	{
		const FWoTManagedKeyBinding* Binding = Settings.FindBinding(Entry.Id);
		if (Binding)
		{
			Text += FString::Printf(TEXT("GameBinding.%s=%s,%s\n"), Entry.Name,
				Binding->Primary.IsValid()
					? *Binding->Primary.ToString() : TEXT("-"),
				Binding->Secondary.IsValid() ? *Binding->Secondary.ToString() : TEXT("-"));
		}
	}
	return Text;
}

bool FWoTGameSettingsCodec::Parse(TConstArrayView<uint8> Bytes,
	FWoTGameSettings& Out, FString& OutError)
{
	OutError.Reset();
	if (Bytes.Num() > static_cast<int32>(FWoTEarlyUILimits::Production().MaxSerializedBytes))
	{ OutError = TEXT("Preference file exceeds the size limit."); return false; }
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{ OutError = TEXT("Preference file is not plain ASCII."); return false; }
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	const FString Source(Converted.Length(), Converted.Get());
	if (!Source.IsEmpty() && !Source.EndsWith(TEXT("\n")))
	{ OutError = TEXT("Preference record is incomplete."); return false; }
	FWoTGameSettings Candidate = FWoTGameSettings::Defaults();
	TSet<FString> Seen;
	bool bHasGameRecord = false;
	uint32 SettingsVersion = 0;
	TArray<FString> Lines;
	Source.ParseIntoArrayLines(Lines, false);
	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine;
		Line.RemoveFromEnd(TEXT("\r"));
		if (Line.IsEmpty()) { continue; }
		FString Key, Value;
		if (!Line.Split(TEXT("="), &Key, &Value) || Key.IsEmpty() || Key.Len() > 80 || Value.Len() > 80)
		{ OutError = TEXT("Malformed preference record."); return false; }
		if (!Key.StartsWith(TEXT("Game"))) { continue; }
		bHasGameRecord = true;
		if (Seen.Contains(Key))
		{ OutError = TEXT("Duplicate game setting record."); return false; }
		Seen.Add(Key);
		uint32 Number = 0;
		float Scalar = 0;
		if (Key == TEXT("GameSettingsVersion"))
		{
			if (!ParseUnsigned(Value, SettingsVersion)
				|| (SettingsVersion != 1 && SettingsVersion != 2
					&& SettingsVersion != 3 && SettingsVersion != 4
					&& SettingsVersion != 5 && SettingsVersion != 6
					&& SettingsVersion != 7 && SettingsVersion != 8
					&& SettingsVersion != 9))
			{
				OutError = TEXT("Unsupported game settings version.");
				return false;
			}
		}
		else if (Key == TEXT("GameMouseSensitivity"))
		{
			if (!ParseScalar(Value, Scalar)) { OutError = TEXT("Invalid mouse sensitivity."); return false; }
			Candidate.MouseSensitivity = Scalar;
		}
		else if (Key == TEXT("GameBrightness"))
		{
			if (!ParseScalar(Value, Scalar)) { OutError = TEXT("Invalid brightness."); return false; }
			Candidate.Brightness = Scalar;
		}
		else if (Key == TEXT("GameMasterVolume"))
		{
			if (!ParseScalar(Value, Scalar)) { OutError = TEXT("Invalid master volume."); return false; }
			Candidate.MasterVolume = Scalar;
		}
		else if (Key == TEXT("GameViewBob"))
		{
			if (!ParseScalar(Value, Scalar)) { OutError = TEXT("Invalid view bob."); return false; }
			Candidate.ViewBob = Scalar;
		}
		else if (Key == TEXT("GameAlwaysMouseLook")
			|| Key == TEXT("GameAutoSlopeLook") || Key == TEXT("GamePlayerID")
			|| Key == TEXT("GameLookSpring") || Key == TEXT("GameJoystick"))
		{
			if (Value != TEXT("0") && Value != TEXT("1")) { OutError = TEXT("Invalid presentation flag."); return false; }
			if (Key == TEXT("GameAlwaysMouseLook")) { Candidate.bAlwaysMouseLook = Value == TEXT("1"); }
			else if (Key == TEXT("GameAutoSlopeLook")) { Candidate.bAutoSlopeLook = Value == TEXT("1"); }
			else if (Key == TEXT("GamePlayerID")) { Candidate.bPlayerIDEnabled = Value == TEXT("1"); }
			else if (Key == TEXT("GameLookSpring")) { Candidate.bLookSpring = Value == TEXT("1"); }
			else { Candidate.bJoystickEnabled = Value == TEXT("1"); }
		}
		else if (Key == TEXT("GameCrosshair") || Key == TEXT("GameGoreDetail"))
		{
			if (!ParseUnsigned(Value, Number) || Number > MAX_uint8)
			{ OutError = TEXT("Invalid presentation setting."); return false; }
			if (Key == TEXT("GameCrosshair")) { Candidate.Crosshair = static_cast<uint8>(Number); }
			else { Candidate.GoreDetailLevel = static_cast<uint8>(Number); }
		}
		else if (Key == TEXT("GameInvertMouseY") || Key == TEXT("GameVSync"))
		{
			if (Value != TEXT("0") && Value != TEXT("1")) { OutError = TEXT("Invalid game setting flag."); return false; }
			if (Key == TEXT("GameInvertMouseY")) { Candidate.bInvertMouseY = Value == TEXT("1"); }
			else { Candidate.bVSync = Value == TEXT("1"); }
		}
		else if (Key == TEXT("GameWidth") || Key == TEXT("GameHeight")
			|| Key == TEXT("GameFrameRateLimit") || Key == TEXT("GameGraphicsQuality"))
		{
			if (!ParseUnsigned(Value, Number) || Number > MAX_uint16)
			{ OutError = TEXT("Invalid numeric game setting."); return false; }
			if (Key == TEXT("GameWidth")) { Candidate.Width = static_cast<uint16>(Number); }
			else if (Key == TEXT("GameHeight")) { Candidate.Height = static_cast<uint16>(Number); }
			else if (Key == TEXT("GameFrameRateLimit")) { Candidate.FrameRateLimit = static_cast<uint16>(Number); }
			else { Candidate.GraphicsQuality = static_cast<EWoTGraphicsQuality>(Number); }
		}
		else if (Key == TEXT("GameWindowMode"))
		{
			if (Value == TEXT("Windowed")) { Candidate.WindowMode = EWoTWindowMode::Windowed; }
			else if (Value == TEXT("WindowedFullscreen")) { Candidate.WindowMode = EWoTWindowMode::WindowedFullscreen; }
			else if (Value == TEXT("Fullscreen")) { Candidate.WindowMode = EWoTWindowMode::Fullscreen; }
			else { OutError = TEXT("Invalid window mode."); return false; }
		}
		else if (Key.StartsWith(TEXT("GameBinding.")))
		{
			const FString Identity = Key.RightChop(12);
			const FBindingDefinition* Def = nullptr;
			for (const FBindingDefinition& Entry : BindingDefinitions)
			{ if (Identity == Entry.Name) { Def = &Entry; break; } }
			if (!Def) { OutError = TEXT("Unknown binding identity."); return false; }
			FString Primary, Secondary;
			if (!Value.Split(TEXT(","), &Primary, &Secondary) || Secondary.Contains(TEXT(",")))
			{ OutError = TEXT("Invalid binding record."); return false; }
			FKey ParsedPrimary, ParsedSecondary;
			if (!ParseKey(Primary, ParsedPrimary) || !ParseKey(Secondary, ParsedSecondary))
			{ OutError = TEXT("Invalid keyboard or mouse key."); return false; }
			FWoTManagedKeyBinding* Binding = Candidate.FindBinding(Def->Id);
			Binding->Primary = ParsedPrimary;
			Binding->Secondary = ParsedSecondary;
		}
		else
		{ OutError = TEXT("Unknown game setting record."); return false; }
	}
	if (bHasGameRecord)
	{
		const bool bCompleteCurrent = SettingsVersion == 9
			&& Seen.Num() == BindingCount + 19;
		const bool bCompleteVersion8 = SettingsVersion == 8
			&& Seen.Num() == BindingCount + 17;
		const bool bCompleteVersion7 = SettingsVersion == 7
			&& Seen.Num() == BindingCount + 16;
		const bool bCompleteVersion6 = SettingsVersion == 6
			&& Seen.Num() == BindingCount + 11;
		bool bHasVersion5Identities = true;
		int32 Version5Identities = 0;
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			if (IsVersion6Binding(Entry.Id)) { continue; }
			++Version5Identities;
			bHasVersion5Identities &= Seen.Contains(
				FString::Printf(TEXT("GameBinding.%s"), Entry.Name));
		}
		check(Version5Identities == Version5BindingCount);
		const bool bCompleteVersion5 = SettingsVersion == 5
			&& ((Seen.Num() == Version5BindingCount + 10
					&& bHasVersion5Identities)
				|| Seen.Num() == BindingCount + 11);
		bool bHasVersion4Identities = true;
		int32 Version4Identities = 0;
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			if (IsVersion6Binding(Entry.Id)
				|| Entry.Id == EWoTManagedBinding::Drop
				|| Entry.Id == EWoTManagedBinding::DropTainted)
			{
				continue;
			}
			++Version4Identities;
			bHasVersion4Identities &= Seen.Contains(
				FString::Printf(TEXT("GameBinding.%s"), Entry.Name));
		}
		check(Version4Identities == Version4BindingCount);
		const bool bCompleteVersion4 = SettingsVersion == 4
			&& ((Seen.Num() == Version4BindingCount + 10
					&& bHasVersion4Identities)
				|| Seen.Num() == BindingCount + 11);
		bool bHasLegacyIdentities = true;
		for (EWoTManagedBinding Id : LegacyBindings)
		{
			const FBindingDefinition* Entry = Definition(Id);
			bHasLegacyIdentities &= Entry && Seen.Contains(
				FString::Printf(TEXT("GameBinding.%s"), Entry->Name));
		}
		const bool bCompleteLegacy = (SettingsVersion == 1 || SettingsVersion == 2)
			&& ((Seen.Num() == LegacyBindingCount + 10 && bHasLegacyIdentities)
				|| Seen.Num() == BindingCount + 11);
		bool bHasVersion3Identities = true;
		int32 Version3Identities = 0;
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			if (IsVersion6Binding(Entry.Id)
				|| Entry.Id == EWoTManagedBinding::Type
				|| Entry.Id == EWoTManagedBinding::Talk
				|| Entry.Id == EWoTManagedBinding::TeamTalk
				|| Entry.Id == EWoTManagedBinding::Drop
				|| Entry.Id == EWoTManagedBinding::DropTainted)
			{
				continue;
			}
			++Version3Identities;
			bHasVersion3Identities &= Seen.Contains(
				FString::Printf(TEXT("GameBinding.%s"), Entry.Name));
		}
		check(Version3Identities == Version3BindingCount);
		const bool bCompleteVersion3 = SettingsVersion == 3
			&& ((Seen.Num() == Version3BindingCount + 10
					&& bHasVersion3Identities)
				|| Seen.Num() == BindingCount + 11);
		if (!bCompleteCurrent && !bCompleteVersion8 && !bCompleteVersion7
			&& !bCompleteVersion6 && !bCompleteLegacy
			&& !bCompleteVersion3 && !bCompleteVersion4 && !bCompleteVersion5)
		{ OutError = TEXT("Game settings record set is incomplete."); return false; }
	}
	// Version 1 persisted the old false default even though Hardware was not
	// reachable in game. Treat every v1 record as the intended new safe default;
	// the next ordinary save serializes the complete current record.
	if (SettingsVersion == 1)
	{
		Candidate.bVSync = true;
	}
	if (SettingsVersion == 1 || SettingsVersion == 2)
	{
		// These secondary aliases did not exist in the v1/v2 catalog. Preserve
		// genuine user choices, but fill an old empty slot with the new retail
		// default just as wholly new action records are filled from Defaults().
		const FWoTGameSettings Defaults = FWoTGameSettings::Defaults();
		for (EWoTManagedBinding Id : {
			EWoTManagedBinding::MoveForward, EWoTManagedBinding::MoveBackward,
			EWoTManagedBinding::MoveRight, EWoTManagedBinding::MoveLeft,
			EWoTManagedBinding::SelectHand6, EWoTManagedBinding::SelectHand7,
			EWoTManagedBinding::SelectHand8, EWoTManagedBinding::SelectHand9})
		{
			FWoTManagedKeyBinding* Migrated = Candidate.FindBinding(Id);
			const FWoTManagedKeyBinding* Default = Defaults.FindBinding(Id);
			if (Migrated && Default && !Migrated->Secondary.IsValid())
			{
				Migrated->Secondary = Default->Secondary;
			}
		}
	}
	if (bHasGameRecord && SettingsVersion < 4)
	{
		// Tab/T/Y were legal user choices for any v1-v3 action. Defaults() adds
		// the new chat records before parsing, so resolve only those introduced
		// records when their retail key is already owned. Keeping the new action
		// unbound preserves every saved key and lets the Controls page expose the
		// conflict instead of rejecting the record and resetting all settings.
		TSet<FKey> SavedKeys;
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			const FString Record = FString::Printf(
				TEXT("GameBinding.%s"), Entry.Name);
			if (!Seen.Contains(Record)) { continue; }
			const FWoTManagedKeyBinding* Saved = Candidate.FindBinding(Entry.Id);
			if (!Saved) { continue; }
			if (Saved->Primary.IsValid()) { SavedKeys.Add(Saved->Primary); }
			if (Saved->Secondary.IsValid()) { SavedKeys.Add(Saved->Secondary); }
		}
		for (EWoTManagedBinding Id : {
			EWoTManagedBinding::Type, EWoTManagedBinding::Talk,
			EWoTManagedBinding::TeamTalk})
		{
			const FBindingDefinition* Entry = Definition(Id);
			const FString Record = Entry ? FString::Printf(
				TEXT("GameBinding.%s"), Entry->Name) : FString();
			FWoTManagedKeyBinding* Added = Candidate.FindBinding(Id);
			if (!Entry || Seen.Contains(Record) || !Added) { continue; }
			if (SavedKeys.Contains(Added->Primary))
			{
				Added->Primary = FKey();
			}
			else if (Added->Primary.IsValid())
			{
				SavedKeys.Add(Added->Primary);
			}
		}
	}
	if (bHasGameRecord && SettingsVersion < 5)
	{
		// Ctrl/Backslash/Alt were legal choices for any v1-v4 action. Add the
		// v5 drop defaults only where each key remains free; an owned key leaves
		// that new slot visibly unbound while every old choice is preserved.
		TSet<FKey> SavedKeys;
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			const FString Record = FString::Printf(
				TEXT("GameBinding.%s"), Entry.Name);
			if (!Seen.Contains(Record)) { continue; }
			const FWoTManagedKeyBinding* Saved = Candidate.FindBinding(Entry.Id);
			if (!Saved) { continue; }
			if (Saved->Primary.IsValid()) { SavedKeys.Add(Saved->Primary); }
			if (Saved->Secondary.IsValid()) { SavedKeys.Add(Saved->Secondary); }
		}
		for (EWoTManagedBinding Id : {
			EWoTManagedBinding::Drop, EWoTManagedBinding::DropTainted})
		{
			const FBindingDefinition* Entry = Definition(Id);
			const FString Record = Entry ? FString::Printf(
				TEXT("GameBinding.%s"), Entry->Name) : FString();
			FWoTManagedKeyBinding* Added = Candidate.FindBinding(Id);
			if (!Entry || Seen.Contains(Record) || !Added) { continue; }
			if (SavedKeys.Contains(Added->Primary))
			{
				Added->Primary = FKey();
			}
			else if (Added->Primary.IsValid())
			{
				SavedKeys.Add(Added->Primary);
			}
			if (SavedKeys.Contains(Added->Secondary))
			{
				Added->Secondary = FKey();
			}
			else if (Added->Secondary.IsValid())
			{
				SavedKeys.Add(Added->Secondary);
			}
		}
	}
	if (bHasGameRecord && SettingsVersion < 6)
	{
		// Every newly restored retail key was legal in earlier versions. Preserve
		// all saved ownership and leave only the conflicting new action unbound.
		TSet<FKey> SavedKeys;
		for (const FBindingDefinition& Entry : BindingDefinitions)
		{
			const FString Record = FString::Printf(
				TEXT("GameBinding.%s"), Entry.Name);
			if (!Seen.Contains(Record)) { continue; }
			const FWoTManagedKeyBinding* Saved = Candidate.FindBinding(Entry.Id);
			if (!Saved) { continue; }
			if (Saved->Primary.IsValid()) { SavedKeys.Add(Saved->Primary); }
			if (Saved->Secondary.IsValid()) { SavedKeys.Add(Saved->Secondary); }
		}
		for (const EWoTManagedBinding Id : Version6Bindings)
		{
			const FBindingDefinition* Entry = Definition(Id);
			const FString Record = Entry ? FString::Printf(
				TEXT("GameBinding.%s"), Entry->Name) : FString();
			FWoTManagedKeyBinding* Added = Candidate.FindBinding(Id);
			if (!Entry || Seen.Contains(Record) || !Added) { continue; }
			for (FKey* Key : {&Added->Primary, &Added->Secondary})
			{
				if (SavedKeys.Contains(*Key)) { *Key = FKey(); }
				else if (Key->IsValid()) { SavedKeys.Add(*Key); }
			}
		}
	}
	if (!Candidate.Validate(OutError)) { return false; }
	Out = MoveTemp(Candidate);
	return true;
}

bool FWoTGameSettingsAdapter::PreviewVideo(const FWoTGameSettings& Settings,
	FString& OutError)
{
	if (!Settings.Validate(OutError)) { return false; }
	ProjectVideo(Settings, false);
	return true;
}

void FWoTGameSettingsAdapter::RevertVideo(const FWoTGameSettings& Confirmed)
{
	FString Error;
	if (Confirmed.Validate(Error)) { ProjectVideo(Confirmed, false); }
}

void FWoTGameSettingsAdapter::ApplyVideoOnStartup(const FWoTGameSettings& Settings)
{
	FString Error;
	if (Settings.Validate(Error))
	{
		// Review launches own only the fields they override. VSync and quality
		// still project; a command-line t.MaxFPS separately owns the frame cap.
		ProjectVideo(Settings, true,
			/*bApplyResolutionAndWindowMode*/ !HasReviewDisplayOverrides(),
			/*bApplyFrameRateLimit*/ !HasCommandLineFrameRateOverride());
	}
}

float FWoTGameSettingsAdapter::AdvanceBrightness(float Brightness)
{
	// Step in whole tenths: repeated float +0.1 drifts past 0.9 and would
	// fail validation, stalling the cycle at 0.8.
	const int32 NextTenth = FMath::RoundToInt(Brightness * 10.0f) + 1;
	return NextTenth >= 10 ? 0.0f : static_cast<float>(NextTenth) / 10.0f;
}

FString FWoTGameSettingsAdapter::BrightnessMessage(float Brightness)
{
	return FString::Printf(TEXT("Display brightness: %i of 10"),
		FMath::Clamp(FMath::RoundToInt(Brightness * 10.0f) + 1, 1, 10));
}

float FWoTGameSettingsAdapter::RampInvGammaForBrightness(float Brightness)
{
	// Retail's shipped Glide driver (the 1999 default render device,
	// original/System/Default.ini:20) sets the Voodoo's hardware ramp to
	// grGammaCorrectionValue(0.5 + 1.5 * Brightness) on every Flush: verified in the
	// shipped 333B GlideDrv.dll (fld Brightness; fmul 1.5; fadd 0.5; call
	// grGammaCorrectionValue), as in the original Glide driver. That ramp maps
	// each displayed value D to D^(1/g), so the default 0.6 showed players a
	// 1.4 ramp.  The gamma-boundary material consumes the reciprocal exponent.
	return 1.0f / (0.5f + 1.5f * FMath::Clamp(Brightness, 0.0f, 1.0f));
}

void FWoTGameSettingsAdapter::ApplyBrightness(float Brightness)
{
	const float RampInvGamma = RampInvGammaForBrightness(Brightness);
#if WITH_AUTOMATION_TESTS
	if (BrightnessProjectionForTesting)
	{
		BrightnessProjectionForTesting(RampInvGamma);
		return;
	}
	if (BrightnessWorldForTesting.IsValid())
	{
		UWoTDisplayRampSubsystem::ApplyToWorldForTesting(
			BrightnessWorldForTesting.Get(), RampInvGamma);
		return;
	}
#endif
	if (GIsEditor || IsRunningCommandlet() || IsRunningDedicatedServer()) { return; }
	UWoTDisplayRampSubsystem::SetRampInvGamma(RampInvGamma);
}

namespace
{
float GWoTAppliedMasterVolume = FWoTGameSettings::RetailMasterVolume;
FDelegateHandle GWoTMasterVolumeDeviceCreatedHandle;
#if WITH_AUTOMATION_TESTS
FWoTGameSettingsAdapter::FMasterVolumeProjection MasterVolumeProjectionForTesting;
#endif

void ProjectMasterVolume(FAudioDevice* Device)
{
	if (!Device)
	{
		return;
	}
	USoundSubmix& Main = Device->GetMainSubmixObject();
	Device->SetSubmixOutputVolume(&Main, GWoTAppliedMasterVolume);
#if WITH_AUTOMATION_TESTS
	if (MasterVolumeProjectionForTesting)
	{
		MasterVolumeProjectionForTesting(Device, &Main, GWoTAppliedMasterVolume);
	}
#endif
}
}

void FWoTGameSettingsAdapter::ApplyAudio(const FWoTGameSettings& Settings)
{
	FString Error;
	if (!Settings.Validate(Error))
	{
		return;
	}
	// The master volume is each audio device's main-submix output volume
	// (FMixerSubmix::SetOutputVolume, a [0,1] gain on the whole mix that only
	// this call writes in a game). Both primary-volume stages are engine-owned:
	// FApp's multiplier is focus state that the Windows message pump rewrites
	// on every frame (WindowsPlatformApplicationMisc.cpp PumpMessages: 1 when
	// focused, else [Audio] UnfocusedVolumeMultiplier; the Mac pump on each
	// (de)activation), so a master volume written there lasted one frame; and
	// UEngine::LoadMap resets the device's TransientPrimaryVolume to 1 on every
	// travel (UnrealEngine.cpp, "Stop all audio"), as do camera audio fades.
	GWoTAppliedMasterVolume = Settings.MasterVolume;
	if (!GWoTMasterVolumeDeviceCreatedHandle.IsValid())
	{
		// A device created later (device swap, new world device) inherits it.
		GWoTMasterVolumeDeviceCreatedHandle =
			FAudioDeviceManagerDelegates::OnAudioDeviceCreated.AddLambda(
				[](Audio::FDeviceId DeviceId)
				{
					if (FAudioDeviceManager* Manager = FAudioDeviceManager::Get())
					{
						ProjectMasterVolume(Manager->GetAudioDeviceRaw(DeviceId));
					}
				});
	}
	if (FAudioDeviceManager* Manager = FAudioDeviceManager::Get())
	{
		Manager->IterateOverAllDevices([](Audio::FDeviceId, FAudioDevice* Device)
		{
			ProjectMasterVolume(Device);
		});
	}
}

float FWoTGameSettingsAdapter::GetAppliedMasterVolume()
{
	return GWoTAppliedMasterVolume;
}

bool FWoTGameSettingsAdapter::HasReviewDisplayOverrides()
{
	const TCHAR* Command = FCommandLine::Get();
	int32 Ignored = 0;
	return FParse::Param(Command, TEXT("windowed"))
		|| FParse::Value(Command, TEXT("ResX="), Ignored)
		|| FParse::Value(Command, TEXT("ResY="), Ignored);
}

#if WITH_AUTOMATION_TESTS
void FWoTGameSettingsAdapter::SetDisplayProjectionForTesting(
	FDisplayProjection Projection)
{
	DisplayProjectionForTesting = MoveTemp(Projection);
}

void FWoTGameSettingsAdapter::SetBrightnessProjectionForTesting(
	FBrightnessProjection Projection)
{
	BrightnessProjectionForTesting = MoveTemp(Projection);
}

void FWoTGameSettingsAdapter::SetBrightnessWorldForTesting(UWorld* World)
{
	BrightnessWorldForTesting = World;
}

void FWoTGameSettingsAdapter::SetMasterVolumeProjectionForTesting(
	FMasterVolumeProjection Projection)
{
	MasterVolumeProjectionForTesting = MoveTemp(Projection);
}

bool FWoTGameSettingsAdapter::HasCommandLineFrameRateOverrideForTesting(
	bool bExecCmdsCanRun)
{
	return ::HasCommandLineFrameRateOverride(bExecCmdsCanRun);
}
#endif
