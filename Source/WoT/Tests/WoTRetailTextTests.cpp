// Retail localized text (UE1 `var localized string`): read at runtime from the
// purchaser's .int files, resolved along the class chain like UE1 LoadLocalized,
// and never compiled into Source. These specs pin the .int grammar, the class-
// chain inheritance, the real retail values on every inventory class, and a scan
// that fails if retail prose is ever compiled back in.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../Inventory/WoTAngrealInvChampion.h"
#include "../Inventory/WoTAngrealInvDart.h"
#include "../Inventory/WoTAngrealInvFireworks.h"
#include "../Inventory/WoTLegionInvSeeker.h"
#include "../Inventory/WoTAngrealInvGuardian.h"
#include "../Inventory/WoTAngrealInvMinion.h"
#include "../Inventory/WoTAngrealInvSeeker.h"
#include "../Inventory/WoTAngrealInvTrapDetect.h"
#include "../Inventory/WoTInventory.h"
#include "../Inventory/WoTSeal.h"
#include "../Inventory/WoTTroopInventories.h"
#include "../Traps/WoTAlarm.h"
#include "../WoTCharacter.h"
#include "../WoTRetailText.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	TArray<uint8> WoTRetailTextSpecBytes(const ANSICHAR* Text)
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
		return Bytes;
	}

	FActorSpawnParameters WoTRetailTextSpawnParams()
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		return Params;
	}

	// The real retail packages parsed independently of the loader's state.
	bool WoTRetailTextSpecReference(TMap<FString, FWoTIntFile>& Out)
	{
		const FString Directory = FWoTRetailText::ReferenceSystemDirectory();
		if (Directory.IsEmpty())
		{
			return false;
		}
		for (const TCHAR* Package : FWoTRetailText::BoundPackages())
		{
			TArray<uint8> Bytes;
			FWoTIntFile File;
			if (!FFileHelper::LoadFileToArray(Bytes,
					*FPaths::Combine(Directory, FString(Package) + TEXT(".int")))
				|| !FWoTRetailText::ParseIntBytes(Bytes, File))
			{
				return false;
			}
			Out.Add(Package, MoveTemp(File));
		}
		return true;
	}

	FString WoTRetailTextSpecCollapse(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		bool bSpace = false;
		for (const TCHAR Char : In)
		{
			if (FChar::IsWhitespace(Char))
			{
				bSpace = true;
				continue;
			}
			if (bSpace && !Out.IsEmpty())
			{
				Out.AppendChar(TEXT(' '));
			}
			bSpace = false;
			Out.AppendChar(Char);
		}
		return Out;
	}

	// Source text with adjacent C++ string literals joined ("a" "b" -> ab, also
	// across lines) and whitespace collapsed, so a literal split over several
	// source lines is still found.
	FString WoTRetailTextSpecNormalizeSource(const FString& In)
	{
		FString Joined;
		Joined.Reserve(In.Len());
		for (int32 Index = 0; Index < In.Len(); ++Index)
		{
			if (In[Index] == TEXT('"'))
			{
				int32 Next = Index + 1;
				while (Next < In.Len() && FChar::IsWhitespace(In[Next]))
				{
					++Next;
				}
				if (Next > Index + 1 && Next < In.Len() && In[Next] == TEXT('"'))
				{
					Index = Next;   // skip the closing quote, the gap and the opening quote
					continue;
				}
			}
			Joined.AppendChar(In[Index]);
		}
		return WoTRetailTextSpecCollapse(Joined);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailTextIntGrammarSpec,
	"WoT.RetailText.IntGrammar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailTextIntGrammarSpec::RunTest(const FString&)
{
	// UE1's FConfigFile::Read: untrimmed
	// key/value split at the first '=', one pair of enclosing quotes removed,
	// case-insensitive names, a repeated section continues, the last duplicate
	// key wins, and bytes widen 1:1 (appFromAnsi).
	FWoTIntFile File;
	TestTrue(TEXT("synthetic ini parses"), FWoTRetailText::ParseIntBytes(
		WoTRetailTextSpecBytes(
			"[Alpha]\r\nTitle=One\r\nQuote=\"  spaced  \"\r\nKey = spaced key\r\n"
			"orphan\r\n\r\n[alpha]\r\nTitle=Two\n[Beta]\nX=a=b\nY=\"\nZ=\xe9t\xe9\n"),
		File));
	const FString* Title = File.Find(TEXT("ALPHA"), TEXT("title"));
	TestTrue(TEXT("a repeated section continues and the last key wins"),
		Title && *Title == TEXT("Two"));
	const FString* Quote = File.Find(TEXT("Alpha"), TEXT("Quote"));
	TestTrue(TEXT("enclosing quotes are removed, inner spaces kept"),
		Quote && *Quote == TEXT("  spaced  "));
	TestNull(TEXT("keys are not trimmed"), File.Find(TEXT("Alpha"), TEXT("Key")));
	const FString* Spaced = File.Find(TEXT("Alpha"), TEXT("Key "));
	TestTrue(TEXT("values are not trimmed"), Spaced && *Spaced == TEXT(" spaced key"));
	const FString* Split = File.Find(TEXT("Beta"), TEXT("X"));
	TestTrue(TEXT("only the first '=' splits"), Split && *Split == TEXT("a=b"));
	const FString* Lone = File.Find(TEXT("Beta"), TEXT("Y"));
	TestTrue(TEXT("a lone quote is both ends and strips to empty"),
		Lone && Lone->IsEmpty());
	const FString* Latin = File.Find(TEXT("Beta"), TEXT("Z"));
	TestTrue(TEXT("ANSI bytes widen 1:1"),
		Latin && Latin->Len() == 3 && (*Latin)[0] == TCHAR(0xe9));
	TestEqual(TEXT("two distinct sections"), File.Sections.Num(), 2);

	const uint8 Wide[] = {0xff, 0xfe, '[', 0, 'S', 0, ']', 0, '\n', 0,
		'K', 0, '=', 0, 'v', 0};
	FWoTIntFile WideFile;
	TestTrue(TEXT("a UTF-16LE .int parses"),
		FWoTRetailText::ParseIntBytes(MakeArrayView(Wide), WideFile));
	const FString* WideValue = WideFile.Find(TEXT("S"), TEXT("K"));
	TestTrue(TEXT("UTF-16 value"), WideValue && *WideValue == TEXT("v"));

	FWoTIntFile Rejected;
	const uint8 Nul[] = {'[', 'S', ']', '\n', 'K', '=', 0, '\n'};
	TestFalse(TEXT("an embedded NUL is rejected"),
		FWoTRetailText::ParseIntBytes(MakeArrayView(Nul), Rejected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailTextClassChainSpec,
	"WoT.RetailText.ClassChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailTextClassChainSpec::RunTest(const FString&)
{
	TestEqual(TEXT("UE1 class name of a port class"),
		FWoTRetailText::RetailClassName(AWoTAngrealInvSeeker::StaticClass()),
		FString(TEXT("AngrealInvSeeker")));
	TestEqual(TEXT("AWoTInventory stands for Engine.Inventory"),
		FWoTRetailText::RetailClassName(AWoTInventory::StaticClass()),
		FString(TEXT("Inventory")));

	// Synthetic packages: UE1 LoadLocalized resolves each key along the class
	// chain, the most-derived section that has it winning.
	TMap<FString, TArray<uint8>> Packages;
	Packages.Add(TEXT("Engine"), WoTRetailTextSpecBytes(
		"[Inventory]\nPickupMessage=engine pickup\n"));
	Packages.Add(TEXT("WoT"), WoTRetailTextSpecBytes(
		"[AngrealInventory]\nFailMessage=base fail\nPickupMessage=base pickup\n"
		"[Seal]\nPossesionChangeStr=seal taken\n"
		"[WOTPlayer]\nSelectedStr=mutation selected \n"
		"TerAngrealStr= mutation suffix \n"));
	Packages.Add(TEXT("Angreal"), WoTRetailTextSpecBytes(
		"[AngrealInvSeeker]\nTitle=seeker title\nFailMessage=seeker fail\n"
		"[AngrealInvDart]\nTitle=dart title\nQuote=\"dart quote\"\nPickupMessage=\n"));
	Packages.Add(TEXT("WoTTraps"), WoTRetailTextSpecBytes(
		"[AlarmInventory]\nTitle=alarm title\nDescription=alarm text\n"));
	Packages.Add(TEXT("WoTPawns"), WoTRetailTextSpecBytes(
		"[WarderInventory]\nTitle=warder title\n"));
	FWoTRetailText::SetPackageBytesForTesting(Packages);

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTAngrealInvSeeker* Seeker = World->SpawnActor<AWoTAngrealInvSeeker>(
		FVector(0, 0, 100), FRotator::ZeroRotator, WoTRetailTextSpawnParams());
	AWoTAngrealInvDart* Dart = World->SpawnActor<AWoTAngrealInvDart>(
		FVector(200, 0, 100), FRotator::ZeroRotator, WoTRetailTextSpawnParams());
	AWoTAlarmInventory* Alarm = World->SpawnActor<AWoTAlarmInventory>(
		FVector(400, 0, 100), FRotator::ZeroRotator, WoTRetailTextSpawnParams());
	AWoTWarderInventory* Warder = World->SpawnActor<AWoTWarderInventory>(
		FVector(600, 0, 100), FRotator::ZeroRotator, WoTRetailTextSpawnParams());
	AWoTCharacter* Character = World->SpawnActor<AWoTCharacter>(
		FVector(800, 0, 100), FRotator::ZeroRotator, WoTRetailTextSpawnParams());
	if (TestNotNull(TEXT("seeker"), Seeker) && TestNotNull(TEXT("dart"), Dart)
		&& TestNotNull(TEXT("alarm"), Alarm) && TestNotNull(TEXT("warder"), Warder)
		&& TestNotNull(TEXT("character"), Character))
	{
		TestEqual(TEXT("own section title"), Seeker->Title, FString(TEXT("seeker title")));
		TestEqual(TEXT("own section overrides the parent key"),
			Seeker->FailMessage, FString(TEXT("seeker fail")));
		TestEqual(TEXT("a key the class lacks comes from AngrealInventory"),
			Seeker->PickupMessage, FString(TEXT("base pickup")));
		TestEqual(TEXT("Dart inherits AngrealInventory's FailMessage"),
			Dart->FailMessage, FString(TEXT("base fail")));
		TestEqual(TEXT("quoted value"), Dart->Quote, FString(TEXT("dart quote")));
		TestEqual(TEXT("an empty value is no override: the parent's stands"),
			Dart->PickupMessage, FString(TEXT("base pickup")));
		TestEqual(TEXT("trap inventory text from WoTTraps.int"),
			Alarm->Description, FString(TEXT("alarm text")));
		TestEqual(TEXT("a WOTInventory subclass inherits Engine.Inventory's pickup"),
			Alarm->PickupMessage, FString(TEXT("engine pickup")));
		TestEqual(TEXT("troop inventory text from WoTPawns.int"),
			Warder->Title, FString(TEXT("warder title")));
		TestEqual(TEXT("the player selection prefix follows mutated retail data"),
			Character->SelectedStr, FString(TEXT("mutation selected ")));
		TestEqual(TEXT("the player selection suffix follows mutated retail data"),
			Character->TerAngrealStr, FString(TEXT(" mutation suffix ")));
	}
	TestEqual(TEXT("the seal's localized message (class default)"),
		GetDefault<AWoTSeal>()->PossesionChangeStr, FString(TEXT("seal taken")));
	TestEqual(TEXT("the class default is localized too (CDO readers)"),
		GetDefault<AWoTAngrealInvSeeker>()->Title, FString(TEXT("seeker title")));
	TestEqual(TEXT("the player prefix is a decoded class default"),
		GetDefault<AWoTCharacter>()->SelectedStr,
		FString(TEXT("mutation selected ")));
	TestEqual(TEXT("the player suffix is a decoded class default"),
		GetDefault<AWoTCharacter>()->TerAngrealStr,
		FString(TEXT(" mutation suffix ")));
	const FString* Found = FWoTRetailText::FindLocalized(
		AWoTAngrealInvDart::StaticClass(), TEXT("PickupMessage"));
	TestTrue(TEXT("FindLocalized walks the chain"),
		Found && *Found == TEXT("base pickup"));
	WoTTest::DestroySpecWorld(World);

	// A missing package loads nothing (all or nothing).
	Packages.Remove(TEXT("WoTPawns"));
	FWoTRetailText::SetPackageBytesForTesting(Packages);
	TestFalse(TEXT("an incomplete package set does not load"),
		FWoTRetailText::EnsureLoaded());

	// Restore the real retail text for every later spec.
	FWoTRetailText::SetPackageBytesForTesting({});
	FWoTRetailText::EnsureLoaded();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailTextInventorySpec,
	"WoT.RetailText.RetailInventoryText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailTextInventorySpec::RunTest(const FString&)
{
	TMap<FString, FWoTIntFile> Reference;
	if (!WoTRetailTextSpecReference(Reference))
	{
		AddWarning(TEXT("no retail reference tree (WOT_REFERENCE_ROOT/original/System)"));
		return true;
	}
	FWoTRetailText::SetPackageBytesForTesting({});
	TestTrue(TEXT("the retail text loads"), FWoTRetailText::EnsureLoaded());

	// Every native inventory class whose UE1 section exists carries exactly the
	// retail localized values on its class default.
	static const TCHAR* Keys[] = {
		TEXT("Title"), TEXT("Description"), TEXT("Quote"), TEXT("PickupMessage"),
		TEXT("FailMessage"), TEXT("LimitWarning"), TEXT("PossesionChangeStr")};
	int32 Classes = 0;
	int32 Values = 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(AWoTInventory::StaticClass())
			|| !It->HasAnyClassFlags(CLASS_Native)
			|| It->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		const FString Section = FWoTRetailText::RetailClassName(*It);
		const FWoTIntFile* Owner = nullptr;
		int32 Owners = 0;
		for (const TPair<FString, FWoTIntFile>& Package : Reference)
		{
			if (Package.Value.FindSection(Section))
			{
				Owner = &Package.Value;
				++Owners;
			}
		}
		if (Owners != 1)
		{
			continue;
		}
		++Classes;
		const UObject* Default = It->GetDefaultObject();
		for (const TCHAR* Key : Keys)
		{
			const FString* Expected = Owner->Find(Section, Key);
			FStrProperty* Property = FindFProperty<FStrProperty>(*It, FName(Key));
			// An empty .int value is no override (checked explicitly below).
			if (!Expected || Expected->IsEmpty() || !Property)
			{
				continue;
			}
			++Values;
			TestTrue(FString::Printf(TEXT("%s.%s is the retail .int value"),
					*Section, Key),
				Property->GetPropertyValue_InContainer(Default) == *Expected);
		}
	}
	TestTrue(FString::Printf(TEXT("retail sections bound (%d classes, %d values)"),
		Classes, Values), Classes >= 60 && Values >= 200);

	// The info-window text retail showed: the full TrapDetect description (the
	// exported defaultproperties are truncated) and the Minion / Guardian /
	// Champion quotes the port used to miss.
	UWorld* World = WoTTest::CreateSpecWorld();
	auto Check = [&](UClass* Class)
	{
		AWoTInventory* Item = World->SpawnActor<AWoTInventory>(Class,
			FVector(0, 0, 100), FRotator::ZeroRotator, WoTRetailTextSpawnParams());
		if (!TestNotNull(*Class->GetName(), Item))
		{
			return;
		}
		const FString Section = FWoTRetailText::RetailClassName(Class);
		const FString* Description = Reference[TEXT("Angreal")].Find(Section, TEXT("Description"));
		const FString* Quote = Reference[TEXT("Angreal")].Find(Section, TEXT("Quote"));
		TestTrue(Section + TEXT(" description"), Description
			&& !Description->IsEmpty() && Item->Description == *Description);
		TestTrue(Section + TEXT(" quote"), Quote && !Quote->IsEmpty()
			&& Item->Quote == *Quote);
	};
	// Empty .int values are no override in UE1: the shipped class
	// defaults stand (LegionInvSeeker.uc:28; AngrealInvFireworks.uc:24-25).
	TestEqual(TEXT("LegionInvSeeker keeps its empty PickupMessage"),
		GetDefault<AWoTLegionInvSeeker>()->PickupMessage, FString());
	TestEqual(TEXT("AngrealInvFireworks keeps its Description placeholder"),
		GetDefault<AWoTAngrealInvFireworks>()->Description, FString(TEXT("None")));
	TestEqual(TEXT("AngrealInvFireworks keeps its Quote placeholder"),
		GetDefault<AWoTAngrealInvFireworks>()->Quote, FString(TEXT("None")));
	Check(AWoTAngrealInvTrapDetect::StaticClass());
	Check(AWoTAngrealInvMinion::StaticClass());
	Check(AWoTAngrealInvGuardian::StaticClass());
	Check(AWoTAngrealInvChampion::StaticClass());
	WoTTest::DestroySpecWorld(World);
	return true;
}

namespace
{
	// One-way fingerprints of the retail prose (not text), generated from the
	// retail .int by Tools/test/retail_text/gen_prose_fingerprints.py, so the scan
	// below also runs in a checkout without the retail reference tree.
	struct FWoTProseFingerprint
	{
		int32 Length;
		uint64 Head;
		uint64 Full;
	};
	const FWoTProseFingerprint GWoTProseFingerprints[] = {
#include "WoTRetailProseFingerprints.inl"
	};
	constexpr int32 GWoTProseWindow = 16;

	uint64 WoTProseHash(const TCHAR* Text, int32 Count)
	{
		uint64 Hash = 0;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Hash = Hash * 1000003ull + static_cast<uint64>(static_cast<uint16>(Text[Index]));
		}
		return Hash;
	}

	// The candidate rule: Description / Quote / PickupMessage / LimitWarning of
	// 20+ characters and any value of 48+ characters, whitespace collapsed.
	TArray<FString> WoTProseCandidates(const TMap<FString, FWoTIntFile>& Reference)
	{
		TArray<FString> Candidates;
		for (const TCHAR* Package : FWoTRetailText::BoundPackages())
		{
			const FWoTIntFile* File = Reference.Find(Package);
			if (!File)
			{
				continue;
			}
			for (const FWoTIntFile::FSection& Section : File->Sections)
			{
				for (const TPair<FString, FString>& Entry : Section.Entries)
				{
					const bool bProseKey =
						Entry.Key.Equals(TEXT("Description"), ESearchCase::IgnoreCase)
						|| Entry.Key.Equals(TEXT("Quote"), ESearchCase::IgnoreCase)
						|| Entry.Key.Equals(TEXT("PickupMessage"), ESearchCase::IgnoreCase)
						|| Entry.Key.Equals(TEXT("LimitWarning"), ESearchCase::IgnoreCase);
					const FString Value = WoTRetailTextSpecCollapse(Entry.Value);
					if ((bProseKey && Value.Len() >= 20) || Value.Len() >= 48)
					{
						Candidates.AddUnique(Value);
					}
				}
			}
		}
		return Candidates;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRetailTextNoCompiledProseSpec,
	"WoT.RetailText.NoCompiledProse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRetailTextNoCompiledProseSpec::RunTest(const FString&)
{
	TestTrue(FString::Printf(TEXT("embedded retail prose fingerprints (%d)"),
			static_cast<int32>(UE_ARRAY_COUNT(GWoTProseFingerprints))),
		UE_ARRAY_COUNT(GWoTProseFingerprints) > 150);

	// With the retail reference present, the embedded fingerprints must be
	// exactly the reference's (regenerate with gen_prose_fingerprints.py).
	TMap<FString, FWoTIntFile> Reference;
	if (WoTRetailTextSpecReference(Reference))
	{
		TSet<FString> Expected;
		for (const FString& Candidate : WoTProseCandidates(Reference))
		{
			Expected.Add(FString::Printf(TEXT("%d:%016llx:%016llx"), Candidate.Len(),
				WoTProseHash(*Candidate, FMath::Min(GWoTProseWindow, Candidate.Len())),
				WoTProseHash(*Candidate, Candidate.Len())));
		}
		TSet<FString> Embedded;
		for (const FWoTProseFingerprint& Print : GWoTProseFingerprints)
		{
			Embedded.Add(FString::Printf(TEXT("%d:%016llx:%016llx"), Print.Length,
				Print.Head, Print.Full));
		}
		TestTrue(FString::Printf(TEXT("the embedded fingerprints match the retail reference (%d vs %d)"),
				Embedded.Num(), Expected.Num()),
			Embedded.Num() == Expected.Num() && Embedded.Includes(Expected));
	}
	else
	{
		AddInfo(TEXT("no retail reference tree: scanning with the embedded fingerprints only"));
	}

	TMap<uint64, TArray<int32>> Heads;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(GWoTProseFingerprints); ++Index)
	{
		Heads.FindOrAdd(GWoTProseFingerprints[Index].Head).Add(Index);
	}
	uint64 Power = 1;
	for (int32 Index = 0; Index < GWoTProseWindow - 1; ++Index)
	{
		Power *= 1000003ull;
	}

	TArray<FString> Files;
	const FString SourceRoot = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"));
	for (const TCHAR* Extension : {TEXT("*.cpp"), TEXT("*.h"), TEXT("*.inl"), TEXT("*.cs")})
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *SourceRoot, Extension, true, false);
		Files.Append(Found);
	}
	TestTrue(TEXT("the Source tree was scanned"), Files.Num() > 100);
	int32 Hits = 0;
	for (const FString& Path : Files)
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *Path))
		{
			continue;
		}
		const FString Text = WoTRetailTextSpecNormalizeSource(Raw);
		if (Text.Len() < GWoTProseWindow)
		{
			continue;
		}
		const TCHAR* Chars = *Text;
		uint64 Hash = WoTProseHash(Chars, GWoTProseWindow);
		for (int32 Offset = 0; Offset + GWoTProseWindow <= Text.Len(); ++Offset)
		{
			if (Offset > 0)
			{
				Hash = (Hash - static_cast<uint64>(static_cast<uint16>(Chars[Offset - 1])) * Power)
						* 1000003ull
					+ static_cast<uint64>(static_cast<uint16>(Chars[Offset + GWoTProseWindow - 1]));
			}
			const TArray<int32>* Matches = Heads.Find(Hash);
			if (!Matches)
			{
				continue;
			}
			for (const int32 Candidate : *Matches)
			{
				const FWoTProseFingerprint& Print = GWoTProseFingerprints[Candidate];
				if (Offset + Print.Length <= Text.Len()
					&& WoTProseHash(Chars + Offset, Print.Length) == Print.Full)
				{
					++Hits;
					AddError(FString::Printf(
						TEXT("retail prose compiled into %s at normalized offset %d (%d chars)"),
						*Path, Offset, Print.Length));
				}
			}
		}
	}
	TestEqual(TEXT("no retail prose in Source"), Hits, 0);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
