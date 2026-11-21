#include "Misc/AutomationTest.h"
#include "World/WoTWorldProjector.h"
#include "Inventory/WoTAngrealInventory.h"

#if WITH_AUTOMATION_TESTS

#include "WoTBotFactionProfile.h"
#include "AI/WoTCreatureItemBinding.h"
#include "WoTCharacter.h"
#include "WoTGameMode.h"
#include "WoTPayloadSubsystem.h"
#include "WoTRetailText.h"
#include "World/WoTRuntimeClassDefaults.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "RetailData/WoTDevRetailFamilies.h"
#endif

namespace
{
constexpr EAutomationTestFlags ProfileFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

FWoTRuntimeWorldProperty ProfileFloat(const TCHAR* Name, float Value,
	uint32 Index = 0)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = Name;
	Property.Type = EWoTRuntimeWorldPropertyType::Float;
	Property.ArrayIndex = Index;
	Property.Value.SetNumUninitialized(sizeof(Value));
	FMemory::Memcpy(Property.Value.GetData(), &Value, sizeof(Value));
	return Property;
}

FWoTRuntimeWorldProperty ProfileString(const TCHAR* Value, uint32 Index)
{
	FWoTRuntimeWorldProperty Property;
	Property.Name = TEXT("AII_AngrealInventoryStr");
	Property.Type = EWoTRuntimeWorldPropertyType::String;
	Property.ArrayIndex = Index;
	FTCHARToUTF8 Utf8(Value);
	check(Utf8.Length() < 63);
	Property.Value.Add(static_cast<uint8>(Utf8.Length() + 1));
	Property.Value.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Property.Value.Add(0);
	return Property;
}

TArray<FWoTRuntimeClassDefaultRecord> ProfileRecords()
{
	FWoTRuntimeClassDefaultRecord Base;
	Base.Identity = TEXT("WoTPawns.WOTBot");
	Base.SuperclassIdentity = TEXT("WoTPawns.AngrealUser");
	Base.Properties = {
		ProfileFloat(TEXT("MinOffensiveAngrealUsageInterval"), 0.75f),
		ProfileFloat(TEXT("MinDefensiveAngrealUsageInterval"), 0.25f),
		ProfileString(TEXT("Angreal.AngrealInvDart"), 0),
		ProfileFloat(TEXT("AII_Priority"), 2.5f),
		ProfileString(TEXT(""), 1)};
	FWoTRuntimeClassDefaultRecord User;
	User.Identity = Base.SuperclassIdentity;
	User.SuperclassIdentity = TEXT("Engine.Pawn");
	FWoTRuntimeClassDefaultRecord Pawn;
	Pawn.Identity = User.SuperclassIdentity;
	TArray<FWoTRuntimeClassDefaultRecord> Records = {
		MoveTemp(Pawn), MoveTemp(User), MoveTemp(Base)};
	for (const TCHAR* Identity : {TEXT("WoTPawns.AesSedaiBot"),
		TEXT("WoTPawns.ForsakenBot"), TEXT("WoTPawns.HoundBot"),
		TEXT("WoTPawns.WhitecloakBot")})
	{
		FWoTRuntimeClassDefaultRecord& Leaf = Records.AddDefaulted_GetRef();
		Leaf.Identity = Identity;
		Leaf.SuperclassIdentity = TEXT("WoTPawns.WOTBot");
	}
	Records[3].Properties = {
		ProfileFloat(TEXT("AII_Priority"), 7.25f),
		ProfileString(TEXT("Angreal.AngrealInvShield"), 15),
		ProfileFloat(TEXT("AII_Priority"), 3.5f, 15),
		ProfileFloat(TEXT("MinDefensiveAngrealUsageInterval"), 1.25f)};
	return Records;
}

UClass* ProfilePlayer(const TCHAR* Path)
{
	return AWoTGameMode::ResolvePlayerClassOption(FString(TEXT("?Class=")) + Path);
}

TArray<uint8> ProfileTextBytes(const ANSICHAR* Text)
{
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	return Bytes;
}

struct FProfileRetailTextScope
{
	~FProfileRetailTextScope()
	{
		FWoTRetailText::SetPackageBytesForTesting({});
		FWoTRetailText::EnsureLoaded();
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotProfileFixtureSpec,
	"WoT.Bots.Profiles.InheritedAndSparseFixture", ProfileFlags)

bool FWoTBotProfileFixtureSpec::RunTest(const FString&)
{
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("typed class-default graph loads"),
		Defaults.LoadFixture(ProfileRecords(), Error))) return false;
	TMap<UClass*, FWoTBotFactionProfile> Profiles;
	if (!TestTrue(TEXT("valid profiles decode"), WoTBotProfiles::Decode(Defaults, Profiles, Error))) return false;
	TestEqual(TEXT("four player classes receive independent profiles"), Profiles.Num(), 4);
	const FWoTBotFactionProfile* First = Profiles.Find(ProfilePlayer(TEXT("WoTPawns.AesSedai")));
	const FWoTBotFactionProfile* Second = Profiles.Find(ProfilePlayer(TEXT("WoTPawns.Forsaken")));
	if (!TestNotNull(TEXT("first class is bound through the player resolver"), First)
		|| !TestNotNull(TEXT("second class is bound through the player resolver"), Second)) return false;
	UClass* Dart = WoTCreatureBoundWeave(TEXT("Angreal.AngrealInvDart"));
	UClass* Shield = WoTCreatureBoundWeave(TEXT("Angreal.AngrealInvShield"));
	TestEqual(TEXT("own priority overrides inherited priority"), First->Priorities.FindRef(Dart), 7.25f);
	TestEqual(TEXT("other classes retain inherited priority"), Second->Priorities.FindRef(Dart), 2.5f);
	TestEqual(TEXT("sparse final static-array slot is decoded"), First->Priorities.FindRef(Shield), 3.5f);
	TestEqual(TEXT("empty inherited item is omitted"), First->Priorities.Num(), 2);
	TestEqual(TEXT("offensive interval follows inheritance"), First->MinOffensiveUsageInterval, 0.75f);
	TestEqual(TEXT("defensive interval accepts the leaf override"), First->MinDefensiveUsageInterval, 1.25f);
	TestEqual(TEXT("other defensive interval remains inherited"), Second->MinDefensiveUsageInterval, 0.25f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotProfileShippedItemsBindSpec,
	"WoT.Bots.Profiles.ShippedBotItemsBind", ProfileFlags)

bool FWoTBotProfileShippedItemsBindSpec::RunTest(const FString&)
{
	// The items the shipped bot classes list that the NPC weave table does not
	// cover; they bind through the same resolver that places map pickups.
	const TCHAR* const Items[] = {
		TEXT("Angreal.AngrealInvTaint"), TEXT("Angreal.AngrealInvSwapPlaces"),
		TEXT("Angreal.AngrealInvReflect"), TEXT("Angreal.AngrealInvMinion"),
		TEXT("Angreal.AngrealInvHeal"), TEXT("Angreal.AngrealInvFork"),
		TEXT("Angreal.AngrealInvDecay"), TEXT("Angreal.AngrealInvAirShield"),
		TEXT("Angreal.AngrealInvAbsorb")};
	for (const TCHAR* Item : Items)
	{
		UClass* Class = WoTResolveNativePickupClass(Item);
		if (TestNotNull(FString::Printf(TEXT("%s binds"), Item), Class))
		{
			TestTrue(FString::Printf(TEXT("%s is an artifact"), Item),
				Class->IsChildOf(AWoTAngrealInventory::StaticClass()));
		}
	}
	TestNull(TEXT("an unknown item stays unbound"),
		WoTResolveNativePickupClass(TEXT("Angreal.UnboundFixtureItem")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotProfileUnboundSpec,
	"WoT.Bots.Profiles.SkipUnboundClassAndItem", ProfileFlags)

bool FWoTBotProfileUnboundSpec::RunTest(const FString&)
{
	TArray<FWoTRuntimeClassDefaultRecord> Records = ProfileRecords();
	Records[3].Properties.Add(ProfileString(TEXT("Angreal.UnboundFixtureItem"), 2));
	Records[3].Properties.Add(ProfileFloat(TEXT("AII_Priority"), 88.0f, 2));
	FWoTRuntimeClassDefaultRecord& Unknown = Records.AddDefaulted_GetRef();
	Unknown.Identity = TEXT("WoTPawns.UnboundFixtureBot");
	Unknown.SuperclassIdentity = TEXT("WoTPawns.WOTBot");
	FWoTRuntimeClassDefaults Defaults;
	FString Error;
	if (!TestTrue(TEXT("unbound source fixture loads"), Defaults.LoadFixture(MoveTemp(Records), Error))) return false;
	TMap<UClass*, FWoTBotFactionProfile> Profiles;
	TestTrue(TEXT("unbound paths do not reject bound profiles"), WoTBotProfiles::Decode(Defaults, Profiles, Error));
	TestEqual(TEXT("unknown class cannot replace the default player class"), Profiles.Num(), 4);
	const FWoTBotFactionProfile* First = Profiles.Find(ProfilePlayer(TEXT("WoTPawns.AesSedai")));
	if (TestNotNull(TEXT("known class remains"), First))
	{
		TestEqual(TEXT("only mapped items are retained"), First->Priorities.Num(), 2);
		TestFalse(TEXT("an unbound item never inserts a null key"), First->Priorities.Contains(nullptr));
	}
	TestTrue(TEXT("repeated decoding remains safe"), WoTBotProfiles::Decode(Defaults, Profiles, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotProfileMalformedSpec,
	"WoT.Bots.Profiles.MalformedInputIsAtomic", ProfileFlags)

bool FWoTBotProfileMalformedSpec::RunTest(const FString&)
{
	for (int32 Case = 0; Case < 3; ++Case)
	{
		TArray<FWoTRuntimeClassDefaultRecord> Records = ProfileRecords();
		if (Case == 0) Records[3].Properties[0].Value.SetNum(1);
		if (Case == 1) Records[3].Properties[1].Value.Pop();
		if (Case == 2) Records[3].Properties.Last().Type = EWoTRuntimeWorldPropertyType::Integer;
		FWoTRuntimeClassDefaults Defaults;
		FString Error;
		if (!TestTrue(TEXT("malformed typed field can enter the decoded graph"),
			Defaults.LoadFixture(MoveTemp(Records), Error))) return false;
		TMap<UClass*, FWoTBotFactionProfile> Profiles;
		FWoTBotFactionProfile Previous;
		Previous.MinOffensiveUsageInterval = 19.0f;
		UClass* PreviousClass = ProfilePlayer(TEXT("WoTPawns.Hound"));
		Profiles.Add(PreviousClass, Previous);
		TestFalse(TEXT("malformed profile is rejected"), WoTBotProfiles::Decode(Defaults, Profiles, Error));
		TestFalse(TEXT("rejection explains the invalid field"), Error.IsEmpty());
		TestEqual(TEXT("failure does not publish a partial set"), Profiles.Num(), 1);
		const FWoTBotFactionProfile* Preserved = Profiles.Find(PreviousClass);
		if (TestNotNull(TEXT("previous profile remains"), Preserved))
			TestEqual(TEXT("previous profile content remains"), Preserved->MinOffensiveUsageInterval, 19.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotProfileNameSpec,
	"WoT.Bots.Profiles.DisplayNameUsesRuntimeRetailText", ProfileFlags)

bool FWoTBotProfileNameSpec::RunTest(const FString&)
{
	FProfileRetailTextScope Restore;
	TMap<FString, TArray<uint8>> Packages;
	for (const TCHAR* Package : FWoTRetailText::BoundPackages()) Packages.Add(Package, {});
	Packages[TEXT("WoTPawns")] = ProfileTextBytes(
		"[AesSedai]\nTeamDescription=Fixture Class One\n"
		"[Forsaken]\nTeamDescription=Fixture Class Two\n"
		"[Hound]\nTeamDescription=Fixture Class Three\n"
		"[Whitecloak]\nTeamDescription=Fixture Class Four\n");
	FWoTRetailText::SetPackageBytesForTesting(MoveTemp(Packages));
	TestEqual(TEXT("first name follows runtime text"),
		WoTBotProfiles::DisplayName(ProfilePlayer(TEXT("WoTPawns.AesSedai"))), FString(TEXT("Fixture Class One")));
	TestEqual(TEXT("second name follows runtime text"),
		WoTBotProfiles::DisplayName(ProfilePlayer(TEXT("WoTPawns.Forsaken"))), FString(TEXT("Fixture Class Two")));
	TestEqual(TEXT("third name follows runtime text"),
		WoTBotProfiles::DisplayName(ProfilePlayer(TEXT("WoTPawns.Hound"))), FString(TEXT("Fixture Class Three")));
	TestEqual(TEXT("fourth name follows runtime text"),
		WoTBotProfiles::DisplayName(ProfilePlayer(TEXT("WoTPawns.Whitecloak"))), FString(TEXT("Fixture Class Four")));
	TestTrue(TEXT("missing class has no invented display name"), WoTBotProfiles::DisplayName(nullptr).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotProfileActiveGenerationSpec,
	"WoT.Bots.Profiles.ActiveGenerationWhenAvailable", ProfileFlags)

bool FWoTBotProfileActiveGenerationSpec::RunTest(const FString&)
{
	FString Error;
	const FWoTRuntimeClassDefaults* Defaults = UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
	if (Defaults == nullptr)
	{
		AddInfo(FString::Printf(TEXT("SKIP: no active payload generation: %s"), *Error));
		return true;
	}
	TMap<UClass*, FWoTBotFactionProfile> Profiles;
	if (!TestTrue(TEXT("active profiles decode"), WoTBotProfiles::Decode(*Defaults, Profiles, Error))) return false;
	TestEqual(TEXT("active retail graph binds all playable profiles"), Profiles.Num(), 4);
	for (const TPair<UClass*, FWoTBotFactionProfile>& Pair : Profiles)
	{
		TestTrue(TEXT("retail profile has mapped priorities"), Pair.Value.Priorities.Num() > 0);
		TestTrue(TEXT("offensive interval is nonnegative"), Pair.Value.MinOffensiveUsageInterval >= 0.0f);
		TestTrue(TEXT("defensive interval is nonnegative"), Pair.Value.MinDefensiveUsageInterval >= 0.0f);
	}
	TestEqual(TEXT("runtime cache exposes the active generation"), WoTBotProfiles::Get().Num(), Profiles.Num());
	return true;
}

#if !WOT_GUARDED_BOOTSTRAP
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBotProfileDevelopmentSourceSpec,
	"WoT.Bots.Profiles.DevelopmentSourceWhenAvailable", ProfileFlags)

bool FWoTBotProfileDevelopmentSourceSpec::RunTest(const FString&)
{
	WoTDevRetailFamiliesEnsureActivated();
	if (WoTDevRetailClassDefaults() == nullptr || !FWoTRetailText::EnsureLoaded())
	{
		AddInfo(TEXT("SKIP: development class defaults or runtime retail text are unavailable"));
		return true;
	}
	const TMap<UClass*, FWoTBotFactionProfile>& Profiles = WoTBotProfiles::Get();
	TestEqual(TEXT("runtime source binds all four player classes"), Profiles.Num(), 4);
	for (const TPair<UClass*, FWoTBotFactionProfile>& Pair : Profiles)
	{
		TestTrue(TEXT("runtime class has mapped item priorities"), Pair.Value.Priorities.Num() > 0);
		TestFalse(TEXT("runtime retail text supplies a class name"), WoTBotProfiles::DisplayName(Pair.Key).IsEmpty());
	}
	return true;
}
#endif

#endif
