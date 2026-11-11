#include "WoTPreferencesSubsystem.h"

#include "WoTBootFailureReceipt.h"
#include "Payload/WoTPayloadActivation.h"

#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTUserDataRoot.h"
#include "WoTUpdaterCore/UpdateFailureNotice.h"

#if PLATFORM_WINDOWS && !WOT_GUARDED_BOOTSTRAP
#include "Windows/AllowWindowsPlatformTypes.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
#if WITH_AUTOMATION_TESTS
bool bWoTPreferencesCreationEnabledForTesting = false;
bool bWoTPreferencesDelayEarlyUIAdoptionForTesting = false;
FString WoTPreferencePathForTesting;
bool bWoTUpdateFailedNoticePersistenceFailureForTesting = false;
TFunction<TOptional<bool>(const FString&, bool)> WoTSideFileAccessHookForTesting;
#endif

FWoTBootstrapStatus PreferencesError(
	EWoTBootstrapError Code, const TCHAR* Message)
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		Code, Message, TEXT("WoTUserPreferences")));
}

const TCHAR* WindowModeName(EWoTWindowMode Mode)
{
	switch (Mode)
	{
	case EWoTWindowMode::Windowed: return TEXT("Windowed");
	case EWoTWindowMode::WindowedFullscreen: return TEXT("WindowedFullscreen");
	case EWoTWindowMode::Fullscreen: return TEXT("Fullscreen");
	default: return TEXT("Windowed");
	}
}

const TCHAR* LocaleName(EWoTShippedLocale Locale)
{
	switch (Locale)
	{
	case EWoTShippedLocale::EnUS: return TEXT("en-US");
	case EWoTShippedLocale::FrFR: return TEXT("fr-FR");
	case EWoTShippedLocale::DeDE: return TEXT("de-DE");
	case EWoTShippedLocale::EsES: return TEXT("es-ES");
	case EWoTShippedLocale::ItIT: return TEXT("it-IT");
	default: return TEXT("en-US");
	}
}

const TCHAR* FactionName(EWoTPlayerFaction Faction)
{
	switch (Faction)
	{
	case EWoTPlayerFaction::AesSedai: return TEXT("AesSedai");
	case EWoTPlayerFaction::Forsaken: return TEXT("Forsaken");
	case EWoTPlayerFaction::Hound: return TEXT("Hound");
	case EWoTPlayerFaction::Whitecloak: return TEXT("Whitecloak");
	default: return TEXT("AesSedai");
	}
}

const TCHAR* SkinColorName(EWoTPlayerSkinColor Color)
{
	switch (Color)
	{
	case EWoTPlayerSkinColor::Black: return TEXT("Black");
	case EWoTPlayerSkinColor::Blue: return TEXT("Blue");
	case EWoTPlayerSkinColor::Red: return TEXT("Red");
	case EWoTPlayerSkinColor::Green: return TEXT("Green");
	case EWoTPlayerSkinColor::Base: return TEXT("Base");
	default: return TEXT("Base");
	}
}

TArray<uint8> SerializePreferences(const FWoTPlayerPreferences& Value,
	const FWoTGameSettings& GameSettings)
{
	const FWoTEarlyUIV1& UI = Value.EarlyUI;
	const FString PlayerName =
		FWoTFrontEndModel::SanitizePlayerName(Value.PlayerName);
	FString Text = FString::Printf(
		TEXT("Schema=EarlyUI-v1\n")
		TEXT("Width=%u\nHeight=%u\nWindowMode=%s\n")
		TEXT("UIScale=%.6g\nLocale=%s\n")
		TEXT("ReducedMotion=%d\nHighContrast=%d\nScreenReader=%d\n")
		TEXT("BootstrapVolume=%.6g\nInputSensitivity=%.6g\n")
		TEXT("SelectedFaction=%s\nPlayerSkin=%s\nCitadelSkin=%s\n")
		TEXT("AlwaysUseEpicRelay=%d\nPlayerName=%s\n"),
		static_cast<uint32>(UI.Width), static_cast<uint32>(UI.Height),
		WindowModeName(UI.WindowMode), UI.UIScale, LocaleName(UI.Locale),
		UI.bReducedMotion ? 1 : 0, UI.bHighContrast ? 1 : 0,
		UI.bScreenReader ? 1 : 0, UI.BootstrapVolume,
		UI.InputSensitivity, FactionName(Value.SelectedFaction),
		SkinColorName(Value.SelectedSkinColor),
		SkinColorName(Value.CitadelSkinColor),
		Value.bAlwaysUseEpicRelay ? 1 : 0, *PlayerName);
	for (int32 Index = 0;
		Index < Value.SavedPasswords.Num() && Index < 10; ++Index)
	{
		const FWoTSavedServerPassword& Saved = Value.SavedPasswords[Index];
		Text += FString::Printf(
			TEXT("SavedPassword%dServer=%s\nSavedPassword%dValue=%s\n"),
			Index, *Saved.Server, Index, *Saved.Password);
	}
	Text += FWoTGameSettingsCodec::Serialize(GameSettings);
	FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

bool FixedPreferencesPath(FString& OutDirectory, FString& OutLeaf)
{
#if WITH_AUTOMATION_TESTS
	if (!WoTPreferencePathForTesting.IsEmpty())
	{
		OutLeaf = WoTPreferencePathForTesting;
		OutDirectory = FPaths::GetPath(OutLeaf);
		return true;
	}
#endif
#if WOT_WITH_BOOTSTRAP_TEST_HARNESSES
	// Guarded packaged-Test harness only, like -wottestinstallroot: an isolated
	// profile's preference directory. The Mac path below deliberately ignores
	// HOME, so HOME-isolated funnel and probe runs otherwise read and wrote the
	// player's real preferences and markers. Shipping
	// does not compile this override (release_formats.TEST_SEAM_MARKERS).
	FString HarnessDirectory;
	if (FParse::Value(FCommandLine::Get(), TEXT("wottestpreferencesdir="),
		HarnessDirectory))
	{
		HarnessDirectory = FPaths::ConvertRelativePathToFull(HarnessDirectory);
		FPaths::NormalizeDirectoryName(HarnessDirectory);
		if (FPaths::IsRelative(HarnessDirectory)
			|| !FPaths::CollapseRelativeDirectories(HarnessDirectory))
		{
			return false;
		}
		OutDirectory = HarnessDirectory;
		OutLeaf = FPaths::Combine(OutDirectory, TEXT("WoTUserPreferences"));
		return true;
	}
#endif
#if PLATFORM_MAC || WOT_GUARDED_BOOTSTRAP
	const FString Root = WoTUserDataRoot();
	if (Root.IsEmpty())
	{
		return false;
	}
	OutDirectory = FPaths::Combine(Root, TEXT("Preferences"));
#elif PLATFORM_WINDOWS
	PWSTR KnownPath = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,
		KF_FLAG_DONT_VERIFY, nullptr, &KnownPath)) || KnownPath == nullptr)
	{
		return false;
	}
	OutDirectory = KnownPath;
	CoTaskMemFree(KnownPath);
	OutDirectory += TEXT("\\WoT");
#else
	OutDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WoT"));
#endif
	OutLeaf = FPaths::Combine(OutDirectory, TEXT("WoTUserPreferences"));
	return true;
}

bool PrepareFixedPreferencesDirectory(const FString& Directory)
{
#if WOT_GUARDED_BOOTSTRAP
	const FString Root = WoTUserDataRoot();
	FString Expected = FPaths::Combine(Root, TEXT("Preferences"));
	FString Requested = Directory;
	FPaths::NormalizeDirectoryName(Expected);
	FPaths::NormalizeDirectoryName(Requested);
	if (!Root.IsEmpty() && Requested == Expected)
	{
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenRoot =
			OpenNativeDirectoryNoFollow(Root);
		if (OpenRoot.IsError())
		{
			return false;
		}
		bool bPreferencesCreated = false;
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Preferences =
			OpenOrCreateNativeDirectoryAtNoFollow(
				OpenRoot.GetValue(), TEXT("Preferences"), bPreferencesCreated);
		if (Preferences.IsError())
		{
			return false;
		}
#if PLATFORM_MAC
		return !SetNativeMode(Preferences.GetValue(), 0700).IsError();
#else
		return true;
#endif
	}
#endif
	return IFileManager::Get().MakeDirectory(*Directory, true);
}

bool IsGuardedPreferencesDirectory(const FString& Directory)
{
#if WOT_GUARDED_BOOTSTRAP
	const FString Root = WoTUserDataRoot();
	if (Root.IsEmpty())
	{
		return false;
	}
	FString Expected = FPaths::Combine(Root, TEXT("Preferences"));
	FString Requested = Directory;
	FPaths::NormalizeDirectoryName(Expected);
	FPaths::NormalizeDirectoryName(Requested);
	return Requested == Expected;
#else
	(void)Directory;
	return false;
#endif
}

TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenGuardedPreferencesDirectory(
	bool bCreate)
{
	using FResult = TWoTBootstrapResult<FWoTNativeDirectoryHandle>;
	const FString Root = WoTUserDataRoot();
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> RootHandle =
		OpenNativeDirectoryNoFollow(Root);
	if (RootHandle.IsError())
	{
		return FResult::Error(MoveTemp(RootHandle).GetError());
	}
	if (!bCreate)
	{
		return OpenNativeDirectoryAtNoFollow(
			RootHandle.GetValue(), TEXT("Preferences"));
	}
	bool bCreated = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Preferences =
		OpenOrCreateNativeDirectoryAtNoFollow(
			RootHandle.GetValue(), TEXT("Preferences"), bCreated);
#if PLATFORM_MAC
	if (!Preferences.IsError())
	{
		if (FWoTBootstrapStatus Mode = SetNativeMode(
				Preferences.GetValue(), 0700); Mode.IsError())
		{
			return FResult::Error(MoveTemp(Mode).GetError());
		}
	}
#endif
	return Preferences;
}

// A fresh data root has no Preferences folder until the first save creates it,
// and nothing before cache activation saves. A read or remove must treat the
// missing folder as "no child yet": opening it with bCreate=false failed with
// "file not found", so ApplyFullPreferencesAfterCacheActivation failed and a
// fresh install stopped at PAYLOAD-CAPABILITY-ABSENT. A Preferences entry
// that exists but is not a directory still fails in the open that follows.
TWoTBootstrapResult<bool> GuardedPreferencesDirectoryExists()
{
	using FResult = TWoTBootstrapResult<bool>;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> RootHandle =
		OpenNativeDirectoryNoFollow(WoTUserDataRoot());
	if (RootHandle.IsError())
	{
		return FResult::Error(MoveTemp(RootHandle).GetError());
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
		QueryNativeEntryTypeAtNoFollow(RootHandle.GetValue(), TEXT("Preferences"));
	if (Found.IsError())
	{
		return FResult::Error(MoveTemp(Found).GetError());
	}
	return FResult::Success(Found.GetValue().IsSet());
}

FWoTBootstrapStatus ReadPreferencesChild(const FString& Directory,
	const FString& Path, uint64 MaxBytes, TArray<uint8>& OutBytes,
	bool& bOutExists)
{
	OutBytes.Reset();
	bOutExists = false;
	if (!IsGuardedPreferencesDirectory(Directory))
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size < 0)
		{
			return FWoTBootstrapStatus::Success();
		}
		bOutExists = true;
		if (static_cast<uint64>(Size) > MaxBytes
			|| !FFileHelper::LoadFileToArray(OutBytes, *Path))
		{
			return PreferencesError(EWoTBootstrapError::Io,
				TEXT("the preference child could not be read within its bound"));
		}
		return FWoTBootstrapStatus::Success();
	}
	TWoTBootstrapResult<bool> DirectoryExists =
		GuardedPreferencesDirectoryExists();
	if (DirectoryExists.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(DirectoryExists).GetError());
	}
	if (!DirectoryExists.GetValue())
	{
		return FWoTBootstrapStatus::Success();
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Preferences =
		OpenGuardedPreferencesDirectory(false);
	if (Preferences.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Preferences).GetError());
	}
	const FString Leaf = FPaths::GetCleanFilename(Path);
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
		QueryNativeEntryTypeAtNoFollow(Preferences.GetValue(), Leaf);
	if (Found.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Found).GetError());
	}
	if (!Found.GetValue().IsSet())
	{
		return FWoTBootstrapStatus::Success();
	}
	bOutExists = true;
	if (Found.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		return PreferencesError(EWoTBootstrapError::UnsafePath,
			TEXT("the preference child is not a regular file"));
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> File =
		OpenNativeFileAtReadNoFollow(Preferences.GetValue(), Leaf);
	if (File.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(File).GetError());
	}
	if (File.GetValue().GetIdentity().LinkCount != 1
		|| File.GetValue().GetIdentity().Size > MaxBytes)
	{
		return PreferencesError(EWoTBootstrapError::UnsafePath,
			TEXT("the preference child identity is unsafe"));
	}
	TWoTBootstrapResult<TArray<uint8>> Read = File.GetValue().ReadAll();
	if (Read.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Read).GetError());
	}
	OutBytes = MoveTemp(Read).GetValue();
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus WritePreferencesChild(const FString& Directory,
	const FString& Path, TConstArrayView<uint8> Bytes)
{
	if (!IsGuardedPreferencesDirectory(Directory))
	{
		if (!PrepareFixedPreferencesDirectory(Directory))
		{
			return PreferencesError(EWoTBootstrapError::Io,
				TEXT("the fixed preference directory could not be prepared"));
		}
		const FString Temporary = Path + TEXT(".tmp");
		if (!FFileHelper::SaveArrayToFile(Bytes, *Temporary)
			|| !IFileManager::Get().Move(*Path, *Temporary,
				/*Replace*/ true, /*EvenIfReadOnly*/ false,
				/*Attributes*/ false, /*DoNotRetryOrError*/ true))
		{
			IFileManager::Get().Delete(*Temporary, false, true, true);
			return PreferencesError(EWoTBootstrapError::Durability,
				TEXT("the preference child replacement could not be published"));
		}
		return FWoTBootstrapStatus::Success();
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Preferences =
		OpenGuardedPreferencesDirectory(true);
	if (Preferences.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Preferences).GetError());
	}
	FWoTNativeDirectoryHandle& DirectoryHandle = Preferences.GetValue();
	const FString Leaf = FPaths::GetCleanFilename(Path);
	const FString TemporaryLeaf = FString::Printf(TEXT("%s.%u.tmp"), *Leaf,
		FPlatformProcess::GetCurrentProcessId());
	auto Find = [&DirectoryHandle](const FString& Name)
	{
		return QueryNativeEntryTypeAtNoFollow(DirectoryHandle, Name);
	};
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Stale = Find(TemporaryLeaf);
	if (Stale.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Stale).GetError());
	}
	if (Stale.GetValue().IsSet())
	{
		if (Stale.GetValue().GetValue() != EWoTTreeEntryType::File)
		{
			return PreferencesError(EWoTBootstrapError::UnsafePath,
				TEXT("the preference temporary is not a regular file"));
		}
		if (FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollow(
				DirectoryHandle, TemporaryLeaf, EWoTTreeEntryType::File);
			Removed.IsError())
		{
			return Removed;
		}
	}
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Existing = Find(Leaf);
	if (Existing.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Existing).GetError());
	}
	if (Existing.GetValue().IsSet()
		&& Existing.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		return PreferencesError(EWoTBootstrapError::UnsafePath,
			TEXT("the preference destination is not a regular file"));
	}
	TWoTBootstrapResult<FWoTNativeFileHandle> Temporary =
		CreateNativeFileAtWriteNoFollow(DirectoryHandle, TemporaryLeaf);
	if (Temporary.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Temporary).GetError());
	}
	FWoTBootstrapStatus Written = WriteNativeFile(Temporary.GetValue(), Bytes);
	if (!Written.IsError())
	{
		Written = FlushNativeFile(Temporary.GetValue());
	}
	Temporary.GetValue() = FWoTNativeFileHandle();
	if (Written.IsError())
	{
		(void)RemoveNativeEntryAtNoFollow(
			DirectoryHandle, TemporaryLeaf, EWoTTreeEntryType::File);
		return Written;
	}
	FWoTBootstrapStatus Published = FWoTBootstrapStatus::Success();
	Published = RenameNativeEntryAtNoFollow(
			DirectoryHandle, TemporaryLeaf, DirectoryHandle, Leaf,
			Existing.GetValue().IsSet());
	if (Published.IsError())
	{
		(void)RemoveNativeEntryAtNoFollow(
			DirectoryHandle, TemporaryLeaf, EWoTTreeEntryType::File);
		return Published;
	}
	FWoTBootstrapStatus Barrier = FWoTBootstrapStatus::Success();
	Barrier = FlushNativeDirectory(DirectoryHandle);
	return Barrier;
}

FWoTBootstrapStatus RemovePreferencesChild(const FString& Directory,
	const FString& Path)
{
	if (!IsGuardedPreferencesDirectory(Directory))
	{
		return !IFileManager::Get().FileExists(*Path)
			|| IFileManager::Get().Delete(*Path, false, true, true)
			? FWoTBootstrapStatus::Success()
			: PreferencesError(EWoTBootstrapError::Durability,
				TEXT("the preference child could not be removed"));
	}
	TWoTBootstrapResult<bool> DirectoryExists =
		GuardedPreferencesDirectoryExists();
	if (DirectoryExists.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(DirectoryExists).GetError());
	}
	if (!DirectoryExists.GetValue())
	{
		return FWoTBootstrapStatus::Success();
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Preferences =
		OpenGuardedPreferencesDirectory(false);
	if (Preferences.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Preferences).GetError());
	}
	const FString Leaf = FPaths::GetCleanFilename(Path);
	TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> Found =
		QueryNativeEntryTypeAtNoFollow(Preferences.GetValue(), Leaf);
	if (Found.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Found).GetError());
	}
	if (!Found.GetValue().IsSet())
	{
		return FWoTBootstrapStatus::Success();
	}
	if (Found.GetValue().GetValue() != EWoTTreeEntryType::File)
	{
		return PreferencesError(EWoTBootstrapError::UnsafePath,
			TEXT("the preference child to remove is not a regular file"));
	}
	FWoTBootstrapStatus Removed = RemoveNativeEntryAtNoFollow(
		Preferences.GetValue(), Leaf, EWoTTreeEntryType::File);
	if (Removed.IsError())
	{
		return Removed;
	}
	FWoTBootstrapStatus Barrier = FWoTBootstrapStatus::Success();
	Barrier = FlushNativeDirectory(Preferences.GetValue());
	return Barrier;
}

bool ParseFaction(TConstArrayView<uint8> Bytes, EWoTPlayerFaction& OutFaction)
{
	OutFaction = EWoTPlayerFaction::AesSedai;
	if (Bytes.Num() > 4096)
	{
		return true;
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return true;
		}
	}
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	constexpr ANSICHAR Prefix[] = "SelectedFaction=";
	int32 Count = 0;
	FAnsiStringView Found;
	int32 Start = 0;
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		if (Data[Index] != '\n')
		{
			continue;
		}
		int32 Length = Index - Start;
		if (Length > 0 && Data[Index - 1] == '\r') { --Length; }
		FAnsiStringView Line(Data + Start, Length);
		if (Line.StartsWith(FAnsiStringView(Prefix)))
		{
			++Count;
			Found = FAnsiStringView(
				Line.GetData() + sizeof(Prefix) - 1,
				Line.Len() - (sizeof(Prefix) - 1));
		}
		Start = Index + 1;
	}
	if (Count != 1)
	{
		return true;
	}
	struct FFactionName
	{
		const ANSICHAR* Name;
		EWoTPlayerFaction Faction;
	};
	static const FFactionName Names[] = {
		{"AesSedai", EWoTPlayerFaction::AesSedai},
		{"Forsaken", EWoTPlayerFaction::Forsaken},
		{"Hound", EWoTPlayerFaction::Hound},
		{"Whitecloak", EWoTPlayerFaction::Whitecloak},
	};
	for (const FFactionName& Name : Names)
	{
		if (Found.Equals(FAnsiStringView(Name.Name), ESearchCase::CaseSensitive))
		{
			OutFaction = Name.Faction;
			break;
		}
	}
	return true;
}

EWoTPlayerSkinColor ParseSkinColor(TConstArrayView<uint8> Bytes,
	FAnsiStringView Prefix)
{
	if (Bytes.Num() > 4096)
	{
		return EWoTPlayerSkinColor::Base;
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return EWoTPlayerSkinColor::Base;
		}
	}
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	int32 Count = 0;
	FAnsiStringView Found;
	int32 Start = 0;
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		if (Data[Index] != '\n')
		{
			continue;
		}
		int32 Length = Index - Start;
		if (Length > 0 && Data[Index - 1] == '\r') { --Length; }
		const FAnsiStringView Line(Data + Start, Length);
		if (Line.StartsWith(Prefix))
		{
			++Count;
			Found = FAnsiStringView(Line.GetData() + Prefix.Len(),
				Line.Len() - Prefix.Len());
		}
		Start = Index + 1;
	}
	if (Count != 1)
	{
		return EWoTPlayerSkinColor::Base;
	}
	struct FSkinName
	{
		const ANSICHAR* Name;
		EWoTPlayerSkinColor Color;
	};
	static const FSkinName Names[] = {
		{"Black", EWoTPlayerSkinColor::Black},
		{"Blue", EWoTPlayerSkinColor::Blue},
		{"Red", EWoTPlayerSkinColor::Red},
		{"Green", EWoTPlayerSkinColor::Green},
		{"Base", EWoTPlayerSkinColor::Base},
	};
	for (const FSkinName& Name : Names)
	{
		if (Found.Equals(FAnsiStringView(Name.Name), ESearchCase::CaseSensitive))
		{
			return Name.Color;
		}
	}
	return EWoTPlayerSkinColor::Base;
}

bool ParseAlwaysUseEpicRelay(TConstArrayView<uint8> Bytes)
{
	if (Bytes.Num() > 4096)
	{
		return false;
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return false;
		}
	}
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	constexpr ANSICHAR Prefix[] = "AlwaysUseEpicRelay=";
	int32 Count = 0;
	bool bForceRelays = false;
	int32 Start = 0;
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		if (Data[Index] != '\n')
		{
			continue;
		}
		int32 Length = Index - Start;
		if (Length > 0 && Data[Index - 1] == '\r') { --Length; }
		const FAnsiStringView Line(Data + Start, Length);
		if (Line.StartsWith(FAnsiStringView(Prefix)))
		{
			++Count;
			const FAnsiStringView Value(
				Line.GetData() + sizeof(Prefix) - 1,
				Line.Len() - (sizeof(Prefix) - 1));
			bForceRelays = Value.Equals(FAnsiStringView("1"),
				ESearchCase::CaseSensitive);
		}
		Start = Index + 1;
	}
	return Count == 1 && bForceRelays;
}

FString ParsePlayerName(TConstArrayView<uint8> Bytes)
{
	if (Bytes.IsEmpty() || Bytes.Num() > 4096)
	{
		return FString();
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return FString();
		}
	}

	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	constexpr ANSICHAR Prefix[] = "PlayerName=";
	int32 Count = 0;
	FAnsiStringView Found;
	int32 Start = 0;
	// Like the EarlyUI codec, a final unterminated (torn) record is ignored.
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		if (Data[Index] != '\n')
		{
			continue;
		}
		int32 Length = Index - Start;
		if (Length > 0 && Data[Index - 1] == '\r') { --Length; }
		const FAnsiStringView Line(Data + Start, Length);
		if (Line.StartsWith(FAnsiStringView(Prefix)))
		{
			++Count;
			Found = FAnsiStringView(
				Line.GetData() + sizeof(Prefix) - 1,
				Line.Len() - (sizeof(Prefix) - 1));
		}
		Start = Index + 1;
	}
	if (Count != 1)
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(Found.GetData(), Found.Len());
	return FWoTFrontEndModel::SanitizePlayerName(
		FString(Converted.Length(), Converted.Get()));
}

FString ParseSavedPasswordField(TConstArrayView<uint8> Bytes,
	const FString& Field)
{
	if (Bytes.IsEmpty() || Bytes.Num() > 4096)
	{
		return FString();
	}
	for (uint8 Byte : Bytes)
	{
		if (Byte == 0 || Byte > 0x7f)
		{
			return FString();
		}
	}
	const FString Prefix = Field + TEXT("=");
	const ANSICHAR* Data = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	int32 Count = 0;
	FString Found;
	int32 Start = 0;
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		if (Data[Index] != '\n')
		{
			continue;
		}
		int32 Length = Index - Start;
		if (Length > 0 && Data[Index - 1] == '\r')
		{
			--Length;
		}
		const FAnsiStringView Line(Data + Start, Length);
		FTCHARToUTF8 Utf8Prefix(*Prefix);
		const FAnsiStringView AnsiPrefix(Utf8Prefix.Get(), Utf8Prefix.Length());
		if (Line.StartsWith(AnsiPrefix))
		{
			++Count;
			const FAnsiStringView Value(Line.GetData() + AnsiPrefix.Len(),
				Line.Len() - AnsiPrefix.Len());
			const FUTF8ToTCHAR Converted(Value.GetData(), Value.Len());
			Found = FString(Converted.Length(), Converted.Get());
		}
		Start = Index + 1;
	}
	return Count == 1 ? Found : FString();
}

bool ValidSavedPasswordServer(const FString& Server)
{
	if (Server.IsEmpty() || Server.Len() > 128)
	{
		return false;
	}
	for (TCHAR Character : Server)
	{
		if (Character < 0x21 || Character > 0x7e || Character == TEXT('='))
		{
			return false;
		}
	}
	return true;
}

TArray<FWoTSavedServerPassword> ParseSavedPasswords(
	TConstArrayView<uint8> Bytes)
{
	TArray<FWoTSavedServerPassword> Result;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		const FString Server = ParseSavedPasswordField(Bytes,
			FString::Printf(TEXT("SavedPassword%dServer"), Index));
		const FString Password = ParseSavedPasswordField(Bytes,
			FString::Printf(TEXT("SavedPassword%dValue"), Index));
		FString SanitizedPassword;
		if (!ValidSavedPasswordServer(Server)
			|| !FWoTFrontEndModel::SanitizeHostPassword(
				Password, SanitizedPassword))
		{
			continue;
		}
		Result.Add({Server, MoveTemp(SanitizedPassword)});
	}
	return Result;
}

bool PreferencesAreValid(const FWoTPlayerPreferences& Value)
{
	if (FWoTFrontEndModel::NormalizeFaction(Value.SelectedFaction)
		!= Value.SelectedFaction)
	{
		return false;
	}
	if (FWoTFrontEndModel::NormalizeSkinColor(Value.SelectedSkinColor)
		!= Value.SelectedSkinColor)
	{
		return false;
	}
	if (FWoTFrontEndModel::NormalizeSkinColor(Value.CitadelSkinColor)
		!= Value.CitadelSkinColor)
	{
		return false;
	}
	if (FWoTFrontEndModel::SanitizePlayerName(Value.PlayerName)
		!= Value.PlayerName)
	{
		return false;
	}
	if (Value.SavedPasswords.Num() > 10)
	{
		return false;
	}
	for (const FWoTSavedServerPassword& Saved : Value.SavedPasswords)
	{
		FString SanitizedPassword;
		if (!ValidSavedPasswordServer(Saved.Server)
			|| !FWoTFrontEndModel::SanitizeHostPassword(
				Saved.Password, SanitizedPassword)
			|| SanitizedPassword != Saved.Password)
		{
			return false;
		}
	}
	TArray<uint8> Serialized = SerializePreferences(Value,
		FWoTGameSettings::Defaults());
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
		Serialized, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
	return !Parsed.IsError() && Parsed.GetValue().Get() == Value.EarlyUI;
}

FWoTBootstrapStatus WritePreferenceFile(const FWoTPlayerPreferences& Value,
	const FWoTGameSettings& GameSettings)
{
	FString Directory;
	FString Leaf;
	if (!FixedPreferencesPath(Directory, Leaf))
	{
		return PreferencesError(EWoTBootstrapError::Io,
			TEXT("the fixed preference directory could not be prepared"));
	}
	const TArray<uint8> Bytes = SerializePreferences(Value, GameSettings);
	if (Bytes.Num() > static_cast<int32>(FWoTEarlyUILimits::Production().MaxSerializedBytes))
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the typed preference file exceeds its size limit"));
	}
	return WritePreferencesChild(Directory, Leaf, Bytes);
}
} // namespace

bool UWoTPreferencesSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
#if WOT_GUARDED_BOOTSTRAP
	return true;
#elif WITH_AUTOMATION_TESTS
	return bWoTPreferencesCreationEnabledForTesting;
#else
	return false;
#endif
}

void UWoTPreferencesSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Taken =
		GetEarlyUIAuthority().TakeOnce();
	if (Taken.IsError())
	{
	#if WOT_GUARDED_BOOTSTRAP
		(void)WoTWriteBootFailureReceipt(
			EWoTBootstrapDiagnosticCode::EarlyUINotAdopted,
			EWoTBootstrapPhase::EstablishingIdentity);
	#endif
		UE_LOG(LogTemp, Fatal, TEXT("Authenticated EarlyUI take failed: %s"),
			*Taken.GetError().ToString());
		return;
	}
#if WITH_AUTOMATION_TESTS
	if (bWoTPreferencesDelayEarlyUIAdoptionForTesting)
	{
		// Dependency-order fault injection: the authenticated value was really
		// taken, but adoption is deliberately late so the bootstrap subsystem must fail before
		// taking its own authority or starting work/UI.
		return;
	}
#endif
	FWoTBootstrapStatus Adopted = AdoptAuthenticatedEarlyUI(
		MoveTemp(Taken).GetValue());
	if (Adopted.IsError())
	{
	#if WOT_GUARDED_BOOTSTRAP
		(void)WoTWriteBootFailureReceipt(
			EWoTBootstrapDiagnosticCode::EarlyUINotAdopted,
			EWoTBootstrapPhase::EstablishingIdentity);
	#endif
		UE_LOG(LogTemp, Fatal, TEXT("Authenticated EarlyUI adoption failed: %s"),
			*Adopted.GetError().ToString());
	}
}

void UWoTPreferencesSubsystem::Deinitialize()
{
	bLoadedFullPreferences = false;
	Super::Deinitialize();
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::AdoptAuthenticatedEarlyUI(
	FWoTAuthenticatedEarlyUI&& Authenticated)
{
	if (bAdoptedEarlyUI)
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("authenticated EarlyUI adoption was replayed"));
	}
	const FWoTEarlyUIV1 Value = Authenticated.Get();
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetApplicationScale(Value.UIScale);
		if (FSlateApplication::Get().GetApplicationScale() != Value.UIScale)
		{
			return PreferencesError(EWoTBootstrapError::CorruptInstallation,
				TEXT("Slate refused the authenticated EarlyUI scale"));
		}
	}
	if (!FInternationalization::Get().SetCurrentCulture(LocaleName(Value.Locale)))
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the inventory-bound authenticated locale is unavailable"));
	}
	FWoTGameSettings BootstrapAudio = FWoTGameSettings::Defaults();
	BootstrapAudio.MasterVolume = Value.BootstrapVolume;
	FWoTGameSettingsAdapter::ApplyAudio(BootstrapAudio);
	// The durable master gain lives on every device's main submix. FApp remains
	// the authenticated early-UI feedback gate consumed before an audio device is
	// guaranteed to exist; the platform focus pump may later restore that stage.
	FApp::SetVolumeMultiplier(Value.BootstrapVolume);
	if (FWoTGameSettingsAdapter::GetAppliedMasterVolume() != Value.BootstrapVolume
		|| FApp::GetVolumeMultiplier() != Value.BootstrapVolume)
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the bootstrap audio projection was not observable"));
	}
	Preferences.EarlyUI = Value;
	Preferences.SelectedFaction = EWoTPlayerFaction::AesSedai;
	Preferences.SelectedSkinColor = EWoTPlayerSkinColor::Base;
	Preferences.CitadelSkinColor = EWoTPlayerSkinColor::Base;
	GameSettings = FWoTGameSettings::Defaults();
	// Reduced motion, contrast, screen-reader intent, and input sensitivity are
	// retained as direct typed bootstrap state for the code-only bootstrap Slate
	// flow; none is translated into a CVar, config key, object, or command.
	bAdoptedEarlyUI = true;
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus
UWoTPreferencesSubsystem::ApplyFullPreferencesAfterCacheActivation()
{
	if (!bAdoptedEarlyUI || bLoadedFullPreferences
		|| !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("full preferences require one observable verified-cache activation"));
	}
	FString Directory;
	FString Leaf;
	if (!FixedPreferencesPath(Directory, Leaf))
	{
		return PreferencesError(EWoTBootstrapError::Io,
			TEXT("the fixed player-preference location is unavailable"));
	}
	TArray<uint8> Bytes;
	bool bExists = false;
	FWoTBootstrapStatus Read = ReadPreferencesChild(Directory, Leaf,
		FWoTEarlyUILimits::Production().MaxSerializedBytes, Bytes, bExists);
	if (Read.IsError())
	{
		return Read;
	}
	(void)bExists;
	EWoTPlayerFaction Faction = EWoTPlayerFaction::AesSedai;
	ParseFaction(Bytes, Faction);
	const EWoTPlayerSkinColor SkinColor = ParseSkinColor(
		Bytes, FAnsiStringView("PlayerSkin="));
	const EWoTPlayerSkinColor CitadelSkinColor = ParseSkinColor(
		Bytes, FAnsiStringView("CitadelSkin="));
	const bool bAlwaysUseEpicRelay = ParseAlwaysUseEpicRelay(Bytes);
	const FString PlayerName = ParsePlayerName(Bytes);
	TArray<FWoTSavedServerPassword> SavedPasswords =
		ParseSavedPasswords(Bytes);
	FIntPoint WorkAreaPixels = FIntPoint::ZeroValue;
	float DpiScale = 1.0f;
	if (!GIsEditor && !IsRunningCommandlet() && !IsRunningDedicatedServer()
		&& FSlateApplication::IsInitialized())
	{
		FDisplayMetrics DisplayMetrics;
		FDisplayMetrics::RebuildDisplayMetrics(DisplayMetrics);
		const FPlatformRect& WorkArea = DisplayMetrics.PrimaryDisplayWorkAreaRect;
		WorkAreaPixels = FIntPoint(WorkArea.Right - WorkArea.Left,
			WorkArea.Bottom - WorkArea.Top);
		DpiScale = FPlatformApplicationMisc::GetDPIScaleFactorAtPoint(
			WorkArea.Left + WorkAreaPixels.X * 0.5f,
			WorkArea.Top + WorkAreaPixels.Y * 0.5f);
	}
	FWoTGameSettings ParsedSettings;
	FString ParseError;
	if (!FWoTGameSettingsCodec::Parse(Bytes, ParsedSettings, ParseError,
		WorkAreaPixels, DpiScale))
	{
		UE_LOG(LogTemp, Warning, TEXT("Invalid saved game settings; using defaults: %s"),
			*ParseError);
		ParsedSettings = FWoTGameSettings::Defaults(WorkAreaPixels, DpiScale);
	}
	FWoTBootstrapStatus AppliedFaction =
		AssignSelectedFactionAfterCacheActivation(Faction);
	if (AppliedFaction.IsError())
	{
		return AppliedFaction;
	}
	GameSettings = MoveTemp(ParsedSettings);
	Preferences.SelectedSkinColor = SkinColor;
	Preferences.CitadelSkinColor = CitadelSkinColor;
	Preferences.bAlwaysUseEpicRelay = bAlwaysUseEpicRelay;
	Preferences.PlayerName = PlayerName;
	Preferences.SavedPasswords = MoveTemp(SavedPasswords);
	bLoadedFullPreferences = true;
	FWoTGameSettingsAdapter::ApplyAudio(GameSettings);
	FWoTGameSettingsAdapter::ApplyVideoOnStartup(GameSettings);
	if (UGameInstance* Instance = GetGameInstance())
	{
		for (ULocalPlayer* Local : Instance->GetLocalPlayers())
		{
			if (Local && Local->PlayerController && Local->PlayerController->PlayerInput)
			{
				ApplyGameSettingsToInput(*Local->PlayerController->PlayerInput);
			}
		}
	}
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetSelectedFaction(
	EWoTPlayerFaction Faction, bool bSave)
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("faction preferences are dormant before cache activation"));
	}
	if (!bSave)
	{
		return AssignSelectedFactionAfterCacheActivation(Faction);
	}
	FWoTPlayerPreferences Candidate = Preferences;
	Candidate.SelectedFaction = FWoTFrontEndModel::NormalizeFaction(Faction);
	return Save(Candidate);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetPlayerName(
	const FString& Name, bool bSave)
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("player-name preferences are dormant before cache activation"));
	}
	const FString Sanitized = FWoTFrontEndModel::SanitizePlayerName(Name);
	if (!bSave)
	{
		Preferences.PlayerName = Sanitized;
		return FWoTBootstrapStatus::Success();
	}
	FWoTPlayerPreferences Candidate = Preferences;
	Candidate.PlayerName = Sanitized;
	return Save(Candidate);
}

bool UWoTPreferencesSubsystem::GetSavedServerPassword(
	const FString& Server, FString& OutPassword) const
{
	OutPassword.Reset();
	if (!bLoadedFullPreferences || !ValidSavedPasswordServer(Server))
	{
		return false;
	}
	const FWoTSavedServerPassword* Saved =
		Preferences.SavedPasswords.FindByPredicate(
			[&Server](const FWoTSavedServerPassword& Candidate)
			{
				return Candidate.Server.Equals(Server, ESearchCase::IgnoreCase);
			});
	if (!Saved)
	{
		return false;
	}
	OutPassword = Saved->Password;
	return true;
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SaveServerPassword(
	const FString& Server, const FString& Password)
{
	FString SanitizedPassword;
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive()
		|| !ValidSavedPasswordServer(Server)
		|| !FWoTFrontEndModel::SanitizeHostPassword(
			Password, SanitizedPassword))
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the saved server password is invalid or unavailable"));
	}
	FWoTPlayerPreferences Candidate = Preferences;
	Candidate.SavedPasswords.RemoveAll(
		[&Server](const FWoTSavedServerPassword& Saved)
		{
			return Saved.Server.Equals(Server, ESearchCase::IgnoreCase);
		});
	Candidate.SavedPasswords.Insert({Server, MoveTemp(SanitizedPassword)}, 0);
	if (Candidate.SavedPasswords.Num() > 10)
	{
		Candidate.SavedPasswords.SetNum(10);
	}
	return Save(Candidate);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetSelectedSkinColor(
	EWoTPlayerSkinColor Color, bool bSave)
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("skin preferences are dormant before cache activation"));
	}
	const EWoTPlayerSkinColor Normalized =
		FWoTFrontEndModel::NormalizeSkinColor(Color);
	if (!bSave)
	{
		Preferences.SelectedSkinColor = Normalized;
		return FWoTBootstrapStatus::Success();
	}
	FWoTPlayerPreferences Candidate = Preferences;
	Candidate.SelectedSkinColor = Normalized;
	return Save(Candidate);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetCitadelSkinColor(
	EWoTPlayerSkinColor Color, bool bSave)
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("citadel skin preferences are dormant before cache activation"));
	}
	const EWoTPlayerSkinColor Normalized =
		FWoTFrontEndModel::NormalizeSkinColor(Color);
	if (!bSave)
	{
		Preferences.CitadelSkinColor = Normalized;
		return FWoTBootstrapStatus::Success();
	}
	FWoTPlayerPreferences Candidate = Preferences;
	Candidate.CitadelSkinColor = Normalized;
	return Save(Candidate);
}

FWoTBootstrapStatus
UWoTPreferencesSubsystem::AssignSelectedFactionAfterCacheActivation(
	EWoTPlayerFaction Faction)
{
	if (!bAdoptedEarlyUI || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("faction assignment requires verified cache activation"));
	}
	Preferences.SelectedFaction = FWoTFrontEndModel::NormalizeFaction(Faction);
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::Save(
	const FWoTPlayerPreferences& NewPreferences)
{
	if (!bAdoptedEarlyUI || !PreferencesAreValid(NewPreferences)
		|| !(NewPreferences.EarlyUI == Preferences.EarlyUI))
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("only the adopted typed preference domain may be persisted"));
	}
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("full preference persistence is dormant before cache activation"));
	}
	FWoTBootstrapStatus Written = WritePreferenceFile(NewPreferences, GameSettings);
	if (Written.IsError()) { return Written; }
	Preferences = NewPreferences;
	return FWoTBootstrapStatus::Success();
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SaveGameSettings(
	const FWoTGameSettings& NewSettings)
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("game settings are dormant before cache activation"));
	}
	FString Error;
	if (!NewSettings.Validate(Error))
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation, *Error);
	}
	FWoTBootstrapStatus Written = WritePreferenceFile(Preferences, NewSettings);
	if (Written.IsError()) { return Written; }
	GameSettings = NewSettings;
	FWoTGameSettingsAdapter::ApplyAudio(GameSettings);
	FWoTGameSettingsAdapter::ApplyBrightness(GameSettings.Brightness);
	if (UGameInstance* Instance = GetGameInstance())
	{
		for (ULocalPlayer* Local : Instance->GetLocalPlayers())
		{
			if (Local && Local->PlayerController && Local->PlayerController->PlayerInput)
			{
				ApplyGameSettingsToInput(*Local->PlayerController->PlayerInput);
			}
		}
	}
	return FWoTBootstrapStatus::Success();
}

namespace
{
// One-time notice markers live beside the preference file. Production:
// <preference dir>/<Name>. A test-overridden preference leaf owns its own
// markers so specs never see the player's.
bool NoticeMarkerPath(const TCHAR* Name, FString& OutDirectory, FString& OutMarker)
{
	FString Leaf;
	if (!FixedPreferencesPath(OutDirectory, Leaf))
	{
		return false;
	}
	if (FCString::Strcmp(Name, TEXT("WoTUpdateFailedNotice")) == 0)
	{
		OutMarker = FPaths::Combine(OutDirectory, Name);
		return true;
	}
#if WITH_AUTOMATION_TESTS
	if (!WoTPreferencePathForTesting.IsEmpty())
	{
		OutMarker = Leaf + TEXT(".") + Name;
		return true;
	}
#endif
	OutMarker = FPaths::Combine(OutDirectory, Name);
	return true;
}

bool HasNoticeMarker(const TCHAR* Name, const TCHAR* Text)
{
	FString Directory;
	FString Marker;
	TArray<uint8> Bytes;
	bool bExists = false;
	if (!NoticeMarkerPath(Name, Directory, Marker)
		|| ReadPreferencesChild(Directory, Marker, 255, Bytes, bExists).IsError()
		|| !bExists || Bytes.IsEmpty())
	{
		return false;
	}
	FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return FString(Converted.Length(), Converted.Get()) == Text;
}

FWoTBootstrapStatus SetNoticeMarker(const TCHAR* Name, const TCHAR* Text,
	const TCHAR* What, bool bSet)
{
#if WITH_AUTOMATION_TESTS
	if (bSet && bWoTUpdateFailedNoticePersistenceFailureForTesting
		&& FCString::Strcmp(Name, TEXT("WoTUpdateFailedNotice")) == 0)
	{
		return PreferencesError(EWoTBootstrapError::Durability,
			TEXT("the update-failed notice persistence failure was injected"));
	}
#endif
	FString Directory;
	FString Marker;
	if (!NoticeMarkerPath(Name, Directory, Marker))
	{
		return PreferencesError(EWoTBootstrapError::Io,
			TEXT("the fixed preference directory could not be resolved"));
	}
	if (!bSet)
	{
		if (FWoTBootstrapStatus Removed = RemovePreferencesChild(
				Directory, Marker); Removed.IsError())
		{
			return Removed;
		}
		return FWoTBootstrapStatus::Success();
	}
	FTCHARToUTF8 Utf8(Text);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	if (FWoTBootstrapStatus Written = WritePreferencesChild(
			Directory, Marker, Bytes); Written.IsError())
	{
		return Written;
	}
	(void)What;
	return FWoTBootstrapStatus::Success();
}

constexpr const TCHAR* WoTOnlineIdentityMarkerName = TEXT("WoTOnlineIdentity");
constexpr const TCHAR* WoTOnlineIdentityMarkerText =
	TEXT("OnlineIdentityDisclosure=v1\n");
constexpr const TCHAR* WoTLocalNetworkMarkerName = TEXT("WoTLocalNetworkHeadsUp");
constexpr const TCHAR* WoTLocalNetworkMarkerText =
	TEXT("LocalNetworkHeadsUp=v1\n");
constexpr const TCHAR* WoTReleaseNotesMarkerName = TEXT("WoTReleaseNotesSeen");
constexpr const TCHAR* WoTUpdateFailedMarkerName = TEXT("WoTUpdateFailedNotice");

bool ParseUpdateFailedMarker(const FString& Text, uint64& OutFailed,
	uint64& OutRestored)
{
	if (!Text.EndsWith(TEXT("\n"), ESearchCase::CaseSensitive)) return false;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	if (Lines.Num() == 5 && Lines.Last().IsEmpty()) Lines.Pop();
	if (Lines.Num() != 4 || Lines[0] != TEXT("WUF1")
		|| !Lines[1].StartsWith(TEXT("failed="), ESearchCase::CaseSensitive)
		|| !Lines[2].StartsWith(TEXT("restored="), ESearchCase::CaseSensitive)
		|| Lines[3] != TEXT("state=pending"))
	{
		return false;
	}
	const FString FailedText = Lines[1].Mid(7);
	const FString RestoredText = Lines[2].Mid(9);
	if (FailedText.IsEmpty() || RestoredText.IsEmpty()
		|| !FailedText.IsNumeric() || !RestoredText.IsNumeric())
	{
		return false;
	}
	OutFailed = FCString::Strtoui64(*FailedText, nullptr, 10);
	OutRestored = FCString::Strtoui64(*RestoredText, nullptr, 10);
	// restored=0: the update rolled back to an unsequenced build.
	return OutFailed != 0;
}
}

bool UWoTPreferencesSubsystem::SideFilePath(const TCHAR* Name, FString& OutPath)
{
	FString Directory;
	return NoticeMarkerPath(Name, Directory, OutPath);
}

bool UWoTPreferencesSubsystem::LoadSideFileBytes(const FString& Path,
	uint64 MaxBytes, TArray<uint8>& OutBytes)
{
#if WITH_AUTOMATION_TESTS
	if (WoTSideFileAccessHookForTesting)
	{
		if (TOptional<bool> Result =
			WoTSideFileAccessHookForTesting(Path, false); Result.IsSet())
		{
			OutBytes.Reset();
			return Result.GetValue();
		}
	}
#endif
	const FString Directory = FPaths::GetPath(Path);
	if (Path.IsEmpty() || Directory.IsEmpty())
	{
		return false;
	}
	bool bExists = false;
	return !ReadPreferencesChild(
		Directory, Path, MaxBytes, OutBytes, bExists).IsError() && bExists;
}

bool UWoTPreferencesSubsystem::SaveSideFileBytes(const FString& Path,
	TConstArrayView<uint8> Bytes)
{
#if WITH_AUTOMATION_TESTS
	if (WoTSideFileAccessHookForTesting)
	{
		if (TOptional<bool> Result =
			WoTSideFileAccessHookForTesting(Path, true); Result.IsSet())
		{
			return Result.GetValue();
		}
	}
#endif
	const FString Directory = FPaths::GetPath(Path);
	return !Path.IsEmpty() && !Directory.IsEmpty()
		&& !WritePreferencesChild(Directory, Path, Bytes).IsError();
}

bool UWoTPreferencesSubsystem::HasAcknowledgedOnlineIdentity() const
{
	return bLoadedFullPreferences && IsWoTPayloadBootstrapActive()
		&& HasNoticeMarker(WoTOnlineIdentityMarkerName, WoTOnlineIdentityMarkerText);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetOnlineIdentityAcknowledged(
	bool bAcknowledged)
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("online identity preferences are dormant before cache activation"));
	}
	return SetNoticeMarker(WoTOnlineIdentityMarkerName, WoTOnlineIdentityMarkerText,
		TEXT("online identity"), bAcknowledged);
}

bool UWoTPreferencesSubsystem::HasSeenLocalNetworkHeadsUp() const
{
	return bLoadedFullPreferences && IsWoTPayloadBootstrapActive()
		&& HasNoticeMarker(WoTLocalNetworkMarkerName, WoTLocalNetworkMarkerText);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetLocalNetworkHeadsUpSeen()
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("local network preferences are dormant before cache activation"));
	}
	return SetNoticeMarker(WoTLocalNetworkMarkerName, WoTLocalNetworkMarkerText,
		TEXT("local network notice"), true);
}

bool UWoTPreferencesSubsystem::HasSeenReleaseNotes(
	const FString& ReleaseKey) const
{
	if (ReleaseKey.IsEmpty() || !bLoadedFullPreferences
		|| !IsWoTPayloadBootstrapActive())
	{
		return false;
	}
	const FString MarkerText = FString::Printf(
		TEXT("ReleaseNotesSeen=%s\n"), *ReleaseKey);
	return HasNoticeMarker(WoTReleaseNotesMarkerName, *MarkerText);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetReleaseNotesSeen(
	const FString& ReleaseKey)
{
	if (ReleaseKey.IsEmpty())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("an empty release key cannot be recorded"));
	}
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("release-note preferences are dormant before cache activation"));
	}
	const FString MarkerText = FString::Printf(
		TEXT("ReleaseNotesSeen=%s\n"), *ReleaseKey);
	return SetNoticeMarker(WoTReleaseNotesMarkerName, *MarkerText,
		TEXT("release notes"), true);
}

bool UWoTPreferencesSubsystem::GetPendingUpdateFailedNotice(
	uint64& OutFailedRelease, uint64& OutRestoredRelease) const
{
	OutFailedRelease = 0;
	OutRestoredRelease = 0;
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive()) return false;
	FString Directory;
	FString Marker;
	TArray<uint8> Bytes;
	bool bExists = false;
	if (!NoticeMarkerPath(WoTUpdateFailedMarkerName, Directory, Marker)
		|| ReadPreferencesChild(Directory, Marker, 255, Bytes, bExists).IsError()
		|| !bExists || Bytes.IsEmpty())
	{
		return false;
	}
	FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	return ParseUpdateFailedMarker(
		FString(Converted.Length(), Converted.Get()),
		OutFailedRelease, OutRestoredRelease);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetUpdateFailedNoticeSeen(
	uint64 FailedRelease)
{
	if (FailedRelease == 0 || !bLoadedFullPreferences
		|| !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("the update-failed notice cannot be acknowledged"));
	}
	const FString Text = FString::Printf(
		TEXT("WUF1\nfailed=%llu\nstate=seen\n"), FailedRelease);
	return SetNoticeMarker(WoTUpdateFailedMarkerName, *Text,
		TEXT("update-failed notice"), true);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::ApplyGameSettingsToInput(
	UPlayerInput& Input) const
{
	if (!bLoadedFullPreferences || !IsWoTPayloadBootstrapActive())
	{
		return PreferencesError(EWoTBootstrapError::CorruptInstallation,
			TEXT("game input settings are dormant before cache activation"));
	}
	GameSettings.ApplyToInput(Input);
	return FWoTBootstrapStatus::Success();
}

#if WITH_AUTOMATION_TESTS
void UWoTPreferencesSubsystem::SetCreationEnabledForTesting(bool bEnabled)
{
	bWoTPreferencesCreationEnabledForTesting = bEnabled;
}

void UWoTPreferencesSubsystem::SetDelayEarlyUIAdoptionForTesting(bool bEnabled)
{
	bWoTPreferencesDelayEarlyUIAdoptionForTesting = bEnabled;
}

TArray<uint8> UWoTPreferencesSubsystem::SerializeForTesting(
	const FWoTPlayerPreferences& Value)
{
	return SerializePreferences(Value, FWoTGameSettings::Defaults());
}

void UWoTPreferencesSubsystem::SetPreferencePathForTesting(const FString& Leaf)
{
	WoTPreferencePathForTesting = Leaf;
	WoTSetUpdateFailureNoticeDirectoryForTesting(
		Leaf.IsEmpty() ? FString() : FPaths::GetPath(Leaf));
}

bool UWoTPreferencesSubsystem::HasPreferencePathForTesting()
{
	return !WoTPreferencePathForTesting.IsEmpty();
}

void UWoTPreferencesSubsystem::SetSideFileAccessHookForTesting(
	TFunction<TOptional<bool>(const FString&, bool)>&& Hook)
{
	WoTSideFileAccessHookForTesting = MoveTemp(Hook);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::SetUpdateFailedNoticePendingForTesting(
	uint64 FailedRelease, uint64 RestoredRelease)
{
	if (FailedRelease == 0)
	{
		return PreferencesError(EWoTBootstrapError::InvalidNumber,
			TEXT("the test update-failed notice identity is invalid"));
	}
	const FString Text = FString::Printf(
		TEXT("WUF1\nfailed=%llu\nrestored=%llu\nstate=pending\n"),
		FailedRelease, RestoredRelease);
	return SetNoticeMarker(WoTUpdateFailedMarkerName, *Text,
		TEXT("test update-failed notice"), true);
}

void UWoTPreferencesSubsystem::
SetUpdateFailedNoticePersistenceFailureForTesting(bool bFail)
{
	bWoTUpdateFailedNoticePersistenceFailureForTesting = bFail;
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::ReadPreferenceChildForTesting(
	const FString& Path, TArray<uint8>& OutBytes, bool& bOutExists)
{
	bOutExists = false;
	return ReadPreferencesChild(FPaths::GetPath(Path), Path,
		FWoTEarlyUILimits::Production().MaxSerializedBytes, OutBytes, bOutExists);
}

FWoTBootstrapStatus UWoTPreferencesSubsystem::RemovePreferenceChildForTesting(
	const FString& Path)
{
	return RemovePreferencesChild(FPaths::GetPath(Path), Path);
}
#endif
