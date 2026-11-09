#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTAngrealGuide.h"
#include "WoTOnboardingModule.h"
#include "WoTOnboardingSeams.h"
#include "WoTOnboardingSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

// The plan's two Task 7 gates.
//
// Parity-off: with the master switch off the plugin holds no seam binding,
// shows no widget and registers no menu row, so a switched-off build behaves
// like one without the plugin (the game module's seam spec already proves an
// unbound seam is inert).
//
// Honesty: every claim in the checked-in guide facts is backed by the ported
// classes. The plugin links no game class, so the gate reads the game's class
// defaults through reflection only. Helper names carry the WoTOnboardingGate
// prefix for the shared unity translation unit.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingGateFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

// --- parity-off ----------------------------------------------------------

/** Pins the live toggles and seen list; restores them through the setters. */
struct FWoTOnboardingGateSettingsSnapshot
{
	bool bMaster;
	bool bCards;
	TArray<FName> Seen;

	FWoTOnboardingGateSettingsSnapshot()
	{
		const UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		bMaster = Live.bEnableOnboardingAids;
		bCards = Live.bShowFirstPickupCards;
		Seen = Live.SeenPickupCards;
	}

	~FWoTOnboardingGateSettingsSnapshot()
	{
		UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
		Live.SetEnableOnboardingAids(bMaster);
		Live.SetShowFirstPickupCards(bCards);
		Live.SetSeenPickupCards(Seen);
	}
};

int32 WoTOnboardingGateOwnedRows(const TArray<FWoTOnboardingRow>& Rows)
{
	return Rows.FilterByPredicate([](const FWoTOnboardingRow& Row)
	{
		return Row.Owner == FWoTOnboardingModule::SettingsRowOwner();
	}).Num();
}

UWoTAngrealGuide* WoTOnboardingGateMakeGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	FWoTAngrealGuideRow Row;
	Row.ClassName = FName(TEXT("EmberLob"));
	Row.Rarity = FName(TEXT("Common"));
	Guide->Rows.Add(Row);
	return Guide;
}

/** Reports which seams hold a binding to the module, by seam name. */
void WoTOnboardingGateExpectBindings(FAutomationTestBase& Test,
	const FWoTOnboardingModule& Module, bool bExpected, const TCHAR* When)
{
	const FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();
	const struct
	{
		const TCHAR* Seam;
		bool bBound;
	} Checks[] = {
		{TEXT("ShowGuideRequested"), Seams.ShowGuideRequested.IsBoundToObject(&Module)},
		{TEXT("CloseClientWindows"), Seams.CloseClientWindows.IsBoundToObject(&Module)},
		{TEXT("ClientItemAdded"), Seams.ClientItemAdded.IsBoundToObject(&Module)},
		{TEXT("ClientKilledBy"), Seams.ClientKilledBy.IsBoundToObject(&Module)},
	};
	for (const auto& Check : Checks)
	{
		if (Check.bBound != bExpected)
		{
			Test.AddError(FString::Printf(TEXT("%s: %s %s a module binding"),
				When, Check.Seam, Check.bBound ? TEXT("holds") : TEXT("lacks")));
		}
	}
}

// --- honesty ---------------------------------------------------------------

const TCHAR* const WoTOnboardingGateFactsHeader = TEXT(
	"Class,CppClass,Elements,Rarity,Roles,MaxInitialCharges,MaxCharges,"
	"Counters,HintKey");

/** Guide classes the port does not carry yet. Printed on every run so the
    list's shrinkage shows; a listed class that resolves fails the gate. */
TArray<FString> WoTOnboardingGateNotYetPorted()
{
	return {};
}

struct FWoTOnboardingGateRow
{
	FString Class;
	FString CppClass;
	TArray<FString> Elements;
	FString Rarity;
	TArray<FString> Roles;
	FString MaxInitialCharges;
	FString MaxCharges;
	TArray<FString> Counters;
};

/** A claim word and the class-default flag that backs it. */
struct FWoTOnboardingGateFlag
{
	const TCHAR* Claim;
	const TCHAR* Property;
};

const FWoTOnboardingGateFlag WoTOnboardingGateElements[] = {
	{TEXT("Air"), TEXT("bElementAir")},
	{TEXT("Earth"), TEXT("bElementEarth")},
	{TEXT("Fire"), TEXT("bElementFire")},
	{TEXT("Spirit"), TEXT("bElementSpirit")},
	{TEXT("Water"), TEXT("bElementWater")},
};
const FWoTOnboardingGateFlag WoTOnboardingGateRarities[] = {
	{TEXT("Common"), TEXT("bCommon")},
	{TEXT("Uncommon"), TEXT("bUncommon")},
	{TEXT("Rare"), TEXT("bRare")},
};
const FWoTOnboardingGateFlag WoTOnboardingGateRoles[] = {
	{TEXT("Combat"), TEXT("bCombat")},
	{TEXT("Defensive"), TEXT("bDefensive")},
	{TEXT("Offensive"), TEXT("bOffensive")},
};

/** The ported classes the counter rules key on. */
struct FWoTOnboardingGateFramework
{
	const UClass* Inventory = nullptr;
	const UClass* Angreal = nullptr;
	const UClass* Launcher = nullptr;
	const UClass* IgnoreElement = nullptr;
	const UClass* ReturnToSender = nullptr;
	const UClass* TrapDetect = nullptr;
	const UClass* Trap = nullptr;
	const UClass* WardProjectile = nullptr;
	const UClass* RemoveCurse = nullptr;
	const UClass* Taint = nullptr;
	TArray<FName> Detectable;
};

const UClass* WoTOnboardingGateFindClass(const FString& Name)
{
	return FindObject<UClass>(nullptr,
		*FString::Printf(TEXT("/Script/WoT.%s"), *Name));
}

bool WoTOnboardingGateReadBool(const UObject* Object, const TCHAR* Name, bool& bOut)
{
	const FBoolProperty* Property =
		FindFProperty<FBoolProperty>(Object->GetClass(), Name);
	if (!Property)
	{
		return false;
	}
	bOut = Property->GetPropertyValue_InContainer(Object);
	return true;
}

bool WoTOnboardingGateReadInt(const UObject* Object, const TCHAR* Name, int32& Out)
{
	const FIntProperty* Property =
		FindFProperty<FIntProperty>(Object->GetClass(), Name);
	if (!Property)
	{
		return false;
	}
	Out = Property->GetPropertyValue_InContainer(Object);
	return true;
}

FName WoTOnboardingGateReadName(const UObject* Object, const TCHAR* Name)
{
	const FNameProperty* Property =
		FindFProperty<FNameProperty>(Object->GetClass(), Name);
	return Property ? Property->GetPropertyValue_InContainer(Object) : NAME_None;
}

const UClass* WoTOnboardingGateReadClass(const UObject* Object, const TCHAR* Name)
{
	const FClassProperty* Property =
		FindFProperty<FClassProperty>(Object->GetClass(), Name);
	return Property
		? Cast<UClass>(Property->GetObjectPropertyValue_InContainer(Object))
		: nullptr;
}

TArray<const UClass*> WoTOnboardingGateReadClassArray(
	const UObject* Object, const TCHAR* Name)
{
	TArray<const UClass*> Classes;
	const FArrayProperty* Array =
		FindFProperty<FArrayProperty>(Object->GetClass(), Name);
	const FObjectPropertyBase* Inner =
		Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
	if (!Inner)
	{
		return Classes;
	}
	FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Object));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		if (const UClass* Class =
			Cast<UClass>(Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index))))
		{
			Classes.Add(Class);
		}
	}
	return Classes;
}

TArray<FName> WoTOnboardingGateReadNameArray(const UObject* Object, const TCHAR* Name)
{
	TArray<FName> Names;
	const FArrayProperty* Array =
		FindFProperty<FArrayProperty>(Object->GetClass(), Name);
	const FNameProperty* Inner =
		Array ? CastField<FNameProperty>(Array->Inner) : nullptr;
	if (!Inner)
	{
		return Names;
	}
	FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Object));
	for (int32 Index = 0; Index < Helper.Num(); ++Index)
	{
		Names.Add(Inner->GetPropertyValue(Helper.GetRawPtr(Index)));
	}
	return Names;
}

TArray<FString> WoTOnboardingGateSplitList(const FString& Cell)
{
	TArray<FString> Items;
	Cell.ParseIntoArray(Items, TEXT(";"), true);
	return Items;
}

bool WoTOnboardingGateLoadFacts(FAutomationTestBase& Test,
	TArray<FWoTOnboardingGateRow>& OutRows)
{
	const FString Path = FPaths::Combine(FPaths::ProjectDir(),
		TEXT("Tools/import/data/angreal_guide_facts.csv"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		Test.AddError(FString::Printf(TEXT("cannot read %s"), *Path));
		return false;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, true);
	if (Lines.IsEmpty() || Lines[0] != WoTOnboardingGateFactsHeader)
	{
		Test.AddError(TEXT("the facts CSV header is not the documented one"));
		return false;
	}
	for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
	{
		TArray<FString> Cells;
		Lines[LineIndex].ParseIntoArray(Cells, TEXT(","), false);
		if (Cells.Num() != 9 || Lines[LineIndex].Contains(TEXT("\"")))
		{
			Test.AddError(FString::Printf(
				TEXT("facts CSV line %d is not nine plain fields"), LineIndex + 1));
			return false;
		}
		FWoTOnboardingGateRow& Row = OutRows.AddDefaulted_GetRef();
		Row.Class = Cells[0];
		Row.CppClass = Cells[1];
		Row.Elements = WoTOnboardingGateSplitList(Cells[2]);
		Row.Rarity = Cells[3];
		Row.Roles = WoTOnboardingGateSplitList(Cells[4]);
		Row.MaxInitialCharges = Cells[5];
		Row.MaxCharges = Cells[6];
		Row.Counters = WoTOnboardingGateSplitList(Cells[7]);
	}
	return Test.TestTrue(TEXT("the facts CSV carries rows"), OutRows.Num() > 0);
}

/** cpp_class -> the top of its multiplayer initial-charge envelope, from the
    angreal verification corpus. Cast variants share their class's envelope. */
bool WoTOnboardingGateLoadEnvelopes(FAutomationTestBase& Test,
	TMap<FString, int32>& OutMaxInitial)
{
	const FString Path = FPaths::Combine(FPaths::ProjectDir(),
		TEXT("Tools/test/angreal/angreal_expectations.json"));
	FString Text;
	TSharedPtr<FJsonObject> Root;
	const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
	if (!FFileHelper::LoadFileToString(Text, *Path)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root)
		|| !Root.IsValid()
		|| !Root->TryGetArrayField(TEXT("classes"), Classes))
	{
		Test.AddError(FString::Printf(TEXT("cannot read the classes of %s"), *Path));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Classes)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		FString CppClass;
		const TArray<TSharedPtr<FJsonValue>>* Envelope = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Entry)
			|| (*Entry)->HasField(TEXT("variant_of")))
		{
			continue;
		}
		if (!(*Entry)->TryGetStringField(TEXT("cpp_class"), CppClass)
			|| !(*Entry)->TryGetArrayField(TEXT("initial_charges_mp"), Envelope)
			|| Envelope->Num() != 2)
		{
			Test.AddError(TEXT("an expectations entry lacks cpp_class or a two-number envelope"));
			return false;
		}
		OutMaxInitial.Add(CppClass, static_cast<int32>((*Envelope)[1]->AsNumber()));
	}
	return Test.TestTrue(TEXT("the expectations carry envelopes"),
		OutMaxInitial.Num() > 0);
}

/** The claim words whose flags the class default sets, sorted; false (with
    an error naming the row) when a flag property is missing. */
bool WoTOnboardingGateSetFlags(FAutomationTestBase& Test, const FString& Row,
	const UObject* Defaults, TConstArrayView<FWoTOnboardingGateFlag> Table,
	TArray<FString>& Out)
{
	for (const FWoTOnboardingGateFlag& Flag : Table)
	{
		bool bSet = false;
		if (!WoTOnboardingGateReadBool(Defaults, Flag.Property, bSet))
		{
			Test.AddError(FString::Printf(TEXT("%s: the ported class has no %s"),
				*Row, Flag.Property));
			return false;
		}
		if (bSet)
		{
			Out.Add(Flag.Claim);
		}
	}
	Out.Sort();
	return true;
}

void WoTOnboardingGateExpectList(FAutomationTestBase& Test, const FString& Row,
	const TCHAR* What, TArray<FString> Claimed, const TArray<FString>& Ported)
{
	Claimed.Sort();
	if (Claimed != Ported)
	{
		Test.AddError(FString::Printf(
			TEXT("%s: the guide claims %s [%s]; the ported class sets [%s]"),
			*Row, What, *FString::Join(Claimed, TEXT(";")),
			*FString::Join(Ported, TEXT(";"))));
	}
}

void WoTOnboardingGateExpectCharge(FAutomationTestBase& Test, const FString& Row,
	const UObject* Defaults, const TCHAR* Property, const FString& Claimed)
{
	int32 Ported = 0;
	if (!Claimed.IsNumeric() || !WoTOnboardingGateReadInt(Defaults, Property, Ported))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s is not a number backed by the ported class"),
			*Row, Property));
		return;
	}
	if (FCString::Atoi(*Claimed) != Ported)
	{
		Test.AddError(FString::Printf(
			TEXT("%s: the guide claims %s %s; the ported class sets %d"),
			*Row, Property, *Claimed, Ported));
	}
}

/** The detector's identity for a placed piece, as the port's trap detector
    computes it: a trap's TrapType when set, else its class name; the
    explosive ward by its shipped class name; anything else is not a piece
    the detector considers. */
FName WoTOnboardingGateTrapID(const FWoTOnboardingGateFramework& Framework,
	const UClass* Piece)
{
	if (Piece->IsChildOf(Framework.Trap))
	{
		const FName TrapType =
			WoTOnboardingGateReadName(Piece->GetDefaultObject(), TEXT("TrapType"));
		return TrapType.IsNone() ? Piece->GetFName() : TrapType;
	}
	if (Piece->IsChildOf(Framework.WardProjectile))
	{
		return FName(TEXT("AngrealExpWardProjectile"));
	}
	return NAME_None;
}

/** Whether the ported classes back "Counterer counters Target" under one of
    the guide's counter rules. */
bool WoTOnboardingGateEdgeBacked(const FWoTOnboardingGateFramework& Framework,
	const UClass* Counterer, const UClass* Target)
{
	const UObject* Defaults = Target->GetDefaultObject();
	bool bOffensive = false;
	WoTOnboardingGateReadBool(Defaults, TEXT("bOffensive"), bOffensive);

	// Element shields and the reflect family: the reflectors the counterer
	// installs on cast.
	for (const UClass* Reflector : WoTOnboardingGateReadClassArray(
		Counterer->GetDefaultObject(), TEXT("ReflectorClasses")))
	{
		if (Reflector->IsChildOf(Framework.IgnoreElement))
		{
			const FName Element = WoTOnboardingGateReadName(
				Reflector->GetDefaultObject(), TEXT("IgnoredElement"));
			bool bCarries = false;
			if (bOffensive && !Element.IsNone()
				&& WoTOnboardingGateReadBool(Defaults,
					*(FString(TEXT("bElement")) + Element.ToString()), bCarries)
				&& bCarries)
			{
				return true;
			}
		}
		else if (Reflector->IsChildOf(Framework.ReturnToSender)
			&& bOffensive && Target->IsChildOf(Framework.Launcher))
		{
			return true;
		}
	}

	// Trap detection: a piece the target places carries an identity in the
	// detector's table. Traps place their resource and a wall's slabs;
	// launchers place their projectile.
	if (Counterer->IsChildOf(Framework.TrapDetect))
	{
		TArray<const UClass*> Pieces;
		if (const UClass* Resource =
			WoTOnboardingGateReadClass(Defaults, TEXT("ResourceClass")))
		{
			Pieces.Add(Resource);
			if (const UClass* Slab = WoTOnboardingGateReadClass(
				Resource->GetDefaultObject(), TEXT("SlabClass")))
			{
				Pieces.Add(Slab);
			}
		}
		if (const UClass* Projectile =
			WoTOnboardingGateReadClass(Defaults, TEXT("ProjectileClass")))
		{
			Pieces.Add(Projectile);
		}
		for (const UClass* Piece : Pieces)
		{
			const FName Id = WoTOnboardingGateTrapID(Framework, Piece);
			if (!Id.IsNone() && Framework.Detectable.Contains(Id))
			{
				return true;
			}
		}
	}

	// Curse removal lifts the Taint.
	return Counterer->IsChildOf(Framework.RemoveCurse)
		&& Target->IsChildOf(Framework.Taint);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingParityOffGateSpec,
	"WoT.Onboarding.ParityOffGate", WoTOnboardingGateFlags)

bool FWoTOnboardingParityOffGateSpec::RunTest(const FString&)
{
	FWoTOnboardingModule& Module =
		FModuleManager::GetModuleChecked<FWoTOnboardingModule>(TEXT("WoTOnboarding"));
	FWoTOnboardingGateSettingsSnapshot Snapshot;
	UWoTOnboardingSettings& Live = UWoTOnboardingSettings::Get();
	FWoTOnboardingSeams& Seams = FWoTOnboardingSeams::Get();

	// The positive control: master on, the module listens, so the checks
	// below cannot pass vacuously.
	Live.SetEnableOnboardingAids(true);
	WoTOnboardingGateExpectBindings(*this, Module, true, TEXT("master on"));
	TestEqual(TEXT("master on: the guide row is registered"),
		WoTOnboardingGateOwnedRows(Seams.MenuRows), 1);

	Live.SetEnableOnboardingAids(false);
	WoTOnboardingGateExpectBindings(*this, Module, false, TEXT("master off"));
	TestFalse(TEXT("master off: no guide browser"), Module.IsGuideBrowserOpen());
	TestFalse(TEXT("master off: no pickup card"), Module.IsPickupCardOpen());
	TestEqual(TEXT("master off: no menu row"),
		WoTOnboardingGateOwnedRows(Seams.MenuRows), 0);
	TestEqual(TEXT("master off: the three settings rows stay (the way back on)"),
		WoTOnboardingGateOwnedRows(Seams.SettingsRows), 3);

	// Traffic while off changes nothing, even with a spec guide that would let
	// the item handler decide.
	const TStrongObjectPtr<UWoTAngrealGuide> Guide(WoTOnboardingGateMakeGuide());
	// No retail window open, whatever an earlier spec's world left installed.
	TFunction<bool()> SavedWindowQuery = MoveTemp(Seams.IsClientWindowOpen);
	Seams.IsClientWindowOpen = nullptr;
	Module.SetPickupCardGuideForTesting(Guide.Get());
	// Declared after the snapshot, so this runs first: the handler goes inert
	// again before the settings restore.
	ON_SCOPE_EXIT
	{
		Module.SetPickupCardGuideForTesting(nullptr);
		FWoTOnboardingSeams::Get().IsClientWindowOpen = MoveTemp(SavedWindowQuery);
	};
	Live.SetShowFirstPickupCards(true);
	Live.SetSeenPickupCards({});
	const FName Ember(TEXT("EmberLob"));
	const int32 Before = Module.GetPickupCardsShownForTesting();
	Seams.ClientItemAdded.Broadcast(Ember);
	Seams.ShowGuideRequested.Broadcast();
	TestEqual(TEXT("master off: an item add decides no card"),
		Module.GetPickupCardsShownForTesting(), Before);
	TestFalse(TEXT("master off: and records nothing"), Live.HasSeenPickupCard(Ember));
	TestFalse(TEXT("master off: a guide request opens nothing"),
		Module.IsGuideBrowserOpen());

	// A restart while off binds nothing either.
	Module.StartupModule();
	WoTOnboardingGateExpectBindings(*this, Module, false, TEXT("restart while off"));
	TestEqual(TEXT("restart while off: no menu row"),
		WoTOnboardingGateOwnedRows(Seams.MenuRows), 0);

	// Master back on rebinds live, and the same add now decides its card.
	Live.SetEnableOnboardingAids(true);
	WoTOnboardingGateExpectBindings(*this, Module, true, TEXT("master back on"));
	Seams.ClientItemAdded.Broadcast(Ember);
	TestEqual(TEXT("master back on: the add decides its card"),
		Module.GetPickupCardsShownForTesting(), Before + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingHonestyGateSpec,
	"WoT.Onboarding.HonestyGate", WoTOnboardingGateFlags)

bool FWoTOnboardingHonestyGateSpec::RunTest(const FString&)
{
	TArray<FWoTOnboardingGateRow> Rows;
	TMap<FString, int32> MaxInitialByCppClass;
	if (!WoTOnboardingGateLoadFacts(*this, Rows)
		|| !WoTOnboardingGateLoadEnvelopes(*this, MaxInitialByCppClass))
	{
		return false;
	}

	FWoTOnboardingGateFramework Framework;
	const struct
	{
		const UClass** Slot;
		const TCHAR* Name;
	} Anchors[] = {
		{&Framework.Inventory, TEXT("WoTInventory")},
		{&Framework.Angreal, TEXT("WoTAngrealInventory")},
		{&Framework.Launcher, TEXT("WoTProjectileLauncher")},
		{&Framework.IgnoreElement, TEXT("WoTIgnoreElementReflector")},
		{&Framework.ReturnToSender, TEXT("WoTReturnToSenderReflector")},
		{&Framework.TrapDetect, TEXT("WoTAngrealInvTrapDetect")},
		{&Framework.Trap, TEXT("WoTTrap")},
		{&Framework.WardProjectile, TEXT("WoTExpWardProjectile")},
		{&Framework.RemoveCurse, TEXT("WoTAngrealInvRemoveCurse")},
		{&Framework.Taint, TEXT("WoTAngrealInvTaint")},
	};
	for (const auto& Anchor : Anchors)
	{
		*Anchor.Slot = WoTOnboardingGateFindClass(Anchor.Name);
		if (!*Anchor.Slot)
		{
			AddError(FString::Printf(TEXT("the counter rules key on %s, which "
				"no longer resolves"), Anchor.Name));
		}
	}
	const UClass* Leech = WoTOnboardingGateFindClass(TEXT("WoTTrapDetectLeech"));
	if (Leech)
	{
		Framework.Detectable = WoTOnboardingGateReadNameArray(
			Leech->GetDefaultObject(), TEXT("DetectableTrapTypes"));
	}
	if (!TestTrue(TEXT("the trap detector's table resolves"),
		Framework.Detectable.Num() > 0) || HasAnyErrors())
	{
		return false;
	}

	const TArray<FString> NotYetPorted = WoTOnboardingGateNotYetPorted();
	AddInfo(FString::Printf(TEXT("Not-yet-ported allowlist (%d): %s"),
		NotYetPorted.Num(),
		NotYetPorted.IsEmpty() ? TEXT("empty") : *FString::Join(NotYetPorted, TEXT(", "))));

	// Each row's own claims: the class resolves, and an angreal's elements,
	// rarity, roles and charges match its class defaults and the corpus.
	TMap<FString, const UClass*> Resolved;
	for (const FWoTOnboardingGateRow& Row : Rows)
	{
		const UClass* Class = WoTOnboardingGateFindClass(Row.CppClass);
		const bool bListed = NotYetPorted.Contains(Row.Class);
		if (!Class)
		{
			if (!bListed)
			{
				AddError(FString::Printf(TEXT("%s: %s is not a ported class"),
					*Row.Class, *Row.CppClass));
			}
			continue;
		}
		if (bListed)
		{
			AddError(FString::Printf(TEXT("%s: ported now; take it off the "
				"not-yet-ported allowlist"), *Row.Class));
		}
		Resolved.Add(Row.Class, Class);
		const UObject* Defaults = Class->GetDefaultObject();
		if (Class->IsChildOf(Framework.Angreal))
		{
			TArray<FString> Elements, Rarities, Roles;
			if (WoTOnboardingGateSetFlags(*this, Row.Class, Defaults,
					WoTOnboardingGateElements, Elements)
				&& WoTOnboardingGateSetFlags(*this, Row.Class, Defaults,
					WoTOnboardingGateRarities, Rarities)
				&& WoTOnboardingGateSetFlags(*this, Row.Class, Defaults,
					WoTOnboardingGateRoles, Roles))
			{
				WoTOnboardingGateExpectList(*this, Row.Class, TEXT("elements"),
					Row.Elements, Elements);
				WoTOnboardingGateExpectList(*this, Row.Class, TEXT("rarity"),
					WoTOnboardingGateSplitList(Row.Rarity), Rarities);
				WoTOnboardingGateExpectList(*this, Row.Class, TEXT("roles"),
					Row.Roles, Roles);
			}
			WoTOnboardingGateExpectCharge(*this, Row.Class, Defaults,
				TEXT("MaxInitialCharges"), Row.MaxInitialCharges);
			WoTOnboardingGateExpectCharge(*this, Row.Class, Defaults,
				TEXT("MaxCharges"), Row.MaxCharges);
			const int32* Envelope = MaxInitialByCppClass.Find(Row.CppClass);
			if (!Envelope)
			{
				AddError(FString::Printf(TEXT("%s: no expectations entry for %s"),
					*Row.Class, *Row.CppClass));
			}
			else if (FCString::Atoi(*Row.MaxInitialCharges) != *Envelope)
			{
				AddError(FString::Printf(TEXT("%s: the guide claims %s initial "
					"charges at most; the expectations envelope tops at %d"),
					*Row.Class, *Row.MaxInitialCharges, *Envelope));
			}
		}
		else if (Class->IsChildOf(Framework.Inventory))
		{
			// A placeable trap carries no angreal facts.
			if (!Row.Elements.IsEmpty() || !Row.Rarity.IsEmpty()
				|| !Row.Roles.IsEmpty() || !Row.MaxInitialCharges.IsEmpty()
				|| !Row.MaxCharges.IsEmpty())
			{
				AddError(FString::Printf(TEXT("%s: a trap row claims angreal "
					"facts its class cannot back"), *Row.Class));
			}
		}
		else
		{
			AddError(FString::Printf(TEXT("%s: %s is not an inventory class"),
				*Row.Class, *Row.CppClass));
		}
	}

	// Every counter edge is one the ported classes back.
	int32 Edges = 0;
	for (const FWoTOnboardingGateRow& Row : Rows)
	{
		const UClass* const* Counterer = Resolved.Find(Row.Class);
		for (const FString& Target : Row.Counters)
		{
			++Edges;
			const UClass* const* TargetClass = Resolved.Find(Target);
			if (!Counterer || !TargetClass)
			{
				if (!Rows.ContainsByPredicate([&Target](const FWoTOnboardingGateRow& Other)
					{
						return Other.Class == Target;
					}))
				{
					AddError(FString::Printf(TEXT("%s: counters %s, which has no "
						"facts row"), *Row.Class, *Target));
				}
				continue;
			}
			if (!WoTOnboardingGateEdgeBacked(Framework, *Counterer, *TargetClass))
			{
				AddError(FString::Printf(TEXT("%s: the guide says it counters %s; "
					"the ported classes do not back that"), *Row.Class, *Target));
			}
		}
	}
	TestTrue(TEXT("the facts claim counter edges"), Edges > 0);
	AddInfo(FString::Printf(TEXT("%d rows and %d counter edges checked"),
		Rows.Num(), Edges));
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
