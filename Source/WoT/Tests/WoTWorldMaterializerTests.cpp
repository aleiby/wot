#include "WoTCacheTestSupport.h"
#include "Misc/AutomationTest.h"

#include "../Payload/WoTCacheStore.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTVerifiedBlobReader.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTMaterializedWorldHost.h"
#include "WoTFrontEndTestData.h"
#include "WoTTestWorld.h"
#include "WoTWorldMaterializerTestData.h"

#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "WoTBootstrapCore/Hash.h"
#include "../WoTGameInstance.h"
#include "../WoTBootFailureReceipt.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTAnimData.h"
#include "../Mesh/WoTRuntimeVertexMesh.h"
#include "../WoTFrontEndCatalog.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "../WoTPlayerController.h"
#include "WoTRetailReader/ProductionContract.h"
#include "OnlineSubsystem.h"
#include "OnlineSubsystemNames.h"
#include "OnlineSubsystemUtils.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTWorldMaterializerSpecFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter;

struct FWoTWorldMaterializerSpecRoot
{
	FWoTWorldMaterializerSpecRoot()
	{
		Path = FPaths::Combine(FPaths::AutomationTransientDir(),
			TEXT("wot-world-materializer-")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(Path),
			TEXT("ContentCache"));
		FPaths::NormalizeDirectoryName(Path);
		IFileManager::Get().MakeDirectory(*Path, true);
	}

	~FWoTWorldMaterializerSpecRoot()
	{
		IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, true);
	}

	FString Path;
};

struct FWoTWorldMaterializerSpecActivationWorld
{
	FWoTWorldMaterializerSpecActivationWorld()
	{
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ true, /*bEnabled*/ false);
		WoTTest::FSpecWorldOptions Options;
		Options.GameInstanceClass = UWoTGameInstance::StaticClass();
		Options.bStartPlay = false;
		Options.bActivatePayload = false;
		World = WoTTest::CreateSpecWorld(Options);
		Payloads = World && World->GetGameInstance()
			? World->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>()
			: nullptr;
	}

	~FWoTWorldMaterializerSpecActivationWorld()
	{
		FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
		WoTTest::DestroySpecWorld(World);
		World = nullptr;
		Payloads = nullptr;
		UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(
			/*bHasOverride*/ false);
		ResetWoTPayloadActivationForTesting();
	}

	UWorld* World = nullptr;
	UWoTPayloadSubsystem* Payloads = nullptr;
};

struct FWoTWorldMaterializerContractScope
{
	explicit FWoTWorldMaterializerContractScope(
		const FWoTRetailContract& Contract)
	{
		WoTSetProductionRetailContractForTesting(&Contract);
	}

	~FWoTWorldMaterializerContractScope()
	{
		WoTSetProductionRetailContractForTesting(nullptr);
	}
};

FString WoTWorldMaterializerSpecBlobLeaf(FStringView LogicalId)
{
	const FTCHARToUTF8 Utf8(LogicalId.GetData(), LogicalId.Len());
	return Sha256(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length())).ToHex()
		+ TEXT(".blob");
}

UWorld* WoTWorldMaterializerSpecCandidate(const TCHAR* PackageName)
{
	UPackage* Package = CreatePackage(PackageName);
	Package->SetFlags(RF_Transient);
	return UWorld::CreateWorld(EWorldType::Game, false,
		MakeUniqueObjectName(
			Package, UWorld::StaticClass(), FName(TEXT("World"))), Package,
		/*bAddToRoot*/ true, ERHIFeatureLevel::Num,
		/*InIVS*/ nullptr, /*bInSkipInitWorld*/ true);
}

bool WoTWorldMaterializerDescriptorsEqual(
	const FWoTWorldDescriptor& Left, const FWoTWorldDescriptor& Right)
{
	if (Left.MapIdentity != Right.MapIdentity
		|| Left.NeutralMapSlot != Right.NeutralMapSlot
		|| Left.BSPFactId != Right.BSPFactId
		|| Left.BSPImporter != Right.BSPImporter
		|| Left.WorldLogicalId != Right.WorldLogicalId
		|| Left.WorldSchema != Right.WorldSchema
		|| Left.WorldFactId != Right.WorldFactId
		|| Left.WorldImporter != Right.WorldImporter
		|| Left.PackageSha256 != Right.PackageSha256
		|| Left.WorldSha256 != Right.WorldSha256
		|| Left.SourceRoot != Right.SourceRoot
		|| Left.MapRoot != Right.MapRoot
		|| Left.ActorCount != Right.ActorCount
		|| Left.AttachmentCount != Right.AttachmentCount
		|| Left.BSPNodeCount != Right.BSPNodeCount
		|| Left.BSPSurfaceCount != Right.BSPSurfaceCount
		|| Left.ZoneCount != Right.ZoneCount
		|| Left.PortalSurfaceCount != Right.PortalSurfaceCount
		|| Left.MoverCount != Right.MoverCount
		|| Left.LightCount != Right.LightCount
		|| Left.SoundCount != Right.SoundCount
		|| Left.NavigationSeedCount != Right.NavigationSeedCount
		|| Left.SemanticObjectCount != Right.SemanticObjectCount
		|| Left.ReachSpecCount != Right.ReachSpecCount
		|| Left.BSPParts.Num() != Right.BSPParts.Num())
	{
		return false;
	}
	for (int32 Index = 0; Index < Left.BSPParts.Num(); ++Index)
	{
		const FWoTBSPPartDescriptor& A = Left.BSPParts[Index];
		const FWoTBSPPartDescriptor& B = Right.BSPParts[Index];
		if (A.Role != B.Role || A.PartIdentity != B.PartIdentity
			|| A.ProceduralMaterialSlot != B.ProceduralMaterialSlot
			|| A.LogicalId != B.LogicalId || A.Schema != B.Schema
			|| A.FactId != B.FactId
			|| A.PayloadSha256 != B.PayloadSha256
			|| A.Sha256 != B.Sha256
			|| A.VertexCount != B.VertexCount
			|| A.IndexCount != B.IndexCount
			|| A.SectionCount != B.SectionCount)
		{
			return false;
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldDescriptorDerivationSpec,
	"WoT.World.Materializer.DescriptorDerivationDeterminismAndTamper",
	WoTWorldMaterializerSpecFlags)

bool FWoTWorldDescriptorDerivationSpec::RunTest(const FString&)
{
	FWoTWorldMaterializerSpecRoot Root;
	const FWoTRetailContract Contract =
		WoTWorldMaterializerTest_WorldContract();
	FWoTWorldMaterializerContractScope ContractScope(Contract);
	TArray<FWoTWorldDescriptor> PublishedDescriptors;
	FString Error;
	if (!TestTrue(TEXT("world generation with sealed sidecar publishes"),
		WoTWorldMaterializerTest_PublishWorldSet(
			Root.Path, PublishedDescriptors, Error)))
	{
		AddError(Error);
		return false;
	}
	auto Opened = FWoTCacheStore::ForTesting(Root.Path);
	if (!TestFalse(TEXT("published store reopens"), Opened.IsError()))
	{
		return false;
	}
	FWoTCacheStore Store = MoveTemp(Opened).GetValue();
	auto Snapshot = WoTVerifyCurrentForTesting(
		Store, Contract.StartupCriticalLogicalIds);
	if (!TestFalse(TEXT("world critical snapshot validates"),
		Snapshot.IsError()))
	{
		return false;
	}
	FWoTCacheGenerationLease Lease = MoveTemp(Snapshot).GetValue();
	auto First = WoTDeriveWorldDescriptorsFromActiveGeneration(Lease);
	auto Second = WoTDeriveWorldDescriptorsFromActiveGeneration(Lease);
	if (TestFalse(TEXT("first derivation succeeds"), First.IsError())
		&& TestFalse(TEXT("second derivation succeeds"), Second.IsError()))
	{
		TestEqual(TEXT("both derivations have the closed census"),
			First.GetValue().Num(), Second.GetValue().Num());
		for (int32 Index = 0;
			Index < FMath::Min(First.GetValue().Num(), Second.GetValue().Num());
			++Index)
		{
			TestTrue(TEXT("all descriptor fields are deterministic"),
				WoTWorldMaterializerDescriptorsEqual(
					First.GetValue()[Index], Second.GetValue()[Index]));
			TestTrue(TEXT("construction and parsed descriptor fields match"),
				PublishedDescriptors.IsValidIndex(Index)
				&& WoTWorldMaterializerDescriptorsEqual(
					PublishedDescriptors[Index], First.GetValue()[Index]));
		}
	}

	const FString ManifestPath =
		FPaths::Combine(Root.Path, TEXT("current/manifest.json"));
	TArray<uint8> ManifestBytes;
	if (TestTrue(TEXT("manifest is readable for the tamper fixture"),
		FFileHelper::LoadFileToArray(ManifestBytes, *ManifestPath))
		&& !ManifestBytes.IsEmpty())
	{
		TArray<uint8> CorruptBytes = ManifestBytes;
		CorruptBytes[0] ^= 1;
		TestTrue(TEXT("tampered manifest is written"),
			FFileHelper::SaveArrayToFile(CorruptBytes, *ManifestPath));
		TestFalse(TEXT("held snapshot ignores later manifest tampering"),
			WoTDeriveWorldDescriptorsFromActiveGeneration(Lease).IsError());
		auto Reopened = Store.TryOpenCurrent({}, nullptr);
		TestTrue(TEXT("a new snapshot rejects malformed manifest metadata"),
			!Reopened.IsError() && !Reopened.GetValue().IsSet());
		TestTrue(TEXT("original manifest is restored"),
			FFileHelper::SaveArrayToFile(ManifestBytes, *ManifestPath));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTWorldRuntimeIntegrityHostJoinSpec,
	"WoT.World.Materializer.RuntimeIntegrityHostAndClientReturn",
	WoTWorldMaterializerSpecFlags)

bool FWoTWorldRuntimeIntegrityHostJoinSpec::RunTest(const FString&)
{
	auto RunPath = [this](EWoTWorldStartPath Path, const TCHAR* Label)
	{
		ResetWoTPayloadActivationForTesting();
		FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
		FWoTWorldMaterializerSpecRoot Root;
		const FString DiagnosticRoot = FPaths::Combine(
			Root.Path, FString(TEXT("diagnostics-")) + Label);
		SetWoTBootstrapHeartbeatRootForTesting(DiagnosticRoot);
		SetWoTBootFailureReceiptRootForTesting(DiagnosticRoot);
		const FWoTRetailContract Contract =
			WoTWorldMaterializerTest_WorldContract();
		FString Error;
		const FWoTFrontEndDescriptor* FrontEnd =
			WoTWorldMaterializerTest_CreateFrontEndAuthority(Error);
		if (!TestNotNull(TEXT("the attested front-end descriptor fixture derives"),
			FrontEnd))
		{
			AddError(Error);
			return false;
		}
		ON_SCOPE_EXIT
		{
			WoTWorldMaterializerTest_DestroyFrontEndAuthority(FrontEnd);
		};
		WoTSetProductionRetailContractForTesting(&Contract);
		ON_SCOPE_EXIT
		{
			SetWoTBootstrapHeartbeatRootForTesting(FString());
			SetWoTBootFailureReceiptRootForTesting(FString());
			WoTSetProductionRetailContractForTesting(nullptr);
			FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
			ResetWoTPayloadActivationForTesting();
		};
		TArray<FWoTWorldDescriptor> Descriptors;
		if (!TestTrue(TEXT("the complete production-path world cache publishes"),
			WoTWorldMaterializerTest_PublishWorldSet(
				Root.Path, Descriptors, Error, FrontEnd)))
		{
			AddError(Error);
			return false;
		}
		TWoTBootstrapResult<FWoTCacheStore> Opened =
			FWoTCacheStore::ForTesting(Root.Path);
		if (Opened.IsError()) return false;
		FWoTCacheStore Store = MoveTemp(Opened).GetValue();
		TArray<FString> CriticalIds = FrontEnd->RequiredLogicalIds;
		auto Snapshot = WoTVerifyCurrentForTesting(Store, CriticalIds);
		if (!TestFalse(TEXT("the attested front-end closure verifies before play"),
			Snapshot.IsError())) return false;
		const FString Generation = TEXT("current");
		FWoTCacheGenerationLease SnapshotLease =
			MoveTemp(Snapshot).GetValue();
		UWorld* Bootstrap = WoTWorldMaterializerSpecCandidate(
			TEXT("/Game/WoT/Maps/WoTBootstrap"));
		if (!Bootstrap) return false;
		ON_SCOPE_EXIT
		{
			if (!Bootstrap->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
				Bootstrap->DestroyWorld(false);
		};
		if (!TestTrue(TEXT("the initial bootstrap package is exposed"),
			WoTGuardedStartup_MaterializeWorld(Bootstrap,
				EWoTWorldStartPath::NormalInitialLoad, &Error)
			&& WoTCommitAuthenticatedWorldExposure(Bootstrap, &Error)))
			return false;

		FWoTWorldMaterializerSpecActivationWorld ActivationWorld;
		if (!ActivationWorld.Payloads) return false;
		UWoTSessionSubsystem* Sessions = ActivationWorld.World->GetGameInstance()
			->GetSubsystem<UWoTSessionSubsystem>();
		if (!TestNotNull(TEXT("the production session presentation hook exists"),
			Sessions)
			|| !TestTrue(TEXT("the production bootstrap prepares session consumers"),
				Sessions->ActivateForAdoptedWorldAuthority()))
		{
			return false;
		}
		FWoTBootstrapStatus Activated =
			ActivationWorld.Payloads->ActivateVerifiedCache(SnapshotLease);
		if (Activated.IsError()) return false;
		if (!TestFalse(TEXT("the shared verified snapshot authorizes worlds"),
			WoTAuthorizeNormalWorldMaterialization(
				SnapshotLease,
				TArray<FWoTWorldDescriptor>(Descriptors)).IsError())) return false;

		const FString MapIdentity(TEXT("Arena_02"));
		const FString LogicalId(TEXT("cache/world/Arena_02"));
		const FString DisplayName =
			FWoTFrontEndMapCatalog::ResolveRetailTitle(MapIdentity);
		const FString ExpectedMessage = FString::Printf(
			TEXT("Some of WoT Reborn's game data is damaged (%s). It will be rebuilt the next time you start the game."),
			*DisplayName);
		const FString BlobPath = FPaths::Combine(Root.Path, Generation,
			TEXT("blobs"), WoTWorldMaterializerSpecBlobLeaf(LogicalId));
		auto DamageTravelOutput = [&BlobPath]()
		{
			TArray<uint8> Damaged;
			if (!FFileHelper::LoadFileToArray(Damaged, *BlobPath)
				|| Damaged.IsEmpty()) return false;
			Damaged[Damaged.Num() / 2] ^= 0x41u;
			return FFileHelper::SaveArrayToFile(Damaged, *BlobPath);
		};

		if (!TestTrue(TEXT("the real session path is authorized"),
			Sessions->CanUseAuthorizedWorldForTesting()))
		{
			return false;
		}
		UWorld* FailedWorld = WoTWorldMaterializerSpecCandidate(
			TEXT("/WoTNeutralShells/WoTMapShell02"));
		if (!FailedWorld) return false;
		FailedWorld->SetGameInstance(
			ActivationWorld.World->GetGameInstance());
		ON_SCOPE_EXIT
		{
			if (!FailedWorld->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
				FailedWorld->DestroyWorld(false);
		};
		FString Presented;
		const FDelegateHandle PresentedHandle = Sessions->OnConnectionFailure().AddLambda(
			[&Presented](const FString& Message) { Presented = Message; });
		ON_SCOPE_EXIT
		{
			Sessions->OnConnectionFailure().Remove(PresentedHandle);
			Sessions->SetServerTravelHookForTesting({});
			Sessions->SetClientTravelHookForTesting({});
			Sessions->SetClientTravelControllerForTesting(nullptr);
		};

		if (Path == EWoTWorldStartPath::ListenHostTravel)
		{
			int32 HostCompletions = 0;
			bool bLastHost = true;
			const FDelegateHandle HostHandle = Sessions->OnHostComplete().AddLambda(
				[&](bool bOk) { ++HostCompletions; bLastHost = bOk; });
			bool bTravelHookRan = false;
			Sessions->SetServerTravelHookForTesting(
				[&](UWorld*, const FString&, bool)
				{
					bTravelHookRan = true;
					return WoTGuardedStartup_MaterializeWorld(FailedWorld,
						EWoTWorldStartPath::ListenHostTravel, &Error);
				});
			FWoTHostRequest Request = FWoTFrontEndModel::DefaultHostRequest(
				TEXT("/Game/WoT/Maps/Arena_02"));
			Request.MapTitle = DisplayName;
			Request.ServerName = TEXT("Runtime Integrity Host Spec");
			Request.bLAN = true;
			if (!DamageTravelOutput()) return false;
			TestTrue(TEXT("the real host request is accepted"), Sessions->Host(Request));
			FWorldDelegates::OnWorldTickStart.Broadcast(ActivationWorld.World, LEVELTICK_All, 0.f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(
				ENamedThreads::GameThread);
			TestTrue(TEXT("the host dispatches its real server-travel hook"),
				bTravelHookRan);
			TestEqual(TEXT("the damaged host completes once"), HostCompletions, 1);
			TestFalse(TEXT("the damaged host reports terminal failure"), bLastHost);
			Sessions->OnHostComplete().Remove(HostHandle);
		}
		else
		{
			const FName RemoteSessionName(
				TEXT("WoTRuntimeIntegrityRemoteSession"));
			IOnlineSubsystem* RemoteOSS = IOnlineSubsystem::Get(
				FName(TEXT("NULL:WoTRuntimeIntegrityRemote")));
			const IOnlineSubsystem* LanOSS = Online::GetSubsystem(
				ActivationWorld.World, NULL_SUBSYSTEM);
			if (!TestNotNull(TEXT("the remote Null host provider exists"), RemoteOSS)
				|| !TestNotNull(TEXT("the client LAN provider exists"), LanOSS))
			{
				return false;
			}
			const IOnlineSessionPtr Remote = RemoteOSS->GetSessionInterface();
			const IOnlineSessionPtr Lan = LanOSS->GetSessionInterface();
			ON_SCOPE_EXIT
			{
				for (const IOnlineSessionPtr& Provider : {Remote, Lan})
				{
					if (Provider.IsValid()
						&& Provider->GetNamedSession(NAME_GameSession))
					{
						Provider->RemoveNamedSession(NAME_GameSession);
					}
					if (Provider.IsValid()
						&& Provider->GetNamedSession(RemoteSessionName))
					{
						Provider->RemoveNamedSession(RemoteSessionName);
					}
				}
			};
			FWoTHostRequest RemoteRequest = FWoTFrontEndModel::DefaultHostRequest(
				TEXT("/Game/WoT/Maps/Arena_02"));
			RemoteRequest.bLAN = true;
			if (!TestTrue(TEXT("the remote real session is created"),
				Remote->CreateSession(0, RemoteSessionName,
					Sessions->BuildSessionSettingsForTesting(RemoteRequest))))
			{
				return false;
			}
			const FNamedOnlineSession* Hosted =
				Remote->GetNamedSession(RemoteSessionName);
			if (!TestNotNull(TEXT("the remote map session exists"), Hosted))
				return false;
			FOnlineSessionSearchResult Row;
			Row.Session = FOnlineSession(*Hosted);
			const FWoTSessionListEntry Entry =
				Sessions->InjectSearchResultForTesting(Lan, false, Row);
			AWoTPlayerController* Controller =
				ActivationWorld.World->SpawnActor<AWoTPlayerController>();
			if (!TestNotNull(TEXT("the client travel controller exists"), Controller))
				return false;
			Sessions->SetClientTravelControllerForTesting(Controller);
			bool bClientMaterializerRejected = false;
			Sessions->SetClientTravelHookForTesting(
				[&](APlayerController*, const FString&, ETravelType)
				{
					bClientMaterializerRejected =
						!WoTGuardedStartup_MaterializeWorld(FailedWorld,
							EWoTWorldStartPath::RemoteClientTravel, &Error);
				});
			int32 JoinCompletions = 0;
			const FDelegateHandle JoinHandle =
				Sessions->OnJoinCompleteForUI().AddLambda(
					[&](bool) { ++JoinCompletions; });
			TestTrue(TEXT("the client fixture readopts its production session consumer"),
				Sessions->ActivateForAdoptedWorldAuthority());
			TestTrue(TEXT("the client retains normal-world authority before join"),
				IsWoTNormalWorldAuthorityAdopted());
			TestTrue(TEXT("the client retains payload activation before join"),
				IsWoTPayloadBootstrapActive());
			TestTrue(TEXT("the client authority remains live immediately before join"),
				Sessions->CanUseAuthorizedWorldForTesting());
			TestTrue(TEXT("no client integrity event precedes the join travel"),
				Presented.IsEmpty());
			if (!DamageTravelOutput()) return false;
			const bool bJoinAccepted = Sessions->Join(Entry);
			if (!TestTrue(TEXT("the real client join is dispatched"), bJoinAccepted))
			{
				AddError(FString(TEXT("join outcome: "))
					+ Sessions->LastJoinOutcomeForTesting());
			}
			FWorldDelegates::OnWorldTickStart.Broadcast(ActivationWorld.World, LEVELTICK_All, 0.f);
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(
				ENamedThreads::GameThread);
			TestTrue(TEXT("the joined client's damaged map is rejected"),
				bClientMaterializerRejected);
			TestEqual(TEXT("the real join completes once"), JoinCompletions, 1);
			TestFalse(TEXT("integrity recovery destroys the joined session"),
				Sessions->HasSession());
			Sessions->OnJoinCompleteForUI().Remove(JoinHandle);
		}
		TestEqual(TEXT("the presentation hook receives the exact damage message"),
			Presented, ExpectedMessage);
		TestEqual(TEXT("the exact damage message is retained for the returned menu"),
			Sessions->RetainedConnectionFailure(), ExpectedMessage);
		TestEqual(TEXT("the real session cleanup selects the authenticated front end"),
			Sessions->GetLastReturnTravelURL(), FWoTLeaveStateMachine::FrontEndURL());
		TestTrue(TEXT("the independently retained front end is recovery-authorized"),
			IsWoTFrontEndIntegrityRecoveryAuthorized());
		TestTrue(TEXT("the damaged current is excluded on next open"),
			!Store.TryOpenCurrent({}, nullptr).GetValue().IsSet());
		return !HasAnyErrors();
	};

	const bool bClientCompleted =
		RunPath(EWoTWorldStartPath::RemoteClientTravel, TEXT("client"));
	TestTrue(TEXT("the real client join recovery path completes"),
		bClientCompleted);
	const bool bHostCompleted =
		RunPath(EWoTWorldStartPath::ListenHostTravel, TEXT("host"));
	TestTrue(TEXT("the real host recovery path completes"), bHostCompleted);
	return bHostCompleted && bClientCompleted && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeIntegrityNonWorldLoaderSpec,
	"WoT.World.Materializer.RuntimeIntegrityNonWorldLoaderRecovery",
	WoTWorldMaterializerSpecFlags)

bool FWoTRuntimeIntegrityNonWorldLoaderSpec::RunTest(const FString&)
{
	ResetWoTPayloadActivationForTesting();
	FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
	FWoTWorldMaterializerSpecRoot Root;
	const FString DiagnosticRoot = FPaths::Combine(Root.Path, TEXT("diagnostics-animation"));
	SetWoTBootstrapHeartbeatRootForTesting(DiagnosticRoot);
	SetWoTBootFailureReceiptRootForTesting(DiagnosticRoot);
	const FWoTRetailContract Contract = WoTWorldMaterializerTest_WorldContract();
	FString Error;
	const FString LogicalId(TEXT("cache/animation/RuntimeIntegritySpec"));
	const FWoTFrontEndDescriptor* FrontEnd =
		WoTWorldMaterializerTest_CreateFrontEndAuthority(Error);
	if (!TestNotNull(TEXT("the non-world recovery front-end authority derives"),
		FrontEnd))
	{
		AddError(Error);
		return false;
	}
	int32 LazyReadsBeforeDamage = 0;
	TSharedPtr<uint8, ESPMode::ThreadSafe> ReadObserver =
		WoTObserveCacheSnapshotPhysicalReadsForTesting(
			[&](FStringView Object)
			{
				if (Object == FStringView(
					TEXT("blobs/cache/animation/RuntimeIntegritySpec")))
				{
					++LazyReadsBeforeDamage;
				}
			});
	WoTSetProductionRetailContractForTesting(&Contract);
	ON_SCOPE_EXIT
	{
		WoTWorldMaterializerTest_DestroyFrontEndAuthority(FrontEnd);
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		SetWoTBootFailureReceiptRootForTesting(FString());
		WoTSetProductionRetailContractForTesting(nullptr);
		FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
		ResetWoTPayloadActivationForTesting();
	};
	TArray<FWoTWorldDescriptor> Descriptors;
	if (!TestTrue(TEXT("the lazy animation fixture publishes through the world set"),
		WoTWorldMaterializerTest_PublishWorldSet(Root.Path, Descriptors, Error,
			FrontEnd, WoTFrontEndWorldManifestAttackNone,
			/*bIncludeLazyRuntimeAnimation*/ true)))
	{
		AddError(Error);
		return false;
	}
	auto Opened = FWoTCacheStore::ForTesting(Root.Path);
	if (Opened.IsError()) return false;
	FWoTCacheStore Store = MoveTemp(Opened).GetValue();
	TArray<FString> CriticalIds = FrontEnd->RequiredLogicalIds;
	auto Snapshot = WoTVerifyCurrentForTesting(Store, CriticalIds);
	if (!TestFalse(TEXT("the non-world front-end closure verifies before play"),
		Snapshot.IsError())) return false;
	const FString Generation = TEXT("current");
	FWoTCacheGenerationLease SnapshotLease = MoveTemp(Snapshot).GetValue();
	UWorld* Bootstrap = WoTWorldMaterializerSpecCandidate(
		TEXT("/Game/WoT/Maps/WoTBootstrap"));
	if (!Bootstrap) return false;
	ON_SCOPE_EXIT
	{
		if (!Bootstrap->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			Bootstrap->DestroyWorld(false);
	};
	if (!TestTrue(TEXT("the initial bootstrap is exposed for non-world recovery"),
		WoTGuardedStartup_MaterializeWorld(Bootstrap,
			EWoTWorldStartPath::NormalInitialLoad, &Error)
		&& WoTCommitAuthenticatedWorldExposure(Bootstrap, &Error)))
	{
		return false;
	}

	FWoTWorldMaterializerSpecActivationWorld ActivationWorld;
	if (!ActivationWorld.Payloads) return false;
	UWoTSessionSubsystem* Sessions = ActivationWorld.World->GetGameInstance()
		->GetSubsystem<UWoTSessionSubsystem>();
	if (!TestNotNull(TEXT("the non-world recovery coordinator has sessions"),
		Sessions)
		|| !TestTrue(TEXT("the production bootstrap prepares non-world recovery"),
			Sessions->ActivateForAdoptedWorldAuthority()))
	{
		return false;
	}
	FWoTBootstrapStatus Activated =
		ActivationWorld.Payloads->ActivateVerifiedCache(SnapshotLease);
	if (!TestFalse(TEXT("the non-world payload fixture activates"),
		Activated.IsError()))
	{
		AddError(Activated.GetError().ToString());
		return false;
	}
	if (!TestFalse(TEXT("the non-world fixture authorizes its worlds"),
		WoTAuthorizeNormalWorldMaterialization(
			SnapshotLease, MoveTemp(Descriptors)).IsError())) return false;
	WoTAnimData::ResetRuntimeCache();
	ReadObserver.Reset();
	TestEqual(TEXT("the non-world animation remains lazy until the play load"),
		LazyReadsBeforeDamage, 0);
	FString Presented;
	const FDelegateHandle PresentationHandle = Sessions->OnConnectionFailure().AddLambda(
		[&Presented](const FString& Message) { Presented = Message; });
	ON_SCOPE_EXIT
	{
		Sessions->OnConnectionFailure().Remove(PresentationHandle);
	};
	const FString MeshLogicalId(TEXT("cache/mesh/RuntimeIntegritySpec"));
	const FWoTNeutralResourceId RuntimeAnimation{
		LogicalId, EWoTNeutralResourceKind::Animation};
	TestTrue(TEXT("the activated non-world generation is published"),
		IsWoTPayloadBootstrapActive());
	TestTrue(TEXT("the lazy animation is registered in the active generation"),
		FWoTRuntimeRegistry::Get().FindDescriptor(RuntimeAnimation).IsSet());
	TestFalse(TEXT("the lazy animation is independent of the retained front end"),
		CriticalIds.Contains(LogicalId));
	const FString BlobPath = FPaths::Combine(Root.Path, Generation,
		TEXT("blobs"), WoTWorldMaterializerSpecBlobLeaf(LogicalId));
	TArray<uint8> Damaged;
	if (!FFileHelper::LoadFileToArray(Damaged, *BlobPath) || Damaged.IsEmpty())
		return false;
	Damaged[0] ^= 0x71u;
	if (!FFileHelper::SaveArrayToFile(Damaged, *BlobPath)) return false;
	int32 PhysicalReads = 0;
	WoTSetVerifiedBlobBodyReadObserverForTesting(
		[&PhysicalReads]() { ++PhysicalReads; });
	ON_SCOPE_EXIT
	{
		WoTSetVerifiedBlobBodyReadObserverForTesting({});
	};
	TestFalse(TEXT("the production vertex-mesh loader rejects the damaged animation"),
		FWoTRuntimeVertexMesh::Load(MeshLogicalId, LogicalId).IsValid());
	TestEqual(TEXT("the production vertex-mesh loader performs both verified lazy reads"),
		PhysicalReads, 2);
	FWorldDelegates::OnWorldTickStart.Broadcast(ActivationWorld.World, LEVELTICK_All, 0.f);
	FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	const FString ExpectedMessage = FString::Printf(
		TEXT("Some of WoT Reborn's game data is damaged (%s). It will be rebuilt the next time you start the game."),
		*LogicalId);
	TestEqual(TEXT("the non-world loader publishes the exact visible message"),
		Presented, ExpectedMessage);
	TestEqual(TEXT("the exact message remains retained across return travel"),
		Sessions->RetainedConnectionFailure(), ExpectedMessage);
	TestEqual(TEXT("the non-world failure uses the authenticated front-end URL"),
		Sessions->GetLastReturnTravelURL(), FWoTLeaveStateMachine::FrontEndURL());
	TestTrue(TEXT("lazy damage leaves the empty restart marker"),
		IFileManager::Get().FileSize(*FPaths::Combine(Root.Path, TEXT("current/damaged"))) == 0);

	UWorld* Returned = WoTWorldMaterializerSpecCandidate(
		TEXT("/Game/WoT/Maps/WoTBootstrap"));
	if (!Returned) return false;
	const bool bReturned = WoTGuardedStartup_MaterializeWorld(
		Returned, EWoTWorldStartPath::NormalInitialLoad, &Error)
		&& WoTCommitAuthenticatedWorldExposure(Returned, &Error);
	TestTrue(TEXT("the non-world failure reaches the authenticated front end"),
		bReturned);
	if (!Returned->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		Returned->DestroyWorld(false);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
