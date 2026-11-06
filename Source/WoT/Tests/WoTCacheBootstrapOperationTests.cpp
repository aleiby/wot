// Production installer -> reader -> current -> runtime consumers, without injected builds.
#if WITH_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "UObject/Package.h"
#include "WoTTestWorld.h"
#include "WoTX6BrazierHallTestUtils.h"
#include "../WoTGameInstance.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTPayloadSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "../Payload/WoTCacheStore.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../Payload/WoTVerifiedBlobReader.h"
#include "../World/WoTWorldMaterializer.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../Mesh/WoTRuntimeVertexMesh.h"
#include "../Updater/WoTCacheBootstrapOperation.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

namespace
{
struct FCacheProductionProgress final : IWoTProgressSink
{
    bool bInterrupt = false;
    uint64 Last = 0;
    uint64 First = MAX_uint64;
    virtual FWoTBootstrapStatus ReportProgress(FStringView, uint64 Completed, uint64 Total) override
    {
        if (First == MAX_uint64) First = Completed;
        if (Completed < Last || Completed > Total)
            return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
                EWoTBootstrapError::InvalidNumber, TEXT("production progress regressed")));
        Last = Completed;
        return FWoTBootstrapStatus::Success();
    }
    virtual bool IsCancellationRequested() const override { return bInterrupt && Last > 0; }
};

FString CacheProductionBlobPath(const FString& Root, const FString& LogicalId)
{
    const FTCHARToUTF8 Utf8(*LogicalId);
    return Root / TEXT("current/blobs") / (Sha256(TConstArrayView<uint8>(
        reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length())).ToHex() + TEXT(".blob"));
}

bool DamageProductionBlob(FAutomationTestBase& Test, const FString& Root, const FString& LogicalId)
{
    TArray<uint8> Bytes;
    const FString Path = CacheProductionBlobPath(Root, LogicalId);
    if (!Test.TestTrue(TEXT("real emitted blob loads"),
        FFileHelper::LoadFileToArray(Bytes, *Path) && !Bytes.IsEmpty())) return false;
    Bytes.Last() ^= 0x55;
    return Test.TestTrue(TEXT("real emitted blob is damaged"), FFileHelper::SaveArrayToFile(Bytes, *Path));
}

// The identity a production binary requires of its current cache.
TWoTBootstrapResult<FWoTCacheGenerationBuildIdentity> CacheProductionExpectedIdentity()
{
    using FResult = TWoTBootstrapResult<FWoTCacheGenerationBuildIdentity>;
    auto InstallerSha = FWoTSha256::FromHex(WoT::Bootstrap::InstallerSha256);
    auto ContentLabel = GetCacheContentLabel();
    auto Contract = WoTLoadProductionRetailContract();
    if (InstallerSha.IsError()) return FResult::Error(MoveTemp(InstallerSha).GetError());
    if (ContentLabel.IsError()) return FResult::Error(MoveTemp(ContentLabel).GetError());
    if (Contract.IsError()) return FResult::Error(MoveTemp(Contract).GetError());
    FWoTCacheGenerationBuildIdentity Expected;
    Expected.bHasBuildIdentity = true;
    Expected.InstallerSha256 = InstallerSha.GetValue();
    Expected.ContractSha256 = ComputeRetailContractIdentity(Contract.GetValue());
    Expected.ProductionDescriptorSha256 = ContentLabel.GetValue();
    return FResult::Success(MoveTemp(Expected));
}

// Names what differs between two canonical cache manifests: each top-level
// field other than the blob table, then each blob record that differs or is
// present in only one of them, by logical id.
TArray<FString> CacheProductionManifestDifferences(
    TConstArrayView<uint8> Fresh, TConstArrayView<uint8> Previous)
{
    TArray<FString> Differences;
    auto FreshParsed = ParseCanonicalJson(Fresh);
    auto PreviousParsed = ParseCanonicalJson(Previous);
    if (FreshParsed.IsError() || PreviousParsed.IsError()
        || !FreshParsed.GetValue().IsObject() || !PreviousParsed.GetValue().IsObject())
    {
        Differences.Add(TEXT("a manifest is not a canonical JSON object"));
        return Differences;
    }
    const FWoTCanonicalJsonValue& FreshRoot = FreshParsed.GetValue();
    const FWoTCanonicalJsonValue& PreviousRoot = PreviousParsed.GetValue();
    auto MemberBytes = [](const FWoTCanonicalJsonValue* Value) -> TArray<uint8>
    {
        if (Value == nullptr) return {};
        auto Written = WriteCanonicalJson(*Value);
        return Written.IsError() ? TArray<uint8>() : MoveTemp(Written).GetValue();
    };
    TSet<FString> Keys;
    for (int32 Index = 0; Index < FreshRoot.Num(); ++Index) Keys.Add(FreshRoot.GetMemberKey(Index));
    for (int32 Index = 0; Index < PreviousRoot.Num(); ++Index) Keys.Add(PreviousRoot.GetMemberKey(Index));
    for (const FString& Key : Keys)
    {
        if (Key != TEXT("blobs")
            && MemberBytes(FreshRoot.FindMember(Key)) != MemberBytes(PreviousRoot.FindMember(Key)))
        {
            Differences.Add(TEXT("field ") + Key);
        }
    }
    auto BlobRecords = [&MemberBytes](const FWoTCanonicalJsonValue& Root)
    {
        TMap<FString, TArray<uint8>> Records;
        const FWoTCanonicalJsonValue* Blobs = Root.FindMember(TEXT("blobs"));
        if (Blobs == nullptr || !Blobs->IsArray()) return Records;
        for (int32 Index = 0; Index < Blobs->Num(); ++Index)
        {
            const FWoTCanonicalJsonValue& Record = Blobs->GetElement(Index);
            const FWoTCanonicalJsonValue* LogicalId = Record.FindMember(TEXT("logical_id"));
            if (LogicalId != nullptr && LogicalId->GetKind() == EWoTCanonicalJsonKind::String)
                Records.Add(LogicalId->GetString(), MemberBytes(&Record));
        }
        return Records;
    };
    const TMap<FString, TArray<uint8>> FreshBlobs = BlobRecords(FreshRoot);
    const TMap<FString, TArray<uint8>> PreviousBlobs = BlobRecords(PreviousRoot);
    TArray<FString> BlobDifferences;
    for (const TPair<FString, TArray<uint8>>& Pair : FreshBlobs)
    {
        const TArray<uint8>* Other = PreviousBlobs.Find(Pair.Key);
        if (Other == nullptr) BlobDifferences.Add(TEXT("blob only in the fresh build: ") + Pair.Key);
        else if (*Other != Pair.Value) BlobDifferences.Add(TEXT("blob differs: ") + Pair.Key);
    }
    for (const TPair<FString, TArray<uint8>>& Pair : PreviousBlobs)
    {
        if (!FreshBlobs.Contains(Pair.Key))
            BlobDifferences.Add(TEXT("blob only in the previous release: ") + Pair.Key);
    }
    Differences.Sort();
    BlobDifferences.Sort();
    Differences.Append(MoveTemp(BlobDifferences));
    return Differences;
}

// Opt-in proof against a retained previous-release cache: the directory named
// by WOT_PREVIOUS_RELEASE_CACHE holds current/manifest.json and current/blobs/.
// (a) The fresh build's manifest is byte-identical to it. (b) A copy of it,
// placed under this spec's fixture root, is accepted as current without a
// rebuild. The retained cache itself is only read.
void CheckPreviousReleaseCache(FAutomationTestBase& Test, const FString& FixtureParent,
    const FString& FreshRoot, const FWoTSha256& FreshManifestSha)
{
    const FString PreviousRelease =
        FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_PREVIOUS_RELEASE_CACHE"));
    if (PreviousRelease.IsEmpty())
    {
        Test.AddWarning(TEXT("WOT_PREVIOUS_RELEASE_CACHE is unset: previous-release cache identity and acceptance not verified"));
        return;
    }
    const FString PreviousCurrent =
        FPaths::ConvertRelativePathToFull(PreviousRelease) / TEXT("current");
    TArray<uint8> PreviousManifest;
    if (!Test.TestTrue(TEXT("previous release manifest loads"),
        FFileHelper::LoadFileToArray(PreviousManifest, *(PreviousCurrent / TEXT("manifest.json")))))
        return;
    const FWoTSha256 PreviousManifestSha = Sha256(PreviousManifest);
    Test.AddInfo(FString::Printf(TEXT("previous release manifest sha256=%s fresh manifest sha256=%s"),
        *PreviousManifestSha.ToHex(), *FreshManifestSha.ToHex()));
    if (!Test.TestEqual(TEXT("(a) fresh manifest is byte-identical to the previous release's"),
        FreshManifestSha.ToHex(), PreviousManifestSha.ToHex()))
    {
        TArray<uint8> FreshManifest;
        if (FFileHelper::LoadFileToArray(FreshManifest, *(FreshRoot / TEXT("current/manifest.json"))))
        {
            for (const FString& Difference :
                CacheProductionManifestDifferences(FreshManifest, PreviousManifest))
            {
                Test.AddError(TEXT("fresh manifest differs from the previous release: ") + Difference);
            }
        }
    }

    const FString CopyParent = FixtureParent / TEXT("PreviousRelease");
    const FString CopyRoot = CopyParent / TEXT("ContentCache");
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    const bool bCopied = PlatformFile.CreateDirectoryTree(*(CopyRoot / TEXT("current")))
        && PlatformFile.CopyDirectoryTree(*(CopyRoot / TEXT("current")), *PreviousCurrent, false);
    if (Test.TestTrue(TEXT("previous release cache copies into the fixture"), bCopied))
    {
        auto CopyStore = FWoTCacheStore::Open(CopyRoot);
        auto Expected = CacheProductionExpectedIdentity();
        if (CopyStore.IsError() || Expected.IsError())
        {
            Test.AddError(CopyStore.IsError() ? CopyStore.GetError().ToString()
                : Expected.GetError().ToString());
        }
        else
        {
            auto Accepted = CopyStore.GetValue().TryOpenCurrent(Expected.GetValue(), nullptr);
            if (Accepted.IsError()) Test.AddError(Accepted.GetError().ToString());
            else if (Test.TestTrue(TEXT("(b) this binary accepts the previous release cache without a rebuild"),
                Accepted.GetValue().IsSet()))
            {
                Test.TestEqual(TEXT("(b) the accepted lease holds the previous release manifest"),
                    Accepted.GetValue()->GetManifestSha256().ToHex(), PreviousManifestSha.ToHex());
                Test.AddInfo(FString::Printf(TEXT("previous release cache accepted as current: lease manifest sha256=%s"),
                    *Accepted.GetValue()->GetManifestSha256().ToHex()));
            }
        }
    }
    IFileManager::Get().DeleteDirectory(*CopyParent, false, true);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTCacheProductionRecoverySpec,
    "WoT.Bootstrap.CacheOperation.RealRetailFreshWarmWorldsAndDamageRebuild",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTCacheProductionRecoverySpec::RunTest(const FString&)
{
    FString InstallerPath = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_CACHE_TEST_INSTALLER"));
    if (InstallerPath.IsEmpty()) InstallerPath = WoTRepositoryRoot() / TEXT("Saved/FunnelCache/setup_the_wheel_of_time_333b_(54846).exe");
    if (!TestTrue(TEXT("required pinned installer exists (set WOT_CACHE_TEST_INSTALLER)"),
        FPaths::FileExists(InstallerPath))) return false;
    const FString FixtureBase = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()
        / TEXT("Automation/CacheProduction"));
    const FString RequestedParent = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_CACHE_TEST_ROOT"));
    const FString Parent = RequestedParent.IsEmpty()
        ? FixtureBase / FGuid::NewGuid().ToString(EGuidFormats::Digits)
        : FPaths::ConvertRelativePathToFull(RequestedParent);
    if (!TestTrue(TEXT("acceptance fixture stays below the worktree automation directory"),
        FPaths::IsUnderDirectory(Parent, FixtureBase))) return false;
    const bool bLeaveDamaged = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_CACHE_TEST_LEAVE_DAMAGED")) == TEXT("1");
    const bool bExpectDamaged = FPlatformMisc::GetEnvironmentVariable(TEXT("WOT_CACHE_TEST_EXPECT_DAMAGED")) == TEXT("1");
    const FString Root = Parent / TEXT("ContentCache");
    if (!IFileManager::Get().MakeDirectory(*Parent, true)) return false;
    WoTResetProcessProfileSessionForTesting();
    FWoTBootstrapStatus ProfileSession =
        WoTAcquireProcessProfileSessionShared(Parent);
    if (!TestFalse(TEXT("production build fixture takes SHARED profile session"),
        ProfileSession.IsError()))
    {
        if (ProfileSession.IsError()) AddError(ProfileSession.GetError().ToString());
        return false;
    }
    SetWoTBootstrapHeartbeatRootForTesting(Parent);
    ON_SCOPE_EXIT
    {
        SetWoTBootstrapHeartbeatRootForTesting({});
        WoTResetProcessProfileSessionForTesting();
        if (!bLeaveDamaged) IFileManager::Get().DeleteDirectory(*Parent, false, true);
    };
    if (bExpectDamaged)
    {
        if (!TestTrue(TEXT("previous process left its runtime damage marker"),
            FPaths::FileExists(Root / TEXT("current/damaged")))) return false;
        auto RestartStore = FWoTCacheStore::Open(Root);
        if (RestartStore.IsError()) { AddError(RestartStore.GetError().ToString()); return false; }
        auto Expected = CacheProductionExpectedIdentity();
        if (Expected.IsError()) return false;
        auto RestartCurrent = RestartStore.GetValue().TryOpenCurrent(Expected.GetValue(), nullptr);
        if (!TestTrue(TEXT("fresh process rejects the previous process's damaged current"),
            !RestartCurrent.IsError() && !RestartCurrent.GetValue().IsSet())) return false;
    }

    auto Build = [&](bool bInterrupt) -> TWoTBootstrapResult<FWoTPublishedCacheReceipt>
    {
        using FResult = TWoTBootstrapResult<FWoTPublishedCacheReceipt>;
        auto Installer = WoTTryAcquirePinnedAdjacentInstaller(InstallerPath);
        if (Installer.IsError()) return FResult::Error(MoveTemp(Installer).GetError());
        if (Installer.GetValue().State != EWoTPinnedAdjacentInstallerProbe::Valid
            || !Installer.GetValue().Verified.IsValid())
            return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::CorruptInstallation,
                TEXT("acceptance requires the pinned real installer"), InstallerPath));
        auto Opened = FWoTCacheStore::Open(Root);
        if (Opened.IsError()) return FResult::Error(MoveTemp(Opened).GetError());
        auto Store = MoveTemp(Opened).GetValue();
        FCacheProductionProgress Progress;
        Progress.bInterrupt = bInterrupt;
        FWoTCacheBootstrapOperation Operation;
        auto Result = Operation.BuildAndPublish(MoveTemp(*Installer.GetValue().Verified), Store, Progress);
        TestEqual(TEXT("each production build starts from zero"), Progress.First, uint64(0));
        return Result;
    };
    auto Interrupted = Build(true);
    TestTrue(TEXT("real build interruption returns an error"), Interrupted.IsError());
    TestFalse(TEXT("interrupted build publishes no current"), FPaths::FileExists(Root / TEXT("current/manifest.json")));
    TestTrue(TEXT("interrupted build leaves disposable building"), FPaths::DirectoryExists(Root / TEXT("building")));
    auto Built = Build(false);
    if (!TestFalse(TEXT("fresh production rebuild succeeds"), Built.IsError()))
    {
        AddError(Built.GetError().ToString()); return false;
    }
    TestFalse(TEXT("published cache has no building remnants"), FPaths::DirectoryExists(Root / TEXT("building")));
    {
        const TWoTBootstrapResult<FWoTSha256> ContentLabel = GetCacheContentLabel();
        TestTrue(TEXT("the fresh build records the cache content label"),
            !ContentLabel.IsError()
            && Built.GetValue().ProductionDescriptorSha256 == ContentLabel.GetValue());
    }
    CheckPreviousReleaseCache(*this, Parent, Root, Built.GetValue().ManifestSha256);
    FWoTCacheGenerationBuildIdentity Identity;
    Identity.bHasBuildIdentity = true;
    Identity.InstallerSha256 = Built.GetValue().InstallerSha256;
    Identity.ContractSha256 = Built.GetValue().ContractSha256;
    Identity.ProductionDescriptorSha256 = Built.GetValue().ProductionDescriptorSha256;
    auto Opened = FWoTCacheStore::Open(Root);
    if (Opened.IsError()) return false;
    auto Store = MoveTemp(Opened).GetValue();
    for (int32 Field = 0; Field < 3; ++Field)
    {
        auto Changed = Identity;
        if (Field == 0) Changed.InstallerSha256.Bytes[0] ^= 1;
        if (Field == 1) Changed.ContractSha256.Bytes[0] ^= 1;
        if (Field == 2) Changed.ProductionDescriptorSha256.Bytes[0] ^= 1;
        auto Incompatible = Store.TryOpenCurrent(Changed, nullptr);
        TestTrue(TEXT("each changed build identity selects rebuild"),
            !Incompatible.IsError() && !Incompatible.GetValue().IsSet());
    }
    auto Contract = WoTLoadProductionRetailContract();
    if (Contract.IsError()) return false;
    if (!DamageProductionBlob(*this, Root, Contract.GetValue().StartupCriticalLogicalIds[0])) return false;
    auto StartupDamage = Store.TryOpenCurrent(Identity, nullptr);
    TestTrue(TEXT("startup critical damage selects rebuild"),
        !StartupDamage.IsError() && !StartupDamage.GetValue().IsSet());
    auto Rebuilt = Build(false);
    if (!TestFalse(TEXT("startup damage rebuild succeeds"), Rebuilt.IsError()))
    {
        AddError(Rebuilt.GetError().ToString()); return false;
    }

    const FString LazyId(TEXT("cache/animation/Angreal__AbsorbVisual"));
    TestFalse(TEXT("lazy runtime animation is outside startup closure"),
        Contract.GetValue().StartupCriticalLogicalIds.Contains(LazyId));
    int32 LazyStartupReads = 0;
    auto Observer = WoTObserveCacheSnapshotPhysicalReadsForTesting([&](FStringView Id)
    {
        if (Id == TEXT("blobs/") + LazyId) ++LazyStartupReads;
    });
    auto Current = Store.TryOpenCurrent(Identity, nullptr);
    Observer.Reset();
    if (!TestTrue(TEXT("warm production current opens"), !Current.IsError() && Current.GetValue().IsSet()))
    {
        if (Current.IsError()) AddError(Current.GetError().ToString()); return false;
    }
    TestEqual(TEXT("startup leaves noncritical payload hash-on-read"), LazyStartupReads, 0);
    TestEqual(TEXT("manifest carries all nineteen gameplay maps"),
        Current.GetValue()->GetWorldDescriptors().Num(), 19);
    FWoTCacheGenerationLease Snapshot = Current.GetValue().GetValue();
    {
        ResetWoTPayloadActivationForTesting();
        FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
        UPackage* BootstrapPackage = CreatePackage(TEXT("/Game/WoT/Maps/WoTBootstrap"));
        BootstrapPackage->SetFlags(RF_Transient);
        UWorld* BootstrapWorld = UWorld::CreateWorld(EWorldType::Game, false,
            MakeUniqueObjectName(BootstrapPackage, UWorld::StaticClass(), TEXT("World")),
            BootstrapPackage, true, ERHIFeatureLevel::Num, nullptr, true);
        if (!BootstrapWorld) return false;
        ON_SCOPE_EXIT
        {
            BootstrapWorld->DestroyWorld(false);
            BootstrapWorld->MarkAsGarbage();
            // This fixture uses the pinned bootstrap name. Release its transient
            // package so later specs can load the committed bootstrap map.
            BootstrapPackage->MarkAsGarbage();
            CollectGarbage(RF_NoFlags);
            TestNull(TEXT("transient bootstrap fixture is unloaded"),
                FindPackage(nullptr, TEXT("/Game/WoT/Maps/WoTBootstrap")));
        };
        FString BootstrapError;
        if (!TestTrue(TEXT("bootstrap authority is consumed before gameplay adoption"),
            WoTGuardedStartup_MaterializeWorld(BootstrapWorld, EWoTWorldStartPath::NormalInitialLoad,
                &BootstrapError) && WoTCommitAuthenticatedWorldExposure(BootstrapWorld, &BootstrapError)))
        {
            AddError(BootstrapError); return false;
        }
        UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(true, false);
        WoTTest::FSpecWorldOptions Options;
        Options.GameInstanceClass = UWoTGameInstance::StaticClass();
        Options.bStartPlay = false;
        Options.bActivatePayload = false;
        UWorld* ActivationWorld = WoTTest::CreateSpecWorld(Options);
        if (!ActivationWorld || !ActivationWorld->GetGameInstance()) return false;
        ON_SCOPE_EXIT
        {
            FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
            WoTTest::DestroySpecWorld(ActivationWorld);
            UWoTPayloadSubsystem::SetInjectionEnabledOverrideForTests(false);
            ResetWoTPayloadActivationForTesting();
        };
        auto* Payloads = ActivationWorld->GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>();
        if (!Payloads) return false;
        FWoTBootstrapStatus Activated = Payloads->ActivateVerifiedCache(Snapshot);
        if (Activated.IsError()) { AddError(Activated.GetError().ToString()); return false; }
        for (const auto& Descriptor : Snapshot.GetWorldDescriptors())
        {
            const FString PackageName = TEXT("/Temp/CacheProduction/") + Descriptor.MapIdentity;
            UPackage* Package = CreatePackage(*PackageName);
            Package->SetFlags(RF_Transient);
            UWorld* Candidate = UWorld::CreateWorld(EWorldType::Game, false,
                MakeUniqueObjectName(Package, UWorld::StaticClass(), TEXT("World")), Package,
                true, ERHIFeatureLevel::Num, nullptr, true);
            if (!Candidate) return false;
            FWoTWorldMaterializer Materializer;
            auto Materialized = Materializer.MaterializeBeforePlay(
                *Candidate, Descriptor, Snapshot);
            TestFalse(Descriptor.MapIdentity + TEXT(" materializes from actual retail cache"), Materialized.IsError());
            if (Materialized.IsError()) AddError(Materialized.GetError().ToString());
            else TestTrue(Descriptor.MapIdentity + TEXT(" validates the complete census"),
                Materialized.GetValue().bFullCensusValidated);
            if (!Materialized.IsError()
                && Descriptor.MapIdentity == TEXT("Battle2_02"))
            {
                TestTrue(TEXT("cache-path Battle2_02 reports repaired ribbon surfaces"),
                    Materialized.GetValue().RepairedLightmapRibbonSurfaces >= 1);
                TestTrue(TEXT("cache-path Battle2_02 reports repaired ribbon triangles"),
                    Materialized.GetValue().RepairedLightmapRibbonTriangles >= 1);
                FString Heartbeat;
                const FString HeartbeatPath = Parent / TEXT("UpdaterState")
                    / WoTBootstrapHeartbeatFileName();
                if (!TestTrue(TEXT("cache-path Battle2_02 heartbeat reads"),
                    FFileHelper::LoadFileToString(Heartbeat, *HeartbeatPath)))
                    return false;
                TArray<FString> HeartbeatLines;
                Heartbeat.ParseIntoArrayLines(HeartbeatLines);
                const FString* Battle2Heartbeat = HeartbeatLines.FindByPredicate(
                    [](const FString& Line)
                    { return Line.Contains(TEXT("map=Battle2_02")); });
                TestTrue(TEXT("cache-path Battle2_02 heartbeat reports ribbon repair"),
                    Battle2Heartbeat != nullptr
                    && Battle2Heartbeat->Contains(FString::Printf(
                        TEXT("ribbon_surfaces=%llu ribbon_triangles=%llu"),
                        Materialized.GetValue().RepairedLightmapRibbonSurfaces,
                        Materialized.GetValue().RepairedLightmapRibbonTriangles)));
                AWoTRuntimeBSPWorld* RuntimeBSP = nullptr;
                for (AActor* Actor : Candidate->PersistentLevel->Actors)
                    if (AWoTRuntimeBSPWorld* CandidateBSP =
                        Cast<AWoTRuntimeBSPWorld>(Actor))
                        RuntimeBSP = CandidateBSP;
                if (!TestNotNull(TEXT("cache-path Battle2_02 builds runtime BSP"),
                    RuntimeBSP)) return false;
                TArray<FWoTBSPPayloadData> SourceParts;
                FString DecodeError;
                for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
                {
                    auto Read = FWoTRuntimeRegistry::Get().ReadObject(
                        {Part.LogicalId, EWoTNeutralResourceKind::BSP},
                        256ull * 1024ull * 1024ull,
                        TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
                    if (!TestFalse(TEXT("cache-path Battle2_02 verified BSP object reads"),
                        Read.IsError()))
                    {
                        if (Read.IsError()) AddError(Read.GetError().ToString());
                        return false;
                    }
                    if (!TestTrue(TEXT("cache-path Battle2_02 verified BSP payload decodes"),
                        UWoTBSPPayloadLibrary::DecodePayload(
                            Read.GetValue().Payload(),
                            SourceParts.AddDefaulted_GetRef(), DecodeError)))
                    { AddError(DecodeError); return false; }
                }
                TArray<FWoTBSPPayloadData> FinalRender;
                for (UStaticMeshComponent* Component : RuntimeBSP->GeometryComponents)
                {
                    if (!Component || !Component->IsVisible()) continue;
                    if (!TestTrue(TEXT("cache-path Battle2_02 final mesh exposes UV1"),
                        WoTX6Test::ReadRenderMesh(Component->GetStaticMesh(),
                            FinalRender.AddDefaulted_GetRef()))) return false;
                }
                const FWoTBSPPayloadData& X6Part = SourceParts[0];
                const int32 X6UpperAt = 3930 * 3;
                const int32 X6LowerAt = 3925 * 3;
                const FVector3f X6Anchor = X6Part.Positions[
                    X6Part.Indices[X6UpperAt + 2]];
                const FVector3f X6UpperEnd = X6Part.Positions[
                    X6Part.Indices[X6UpperAt]];
                const FVector3f X6LowerEnd = X6Part.Positions[
                    X6Part.Indices[X6LowerAt + 1]];
                const double X6EndpointGap = FVector3d(
                    X6UpperEnd - X6LowerEnd).Size();
                const uint32 X6SourceUpperIncidence = WoTX6Test::EdgeIncidence(
                    SourceParts, X6Anchor, X6UpperEnd);
                const uint32 X6SourceLowerIncidence = WoTX6Test::EdgeIncidence(
                    SourceParts, X6Anchor, X6LowerEnd);
                const uint32 X6FinalLowerIncidence = WoTX6Test::EdgeIncidence(
                    FinalRender, X6Anchor, X6LowerEnd);
                AddInfo(FString::Printf(TEXT("X6CacheDiagonal endpoint_gap=%.9f source_incidence=%u/%u final_shared_lower_incidence=%u"),
                    X6EndpointGap, X6SourceUpperIncidence,
                    X6SourceLowerIncidence, X6FinalLowerIncidence));
                TestTrue(TEXT("cache-path X6 diagonal source is the pinned forked boundary"),
                    FMath::IsNearlyEqual(X6EndpointGap, 0.326177722, 0.0001)
                    && X6SourceUpperIncidence == 1
                    && X6SourceLowerIncidence == 1);
                TestTrue(TEXT("cache-path X6 diagonal final render is exactly watertight"),
                    X6FinalLowerIncidence >= 2);
                struct FSample { double Time; int32 X; int32 Y; };
                const FSample Samples[] = {
                    {1.4, 1302, 1360}, {1.6, 1298, 1360},
                    {1.8, 1294, 1360}, {1.6, 1304, 1300}};
                for (const FSample& Sample : Samples)
                {
                    int32 DarkestX = Sample.X;
                    double DarkestLuminance = TNumericLimits<double>::Max();
                    for (int32 X = Sample.X - 20; X <= Sample.X + 20; ++X)
                    {
                        const double Luminance = WoTX6Test::AtlasLuminance(
                            SourceParts, WoTX6Test::HitPayload(
                                SourceParts, Sample.Time, X, Sample.Y));
                        if (Luminance >= 0 && Luminance < DarkestLuminance)
                        { DarkestLuminance = Luminance; DarkestX = X; }
                    }
                    const WoTX6Test::FHit SourceLeft = WoTX6Test::HitPayload(
                        SourceParts, Sample.Time, DarkestX - 1, Sample.Y);
                    const WoTX6Test::FHit SourceCenter = WoTX6Test::HitPayload(
                        SourceParts, Sample.Time, DarkestX, Sample.Y);
                    const WoTX6Test::FHit SourceRight = WoTX6Test::HitPayload(
                        SourceParts, Sample.Time, DarkestX + 1, Sample.Y);
                    const WoTX6Test::FHit FinalLeft = WoTX6Test::HitPayload(
                        FinalRender, Sample.Time, DarkestX - 1, Sample.Y);
                    const WoTX6Test::FHit FinalCenter = WoTX6Test::HitPayload(
                        FinalRender, Sample.Time, DarkestX, Sample.Y);
                    const WoTX6Test::FHit FinalRight = WoTX6Test::HitPayload(
                        FinalRender, Sample.Time, DarkestX + 1, Sample.Y);
                    const double SourceLeftLuma = WoTX6Test::AtlasLuminance(
                        SourceParts, SourceLeft);
                    const double SourceCenterLuma = WoTX6Test::AtlasLuminance(
                        SourceParts, SourceCenter);
                    const double SourceRightLuma = WoTX6Test::AtlasLuminance(
                        SourceParts, SourceRight);
                    const double FinalLeftLuma = WoTX6Test::AtlasLuminance(
                        SourceParts, FinalLeft);
                    const double FinalCenterLuma = WoTX6Test::AtlasLuminance(
                        SourceParts, FinalCenter);
                    const double FinalRightLuma = WoTX6Test::AtlasLuminance(
                        SourceParts, FinalRight);
                    AddInfo(FString::Printf(TEXT("X6CachePath video_t=%.1f pixels=%d/%d/%d source_surface=%d source_uv1=(%.9f,%.9f) final_uv1=(%.9f,%.9f) source_luma=%.6f/%.6f/%.6f final_luma=%.6f/%.6f/%.6f"),
                        Sample.Time, DarkestX - 1, DarkestX, DarkestX + 1,
                        SourceCenter.Surface, SourceCenter.UV1.X,
                        SourceCenter.UV1.Y, FinalCenter.UV1.X,
                        FinalCenter.UV1.Y, SourceLeftLuma, SourceCenterLuma,
                        SourceRightLuma, FinalLeftLuma, FinalCenterLuma,
                        FinalRightLuma));
                    TestTrue(FString::Printf(TEXT("cache-path X6 %.1f source is the dark ribbon"), Sample.Time),
                        SourceCenter.Surface == (Sample.Y == 1360 ? 2462 : 2467)
                        && SourceCenterLuma * 4.0 < FMath::Min(
                            SourceLeftLuma, SourceRightLuma));
                    TestTrue(FString::Printf(TEXT("cache-path X6 %.1f final UV1 changes from the dark source"), Sample.Time),
                        SourceLeft.bHit && SourceCenter.bHit && SourceRight.bHit
                        && FinalLeft.bHit && FinalCenter.bHit && FinalRight.bHit
                        && !FinalCenter.UV1.Equals(
                            SourceCenter.UV1, 1.0e-6));
                    TestTrue(FString::Printf(TEXT("cache-path X6 %.1f final UV1 samples the lit neighbour"), Sample.Time),
                        WoTX6Test::FinalCenterUsesLitNeighbour(
                            SourceLeftLuma, SourceRightLuma, FinalLeftLuma,
                            FinalCenterLuma, FinalRightLuma));
                }
            }
            Candidate->DestroyWorld(false);
            Candidate->MarkAsGarbage();
            if (Materialized.IsError()) return false;
        }
        FWoTFrontEndSceneMaterializer FrontEnd;
        if (!TestNotNull(TEXT("production activation retains font owner"), Payloads->FrontEndFontOwnerForScene())) return false;
        auto Staged = FrontEnd.Stage(*ActivationWorld, Snapshot.GetFrontEndDescriptor(),
            Snapshot, *Payloads, *Payloads->FrontEndFontOwnerForScene(), {}, [] { return false; });
        if (!TestFalse(TEXT("real front end stages"), Staged.IsError()))
        {
            AddError(Staged.GetError().ToString()); return false;
        }
        if (!TestFalse(TEXT("real front end commits"), MoveTemp(Staged).GetValue().Commit().IsError())) return false;
        TArray<FWoTWorldDescriptor> Worlds(Snapshot.GetWorldDescriptors());
        auto Authorized = WoTAuthorizeNormalWorldMaterialization(
            Snapshot, MoveTemp(Worlds));
        if (Authorized.IsError()) { AddError(Authorized.GetError().ToString()); return false; }
        auto* Sessions = ActivationWorld->GetGameInstance()->GetSubsystem<UWoTSessionSubsystem>();
        if (!Sessions || !Sessions->ActivateForAdoptedWorldAuthority()) return false;
        FString Presented;
        const auto Handle = Sessions->OnConnectionFailure().AddLambda([&](const FString& Message) { Presented = Message; });
        ON_SCOPE_EXIT { Sessions->OnConnectionFailure().Remove(Handle); };
        if (!DamageProductionBlob(*this, Root, LazyId)) return false;
        TestFalse(TEXT("actual vertex mesh loader rejects damaged lazy animation"),
            FWoTRuntimeVertexMesh::Load(TEXT("cache/mesh/Angreal__AbsorbVisual"), LazyId).IsValid());
        FWorldDelegates::OnWorldTickStart.Broadcast(ActivationWorld, LEVELTICK_All, 0.0f);
        TestTrue(TEXT("runtime damage keeps the existing player message"),
            Presented.Contains(TEXT("game data is damaged")) && Presented.Contains(TEXT("next time you start")));
        TestEqual(TEXT("runtime damage uses the existing menu return flow"),
            Sessions->GetLastReturnTravelURL(), FWoTLeaveStateMachine::FrontEndURL());
        TestEqual(TEXT("lazy damage persists only an empty marker"),
            IFileManager::Get().FileSize(*(Root / TEXT("current/damaged"))), int64(0));
        const FWoTProfileSession* SharedSession = WoTGetProcessProfileSession();
        TestTrue(TEXT("direct rebuild requires an exclusive profile owner"),
            SharedSession != nullptr && Store.BeginBuild(*SharedSession).IsError());
        FrontEnd.Shutdown();
    }
    auto Restart = Store.TryOpenCurrent(Identity, nullptr);
    TestTrue(TEXT("a new opener rejects runtime-damaged cache"), !Restart.IsError() && !Restart.GetValue().IsSet());
    if (bLeaveDamaged) return !HasAnyErrors();
    Snapshot = FWoTCacheGenerationLease();
    Current.GetValue().Reset();
    auto FinalBuild = Build(false);
    if (!TestFalse(TEXT("runtime damage rebuild succeeds after all readers leave"), FinalBuild.IsError()))
    {
        AddError(FinalBuild.GetError().ToString()); return false;
    }
    auto FinalCurrent = Store.TryOpenCurrent(Identity, nullptr);
    TestTrue(TEXT("rebuilt production cache is usable on next start"),
        !FinalCurrent.IsError() && FinalCurrent.GetValue().IsSet());
    return !HasAnyErrors();
}
#endif
