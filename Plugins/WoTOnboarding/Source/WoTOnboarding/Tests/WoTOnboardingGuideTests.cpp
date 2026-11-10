#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTAngrealGuide.h"
#include "WoTOnboardingHints.h"

#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include <initializer_list>

// Synthetic fixtures throughout: invented class names and invented titles
// only (the same fixture vocabulary as test_import_angreal_guide.py). The
// real compiled rows are touched solely by the load spec below, which asserts
// identifiers and counts, never retail prose.

namespace
{
constexpr EAutomationTestFlags WoTOnboardingGuideFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

FWoTAngrealGuideRow WoTOnboardingMakeRow(const TCHAR* ClassName,
	std::initializer_list<const TCHAR*> Counters)
{
	FWoTAngrealGuideRow Row;
	Row.ClassName = ClassName;
	Row.CppClass = FName(*(FString(TEXT("Fx")) + ClassName));
	for (const TCHAR* Counter : Counters)
	{
		Row.Counters.Add(Counter);
	}
	return Row;
}

UWoTAngrealGuide* WoTOnboardingMakeFixtureGuide()
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	FWoTAngrealGuideRow EmberLob = WoTOnboardingMakeRow(TEXT("EmberLob"), {});
	EmberLob.Elements = {FName(TEXT("Fire"))};
	EmberLob.Rarity = FName(TEXT("Common"));
	EmberLob.Roles = {FName(TEXT("Combat")), FName(TEXT("Offensive"))};
	EmberLob.MaxInitialCharges = 4;
	EmberLob.MaxCharges = 9;
	EmberLob.HintText = TEXT("{counter:CinderWard} quenches it.");
	Guide->Rows.Add(EmberLob);
	Guide->Rows.Add(WoTOnboardingMakeRow(TEXT("EmberLobGreater"), {}));
	Guide->Rows.Add(WoTOnboardingMakeRow(TEXT("CinderWard"),
		{TEXT("EmberLob"), TEXT("EmberLobGreater")}));
	Guide->Rows.Add(WoTOnboardingMakeRow(TEXT("BounceWard"),
		{TEXT("EmberLob")}));
	return Guide;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingGuideFindByClassNameSpec,
	"WoT.Onboarding.GuideFindByClassName", WoTOnboardingGuideFlags)

bool FWoTOnboardingGuideFindByClassNameSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingMakeFixtureGuide();
	const FWoTAngrealGuideRow* Row =
		Guide->FindByClassName(FName(TEXT("EmberLob")));
	if (!TestNotNull(TEXT("EmberLob row"), Row))
	{
		return false;
	}
	TestEqual(TEXT("cpp class"), Row->CppClass, FName(TEXT("FxEmberLob")));
	if (TestEqual(TEXT("element count"), Row->Elements.Num(), 1))
	{
		TestEqual(TEXT("element"), Row->Elements[0], FName(TEXT("Fire")));
	}
	TestEqual(TEXT("rarity"), Row->Rarity, FName(TEXT("Common")));
	TestEqual(TEXT("initial charges"), Row->MaxInitialCharges, 4);
	TestEqual(TEXT("max charges"), Row->MaxCharges, 9);
	TestEqual(TEXT("hint text"), Row->HintText,
		FString(TEXT("{counter:CinderWard} quenches it.")));
	TestNull(TEXT("unknown class finds nothing"),
		Guide->FindByClassName(FName(TEXT("NeverARow"))));
	TestNull(TEXT("NAME_None finds nothing"),
		Guide->FindByClassName(NAME_None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingGuideCounteredBySpec,
	"WoT.Onboarding.GuideCounteredBy", WoTOnboardingGuideFlags)

bool FWoTOnboardingGuideCounteredBySpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = WoTOnboardingMakeFixtureGuide();
	TArray<const FWoTAngrealGuideRow*> Counters =
		Guide->CounteredBy(FName(TEXT("EmberLob")));
	if (TestEqual(TEXT("two rows counter EmberLob"), Counters.Num(), 2))
	{
		TestEqual(TEXT("rows order kept (first)"), Counters[0]->ClassName,
			FName(TEXT("CinderWard")));
		TestEqual(TEXT("rows order kept (second)"), Counters[1]->ClassName,
			FName(TEXT("BounceWard")));
	}
	TestEqual(TEXT("one row counters EmberLobGreater"),
		Guide->CounteredBy(FName(TEXT("EmberLobGreater"))).Num(), 1);
	TestEqual(TEXT("nothing counters an unknown class"),
		Guide->CounteredBy(FName(TEXT("NeverARow"))).Num(), 0);
	TestEqual(TEXT("NAME_None counters nothing"),
		Guide->CounteredBy(NAME_None).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingGuideCompiledRowsSpec,
	"WoT.Onboarding.GuideCompiledRows", WoTOnboardingGuideFlags)

bool FWoTOnboardingGuideCompiledRowsSpec::RunTest(const FString&)
{
	// No skip: the rows are compiled into the plugin, so every build (the
	// guarded one included) carries them. The pins mirror the generator's
	// (test_import_angreal_guide.py): the roster floor and the Fireball row.
	const TStrongObjectPtr<UWoTAngrealGuide> Guide(UWoTAngrealGuide::Load());
	if (!TestNotNull(TEXT("the compiled rows parse"), Guide.Get()))
	{
		return false;
	}
	TestTrue(TEXT("the compiled guide carries the roster"),
		Guide->Rows.Num() >= 25);
	const TStrongObjectPtr<UWoTAngrealGuide> Second(UWoTAngrealGuide::Load());
	TestTrue(TEXT("each load is a fresh object"),
		Second.IsValid() && Second.Get() != Guide.Get());
	const FWoTAngrealGuideRow* Fireball =
		Guide->FindByClassName(FName(TEXT("AngrealInvFireball")));
	if (!TestNotNull(TEXT("the compiled guide resolves the fireball row"),
		Fireball))
	{
		return false;
	}
	TestEqual(TEXT("fireball's C++ class"), Fireball->CppClass,
		FName(TEXT("WoTAngrealInvFireball")));
	TestEqual(TEXT("fireball is fire"), Fireball->Elements,
		TArray<FName>{FName(TEXT("Fire"))});
	TestEqual(TEXT("fireball is common"), Fireball->Rarity,
		FName(TEXT("Common")));
	TestEqual(TEXT("fireball's starting charges"), Fireball->MaxInitialCharges,
		20);
	TestEqual(TEXT("fireball's charge cap"), Fireball->MaxCharges, 35);
	const FWoTAngrealGuideRow* Counter =
		WoTOnboardingHints::HintCounter(*Guide, *Fireball);
	if (TestNotNull(TEXT("fireball's hint names a counter the rows back"),
		Counter))
	{
		TestTrue(TEXT("that counter counters fireball"),
			Counter->Counters.Contains(Fireball->ClassName));
	}
	const TStrongObjectPtr<UWoTAngrealGuide> Reparsed(
		NewObject<UWoTAngrealGuide>(GetTransientPackage()));
	const FString Exported = Guide->ExportRowsAsJson();
	TestEqual(TEXT("the compiled rows re-parse from their own export"),
		Reparsed->ImportRowsFromJson(Exported), FString());
	TestEqual(TEXT("and re-export identically"),
		Reparsed->ExportRowsAsJson(), Exported);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingGuideImportJsonRoundTripSpec,
	"WoT.Onboarding.GuideImportJsonRoundTrip", WoTOnboardingGuideFlags)

bool FWoTOnboardingGuideImportJsonRoundTripSpec::RunTest(const FString&)
{
	const FString Payload = TEXT(R"json({"rows":[
		{"class_name":"EmberLob","cpp_class":"FxEmberLob",
		 "elements":["Fire"],"rarity":"Common",
		 "roles":["Combat","Offensive"],
		 "max_initial_charges":4,"max_charges":9,
		 "counters":[],
		 "hint_text":"{counter:CinderWard} quenches it."},
		{"class_name":"PitSnare","cpp_class":"FxPitSnare",
		 "elements":[],"rarity":"",
		 "roles":[],
		 "max_initial_charges":0,"max_charges":0,
		 "counters":["EmberLob"],
		 "hint_text":""}]})json");

	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	TestEqual(TEXT("import succeeds"), Guide->ImportRowsFromJson(Payload),
		FString());
	if (!TestEqual(TEXT("two rows imported"), Guide->Rows.Num(), 2))
	{
		return false;
	}
	const FWoTAngrealGuideRow& Ember = Guide->Rows[0];
	TestEqual(TEXT("class"), Ember.ClassName, FName(TEXT("EmberLob")));
	TestEqual(TEXT("cpp"), Ember.CppClass, FName(TEXT("FxEmberLob")));
	TestEqual(TEXT("charges"), Ember.MaxCharges, 9);
	TestEqual(TEXT("hint keeps placeholders literal"), Ember.HintText,
		FString(TEXT("{counter:CinderWard} quenches it.")));
	TestTrue(TEXT("empty rarity folds to NAME_None"),
		Guide->Rows[1].Rarity.IsNone());
	TestEqual(TEXT("counters land"), Guide->Rows[1].Counters.Num(), 1);

	// Export -> import into a second object -> identical export: the JSON
	// form round-trips losslessly.
	const FString Exported = Guide->ExportRowsAsJson();
	UWoTAngrealGuide* Second = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	TestEqual(TEXT("re-import succeeds"), Second->ImportRowsFromJson(Exported),
		FString());
	TestEqual(TEXT("round-trip is lossless"), Second->ExportRowsAsJson(),
		Exported);
	TestNotNull(TEXT("finders work on imported rows"),
		Second->FindByClassName(FName(TEXT("PitSnare"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTOnboardingGuideImportJsonRejectsSpec,
	"WoT.Onboarding.GuideImportJsonRejects", WoTOnboardingGuideFlags)

bool FWoTOnboardingGuideImportJsonRejectsSpec::RunTest(const FString&)
{
	UWoTAngrealGuide* Guide = NewObject<UWoTAngrealGuide>(GetTransientPackage());
	FWoTAngrealGuideRow Keep;
	Keep.ClassName = FName(TEXT("KeepMe"));
	Guide->Rows.Add(Keep);

	auto Rejects = [&](const TCHAR* Case, const FString& Payload)
	{
		const FString Error = Guide->ImportRowsFromJson(Payload);
		TestFalse(FString::Printf(TEXT("%s is rejected"), Case),
			Error.IsEmpty());
		TestEqual(FString::Printf(TEXT("%s leaves rows untouched"), Case),
			Guide->Rows.Num(), 1);
	};
	Rejects(TEXT("non-json"), TEXT("not json at all"));
	Rejects(TEXT("missing rows"), TEXT("{}"));
	Rejects(TEXT("non-object row"), TEXT(R"({"rows":[7]})"));
	Rejects(TEXT("missing field"),
		TEXT(R"({"rows":[{"class_name":"A"}]})"));
	Rejects(TEXT("empty class_name"), TEXT(
		R"({"rows":[{"class_name":"","cpp_class":"X","elements":[],)"
		R"("rarity":"","roles":[],"max_initial_charges":0,"max_charges":0,)"
		R"("counters":[],"hint_text":""}]})"));
	Rejects(TEXT("negative charges"), TEXT(
		R"({"rows":[{"class_name":"A","cpp_class":"X","elements":[],)"
		R"("rarity":"","roles":[],"max_initial_charges":-1,"max_charges":0,)"
		R"("counters":[],"hint_text":""}]})"));
	Rejects(TEXT("duplicate class_name"), TEXT(
		R"({"rows":[{"class_name":"A","cpp_class":"X","elements":[],)"
		R"("rarity":"","roles":[],"max_initial_charges":0,"max_charges":0,)"
		R"("counters":[],"hint_text":""},)"
		R"({"class_name":"A","cpp_class":"Y","elements":[],)"
		R"("rarity":"","roles":[],"max_initial_charges":0,"max_charges":0,)"
		R"("counters":[],"hint_text":""}]})"));
	TestEqual(TEXT("surviving row is the original"),
		Guide->Rows[0].ClassName, FName(TEXT("KeepMe")));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
