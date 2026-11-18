#include "WoTRetailText.h"

#include "Inventory/WoTInventory.h"
#include "WoTCharacter.h"
#include "Payload/WoTNeutralResources.h"
#include "Payload/WoTRuntimeLocalizationDefaults.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
struct FWoTRetailTextState
{
	TMap<FString, FWoTIntFile> Packages;
	bool bLoaded = false;
	// A failed load (the authenticated registry not adopted yet, or no retail
	// reference tree in a development checkout) is retried at most this often,
	// so a spawn burst cannot re-read five packages per inventory actor.
	double LastFailedAttempt = -1.0e9;
#if WITH_AUTOMATION_TESTS
	bool bInjected = false;
	TMap<FString, TArray<uint8>> Injected;
#endif
};

FWoTRetailTextState& WoTRetailTextState()
{
	static FWoTRetailTextState State;
	return State;
}

const TCHAR* const GWoTRetailTextPackages[] = {
	// Engine.Inventory (PickupMessage), WOT (AngrealInventory, Seal, SealInventory),
	// Angreal (every AngrealInv*), WoTTraps (trap inventories), WoTPawns (troop
	// inventories, LegionInvSeeker, MyrddraalSwordAngreal).
	TEXT("Engine"), TEXT("WoT"), TEXT("Angreal"), TEXT("WoTTraps"), TEXT("WoTPawns")
};

bool WoTRetailTextReadPackage(const FString& Package, TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
#if WITH_AUTOMATION_TESTS
	FWoTRetailTextState& State = WoTRetailTextState();
	if (State.bInjected)
	{
		if (const TArray<uint8>* Bytes = State.Injected.Find(Package))
		{
			OutBytes = *Bytes;
			return true;
		}
		return false;
	}
#endif
	// The authenticated cache: the same verified localization reader the menu
	// strings use (FWoTMenuStyle::LoadAuthenticatedMainMenuLocalization).
	{
		FWoTRuntimeLocalizationDefaults Defaults;
		const FWoTBootstrapStatus Status = Defaults.LoadLocalizationFirstUse(
			FWoTNeutralResourceId{
				FString::Printf(TEXT("cache/localization/%s.int"), *Package),
				EWoTNeutralResourceKind::Localization});
		if (!Status.IsError() && !Defaults.LocalizationBytes().IsEmpty())
		{
			OutBytes.Append(Defaults.LocalizationBytes().GetData(),
				Defaults.LocalizationBytes().Num());
			return true;
		}
	}
#if !WOT_GUARDED_BOOTSTRAP
	// Development builds read the purchaser's retail files directly.
	const FString Directory = FWoTRetailText::ReferenceSystemDirectory();
	if (!Directory.IsEmpty())
	{
		return FFileHelper::LoadFileToArray(OutBytes,
			*FPaths::Combine(Directory, Package + TEXT(".int")), FILEREAD_Silent);
	}
#endif
	return false;
}

bool WoTRetailTextSectionPackage(const FWoTRetailTextState& State,
	const FString& Section, const FWoTIntFile*& OutFile)
{
	OutFile = nullptr;
	int32 Matches = 0;
	for (const TPair<FString, FWoTIntFile>& Package : State.Packages)
	{
		if (Package.Value.FindSection(Section) != nullptr)
		{
			OutFile = &Package.Value;
			++Matches;
		}
	}
	// A section name must identify one package, as a UE1 class name identifies
	// the one package that declares it.
	if (Matches != 1)
	{
		OutFile = nullptr;
	}
	return OutFile != nullptr;
}

// UE1 CDO order: a class's defaultproperties override its parent's (already
// localized) values, then its own .int section overrides those, but only with a
// non-empty value. Where a bound section leaves a key EMPTY
// while the class's defaultproperties set it, that shipped default stands. These
// are the only such keys on ported classes (placeholders, not prose).
struct FWoTStandingDefault
{
	const TCHAR* Section;
	const TCHAR* Key;
	const TCHAR* Value;
};
const FWoTStandingDefault GWoTStandingDefaults[] = {
	{TEXT("LegionInvSeeker"), TEXT("PickupMessage"), TEXT("")},      // LegionInvSeeker.uc:28
	{TEXT("AngrealInvFireworks"), TEXT("Description"), TEXT("None")}, // AngrealInvFireworks.uc:24
	{TEXT("AngrealInvFireworks"), TEXT("Quote"), TEXT("None")},       // AngrealInvFireworks.uc:25
};

const FWoTStandingDefault* WoTRetailTextStandingDefault(FStringView Section, FStringView Key)
{
	for (const FWoTStandingDefault& Standing : GWoTStandingDefaults)
	{
		if (Section.Equals(Standing.Section, ESearchCase::IgnoreCase)
			&& Key.Equals(Standing.Key, ESearchCase::IgnoreCase))
		{
			return &Standing;
		}
	}
	return nullptr;
}

void WoTRetailTextNativeChain(const UClass* Class, TArray<const UClass*>& OutChain)
{
	OutChain.Reset();
	for (const UClass* It = Class; It != nullptr; It = It->GetSuperClass())
	{
		if (It->HasAnyClassFlags(CLASS_Native))
		{
			OutChain.Insert(It, 0);   // root first: a derived section overrides
		}
	}
}
}

const FWoTIntFile::FSection* FWoTIntFile::FindSection(FStringView Section) const
{
	for (const FSection& Candidate : Sections)
	{
		if (FStringView(Candidate.Name).Equals(Section, ESearchCase::IgnoreCase))
		{
			return &Candidate;
		}
	}
	return nullptr;
}

const FString* FWoTIntFile::Find(FStringView Section, FStringView Key) const
{
	const FSection* Found = FindSection(Section);
	if (Found == nullptr)
	{
		return nullptr;
	}
	for (int32 Index = Found->Entries.Num() - 1; Index >= 0; --Index)
	{
		if (FStringView(Found->Entries[Index].Key).Equals(Key, ESearchCase::IgnoreCase))
		{
			return &Found->Entries[Index].Value;
		}
	}
	return nullptr;
}

TConstArrayView<const TCHAR*> FWoTRetailText::BoundPackages()
{
	return MakeArrayView(GWoTRetailTextPackages);
}

bool FWoTRetailText::ParseIntBytes(TConstArrayView<uint8> Bytes, FWoTIntFile& Out)
{
	Out.Sections.Reset();
	FString Text;
	if (Bytes.Num() >= 2 && Bytes[0] == 0xff && Bytes[1] == 0xfe)
	{
		if ((Bytes.Num() % 2) != 0)
		{
			return false;
		}
		Text.Reserve(Bytes.Num() / 2);
		for (int32 Offset = 2; Offset < Bytes.Num(); Offset += 2)
		{
			const TCHAR Char = static_cast<TCHAR>(
				Bytes[Offset] | (static_cast<uint16>(Bytes[Offset + 1]) << 8));
			if (Char == 0)
			{
				return false;
			}
			Text.AppendChar(Char);
		}
	}
	else
	{
		Text.Reserve(Bytes.Num());
		for (const uint8 Byte : Bytes)
		{
			if (Byte == 0)
			{
				return false;
			}
			Text.AppendChar(static_cast<TCHAR>(Byte));   // appFromAnsi: 1:1 widen
		}
	}

	FWoTIntFile::FSection* Current = nullptr;
	int32 Position = 0;
	const int32 Length = Text.Len();
	while (Position < Length)
	{
		while (Position < Length && (Text[Position] == TEXT('\r') || Text[Position] == TEXT('\n')))
		{
			++Position;
		}
		const int32 Start = Position;
		while (Position < Length && Text[Position] != TEXT('\r') && Text[Position] != TEXT('\n'))
		{
			++Position;
		}
		const FString Line = Text.Mid(Start, Position - Start);
		if (Line.IsEmpty())
		{
			continue;
		}
		if (Line[0] == TEXT('[') && Line[Line.Len() - 1] == TEXT(']'))
		{
			const FString Name = Line.Mid(1, Line.Len() - 2);
			Current = nullptr;
			for (FWoTIntFile::FSection& Existing : Out.Sections)
			{
				if (Existing.Name.Equals(Name, ESearchCase::IgnoreCase))
				{
					Current = &Existing;   // a repeated section continues (Find-or-Set)
					break;
				}
			}
			if (Current == nullptr)
			{
				Current = &Out.Sections.AddDefaulted_GetRef();
				Current->Name = Name;
			}
			continue;
		}
		int32 Equals = INDEX_NONE;
		if (Current == nullptr || !Line.FindChar(TEXT('='), Equals))
		{
			continue;
		}
		FString Value = Line.Mid(Equals + 1);
		if (!Value.IsEmpty() && Value[0] == TEXT('"') && Value[Value.Len() - 1] == TEXT('"'))
		{
			Value = Value.Len() >= 2 ? Value.Mid(1, Value.Len() - 2) : FString();
		}
		Current->Entries.Emplace(Line.Left(Equals), MoveTemp(Value));
	}
	return true;
}

bool FWoTRetailText::EnsureLoaded()
{
	FWoTRetailTextState& State = WoTRetailTextState();
	if (State.bLoaded)
	{
		return true;
	}
	if (!IsInGameThread())
	{
		return false;
	}
	const double Now = FPlatformTime::Seconds();
#if WITH_AUTOMATION_TESTS
	if (!State.bInjected)
#endif
	{
		if (Now - State.LastFailedAttempt < 1.0)
		{
			return false;
		}
	}
	TMap<FString, FWoTIntFile> Loaded;
	for (const TCHAR* Package : GWoTRetailTextPackages)
	{
		TArray<uint8> Bytes;
		FWoTIntFile File;
		if (!WoTRetailTextReadPackage(Package, Bytes) || !ParseIntBytes(Bytes, File))
		{
			State.LastFailedAttempt = Now;   // all or nothing; retried later
			return false;
		}
		Loaded.Add(Package, MoveTemp(File));
	}
	State.Packages = MoveTemp(Loaded);
	State.bLoaded = true;

	// UE1 localized class defaults: every inventory and player-pawn class
	// default now carries the retail text, so CDO readers and every later spawn
	// agree with the authenticated source.
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if ((It->IsChildOf(AWoTInventory::StaticClass())
				|| It->IsChildOf(AWoTCharacter::StaticClass()))
			&& !It->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			ApplyLocalizedDefaults(It->GetDefaultObject());
		}
	}
	return true;
}

bool FWoTRetailText::IsLoaded()
{
	return WoTRetailTextState().bLoaded;
}

FString FWoTRetailText::RetailClassName(const UClass* Class)
{
	if (Class == nullptr)
	{
		return FString();
	}
	FString Name = Class->GetName();
	if (Name.StartsWith(TEXT("WoT"), ESearchCase::CaseSensitive))
	{
		Name.RightChopInline(3);
	}
	return Name;
}

TArray<FString> FWoTRetailText::RetailSections(const UClass* Class)
{
	const FString Name = Class ? Class->GetName() : FString();
	if (Name == TEXT("WoTGameMode"))
	{
		return {TEXT("giWOT"), TEXT("giCombatBase")};
	}
	if (Name == TEXT("WoTBattleGameMode"))
	{
		return {TEXT("giMPBattle"), TEXT("WOTPlayer")};
	}
	if (Name == TEXT("WoTCharacter"))
	{
		return {TEXT("WOTPlayer")};
	}
	return {RetailClassName(Class)};
}

const FString* FWoTRetailText::FindLocalized(const UClass* Class, FStringView Key)
{
	FWoTRetailTextState& State = WoTRetailTextState();
	if (!State.bLoaded || Class == nullptr)
	{
		return nullptr;
	}
	TArray<const UClass*> Chain;
	WoTRetailTextNativeChain(Class, Chain);
	for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
	{
		const TArray<FString> Sections = RetailSections(Chain[Index]);
		for (int32 SectionIndex = Sections.Num() - 1; SectionIndex >= 0; --SectionIndex)
		{
			const FWoTIntFile* File = nullptr;
			if (WoTRetailTextSectionPackage(State, Sections[SectionIndex], File))
			{
				// UE1 LoadLocalized: `if( *Text )` - an empty value is no override,
				// but the class's own shipped default then stands at this level.
				const FString* Value = File->Find(Sections[SectionIndex], Key);
				if (Value && !Value->IsEmpty())
				{
					return Value;
				}
				if (Value)
				{
					if (const FWoTStandingDefault* Standing =
							WoTRetailTextStandingDefault(Sections[SectionIndex], Key))
					{
						static const TArray<FString> StandingValues = []()
						{
							TArray<FString> Values;
							for (const FWoTStandingDefault& Entry : GWoTStandingDefaults)
							{
								Values.Add(Entry.Value);
							}
							return Values;
						}();
						return &StandingValues[
							static_cast<int32>(Standing - GWoTStandingDefaults)];
					}
				}
			}
		}
	}
	return nullptr;
}

const FString* FWoTRetailText::FindSectionValue(
	FStringView Section, FStringView Key)
{
	FWoTRetailTextState& State = WoTRetailTextState();
	if (!State.bLoaded)
	{
		return nullptr;
	}
	const FWoTIntFile* File = nullptr;
	return WoTRetailTextSectionPackage(State, FString(Section), File)
		? File->Find(Section, Key) : nullptr;
}

void FWoTRetailText::ApplyLocalizedDefaults(UObject* Object)
{
	if (!IsValid(Object) || !IsInGameThread() || !EnsureLoaded())
	{
		return;
	}
	FWoTRetailTextState& State = WoTRetailTextState();
	UClass* Class = Object->GetClass();
	TArray<const UClass*> Chain;
	WoTRetailTextNativeChain(Class, Chain);
	for (const UClass* Native : Chain)
	{
		for (const FString& Section : RetailSections(Native))
		{
			const FWoTIntFile* File = nullptr;
			if (!WoTRetailTextSectionPackage(State, Section, File))
			{
				continue;
			}
			const FWoTIntFile::FSection* Found = File->FindSection(Section);
			for (const TPair<FString, FString>& Entry : Found->Entries)
			{
				// UE1 LoadLocalized imports a
				// value only `if( *Text )`: an empty .int value leaves the class's
				// own default standing (its shipped defaultproperties, when it set
				// one; otherwise the inherited value).
				FString Value = Entry.Value;
				if (Value.IsEmpty())
				{
					const FWoTStandingDefault* Standing =
						WoTRetailTextStandingDefault(Section, Entry.Key);
					if (Standing == nullptr)
					{
						continue;
					}
					Value = Standing->Value;
				}
				if (FStrProperty* Property =
						FindFProperty<FStrProperty>(Class, FName(*Entry.Key)))
				{
					Property->SetPropertyValue_InContainer(Object, Value);
				}
			}
		}
	}
}

#if WITH_AUTOMATION_TESTS
void FWoTRetailText::SetPackageBytesForTesting(TMap<FString, TArray<uint8>> Packages)
{
	FWoTRetailTextState& State = WoTRetailTextState();
	State.bInjected = Packages.Num() > 0;
	State.Injected = MoveTemp(Packages);
	State.Packages.Reset();
	State.bLoaded = false;
	State.LastFailedAttempt = -1.0e9;
}
#endif

FString FWoTRetailText::ReferenceSystemDirectory()
{
#if WOT_GUARDED_BOOTSTRAP
	return FString();
#else
	const FString Root = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_REFERENCE_ROOT"));
	const FString Directory = Root.IsEmpty()
		? FPaths::Combine(FPaths::ProjectDir(), TEXT("original"), TEXT("System"))
		: FPaths::Combine(Root, TEXT("original"), TEXT("System"));
	return IFileManager::Get().DirectoryExists(*Directory)
		? FPaths::ConvertRelativePathToFull(Directory) : FString();
#endif
}
