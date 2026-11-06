#pragma once

#include "CoreMinimal.h"

// Retail localized text (UE1 `var localized string`) is the purchaser's data: it is
// read at runtime from the retail .int files and never compiled into the port.
//
// UE1 filled a class default's localized properties from <Package>.int, section
// <ClassName>, key <PropertyName> (UObject::LoadLocalized); a subclass whose section
// lacks a key keeps the value its parent's default already loaded. Retail menus and
// info windows therefore showed the .int text, not the .uc defaultproperties (the
// batch export truncates long ones: TrapDetect's Description, the missing
// Minion/Guardian/Champion quotes).
//
// Sources: guarded builds read the authenticated cache (cache/localization/<Pkg>.int,
// the path the menu strings use); dev builds read the same retail files from
// $WOT_REFERENCE_ROOT/original/System, else <Project>/original/System.

/** One parsed UE1 .int file (UE1's FConfigFile::Read). */
struct WOT_API FWoTIntFile
{
	struct FSection
	{
		FString Name;
		TArray<TPair<FString, FString>> Entries;
	};
	TArray<FSection> Sections;

	/** Case-insensitive section/key lookup (UE1 FString compares ignore case); a
	    repeated key resolves to its last definition (UE1 TMultiMap hash chain). */
	const FSection* FindSection(FStringView Section) const;
	const FString* Find(FStringView Section, FStringView Key) const;
};

class WOT_API FWoTRetailText final
{
public:
	/** The retail packages whose localized class defaults the port binds. */
	static TConstArrayView<const TCHAR*> BoundPackages();

	/** UE1 FConfigFile::Read over the raw bytes: CR/LF lines, untrimmed; a line
	    that starts with '[' and ends with ']' opens a section; otherwise the text
	    before the first '=' is the key and the rest the value, with one pair of
	    enclosing double quotes removed. ANSI bytes widen 1:1 (appFromAnsi); a
	    UTF-16LE BOM selects wide text. False on an embedded NUL or bad UTF-16. */
	static bool ParseIntBytes(TConstArrayView<uint8> Bytes, FWoTIntFile& Out);

	/** Loads every bound package (once; a failed attempt is retried on the next
	    call). On first success the native class defaults are localized. */
	static bool EnsureLoaded();
	static bool IsLoaded();

	/** The UE1 class name a native port class stands for ("WoTAngrealInvAMA" ->
	    "AngrealInvAMA"; AWoTInventory -> Engine's "Inventory"). */
	static FString RetailClassName(const UClass* Class);
	/** The UE1 sections a native port class reads, root first: its class name, or
	    for a port class that merges several retail classes, all of them
	    (AWoTGameMode = giWOT + giCombatBase; AWoTBattleGameMode = giMPBattle plus
	    the two WOTPlayer battle prompts it posts). */
	static TArray<FString> RetailSections(const UClass* Class);

	/** The localized value for Key on Class, resolved like UE1 LoadLocalized along
	    the native class chain (most-derived section with the key wins). */
	static const FString* FindLocalized(const UClass* Class, FStringView Key);

	/** Direct lookup for localized data objects such as WOTHelpInfo, whose
	    retail section has no native UObject counterpart in this port. */
	static const FString* FindSectionValue(FStringView Section, FStringView Key);

	/** UE1 LoadLocalized for one object: every non-empty key of every section
	    along its native class chain sets the same-named FString property (an
	    empty .int value is no override). No-op until the text
	    is loaded. Game thread. */
	static void ApplyLocalizedDefaults(UObject* Object);

	/** The dev reference directory the loader reads (empty in guarded builds or
	    when no retail reference tree is present). */
	static FString ReferenceSystemDirectory();

#if WITH_AUTOMATION_TESTS
	/** Replace the package bytes (empty map = forget the injection and reload from
	    the real source on the next EnsureLoaded). */
	static void SetPackageBytesForTesting(TMap<FString, TArray<uint8>> Packages);
#endif
};
