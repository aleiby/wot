// Bootstrap-dormancy coverage only. Runtime registry/localization parity
// belongs to the lookup owners' specs; this file neither substitutes for nor
// pre-implements them.

#include "Misc/AutomationTest.h"

#include "WoTPayloadActivationTestSupport.h"
#include "WoTTestWorld.h"

#include "../Payload/WoTPayloadActivation.h"
#include "../Testing/WoTBSPProofHarness.h"
#include "../Testing/WoTEOFProbe.h"
#include "../Testing/WoTNetTestSubsystem.h"
#include "../WoTAmbientSound.h"
#include "../WoTDynamicLightSubsystem.h"
#include "../WoTEvents.h"
#include "../WoTFogLight.h"
#include "../WoTFractalSubsystem.h"
#include "../WoTGameInstance.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "../WoTShotSubsystem.h"
#include "../WoTZoneVolume.h"
#include "WoTLaunchEngineHooks.h"

#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameDelegates.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "UObject/UObjectGlobals.h"

#if WITH_AUTOMATION_TESTS

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"

#include <atomic>

namespace
{
constexpr EAutomationTestFlags WoTBootstrapDormancySpecFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter;

// IPlatformFile directly, NOT FPlatformFileOpenLog or FLoggedPlatformFile: on
// MSVC, constructing a derived class emits the base's vtable for the base
// subobject, which references the base's out-of-line virtuals -- and both
// wrapper classes have out-of-line virtuals without CORE_API (OpenLog's
// Initialize/OpenMappedEx/OpenMappedEx2 and FLoggingAsyncReadFileHandle;
// LogWrapper's mapped pair), so the installed engine cannot link them
// (LNK2001). IPlatformFile itself exports every out-of-line member per-method,
// which is exactly why installed-engine modules like PakFile can derive it.
// Clang never surfaces this because it elides the unused base vtable.
class FWoTBootstrapDormancyFileObserver final : public IPlatformFile
{
public:
	bool Initialize(IPlatformFile* Inner, const TCHAR*) override
	{
		LowerLevel = Inner;
		return LowerLevel != nullptr;
	}
	IPlatformFile* GetLowerLevel() override
	{
		return LowerLevel;
	}
	void SetLowerLevel(IPlatformFile* NewLowerLevel) override
	{
		LowerLevel = NewLowerLevel;
	}
	const TCHAR* GetName() const override
	{
		return TEXT("WoTBootstrapDormancyFileObserver");
	}
	bool FileExists(const TCHAR* Filename) override
	{
		Observe(Filename, false);
		return LowerLevel->FileExists(Filename);
	}
	int64 FileSize(const TCHAR* Filename) override
	{
		Observe(Filename, false);
		return LowerLevel->FileSize(Filename);
	}
	IFileHandle* OpenRead(
		const TCHAR* Filename, bool bAllowWrite = false) override
	{
		Observe(Filename, true);
		return LowerLevel->OpenRead(Filename, bAllowWrite);
	}
	FOpenMappedResult OpenMappedEx(const TCHAR* Filename,
		EOpenReadFlags Flags = EOpenReadFlags::None,
		int64 MaximumSize = 0) override
	{
		Observe(Filename, true);
		return LowerLevel->OpenMappedEx(Filename, Flags, MaximumSize);
	}
	FOpenMappedResult OpenMappedEx2(const TCHAR* Filename,
		EOpenMappedFlags Flags = EOpenMappedFlags::None,
		int64 MaximumSize = 0) override
	{
		Observe(Filename, true);
		return LowerLevel->OpenMappedEx2(Filename, Flags, MaximumSize);
	}
	// The remaining pure virtuals of IPlatformFile, forwarded unobserved. The
	// using-declarations keep the base's convenience overloads visible.
	using IPlatformFile::IterateDirectory;
	using IPlatformFile::IterateDirectoryRecursively;
	using IPlatformFile::IterateDirectoryStat;
	using IPlatformFile::IterateDirectoryStatRecursively;
	bool DeleteFile(const TCHAR* Filename) override
	{
		return LowerLevel->DeleteFile(Filename);
	}
	bool IsReadOnly(const TCHAR* Filename) override
	{
		return LowerLevel->IsReadOnly(Filename);
	}
	bool MoveFile(const TCHAR* To, const TCHAR* From) override
	{
		return LowerLevel->MoveFile(To, From);
	}
	bool SetReadOnly(const TCHAR* Filename, bool bNewReadOnlyValue) override
	{
		return LowerLevel->SetReadOnly(Filename, bNewReadOnlyValue);
	}
	FDateTime GetTimeStamp(const TCHAR* Filename) override
	{
		return LowerLevel->GetTimeStamp(Filename);
	}
	void SetTimeStamp(const TCHAR* Filename, FDateTime DateTime) override
	{
		LowerLevel->SetTimeStamp(Filename, DateTime);
	}
	FDateTime GetAccessTimeStamp(const TCHAR* Filename) override
	{
		return LowerLevel->GetAccessTimeStamp(Filename);
	}
	FString GetFilenameOnDisk(const TCHAR* Filename) override
	{
		return LowerLevel->GetFilenameOnDisk(Filename);
	}
	IFileHandle* OpenWrite(const TCHAR* Filename, bool bAppend = false,
		bool bAllowRead = false) override
	{
		return LowerLevel->OpenWrite(Filename, bAppend, bAllowRead);
	}
	bool DirectoryExists(const TCHAR* Directory) override
	{
		return LowerLevel->DirectoryExists(Directory);
	}
	bool CreateDirectory(const TCHAR* Directory) override
	{
		return LowerLevel->CreateDirectory(Directory);
	}
	bool DeleteDirectory(const TCHAR* Directory) override
	{
		return LowerLevel->DeleteDirectory(Directory);
	}
	FFileStatData GetStatData(const TCHAR* FilenameOrDirectory) override
	{
		return LowerLevel->GetStatData(FilenameOrDirectory);
	}
	bool IterateDirectory(const TCHAR* Directory,
		FDirectoryVisitor& Visitor) override
	{
		return LowerLevel->IterateDirectory(Directory, Visitor);
	}
	bool IterateDirectoryStat(const TCHAR* Directory,
		FDirectoryStatVisitor& Visitor) override
	{
		return LowerLevel->IterateDirectoryStat(Directory, Visitor);
	}

	int32 PreferenceOpens() const
	{
		return FullPreferenceOpenCount.load(std::memory_order_relaxed);
	}
	int32 PreferenceProbes() const
	{
		return FullPreferenceProbeCount.load(std::memory_order_relaxed);
	}
	int32 ConfigOpens() const
	{
		return GameUserSettingsOpenCount.load(std::memory_order_relaxed);
	}
	int32 ConfigProbes() const
	{
		return GameUserSettingsProbeCount.load(std::memory_order_relaxed);
	}

private:
	void Observe(const TCHAR* Filename, bool bOpen)
	{
		const FString Path(Filename);
		if (Path.EndsWith(TEXT("WoTUserPreferences"),
			ESearchCase::CaseSensitive))
		{
			(bOpen ? FullPreferenceOpenCount
				: FullPreferenceProbeCount).fetch_add(
					1, std::memory_order_relaxed);
		}
		if (Path.EndsWith(TEXT("GameUserSettings.ini"),
			ESearchCase::CaseSensitive))
		{
			(bOpen ? GameUserSettingsOpenCount
				: GameUserSettingsProbeCount).fetch_add(
					1, std::memory_order_relaxed);
		}
	}

	std::atomic<int32> FullPreferenceOpenCount{0};
	std::atomic<int32> FullPreferenceProbeCount{0};
	std::atomic<int32> GameUserSettingsOpenCount{0};
	std::atomic<int32> GameUserSettingsProbeCount{0};
	IPlatformFile* LowerLevel = nullptr;
};

class FWoTBootstrapDormancyScopedFileObserver final
{
public:
	explicit FWoTBootstrapDormancyScopedFileObserver(
		FWoTBootstrapDormancyFileObserver& InObserver)
		: Observer(InObserver)
		, Previous(&FPlatformFileManager::Get().GetPlatformFile())
	{
		check(Observer.Initialize(Previous, TEXT("")));
		FPlatformFileManager::Get().SetPlatformFile(Observer);
	}
	~FWoTBootstrapDormancyScopedFileObserver()
	{
		FPlatformFileManager::Get().SetPlatformFile(*Previous);
	}

private:
	FWoTBootstrapDormancyFileObserver& Observer;
	IPlatformFile* Previous;
};

struct FWoTBootstrapDormancySpecWorld
{
	FWoTBootstrapDormancySpecWorld()
	{
		ResetWoTPayloadActivationForTesting();
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ true, /*bEnabled*/ false);

		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.bActivatePayload = false;
		World = WoTTest::CreateSpecWorld(Options);
		GameInstance = World
			? Cast<UWoTGameInstance>(World->GetGameInstance()) : nullptr;
	}

	~FWoTBootstrapDormancySpecWorld()
	{
		WoTTest::DestroySpecWorld(World);
		World = nullptr;
		GameInstance = nullptr;
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ false);
		ResetWoTPayloadActivationForTesting();
	}

	bool AssertPreActivation(FAutomationTestBase& Test) const
	{
		if (!Test.TestNotNull(TEXT("real pre-activation world exists"), World)
			|| !Test.TestNotNull(
				TEXT("real pre-activation game instance exists"), GameInstance))
		{
			return false;
		}
		Test.TestFalse(TEXT("payload activation was not published"),
			IsWoTPayloadBootstrapActive());
		Test.TestFalse(TEXT("world authorization was not published"),
			IsWoTBootstrapWorldReady());

		UWoTPayloadSubsystem* Payloads =
			GameInstance->GetSubsystem<UWoTPayloadSubsystem>();
		if (!Test.TestNotNull(TEXT("payload subsystem exists dormant"), Payloads))
		{
			return false;
		}
		Test.TestTrue(TEXT("payload subsystem reports dormant"),
			Payloads->IsDormant());
		Test.TestFalse(TEXT("no payload manifest state resolved"),
			Payloads->IsManifestReady());
		Test.TestFalse(TEXT("no payload injection started"),
			Payloads->IsInjectionEnabled());
		return true;
	}

	UWorld* World = nullptr;
	UWoTGameInstance* GameInstance = nullptr;
};

void WoTBootstrapDormancySpec_AssertNoWorldDelegates(
	FAutomationTestBase& Test, const UObject* Owner)
{
	Test.TestFalse(TEXT("no game-instance world-change delegate registered"),
		FWorldDelegates::OnGameInstanceWorldChanged.IsBoundToObject(Owner));
	Test.TestFalse(TEXT("no world-tick delegate registered"),
		FWorldDelegates::OnWorldTickStart.IsBoundToObject(Owner));
	Test.TestFalse(TEXT("no streamed-level delegate registered"),
		FWorldDelegates::LevelAddedToWorld.IsBoundToObject(Owner));
}

void WoTBootstrapDormancySpec_AssertNoObjectDelegates(
	FAutomationTestBase& Test, const UObject* Owner)
{
	Test.TestFalse(TEXT("no package delegate registered"),
		FCoreUObjectDelegates::OnPackageLoadCompleted.IsBoundToObject(Owner));
	Test.TestFalse(TEXT("no garbage-collection delegate registered"),
		FCoreUObjectDelegates::GetPostGarbageCollect().IsBoundToObject(Owner));
}

int32 WoTBootstrapDormancySpec_CountActors(UWorld* World)
{
	int32 Count = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++Count;
	}
	return Count;
}

} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyBSPProofSpec,
	"WoT.BootstrapDormancy.BSPProof",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyBSPProofSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTBSPProofSubsystem* Proof =
		Fixture.GameInstance->GetSubsystem<UWoTBSPProofSubsystem>();
	if (TestNotNull(TEXT("editor-reachable BSP proof owner exists"), Proof))
	{
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Proof);
		TestFalse(TEXT("no world begin-play delegate registered"),
			Fixture.World->OnWorldBeginPlay.IsBoundToObject(Proof));
		const int32 FWoTBootstrapDormancyBSPProofSpec_ActorCount =
			WoTBootstrapDormancySpec_CountActors(Fixture.World);
		WoTTest::TickWorld(Fixture.World, 2, 1.0f / 60.0f);
		TestEqual(TEXT("dormant proof starts no world action"),
			WoTBootstrapDormancySpec_CountActors(Fixture.World),
			FWoTBootstrapDormancyBSPProofSpec_ActorCount);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyDynamicLightSpec,
	"WoT.BootstrapDormancy.DynamicLight",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyDynamicLightSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTDynamicLightSubsystem* Lights =
		Fixture.World->GetSubsystem<UWoTDynamicLightSubsystem>();
	if (TestNotNull(TEXT("dynamic-light owner exists"), Lights))
	{
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Lights);
		TestEqual(TEXT("no dynamic lights resolved"),
			Lights->GetRegisteredCountForTesting(), 0);

		AActor* FWoTBootstrapDormancyDynamicLightSpec_Owner =
			Fixture.World->SpawnActor<AActor>();
		UPointLightComponent* FWoTBootstrapDormancyDynamicLightSpec_Component =
			FWoTBootstrapDormancyDynamicLightSpec_Owner
			? NewObject<UPointLightComponent>(
				FWoTBootstrapDormancyDynamicLightSpec_Owner) : nullptr;
		if (FWoTBootstrapDormancyDynamicLightSpec_Component)
		{
			FWoTBootstrapDormancyDynamicLightSpec_Owner->AddInstanceComponent(
				FWoTBootstrapDormancyDynamicLightSpec_Component);
			Lights->Register(
				FWoTBootstrapDormancyDynamicLightSpec_Component,
				255, 0, 0);
		}
	#if WOT_GUARDED_BOOTSTRAP
		TestEqual(TEXT("pre-activation light registration is refused"),
			Lights->GetRegisteredCountForTesting(), 0);
	#else
		TestEqual(TEXT("direct development worlds keep retail dynamic lights active"),
			Lights->GetRegisteredCountForTesting(), 1);
	#endif
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyEOFProbeSpec,
	"WoT.BootstrapDormancy.EOFProbe",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyEOFProbeSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTEOFProbeSubsystem* FWoTBootstrapDormancyEOFProbeSpec_Probe =
		Fixture.World->GetSubsystem<UWoTEOFProbeSubsystem>();
	TestNull(TEXT("diagnostic probe is absent without its opt-in"),
		FWoTBootstrapDormancyEOFProbeSpec_Probe);
	TestFalse(TEXT("probe creation is refused pre-activation"),
		GetDefault<UWoTEOFProbeSubsystem>()->ShouldCreateSubsystem(Fixture.World));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyEventsSpec,
	"WoT.BootstrapDormancy.Events",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyEventsSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTEventSubsystem* Events =
		Fixture.World->GetSubsystem<UWoTEventSubsystem>();
	if (TestNotNull(TEXT("event owner exists"), Events))
	{
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Events);
		AActor* FWoTBootstrapDormancyEventsSpec_Actor =
			Fixture.World->SpawnActor<AActor>();
		const FName FWoTBootstrapDormancyEventsSpec_Tag(
			TEXT("BootstrapDormancyForbiddenTag"));
		Events->RegisterTag(
			FWoTBootstrapDormancyEventsSpec_Tag,
			FWoTBootstrapDormancyEventsSpec_Actor);
		TestNull(TEXT("pre-activation tag registration/query is refused"),
			Events->FindFirstTagged(FWoTBootstrapDormancyEventsSpec_Tag));
		Events->FireEvent(
			FWoTBootstrapDormancyEventsSpec_Tag,
			FWoTBootstrapDormancyEventsSpec_Actor, nullptr);
		TestNull(TEXT("pre-activation dispatch leaves registry dormant"),
			Events->FindFirstTagged(FWoTBootstrapDormancyEventsSpec_Tag));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyFogGateSpec,
	"WoT.BootstrapDormancy.FogGate",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyFogGateSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTFogGateSubsystem* Fog =
		Fixture.World->GetSubsystem<UWoTFogGateSubsystem>();
	if (TestNotNull(TEXT("fog gate owner exists"), Fog))
	{
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Fog);
		TestFalse(TEXT("fog gate reports non-tickable/dormant"),
			Fog->IsTickable());
		AWoTFogLight* FWoTBootstrapDormancyFogGateSpec_Light =
			Fixture.World->SpawnActor<AWoTFogLight>();
		if (TestNotNull(TEXT("fog refusal fixture exists"),
			FWoTBootstrapDormancyFogGateSpec_Light))
		{
			TestTrue(TEXT("fog actor begins hidden"),
				FWoTBootstrapDormancyFogGateSpec_Light->IsHidden());
			TestNull(TEXT("fog mod sphere has no pre-receipt mesh"),
				FWoTBootstrapDormancyFogGateSpec_Light->SphereMod->GetStaticMesh());
			TestNull(TEXT("fog add sphere has no pre-receipt mesh"),
				FWoTBootstrapDormancyFogGateSpec_Light->SphereAdd->GetStaticMesh());
			TestFalse(TEXT("fog mod sphere begins invisible"),
				FWoTBootstrapDormancyFogGateSpec_Light->SphereMod->IsVisible());
			TestFalse(TEXT("fog add sphere begins invisible"),
				FWoTBootstrapDormancyFogGateSpec_Light->SphereAdd->IsVisible());
			Fog->Tick(1.0f / 60.0f);
			TestTrue(TEXT("pre-activation fog mutation is refused"),
				FWoTBootstrapDormancyFogGateSpec_Light->IsHidden());
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyAmbientActorsSpec,
	"WoT.BootstrapDormancy.AmbientActors",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyAmbientActorsSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	AWoTAmbientSound* Ambient =
		Fixture.World->SpawnActor<AWoTAmbientSound>();
	if (TestNotNull(TEXT("ambient actor exists"), Ambient)
		&& TestNotNull(TEXT("ambient component exists"), Ambient->Ambient.Get()))
	{
		TestNull(TEXT("ambient has no pre-receipt sound resource"),
			Ambient->Ambient->GetSound());
		TestFalse(TEXT("ambient is not playing pre-receipt"),
			Ambient->Ambient->IsPlaying());
	}
	AWoTDynamicAmbientSound* Dynamic =
		Fixture.World->SpawnActor<AWoTDynamicAmbientSound>();
	if (TestNotNull(TEXT("dynamic ambient actor exists"), Dynamic))
	{
		TestFalse(TEXT("dynamic ambient has no pre-receipt timer"),
			Fixture.World->GetTimerManager().TimerExists(
				Dynamic->RecheckTimer));
		TestEqual(TEXT("dynamic ambient has not played"),
			Dynamic->LastSound, -1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyFractalSpec,
	"WoT.BootstrapDormancy.Fractal",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyFractalSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTFractalSubsystem* Fractals =
		Fixture.World->GetSubsystem<UWoTFractalSubsystem>();
	if (TestNotNull(TEXT("fractal owner exists"), Fractals))
	{
	#if WOT_GUARDED_BOOTSTRAP
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Fractals);
	#else
		TestTrue(TEXT("direct development worlds watch streamed fractal consumers"),
			FWorldDelegates::LevelAddedToWorld.IsBoundToObject(Fractals));
	#endif
		TestFalse(TEXT("fractal simulation reports non-tickable/dormant"),
			Fractals->IsTickable());
		TestFalse(TEXT("fractal freeze state was not read"),
			Fractals->IsFrozen());
		TestEqual(TEXT("no procedural payload resolved"),
			Fractals->GetActiveTextureCount(), 0);
		TestNull(TEXT("pre-activation procedural lookup is empty"),
			Fractals->FindLiveTexture(
				FName(TEXT("BootstrapDormancy.MissingFractal"))));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyNetTestSpec,
	"WoT.BootstrapDormancy.NetTest",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyNetTestSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTNetTestSubsystem* FWoTBootstrapDormancyNetTestSpec_NetTest =
		Fixture.GameInstance->GetSubsystem<UWoTNetTestSubsystem>();
	TestNull(TEXT("net-test socket/ticker owner is absent without opt-in"),
		FWoTBootstrapDormancyNetTestSpec_NetTest);
	TestFalse(TEXT("net-test reports dormant through creation refusal"),
		GetDefault<UWoTNetTestSubsystem>()->ShouldCreateSubsystem(
			Fixture.GameInstance));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyPayloadSpec,
	"WoT.BootstrapDormancy.Payload",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyPayloadSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTPayloadSubsystem* Payloads =
		Fixture.GameInstance->GetSubsystem<UWoTPayloadSubsystem>();
	WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Payloads);
	WoTBootstrapDormancySpec_AssertNoObjectDelegates(*this, Payloads);
	TestTrue(TEXT("no command-line/config payload path resolved"),
		Payloads->GetCacheDirectory().IsEmpty());
	TestNull(TEXT("pre-activation texture lookup is refused"),
		Payloads->FindTexturePayload(TEXT("cache/texture/forbidden")));
	TestNull(TEXT("pre-activation sound lookup is refused"),
		Payloads->FindSoundPayload(TEXT("cache/sound/forbidden")));
	TestNull(TEXT("pre-activation mesh lookup is refused"),
		Payloads->FindMeshPayload(TEXT("cache/mesh/forbidden")));
	TestNull(TEXT("pre-activation BSP lookup is refused"),
		Payloads->FindBSPPayload(TEXT("cache/bsp/forbidden")));
	TestFalse(TEXT("pre-activation texture blob read is refused"),
		Payloads->LoadTexturePayload(
			TEXT("cache/texture/forbidden")).IsValid());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancySessionSpec,
	"WoT.BootstrapDormancy.Session",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancySessionSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTSessionSubsystem* Sessions =
		Fixture.GameInstance->GetSubsystem<UWoTSessionSubsystem>();
	if (TestNotNull(TEXT("session owner exists"), Sessions))
	{
		APlayerController* Controller =
			Fixture.World->SpawnActor<APlayerController>();
		TestNotNull(TEXT("real travel controller exists"), Controller);
		Sessions->SetClientTravelControllerForTesting(Controller);
		int32 TravelCount = 0;
		Sessions->SetClientTravelHookForTesting(
			[&TravelCount](APlayerController*, const FString&, ETravelType)
			{
				++TravelCount;
			});
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Sessions);
		if (GEngine)
		{
			TestFalse(TEXT("no network-failure delegate registered"),
				GEngine->OnNetworkFailure().IsBoundToObject(Sessions));
			TestFalse(TEXT("no travel-failure delegate registered"),
				GEngine->OnTravelFailure().IsBoundToObject(Sessions));
		}
		TestFalse(TEXT("no disconnect delegate registered"),
			FGameDelegates::Get().GetHandleDisconnectDelegate()
				.IsBoundToObject(Sessions));
		TestFalse(TEXT("no OSS session resolved"), Sessions->HasSession());
		TestFalse(TEXT("pre-activation hosting is refused"), Sessions->Host());
		TestFalse(TEXT("pre-activation search is refused"), Sessions->Find());
		TestFalse(TEXT("pre-activation join is refused"), Sessions->Join(0));
		TestFalse(TEXT("pre-activation direct travel is refused"),
			Sessions->ConnectDirect(TEXT("127.0.0.1")));
		TestFalse(TEXT("pre-activation console leave is refused"),
			Sessions->Leave());
		TestEqual(TEXT("no client-travel hook fired"), TravelCount, 0);
		TestTrue(TEXT("no host travel URL resolved"),
			Sessions->GetLastHostTravelURL().IsEmpty());
		TestTrue(TEXT("no return travel URL resolved"),
			Sessions->GetLastReturnTravelURL().IsEmpty());
		TestEqual(TEXT("session state reports dormant"),
			Sessions->GetLeaveState(), EWoTLeaveState::Idle);

		TestFalse(TEXT("test payload receipt publishes"),
			PublishWoTPayloadBootstrapActivationForTesting().IsError());
		TestFalse(TEXT("test world receipt publishes"),
			PublishWoTBootstrapWorldReadyForTesting().IsError());
		TestFalse(TEXT("exposure receipt cannot activate session lifecycle"),
			Sessions->ActivateForAuthorizedWorld());
		TestFalse(TEXT("replayed exposure receipt remains unauthorized"),
			Sessions->ActivateForAuthorizedWorld());
		if (GEngine)
		{
			TestFalse(TEXT("network-failure delegate stays dormant without adoption"),
				GEngine->OnNetworkFailure().IsBoundToObject(Sessions));
			TestFalse(TEXT("travel-failure delegate stays dormant without adoption"),
				GEngine->OnTravelFailure().IsBoundToObject(Sessions));
		}
		TestFalse(TEXT("exposure-only direct travel remains refused"),
			Sessions->ConnectDirect(TEXT("127.0.0.1")));
		TestEqual(TEXT("direct travel hook remains dormant"), TravelCount, 0);
		TestFalse(TEXT("exposure-only console leave remains refused"),
			Sessions->Leave());
		TestEqual(TEXT("leave travel hook remains dormant"), TravelCount, 0);
		ResetWoTPayloadActivationForTesting();
		if (GEngine)
		{
			TestFalse(TEXT("network delegate deactivates with receipt"),
				GEngine->OnNetworkFailure().IsBoundToObject(Sessions));
			TestFalse(TEXT("travel delegate deactivates with receipt"),
				GEngine->OnTravelFailure().IsBoundToObject(Sessions));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyPreferencesSpec,
	"WoT.BootstrapDormancy.Preferences",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyPreferencesSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancyFileObserver FileObserver;
	FWoTBootstrapDormancyScopedFileObserver ScopedFileObserver(FileObserver);
	FWoTEarlyUIAuthority& Authority = GetEarlyUIAuthority();
	Authority.ResetForTesting();
	TArray<uint8> Bytes;
	const ANSICHAR Source[] =
		"Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"
		"BootstrapVolume=1.0\nInputSensitivity=1.0\n";
	Bytes.Append(reinterpret_cast<const uint8*>(Source), UE_ARRAY_COUNT(Source) - 1);
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed = ParseEarlyUIV1(
		Bytes, FWoTEarlyUILimits::Production(),
		FWoTInventoryLocaleSet::Production());
	if (!TestFalse(TEXT("real dormancy EarlyUI parses"), Parsed.IsError()))
	{
		return false;
	}
	FWoTBootstrapStatus Published = Authority.PublishOnce(
		MoveTemp(Parsed).GetValue());
	if (!TestFalse(TEXT("real dormancy EarlyUI publishes"), Published.IsError()))
	{
		return false;
	}
	uint16 AppliedWidth = 0;
	auto Apply = [&AppliedWidth](uint16 Width, uint16, EWoTWindowMode)
		-> FWoTBootstrapStatus
	{
		AppliedWidth = Width;
		return FWoTBootstrapStatus::Success();
	};
	const FWoTEarlyUINativeProjectionSink Sink{Apply};
	FString Error;
	if (!TestTrue(TEXT("real dormancy native projection succeeds"),
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			Sink, &Error)))
	{
		Authority.ResetForTesting();
		return false;
	}
	TestEqual(TEXT("projection observed parsed width"),
		static_cast<int32>(AppliedWidth), 1280);

	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	{
		FWoTBootstrapDormancySpecWorld Fixture;
		if (!Fixture.AssertPreActivation(*this))
		{
			UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false);
			Authority.ResetForTesting();
			return false;
		}
		UWoTPreferencesSubsystem* Preferences =
			Fixture.GameInstance->GetSubsystem<UWoTPreferencesSubsystem>();
		if (TestNotNull(TEXT("real preference subsystem exists"), Preferences))
		{
			TestTrue(TEXT("only authenticated EarlyUI was adopted"),
				Preferences->HasAdoptedAuthenticatedEarlyUI());
			TestFalse(TEXT("full-preference state remains unloaded pre-activation"),
				Preferences->HasLoadedFullPreferences());
			WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Preferences);
			WoTBootstrapDormancySpec_AssertNoObjectDelegates(*this, Preferences);
			TestTrue(TEXT("payload remains dormant after preference adoption"),
				Fixture.GameInstance->GetSubsystem<UWoTPayloadSubsystem>()->IsDormant());
		}
	}
	TestEqual(TEXT("observed zero full-preference opens pre-activation"),
		FileObserver.PreferenceOpens(), 0);
	TestEqual(TEXT("observed zero full-preference probes pre-activation"),
		FileObserver.PreferenceProbes(), 0);
	TestEqual(TEXT("observed zero GameUserSettings opens pre-activation"),
		FileObserver.ConfigOpens(), 0);
	TestEqual(TEXT("observed zero GameUserSettings probes pre-activation"),
		FileObserver.ConfigProbes(), 0);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false);
	Authority.ResetForTesting();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyShotSpec,
	"WoT.BootstrapDormancy.Shot",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyShotSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTShotSubsystem* Shot =
		Fixture.World->GetSubsystem<UWoTShotSubsystem>();
	if (TestNotNull(TEXT("editor-reachable shot owner exists dormant"), Shot))
	{
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Shot);
		const int32 FWoTBootstrapDormancyShotSpec_ActorCount =
			WoTBootstrapDormancySpec_CountActors(Fixture.World);
		FScreenshotRequest::Reset();
		WoTTest::TickWorld(Fixture.World, 3.25f, 0.25f);
		TestEqual(TEXT("no capture actor or timer action started"),
			WoTBootstrapDormancySpec_CountActors(Fixture.World),
			FWoTBootstrapDormancyShotSpec_ActorCount);
		TestFalse(TEXT("no screenshot capture requested"),
			FScreenshotRequest::IsScreenshotRequested());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyUnderwaterAudioSpec,
	"WoT.BootstrapDormancy.UnderwaterAudio",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyUnderwaterAudioSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTUnderwaterAudioSubsystem* Underwater =
		Fixture.World->GetSubsystem<UWoTUnderwaterAudioSubsystem>();
	if (TestNotNull(TEXT("underwater-audio owner exists"), Underwater))
	{
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Underwater);
		TestFalse(TEXT("underwater listener begins dormant"),
			Underwater->bListenerUnderwater);
		WoTTest::TickWorld(Fixture.World, 0.25f, 0.05f);
		TestFalse(TEXT("no listener lookup/timer mutation ran"),
			Underwater->bListenerUnderwater);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBootstrapDormancyZoneIndexSpec,
	"WoT.BootstrapDormancy.ZoneIndex",
	WoTBootstrapDormancySpecFlags)

bool FWoTBootstrapDormancyZoneIndexSpec::RunTest(const FString&)
{
	FWoTBootstrapDormancySpecWorld Fixture;
	if (!Fixture.AssertPreActivation(*this))
	{
		return false;
	}
	UWoTZoneIndex* Index = Fixture.World->GetSubsystem<UWoTZoneIndex>();
	if (TestNotNull(TEXT("zone index owner exists"), Index))
	{
		WoTBootstrapDormancySpec_AssertNoWorldDelegates(*this, Index);
		AWoTZoneVolume* FWoTBootstrapDormancyZoneIndexSpec_Zone =
			Fixture.World->SpawnActor<AWoTZoneVolume>();
		if (TestNotNull(TEXT("zone refusal fixture exists"),
			FWoTBootstrapDormancyZoneIndexSpec_Zone))
		{
			FWoTBootstrapDormancyZoneIndexSpec_Zone->bWaterZone = true;
			FWoTBootstrapDormancyZoneIndexSpec_Zone->bPainZone = true;
			TestFalse(TEXT("pre-activation zone tick remains disabled"),
				FWoTBootstrapDormancyZoneIndexSpec_Zone->IsActorTickEnabled());
		}
		TestEqual(TEXT("pre-activation zone registration is refused"),
			Index->Zones.Num(), 0);
		const FVector FWoTBootstrapDormancyZoneIndexSpec_Point =
			FVector::ZeroVector;
		TestFalse(TEXT("pre-activation water query is refused"),
			AWoTZoneVolume::IsPointInWater(
				Fixture.World, FWoTBootstrapDormancyZoneIndexSpec_Point));
		TestFalse(TEXT("pre-activation pain query is refused"),
			AWoTZoneVolume::IsPointInPainZone(
				Fixture.World, FWoTBootstrapDormancyZoneIndexSpec_Point));
		TestNull(TEXT("pre-activation water lookup is refused"),
			AWoTZoneVolume::WaterZoneAt(
				Fixture.World, FWoTBootstrapDormancyZoneIndexSpec_Point));
		TestNull(TEXT("pre-activation pain lookup is refused"),
			AWoTZoneVolume::PainZoneAt(
				Fixture.World, FWoTBootstrapDormancyZoneIndexSpec_Point));
		FWoTZoneMovement FWoTBootstrapDormancyZoneIndexSpec_Movement;
		TestFalse(TEXT("pre-activation movement lookup is refused"),
			AWoTZoneVolume::GetMovementEffectAt(
				Fixture.World, FWoTBootstrapDormancyZoneIndexSpec_Point,
				FWoTBootstrapDormancyZoneIndexSpec_Movement));
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
