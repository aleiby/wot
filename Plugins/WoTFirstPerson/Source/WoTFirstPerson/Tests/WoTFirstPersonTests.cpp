#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "WoTFirstPersonModule.h"
#include "WoTFirstPersonSettings.h"
#include "WoTExtensionSeams.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

namespace
{
constexpr EAutomationTestFlags FirstPersonFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

struct FFirstPersonSettingsSnapshot
{
	bool bShow = UWoTFirstPersonSettings::Get().bShowHeldArtifact;
	float Motion = UWoTFirstPersonSettings::Get().Motion;
	float FieldOfView = UWoTFirstPersonSettings::Get().ModelFieldOfView;

	~FFirstPersonSettingsSnapshot()
	{
		UWoTFirstPersonSettings& Settings = UWoTFirstPersonSettings::Get();
		Settings.SetShowHeldArtifact(bShow);
		Settings.SetMotion(Motion);
		Settings.SetModelFieldOfView(FieldOfView);
	}
};

TArray<FWoTExtensionSettingsSection> FirstPersonSections()
{
	return FWoTExtensionSeams::Get().SettingsSections.FilterByPredicate(
		[](const FWoTExtensionSettingsSection& Section)
		{
			return Section.Owner == FWoTFirstPersonModule::SettingsSectionOwner();
		});
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFirstPersonSectionWhileOffSpec,
	"WoT.FirstPerson.SectionWhileOff", FirstPersonFlags)

bool FWoTFirstPersonSectionWhileOffSpec::RunTest(const FString&)
{
	FFirstPersonSettingsSnapshot Snapshot;
	UWoTFirstPersonSettings::Get().SetShowHeldArtifact(false);
	const auto Sections = FirstPersonSections();
	if (!TestEqual(TEXT("off keeps one settings section"), Sections.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("section identifies the feature"), Sections[0].Header(),
		FString(TEXT("FIRST PERSON")));
	TestEqual(TEXT("off keeps all controls available"), Sections[0].Rows.Num(), 3);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFirstPersonReopenedSettingsSpec,
	"WoT.FirstPerson.ReopenedSettings", FirstPersonFlags)

bool FWoTFirstPersonReopenedSettingsSpec::RunTest(const FString&)
{
	FFirstPersonSettingsSnapshot Snapshot;
	UWoTFirstPersonSettings& Settings = UWoTFirstPersonSettings::Get();
	Settings.SetShowHeldArtifact(true);
	auto Sections = FirstPersonSections();
	if (!TestEqual(TEXT("one loaded section"), Sections.Num(), 1)
		|| !TestEqual(TEXT("three rows"), Sections[0].Rows.Num(), 3))
	{
		return false;
	}
	const FWoTExtensionRow CopiedToggle = Sections[0].Rows[0];
	CopiedToggle.Invoke();
	TestFalse(TEXT("copied callback switches off"), Settings.bShowHeldArtifact);
	Sections = FirstPersonSections();
	if (!TestEqual(TEXT("reopening still finds the section"), Sections.Num(), 1)
		|| !TestEqual(TEXT("reopening still finds the controls"), Sections[0].Rows.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("new label reflects off"), Sections[0].Rows[0].Label().Contains(TEXT("Off")));
	const FWoTExtensionRow ReopenedToggle = Sections[0].Rows[0];
	ReopenedToggle.Invoke();
	TestTrue(TEXT("reopened callback switches on"), Settings.bShowHeldArtifact);
	TestTrue(TEXT("old copied label remains live"), CopiedToggle.Label().Contains(TEXT("On")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFirstPersonSettingsPersistenceSpec,
	"WoT.FirstPerson.SettingsPersistence", FirstPersonFlags)

bool FWoTFirstPersonSettingsPersistenceSpec::RunTest(const FString&)
{
	FFirstPersonSettingsSnapshot Snapshot;
	UWoTFirstPersonSettings& Settings = UWoTFirstPersonSettings::Get();
	Settings.SetShowHeldArtifact(true);
	Settings.SetMotion(1.0f);
	Settings.SetModelFieldOfView(70.0f);
	int32 Changes = 0;
	const FDelegateHandle Changed = Settings.OnChanged.AddLambda([&Changes]() { ++Changes; });
	ON_SCOPE_EXIT { Settings.OnChanged.Remove(Changed); };
	Settings.SetShowHeldArtifact(false);
	Settings.SetMotion(0.25f);
	Settings.SetModelFieldOfView(85.0f);
	TestEqual(TEXT("each change notifies live consumers"), Changes, 3);
	Settings.SetShowHeldArtifact(false);
	Settings.SetMotion(0.25f);
	Settings.SetModelFieldOfView(85.0f);
	TestEqual(TEXT("equal values do not notify"), Changes, 3);
	// Change only memory, then reload to prove the setters wrote the config.
	Settings.bShowHeldArtifact = true;
	Settings.Motion = 1.0f;
	Settings.ModelFieldOfView = 70.0f;
	Settings.ReloadConfig();
	TestFalse(TEXT("master persisted"), Settings.bShowHeldArtifact);
	TestEqual(TEXT("motion persisted"), Settings.Motion, 0.25f);
	TestEqual(TEXT("field of view persisted"), Settings.ModelFieldOfView, 85.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFirstPersonSettingStepsSpec,
	"WoT.FirstPerson.SettingSteps", FirstPersonFlags)

bool FWoTFirstPersonSettingStepsSpec::RunTest(const FString&)
{
	FFirstPersonSettingsSnapshot Snapshot;
	UWoTFirstPersonSettings& Settings = UWoTFirstPersonSettings::Get();
	Settings.SetMotion(0.0f);
	Settings.SetModelFieldOfView(70.0f);
	const auto Sections = FirstPersonSections();
	if (!TestEqual(TEXT("one section"), Sections.Num(), 1)
		|| !TestEqual(TEXT("three controls"), Sections[0].Rows.Num(), 3))
	{
		return false;
	}
	Sections[0].Rows[1].Invoke();
	Sections[0].Rows[2].Invoke();
	TestEqual(TEXT("motion row advances a quarter"), Settings.Motion, 0.25f);
	TestEqual(TEXT("field of view row advances five degrees"), Settings.ModelFieldOfView, 75.0f);
	TestTrue(TEXT("copied motion label follows live value"), Sections[0].Rows[1].Label().Contains(TEXT("25%")));
	TestTrue(TEXT("copied field of view label follows live value"), Sections[0].Rows[2].Label().Contains(TEXT("75")));
	Settings.SetMotion(1.0f);
	Settings.SetModelFieldOfView(120.0f);
	Sections[0].Rows[1].Invoke();
	Sections[0].Rows[2].Invoke();
	TestEqual(TEXT("motion wraps to still"), Settings.Motion, 0.0f);
	TestEqual(TEXT("field of view wraps to its minimum"), Settings.ModelFieldOfView, 40.0f);
	Settings.SetMotion(-1.0f);
	Settings.SetModelFieldOfView(200.0f);
	TestEqual(TEXT("motion cannot become negative"), Settings.Motion, 0.0f);
	TestEqual(TEXT("field of view remains bounded"), Settings.ModelFieldOfView, 120.0f);
	Settings.SetMotion(2.0f);
	Settings.SetModelFieldOfView(0.0f);
	TestEqual(TEXT("motion cannot exceed full strength"), Settings.Motion, 1.0f);
	TestEqual(TEXT("field of view stays positive"), Settings.ModelFieldOfView, 40.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFirstPersonOffLifecycleSpec,
	"WoT.FirstPerson.OffLifecycle", FirstPersonFlags)

bool FWoTFirstPersonOffLifecycleSpec::RunTest(const FString&)
{
	FFirstPersonSettingsSnapshot Snapshot;
	FWoTFirstPersonModule& Module = FModuleManager::GetModuleChecked<FWoTFirstPersonModule>(TEXT("WoTFirstPerson"));
	FWoTExtensionSeams& Seams = FWoTExtensionSeams::Get();
	Module.ShutdownModule();
	auto SavedPressed = MoveTemp(Seams.LocalFirePressed);
	auto SavedReleased = MoveTemp(Seams.LocalFireReleased);
	auto SavedFactory = MoveTemp(Seams.SpawnItemPresentation);
	Seams.LocalFirePressed.Clear();
	Seams.LocalFireReleased.Clear();
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	const int32 InitialActorCount = World ? World->GetActorCount() : 0;
	int32 FactoryCalls = 0;
	Seams.SpawnItemPresentation = [&FactoryCalls](UWorld* TargetWorld, FName, const FTransform& Transform)
	{
		++FactoryCalls;
		return TargetWorld ? TargetWorld->SpawnActor<AActor>(AActor::StaticClass(), Transform) : nullptr;
	};
	ON_SCOPE_EXIT
	{
		Module.ShutdownModule();
		Seams.LocalFirePressed = MoveTemp(SavedPressed);
		Seams.LocalFireReleased = MoveTemp(SavedReleased);
		Seams.SpawnItemPresentation = MoveTemp(SavedFactory);
		if (World) { World->DestroyWorld(false); }
		Module.StartupModule();
	};
	UWoTFirstPersonSettings& Settings = UWoTFirstPersonSettings::Get();
	Settings.SetShowHeldArtifact(true);
	Module.StartupModule();
	Module.StartupModule();
	TestTrue(TEXT("enabled positive control listens for press"), Seams.LocalFirePressed.IsBound());
	TestTrue(TEXT("enabled positive control listens for release"), Seams.LocalFireReleased.IsBound());
	Settings.SetShowHeldArtifact(false);
	TestFalse(TEXT("off removes press subscription"), Seams.LocalFirePressed.IsBound());
	TestFalse(TEXT("off removes release subscription"), Seams.LocalFireReleased.IsBound());
	Seams.LocalFirePressed.Broadcast(FName(TEXT("TestArtifact")));
	Seams.LocalFireReleased.Broadcast();
	Module.StartupModule();
	TestFalse(TEXT("restart while off holds no press subscription"), Seams.LocalFirePressed.IsBound());
	TestFalse(TEXT("restart while off holds no release subscription"), Seams.LocalFireReleased.IsBound());
	TestEqual(TEXT("stub never asks the factory for a model"), FactoryCalls, 0);
	if (TestNotNull(TEXT("world created"), World))
	{
		TestEqual(TEXT("off creates no actors"), World->GetActorCount(), InitialActorCount);
	}
	Settings.SetShowHeldArtifact(true);
	TestTrue(TEXT("switching back on restores press subscription"), Seams.LocalFirePressed.IsBound());
	TestTrue(TEXT("switching back on restores release subscription"), Seams.LocalFireReleased.IsBound());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTFirstPersonRestartOwnershipSpec,
	"WoT.FirstPerson.RestartOwnership", FirstPersonFlags)

bool FWoTFirstPersonRestartOwnershipSpec::RunTest(const FString&)
{
	FFirstPersonSettingsSnapshot Snapshot;
	FWoTFirstPersonModule& Module = FModuleManager::GetModuleChecked<FWoTFirstPersonModule>(TEXT("WoTFirstPerson"));
	FWoTExtensionSeams& Seams = FWoTExtensionSeams::Get();
	const FName OtherOwner(TEXT("FirstPersonSpecOtherOwner"));
	FWoTExtensionSettingsSection OtherSection;
	OtherSection.Owner = OtherOwner;
	OtherSection.Header = []() { return FString(TEXT("Other")); };
	OtherSection.Rows.Add({[]() { return FString(TEXT("Other row")); }, []() {}, OtherOwner});
	Seams.SettingsSections.Add(MoveTemp(OtherSection));
	int32 OtherPresses = 0;
	const FDelegateHandle OtherHandle = Seams.LocalFirePressed.AddLambda([&OtherPresses](FName) { ++OtherPresses; });
	ON_SCOPE_EXIT
	{
		Seams.LocalFirePressed.Remove(OtherHandle);
		Seams.SettingsSections.RemoveAll([OtherOwner](const FWoTExtensionSettingsSection& Section) { return Section.Owner == OtherOwner; });
		Module.StartupModule();
	};
	UWoTFirstPersonSettings::Get().SetShowHeldArtifact(true);
	Module.StartupModule();
	Module.StartupModule();
	TestEqual(TEXT("repeated startup keeps one owned section"), FirstPersonSections().Num(), 1);
	Module.ShutdownModule();
	Module.ShutdownModule();
	TestEqual(TEXT("shutdown removes owned section"), FirstPersonSections().Num(), 0);
	TestFalse(TEXT("shutdown removes every settings subscription"),
		UWoTFirstPersonSettings::Get().OnChanged.IsBoundToObject(&Module));
	TestTrue(TEXT("shutdown preserves another section"), Seams.SettingsSections.ContainsByPredicate(
		[OtherOwner](const FWoTExtensionSettingsSection& Section) { return Section.Owner == OtherOwner; }));
	Seams.LocalFirePressed.Broadcast(NAME_None);
	TestEqual(TEXT("shutdown preserves another listener"), OtherPresses, 1);
	UWoTFirstPersonSettings::Get().SetShowHeldArtifact(false);
	TestEqual(TEXT("settings change after shutdown cannot register anything"), FirstPersonSections().Num(), 0);
	Module.StartupModule();
	TestEqual(TEXT("startup while off restores settings"), FirstPersonSections().Num(), 1);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
