// Cache-v3 vertex-mesh importer specs pinned to Python and real retail data.
#include "Misc/AutomationTest.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeHandleTestFactory.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/MeshAnimation.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTMeshAnimationSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

struct FWoTMeshAnimationSpecRoot
{
	FString Path = WoTNativeHandleTestFactory::CreateTemporaryRoot();
	~FWoTMeshAnimationSpecRoot() { WoTNativeHandleTestFactory::RemoveTree(Path); }
};

FString WoTMeshAnimationSpecRepoPath(const TCHAR* Relative)
{
	return WoTRepositoryOrReferencePath(Relative);
}

bool WoTMeshAnimationSpecLoadFixture(const TCHAR* Name, TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out,
		*WoTMeshAnimationSpecRepoPath(*(FString(TEXT("Tools/dist/fixtures/")) + Name)));
}

TWoTBootstrapResult<FWoTHeldInstallerSnapshot> WoTMeshAnimationSpecSnapshot(
	const FString& Path)
{
	using FResult = TWoTBootstrapResult<FWoTHeldInstallerSnapshot>;
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened = OpenNativeFileReadNoFollow(Path);
	if (Opened.IsError()) return FResult::Error(MoveTemp(Opened).GetError());
	TWoTBootstrapResult<TArray<uint8>> Read = Opened.GetValue().ReadAll();
	if (Read.IsError()) return FResult::Error(MoveTemp(Read).GetError());
	FWoTHeldInstallerSnapshot Snapshot;
	Snapshot.Size = static_cast<uint64>(Read.GetValue().Num());
	Snapshot.Sha256 = Sha256(Read.GetValue());
	Snapshot.Identity = Opened.GetValue().GetIdentity();
	Snapshot.Handle = MoveTemp(Opened).GetValue();
	return FResult::Success(MoveTemp(Snapshot));
}

TWoTBootstrapResult<FWoTRetailPackage> WoTMeshAnimationSpecOpen(
	FWoTMeshAnimationSpecRoot& Root, const TCHAR* FixtureName)
{
	using FResult = TWoTBootstrapResult<FWoTRetailPackage>;
	TArray<uint8> Fixture;
	if (!WoTMeshAnimationSpecLoadFixture(FixtureName, Fixture))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the mesh fixture is missing"), FixtureName));
	}
	const FString Installer = Root.Path / FString(FixtureName);
	if (!WoTNativeHandleTestFactory::WriteFile(Installer, Fixture))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the mesh fixture could not be staged"), Installer));
	}
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTMeshAnimationSpecSnapshot(Installer);
	if (Snapshot.IsError()) return FResult::Error(MoveTemp(Snapshot).GetError());
	return FWoTRetailPackage::Open(MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
}

struct FWoTMeshAnimationSpecWritten
{
	FString LogicalId;
	FString Schema;
	TArray<uint8> Bytes;
	uint64 MaximumBytes = 0;
};

class FWoTMeshAnimationSpecWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (bRejectNext)
		{
			bRejectNext = false;
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io, TEXT("synthetic mesh writer refusal")));
		}
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("mesh writer received oversized bytes")));
		}
		FWoTMeshAnimationSpecWritten& Written = Objects.AddDefaulted_GetRef();
		Written.LogicalId = FString(LogicalId);
		Written.Schema = FString(Schema);
		Written.Bytes.Append(Bytes.GetData(), Bytes.Num());
		Written.MaximumBytes = MaximumBytes;
		if (AfterWrite)
		{
			AfterWrite();
		}
		return FWoTBootstrapStatus::Success();
	}
	const FWoTMeshAnimationSpecWritten* Find(FStringView LogicalId) const
	{
		for (const FWoTMeshAnimationSpecWritten& Written : Objects)
		{
			if (Written.LogicalId == LogicalId) return &Written;
		}
		return nullptr;
	}
	TArray<FWoTMeshAnimationSpecWritten> Objects;
	TFunction<void()> AfterWrite;
	bool bRejectNext = false;
};

class FWoTMeshAnimationSpecProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView Stage, uint64 Completed,
		uint64 Total) override
	{
		LastStage = FString(Stage);
		LastCompleted = Completed;
		LastTotal = Total;
		if (Completed >= CancelAtCompleted) bCancelled = true;
		return FWoTBootstrapStatus::Success();
	}
	bool IsCancellationRequested() const override { return bCancelled; }
	FString LastStage;
	uint64 LastCompleted = 0;
	uint64 LastTotal = 0;
	uint64 CancelAtCompleted = MAX_uint64;
	bool bCancelled = false;
};

void WoTMeshAnimationSpecAddFact(FWoTRetailContract& Contract,
	EWoTFactDomain Domain, const TCHAR* FactId, const TCHAR* SourcePath,
	const TCHAR* SourceExport, const TCHAR* LogicalId,
	uint64 MaximumBytes = 512ull * 1024 * 1024)
{
	const bool bAnimation = Domain == EWoTFactDomain::Animation;
	FWoTRequiredFact& Fact = Contract.Facts.AddDefaulted_GetRef();
	Fact.Domain = Domain;
	Fact.FactId = FactId;
	Fact.SourcePath = SourcePath;
	Fact.SourceExport = SourceExport;
	Fact.Importer = bAnimation ? TEXT("animation-v1") : TEXT("mesh-v1");
	Fact.RuntimeConsumer = bAnimation
		? TEXT("UWoTVertexAnimComponent") : TEXT("UWoTPayloadSubsystem");
	Fact.AuditRule = bAnimation
		? TEXT("audit.animation-payload-v1") : TEXT("audit.mesh-payload-v1");
	FWoTRequiredFactOutput& Output = Fact.Outputs.AddDefaulted_GetRef();
	Output.LogicalId = LogicalId;
	Output.Schema = bAnimation ? TEXT("wot-animation-v3") : TEXT("wot-mesh-v3");
	Output.MaxDecodedSize = MaximumBytes;
}

FWoTRetailContract WoTMeshAnimationSpecContract()
{
	FWoTRetailContract Contract;
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Mesh,
		TEXT("synthetic.mesh.plain"), TEXT("Synthetic/mesh.u"),
		TEXT("mesh.PlainMesh"), TEXT("cache/mesh/synthetic-plain"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Animation,
		TEXT("synthetic.animation.plain"), TEXT("Synthetic/mesh.u"),
		TEXT("mesh.PlainMesh"), TEXT("cache/animation/synthetic-plain"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Mesh,
		TEXT("synthetic.mesh.lod"), TEXT("Synthetic/mesh.u"),
		TEXT("mesh.LodHero"), TEXT("cache/mesh/synthetic-lod"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Animation,
		TEXT("synthetic.animation.lod"), TEXT("Synthetic/mesh.u"),
		TEXT("mesh.LodHero"), TEXT("cache/animation/synthetic-lod"));
	return Contract;
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTMeshAnimationSpecHeader(
	const TArray<uint8>& Object)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	if (Object.Num() < 12 || FMemory::Memcmp(Object.GetData(), "WOTOBJ3", 8) != 0)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh typed-object frame is malformed")));
	}
	const uint32 HeaderSize = static_cast<uint32>(Object[8])
		| (static_cast<uint32>(Object[9]) << 8)
		| (static_cast<uint32>(Object[10]) << 16)
		| (static_cast<uint32>(Object[11]) << 24);
	if (HeaderSize > static_cast<uint32>(Object.Num() - 12))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the mesh typed-object header is truncated")));
	}
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = 1024 * 1024;
	Limits.MaxMembers = 1000000;
	return ParseCanonicalJson(TConstArrayView<uint8>(
		Object.GetData() + 12, static_cast<int32>(HeaderSize)), Limits);
}

bool WoTMeshAnimationSpecHasExactKeys(FAutomationTestBase& Test,
	const FWoTCanonicalJsonValue& Object, const TCHAR* CommaKeys,
	const FString& Label)
{
	TArray<FString> Keys;
	FString(CommaKeys).ParseIntoArray(Keys, TEXT(","), true);
	bool bOkay = Test.TestEqual(Label + TEXT(" member count"), Object.Num(), Keys.Num());
	for (const FString& Key : Keys)
	{
		bOkay &= Test.TestTrue(Label + TEXT(" contains ") + Key,
			Object.FindMember(Key) != nullptr);
	}
	return bOkay;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshAnimationParitySpec,
	"WoT.RetailReader.MeshAnimation.TypedParity", WoTMeshAnimationSpecFlags)

bool FWoTMeshAnimationParitySpec::RunTest(const FString&)
{
	FWoTMeshAnimationSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = WoTMeshAnimationSpecOpen(
		Root, TEXT("cache-v3-mesh-importer-input.bin"));
	if (!TestFalse(TEXT("mesh fixture installer opens"), Opened.IsError()))
	{
		if (Opened.IsError()) AddError(Opened.GetError().ToString());
		return false;
	}
	FWoTMeshAnimationSpecWriter Writer;
	FWoTMeshAnimationSpecProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported = ImportMeshAnimation(
		Opened.GetValue(), WoTMeshAnimationSpecContract(), Writer, Progress);
	if (!TestFalse(TEXT("all plain/LodMesh objects import"), Imported.IsError()))
	{
		if (Imported.IsError()) AddError(Imported.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("four mesh objects are atomically written"), Writer.Objects.Num(), 4);
	TestEqual(TEXT("four mesh blobs are returned"), Imported.GetValue().Num(), 4);
	TestEqual(TEXT("mesh progress completes the closed set"), Progress.LastCompleted,
		static_cast<uint64>(4));
	TestEqual(TEXT("mesh progress total is closed"), Progress.LastTotal,
		static_cast<uint64>(4));
	struct FExpected { const TCHAR* LogicalId; const TCHAR* Fixture; };
	const FExpected Expected[] = {
		{TEXT("cache/mesh/synthetic-plain"), TEXT("cache-v3-mesh-plain.bin")},
		{TEXT("cache/animation/synthetic-plain"), TEXT("cache-v3-animation-plain.bin")},
		{TEXT("cache/mesh/synthetic-lod"), TEXT("cache-v3-mesh-lod.bin")},
		{TEXT("cache/animation/synthetic-lod"), TEXT("cache-v3-animation-lod.bin")},
	};
	for (const FExpected& Item : Expected)
	{
		TArray<uint8> Fixture;
		const FWoTMeshAnimationSpecWritten* Written = Writer.Find(Item.LogicalId);
		TestTrue(FString::Printf(TEXT("%s fixture loads"), Item.Fixture),
			WoTMeshAnimationSpecLoadFixture(Item.Fixture, Fixture));
		if (TestTrue(FString::Printf(TEXT("%s was emitted"), Item.LogicalId),
			Written != nullptr))
		{
			TestTrue(FString::Printf(TEXT("%s matches Python bytes"), Item.LogicalId),
				Written->Bytes == Fixture);
		}
	}
	FWoTCacheManifestV3 Manifest;
	Manifest.Blobs = MoveTemp(Imported).GetValue();
	TWoTBootstrapResult<TArray<uint8>> ManifestBytes =
		WriteCanonicalCacheManifest(Manifest);
	TArray<uint8> ExpectedManifest;
	TestTrue(TEXT("mesh manifest fixture loads"), WoTMeshAnimationSpecLoadFixture(
		TEXT("cache-v3-mesh-manifest.bin"), ExpectedManifest));
	if (TestFalse(TEXT("mesh manifest encodes"), ManifestBytes.IsError()))
	{
		TestTrue(TEXT("native/Python mesh manifest bytes agree"),
			ManifestBytes.GetValue() == ExpectedManifest);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshAnimationPublicationReverifySpec,
	"WoT.RetailReader.MeshAnimation.PerPublicationSnapshotReverification",
	WoTMeshAnimationSpecFlags)

bool FWoTMeshAnimationPublicationReverifySpec::RunTest(const FString&)
{
	constexpr const TCHAR* FixtureName = TEXT("cache-v3-mesh-importer-input.bin");
	FWoTMeshAnimationSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened =
		WoTMeshAnimationSpecOpen(Root, FixtureName);
	if (!TestFalse(TEXT("shared mesh package opens"), Opened.IsError()))
	{
		return false;
	}
	FWoTRetailContract Contract = WoTMeshAnimationSpecContract();
	Contract.Facts.SetNum(2);
	TArray<uint8> Mutated;
	if (!TestTrue(TEXT("shared mesh installer bytes load"),
		WoTMeshAnimationSpecLoadFixture(FixtureName, Mutated))
		|| Mutated.IsEmpty())
	{
		return false;
	}
	// Change the length as well as the bytes. Same-length overwrite detection on
	// NTFS otherwise depends on when the filesystem publishes ChangeTime; a full
	// suite can perform create/overwrite/query inside one coalesced timestamp
	// window even though the isolated test usually crosses it. Size is part of
	// the production held identity and makes this race deterministic without
	// weakening either fail-closed assertion.
	Mutated.Add(0x6b);
	bool bMutationAttempted = false;
	bool bMutationSucceeded = false;
	FWoTMeshAnimationSpecWriter Writer;
	Writer.AfterWrite = [&]()
	{
		if (!bMutationAttempted && Writer.Objects.Num() == 1)
		{
			bMutationAttempted = true;
			bMutationSucceeded = WoTNativeHandleTestFactory::WriteFile(
				Root.Path / FixtureName, Mutated);
		}
	};
	FWoTMeshAnimationSpecProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
		ImportMeshAnimation(Opened.GetValue(), Contract, Writer, Progress);
	TestTrue(TEXT("the held installer is mutated after the first mesh publication"),
		bMutationAttempted && bMutationSucceeded);
	TestTrue(TEXT("the next shared-package mesh fact fails closed"),
		Imported.IsError());
	TestEqual(TEXT("the second mesh object is never written"),
		Writer.Objects.Num(), 1);
	TestEqual(TEXT("mesh publication verification performs no second decode"),
		Opened.GetValue().EntryDecodeCountForTesting(), 1ull);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshAnimationHeaderSpec,
	"WoT.RetailReader.MeshAnimation.CanonicalHeadersAndLodAbsence",
	WoTMeshAnimationSpecFlags)

bool FWoTMeshAnimationHeaderSpec::RunTest(const FString&)
{
	FWoTMeshAnimationSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = WoTMeshAnimationSpecOpen(
		Root, TEXT("cache-v3-mesh-importer-input.bin"));
	if (!TestFalse(TEXT("mesh header fixture opens"), Opened.IsError())) return false;
	FWoTMeshAnimationSpecWriter Writer;
	FWoTMeshAnimationSpecProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported = ImportMeshAnimation(
		Opened.GetValue(), WoTMeshAnimationSpecContract(), Writer, Progress);
	if (!TestFalse(TEXT("mesh header objects import"), Imported.IsError())) return false;
	const TCHAR* MeshKeys =
		TEXT("bounds,class_name,collision,encoding,fact_id,frame_count,importer,")
		TEXT("importer_version,kind,lod,origin_bits,payload_byte_length,render_vertices,")
		TEXT("rotation_origin,scale_bits,sections,skins,source,tangent_rule,triangle_count,")
		TEXT("triangles,uv_count,uvs,vertex_count");
	const TCHAR* AnimationKeys =
		TEXT("encoding,fact_id,frame_byte_length,frame_count,importer,importer_version,")
		TEXT("kind,payload_byte_length,sequence_count,sequences,source,vertex_count");
	for (const FWoTMeshAnimationSpecWritten& Written : Writer.Objects)
	{
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
			WoTMeshAnimationSpecHeader(Written.Bytes);
		if (!TestFalse(Written.LogicalId + TEXT(" header parses"), Header.IsError())) continue;
		WoTMeshAnimationSpecHasExactKeys(*this, Header.GetValue(),
			Written.Schema == TEXT("wot-mesh-v3") ? MeshKeys : AnimationKeys,
			Written.LogicalId);
	}
	const FWoTMeshAnimationSpecWritten* Plain = Writer.Find(TEXT("cache/mesh/synthetic-plain"));
	const FWoTMeshAnimationSpecWritten* Lod = Writer.Find(TEXT("cache/mesh/synthetic-lod"));
	if (Plain != nullptr && Lod != nullptr)
	{
		TWoTBootstrapResult<FWoTCanonicalJsonValue> PlainHeader =
			WoTMeshAnimationSpecHeader(Plain->Bytes);
		TWoTBootstrapResult<FWoTCanonicalJsonValue> LodHeader =
			WoTMeshAnimationSpecHeader(Lod->Bytes);
		if (!PlainHeader.IsError() && !LodHeader.IsError())
		{
			TestTrue(TEXT("plain Mesh has explicit null LOD facts"),
				PlainHeader.GetValue().FindMember(TEXT("lod"))->IsNull());
			const FWoTCanonicalJsonValue* LodFacts =
				LodHeader.GetValue().FindMember(TEXT("lod"));
			TestTrue(TEXT("LodMesh has a complete LOD fact object"),
				LodFacts != nullptr && LodFacts->IsObject());
			const FWoTCanonicalJsonValue* Triangles =
				LodHeader.GetValue().FindMember(TEXT("triangles"));
			bool bFoundUnavailable = false;
			if (Triangles != nullptr && Triangles->IsArray())
			{
				for (int32 Index = 0; Index < Triangles->Num(); ++Index)
				{
					const FWoTCanonicalJsonValue& Triangle = Triangles->GetElement(Index);
					const FWoTCanonicalJsonValue* Attachment =
						Triangle.FindMember(TEXT("lod_attachment"));
					if (Attachment != nullptr && Attachment->GetBool())
					{
						bFoundUnavailable = Triangle.FindMember(TEXT("uv_indices"))->IsNull()
							&& Triangle.FindMember(TEXT("poly_flags"))->IsNull()
							&& Triangle.FindMember(TEXT("texture_index"))->IsNull();
					}
				}
			}
			TestTrue(TEXT("LodMesh attachment UV/material fields are explicitly absent"),
				bFoundUnavailable);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshAnimationRejectSpec,
	"WoT.RetailReader.MeshAnimation.RejectionMatrix", WoTMeshAnimationSpecFlags)

bool FWoTMeshAnimationRejectSpec::RunTest(const FString&)
{
	struct FCase { const TCHAR* Fixture; bool bLod; bool bAnimation; };
	const FCase Cases[] = {
		{TEXT("cache-v3-mesh-triangle-out-of-range.bin"), false, false},
		{TEXT("cache-v3-mesh-corrupt-frame-offset.bin"), false, false},
		{TEXT("cache-v3-mesh-numeric-overflow.bin"), false, false},
		{TEXT("cache-v3-mesh-decompression-limit.bin"), false, false},
		{TEXT("cache-v3-mesh-triangle-count-bomb.bin"), false, false},
		{TEXT("cache-v3-mesh-lod-special-overflow.bin"), true, false},
		{TEXT("cache-v3-animation-sequence-out-of-range.bin"), false, true},
		{TEXT("cache-v3-animation-sequence-overlap.bin"), false, true},
		{TEXT("cache-v3-animation-notify-out-of-range.bin"), false, true},
		{TEXT("cache-v3-mesh-lod-material-out-of-range.bin"), true, false},
		{TEXT("cache-v3-mesh-lod-remap-out-of-range.bin"), true, false},
		{TEXT("cache-v3-mesh-skeletal-class-rejected.bin"), false, false},
	};
	for (const FCase& Case : Cases)
	{
		FWoTMeshAnimationSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTMeshAnimationSpecOpen(Root, Case.Fixture);
		if (!TestFalse(FString(Case.Fixture) + TEXT(" installer opens"),
			Opened.IsError())) continue;
		FWoTRetailContract Contract;
		WoTMeshAnimationSpecAddFact(Contract,
			Case.bAnimation ? EWoTFactDomain::Animation : EWoTFactDomain::Mesh,
			TEXT("synthetic.reject"), TEXT("Synthetic/mesh.u"),
			Case.bLod ? TEXT("mesh.LodHero") : TEXT("mesh.PlainMesh"),
			Case.bAnimation ? TEXT("cache/animation/reject") : TEXT("cache/mesh/reject"));
		FWoTMeshAnimationSpecWriter Writer;
		FWoTMeshAnimationSpecProgress Progress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported = ImportMeshAnimation(
			Opened.GetValue(), Contract, Writer, Progress);
		TestTrue(FString(Case.Fixture) + TEXT(" is rejected"), Imported.IsError());
		TestEqual(FString(Case.Fixture) + TEXT(" publishes nothing"),
			Writer.Objects.Num(), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshAnimationSourceOnlySpec,
	"WoT.RetailReader.MeshAnimation.SourceOnlyAndAtomic", WoTMeshAnimationSpecFlags)

bool FWoTMeshAnimationSourceOnlySpec::RunTest(const FString&)
{
	FWoTMeshAnimationSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = WoTMeshAnimationSpecOpen(
		Root, TEXT("cache-v3-mesh-importer-input.bin"));
	if (!TestFalse(TEXT("source-only fixture opens"), Opened.IsError())) return false;
	FWoTRetailContract One = WoTMeshAnimationSpecContract();
	One.Facts.SetNum(1);
	FWoTMeshAnimationSpecWriter FirstWriter;
	FWoTMeshAnimationSpecProgress FirstProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> First = ImportMeshAnimation(
		Opened.GetValue(), One, FirstWriter, FirstProgress);
	TestFalse(TEXT("baseline source-only mesh imports"), First.IsError());
	FWoTRetailContract NonGeometryContract = One;
	NonGeometryContract.Facts[0].RuntimeConsumer = TEXT("IgnoredConsumerMetadata");
	NonGeometryContract.Facts[0].AuditRule = TEXT("ignored.audit.metadata");
	NonGeometryContract.Facts[0].Outputs[0].MaxDecodedSize = 513ull * 1024 * 1024;
	FWoTMeshAnimationSpecWriter SecondWriter;
	FWoTMeshAnimationSpecProgress SecondProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Second = ImportMeshAnimation(
		Opened.GetValue(), NonGeometryContract, SecondWriter, SecondProgress);
	TestFalse(TEXT("non-geometry contract metadata imports"), Second.IsError());
	TestTrue(TEXT("counts/topology/mapping/bounds/sections/frames/sequences are source-only"),
		FirstWriter.Objects.Num() == 1 && SecondWriter.Objects.Num() == 1
		&& FirstWriter.Objects[0].Bytes == SecondWriter.Objects[0].Bytes);

	FWoTRetailContract ExtraCarrier = One;
	FWoTRequiredFactOutput& Carrier = ExtraCarrier.Facts[0].Outputs.AddDefaulted_GetRef();
	Carrier.LogicalId = TEXT("cache/mesh/cooked-derived-carrier");
	Carrier.Schema = TEXT("runtime-ready-geometry");
	Carrier.MaxDecodedSize = 1024;
	FWoTMeshAnimationSpecWriter CarrierWriter;
	FWoTMeshAnimationSpecProgress CarrierProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> CarrierResult = ImportMeshAnimation(
		Opened.GetValue(), ExtraCarrier, CarrierWriter, CarrierProgress);
	TestTrue(TEXT("a cooked derived carrier output is rejected"), CarrierResult.IsError());
	TestEqual(TEXT("a cooked carrier publishes nothing"), CarrierWriter.Objects.Num(), 0);

	FWoTMeshAnimationSpecWriter CancelWriter;
	FWoTMeshAnimationSpecProgress CancelProgress;
	CancelProgress.CancelAtCompleted = 1;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Cancelled = ImportMeshAnimation(
		Opened.GetValue(), WoTMeshAnimationSpecContract(), CancelWriter, CancelProgress);
	TestTrue(TEXT("cancellation prevents a completed mesh result"), Cancelled.IsError());
	TestEqual(TEXT("only one complete object precedes cancellation"),
		CancelWriter.Objects.Num(), 1);

	FWoTRetailContract Tiny = One;
	Tiny.Facts[0].Outputs[0].MaxDecodedSize = 1;
	FWoTMeshAnimationSpecWriter TinyWriter;
	FWoTMeshAnimationSpecProgress TinyProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Oversized = ImportMeshAnimation(
		Opened.GetValue(), Tiny, TinyWriter, TinyProgress);
	TestTrue(TEXT("the contract byte limit rejects before publication"), Oversized.IsError());
	TestEqual(TEXT("oversized mesh bytes never reach the writer"), TinyWriter.Objects.Num(), 0);

	FWoTMeshAnimationSpecWriter RefusingWriter;
	RefusingWriter.bRejectNext = true;
	FWoTMeshAnimationSpecProgress RefusingProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Refused = ImportMeshAnimation(
		Opened.GetValue(), One, RefusingWriter, RefusingProgress);
	TestTrue(TEXT("atomic writer refusal aborts mesh import"), Refused.IsError());
	TestEqual(TEXT("refused mesh write leaves no partial object"),
		RefusingWriter.Objects.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTMeshAnimationRetailPinSpec,
	"WoT.RetailReader.MeshAnimation.RealRetailDecodeMeshPins", WoTMeshAnimationSpecFlags)

bool FWoTMeshAnimationRetailPinSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	const FString RetailPath = WoTMeshAnimationSpecRepoPath(TEXT("original/System/WoTPawns.u"));
	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes, *RetailPath))
	{
		AddError(TEXT("original/System/WoTPawns.u is required for the retail pin"));
		return false;
	}
	FWoTRetailContract Contract;
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Mesh,
		TEXT("retail.mesh.arrowtrail"), TEXT("System/WoTPawns.u"),
		TEXT("WoTPawns.ArrowTrail"), TEXT("cache/mesh/retail-arrowtrail"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Animation,
		TEXT("retail.animation.arrowtrail"), TEXT("System/WoTPawns.u"),
		TEXT("WoTPawns.ArrowTrail"), TEXT("cache/animation/retail-arrowtrail"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Mesh,
		TEXT("retail.mesh.injuredwarder"), TEXT("System/WoTPawns.u"),
		TEXT("WoTPawns.InjuredWarder"), TEXT("cache/mesh/retail-injuredwarder"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Animation,
		TEXT("retail.animation.injuredwarder"), TEXT("System/WoTPawns.u"),
		TEXT("WoTPawns.InjuredWarder"), TEXT("cache/animation/retail-injuredwarder"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Mesh,
		TEXT("retail.mesh.marcherarrow"), TEXT("System/WoTPawns.u"),
		TEXT("WoTPawns.MArcherArrow"), TEXT("cache/mesh/retail-marcherarrow"));
	WoTMeshAnimationSpecAddFact(Contract, EWoTFactDomain::Animation,
		TEXT("retail.animation.marcherarrow"), TEXT("System/WoTPawns.u"),
		TEXT("WoTPawns.MArcherArrow"), TEXT("cache/animation/retail-marcherarrow"));
	FWoTMeshAnimationSpecWriter Writer;
	FWoTMeshAnimationSpecProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
		ImportMeshAnimationPackageBytesForTesting(
			SourceBytes, TEXT("System/WoTPawns.u"), Contract, Writer, Progress);
	if (!TestFalse(TEXT("the native importer derives both retail layouts"), Imported.IsError()))
	{
		if (Imported.IsError()) AddError(Imported.GetError().ToString());
		return false;
	}
	struct FPin
	{
		const TCHAR* LogicalId;
		const TCHAR* Sha256;
		int64 VertexCount;
		int64 FrameCount;
		int64 TriangleCount;
	};
	const FPin Pins[] = {
		{TEXT("cache/mesh/retail-arrowtrail"),
			TEXT("98f5ab8161b416a532656ed82dbadaecb119df4f55e5ddaa9336adb87ddbc22d"),
			12, 1, 6},
		{TEXT("cache/animation/retail-arrowtrail"),
			TEXT("dc49c4c017ff269ea7540f400f4aa27e58d3b500db338afed0c160e7b3622a39"),
			12, 1, -1},
		{TEXT("cache/mesh/retail-injuredwarder"),
			TEXT("11a49fbbeb41af72017d70c1c1820585d36119486e798c3e170bc886eac7f5e5"),
			464, 48, 874},
		{TEXT("cache/animation/retail-injuredwarder"),
			TEXT("df2a501f7200c97ab7355c48c82318c5f97ed4b54ef156e9c7d1fc13ade6e22a"),
			464, 48, -1},
		{TEXT("cache/mesh/retail-marcherarrow"),
			TEXT("64ceb31087fb5e2ed718fce83aa2bd8125550f1c5e4dbf604fa297cb0fc15316"),
			20, 1, 18},
		{TEXT("cache/animation/retail-marcherarrow"),
			TEXT("0c18ea1a5a7c00d128e93c045cce5b122702f87a4e41939042afcc410bb32cf0"),
			20, 1, -1},
	};
	for (const FPin& Pin : Pins)
	{
		const FWoTMeshAnimationSpecWritten* Written = Writer.Find(Pin.LogicalId);
		if (!TestTrue(FString(Pin.LogicalId) + TEXT(" was emitted"), Written != nullptr))
		{
			continue;
		}
		TestEqual(FString(Pin.LogicalId) + TEXT(" matches decode_mesh object SHA-256"),
			Sha256(Written->Bytes).ToHex(), FString(Pin.Sha256));
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
			WoTMeshAnimationSpecHeader(Written->Bytes);
		if (!TestFalse(FString(Pin.LogicalId) + TEXT(" header parses"), Header.IsError()))
		{
			continue;
		}
		TestEqual(FString(Pin.LogicalId) + TEXT(" vertex count"),
			Header.GetValue().FindMember(TEXT("vertex_count"))->GetInteger(), Pin.VertexCount);
		TestEqual(FString(Pin.LogicalId) + TEXT(" frame count"),
			Header.GetValue().FindMember(TEXT("frame_count"))->GetInteger(), Pin.FrameCount);
		if (Pin.TriangleCount >= 0)
		{
			TestEqual(FString(Pin.LogicalId) + TEXT(" triangle count"),
				Header.GetValue().FindMember(TEXT("triangle_count"))->GetInteger(),
				Pin.TriangleCount);
		}
	}
	const FWoTMeshAnimationSpecWritten* Rotated =
		Writer.Find(TEXT("cache/mesh/retail-marcherarrow"));
	if (Rotated != nullptr)
	{
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
			WoTMeshAnimationSpecHeader(Rotated->Bytes);
		if (TestFalse(TEXT("nonzero-rotation retail header parses"), Header.IsError()))
		{
			const FWoTCanonicalJsonValue* Rotation =
				Header.GetValue().FindMember(TEXT("rotation_origin"));
			TestTrue(TEXT("MArcherArrow carries decode_mesh RotationOrigin"),
				Rotation != nullptr && Rotation->IsArray() && Rotation->Num() == 3
				&& Rotation->GetElement(0).GetInteger() == 16384
				&& Rotation->GetElement(1).GetInteger() == 0
				&& Rotation->GetElement(2).GetInteger() == 0);
		}
	}
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
