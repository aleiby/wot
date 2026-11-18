// Cache-v3 typed importer specs pinned to the Python-authored binary corpus.
#include "Misc/AutomationTest.h"

#include "CanonicalObjectBuilder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "NativeHandleTestFactory.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/CacheBuilder.h"
#include "WoTRetailReader/Package.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "WoTRetailReader/TextureSound.h"

#if WITH_AUTOMATION_TESTS

namespace
{
constexpr EAutomationTestFlags WoTTextureSoundSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

struct FWoTTextureSoundSpecRoot
{
	FString Path = WoTNativeHandleTestFactory::CreateTemporaryRoot();
	~FWoTTextureSoundSpecRoot()
	{
		WoTNativeHandleTestFactory::RemoveTree(Path);
	}
};

FString WoTTextureSoundSpecFixturePath(const TCHAR* Name)
{
	const FString ThisFile(ANSI_TO_TCHAR(__FILE__));
	return FPaths::ConvertRelativePathToFull(FPaths::GetPath(ThisFile)
		/ TEXT("../../../../Tools/dist/fixtures") / Name);
}

bool WoTTextureSoundSpecLoadFixture(const TCHAR* Name, TArray<uint8>& Out)
{
	return FFileHelper::LoadFileToArray(Out, *WoTTextureSoundSpecFixturePath(Name));
}

TWoTBootstrapResult<FWoTHeldInstallerSnapshot> WoTTextureSoundSpecSnapshot(
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

TWoTBootstrapResult<FWoTRetailPackage> WoTTextureSoundSpecOpen(
	FWoTTextureSoundSpecRoot& Root, const TCHAR* FixtureName)
{
	using FResult = TWoTBootstrapResult<FWoTRetailPackage>;
	TArray<uint8> Fixture;
	if (!WoTTextureSoundSpecLoadFixture(FixtureName, Fixture))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the cache-v3 fixture is missing"), FixtureName));
	}
	const FString Installer = Root.Path / FString(FixtureName);
	if (!WoTNativeHandleTestFactory::WriteFile(Installer, Fixture))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("the cache-v3 fixture could not be staged"), Installer));
	}
	TWoTBootstrapResult<FWoTHeldInstallerSnapshot> Snapshot =
		WoTTextureSoundSpecSnapshot(Installer);
	if (Snapshot.IsError()) return FResult::Error(MoveTemp(Snapshot).GetError());
	return FWoTRetailPackage::Open(MoveTemp(Snapshot).GetValue(), FWoTArchiveLimits());
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTTextureSoundSpecParseHeader(
	const TArray<uint8>& Object)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	if (Object.Num() < 12 || FMemory::Memcmp(Object.GetData(), "WOTOBJ3", 8) != 0)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the emitted typed object frame is malformed")));
	}
	const uint32 HeaderSize = static_cast<uint32>(Object[8])
		| (static_cast<uint32>(Object[9]) << 8)
		| (static_cast<uint32>(Object[10]) << 16)
		| (static_cast<uint32>(Object[11]) << 24);
	if (HeaderSize > static_cast<uint32>(Object.Num() - 12))
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the emitted typed object header exceeds its frame")));
	}
	return ParseCanonicalJson(TConstArrayView<uint8>(
		Object.GetData() + 12, static_cast<int32>(HeaderSize)));
}

struct FWoTTextureSoundSpecWritten
{
	FString LogicalId;
	FString Schema;
	TArray<uint8> Bytes;
	uint64 MaximumBytes = 0;
};

class FWoTTextureSoundSpecWriter final : public IWoTObjectWriter
{
public:
	FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
		TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
	{
		if (bRejectNext)
		{
			bRejectNext = false;
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io, TEXT("synthetic atomic writer refusal")));
		}
		if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::SizeLimit, TEXT("writer received an oversized object")));
		}
		FWoTTextureSoundSpecWritten& Object = Objects.AddDefaulted_GetRef();
		Object.LogicalId = FString(LogicalId);
		Object.Schema = FString(Schema);
		Object.Bytes.Append(Bytes.GetData(), Bytes.Num());
		Object.MaximumBytes = MaximumBytes;
		if (AfterWrite)
		{
			AfterWrite();
		}
		return FWoTBootstrapStatus::Success();
	}

	const FWoTTextureSoundSpecWritten* Find(FStringView LogicalId) const
	{
		for (const FWoTTextureSoundSpecWritten& Object : Objects)
		{
			if (Object.LogicalId == LogicalId) return &Object;
		}
		return nullptr;
	}

	TArray<FWoTTextureSoundSpecWritten> Objects;
	TFunction<void()> AfterWrite;
	bool bRejectNext = false;
};

class FWoTTextureSoundSpecProgress final : public IWoTProgressSink
{
public:
	FWoTBootstrapStatus ReportProgress(FStringView Stage, uint64 Completed,
		uint64 Total) override
	{
		LastStage = FString(Stage);
		LastCompleted = Completed;
		LastTotal = Total;
		if (CancelAtCompleted != TNumericLimits<uint64>::Max()
			&& Completed >= CancelAtCompleted)
		{
			bCancelled = true;
		}
		return FWoTBootstrapStatus::Success();
	}

	bool IsCancellationRequested() const override { return bCancelled; }

	FString LastStage;
	uint64 LastCompleted = 0;
	uint64 LastTotal = 0;
	uint64 CancelAtCompleted = TNumericLimits<uint64>::Max();
	bool bCancelled = false;
};

void WoTTextureSoundSpecAddFact(FWoTRetailContract& Contract,
	EWoTFactDomain Domain, const TCHAR* FactId, const TCHAR* SourcePath,
	const TCHAR* SourceExport, const TCHAR* Importer, const TCHAR* LogicalId,
	const TCHAR* Schema, uint64 MaximumBytes = 256ull * 1024 * 1024)
{
	FWoTRequiredFact& Fact = Contract.Facts.AddDefaulted_GetRef();
	Fact.Domain = Domain;
	Fact.FactId = FactId;
	Fact.SourcePath = SourcePath;
	Fact.SourceExport = SourceExport;
	Fact.Importer = Importer;
	Fact.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
	Fact.AuditRule = TEXT("audit.typed-payload-v1");
	FWoTRequiredFactOutput& Output = Fact.Outputs.AddDefaulted_GetRef();
	Output.LogicalId = LogicalId;
	Output.Schema = Schema;
	Output.MaxDecodedSize = MaximumBytes;
}

FWoTRetailContract WoTTextureSoundSpecValidContract()
{
	FWoTRetailContract Contract;
	WoTTextureSoundSpecAddFact(Contract, EWoTFactDomain::Texture,
		TEXT("synthetic.texture.masked"), TEXT("Synthetic/texture.u"),
		TEXT("texture.SurfaceWall.Stone"), TEXT("texture-v2"),
		TEXT("cache/texture/synthetic-masked"), TEXT("wot-texture-v3"));
	WoTTextureSoundSpecAddFact(Contract, EWoTFactDomain::Texture,
		TEXT("synthetic.texture.opaque"), TEXT("Synthetic/texture.u"),
		TEXT("texture.SurfaceFloor.Stone"), TEXT("texture-v2"),
		TEXT("cache/texture/synthetic-opaque"), TEXT("wot-texture-v3"));
	WoTTextureSoundSpecAddFact(Contract, EWoTFactDomain::Sound,
		TEXT("synthetic.sound.two-identical-loops"), TEXT("Synthetic/sound.u"),
		TEXT("sound.Ambient.Hum"), TEXT("sound-v1"),
		TEXT("cache/sound/synthetic-two-identical-loops"), TEXT("wot-sound-v3"));
	WoTTextureSoundSpecAddFact(Contract, EWoTFactDomain::Sound,
		TEXT("synthetic.sound.two-distinct-loops"), TEXT("Synthetic/sound.u"),
		TEXT("sound.Layered"), TEXT("sound-v1"),
		TEXT("cache/sound/synthetic-two-distinct-loops"), TEXT("wot-sound-v3"));
	WoTTextureSoundSpecAddFact(Contract, EWoTFactDomain::Sound,
		TEXT("synthetic.sound.pcm8-stereo"), TEXT("Synthetic/sound.u"),
		TEXT("sound.Click"), TEXT("sound-v1"),
		TEXT("cache/sound/synthetic-pcm8-stereo"), TEXT("wot-sound-v3"));
	return Contract;
}

FWoTRetailContract WoTTextureSoundSpecRejectContract(EWoTFactDomain Domain)
{
	FWoTRetailContract Contract;
	const bool bTexture = Domain == EWoTFactDomain::Texture;
	WoTTextureSoundSpecAddFact(Contract, Domain, TEXT("synthetic.reject"),
		bTexture ? TEXT("Synthetic/reject-texture.u") : TEXT("Synthetic/reject-stream.u"),
		bTexture ? TEXT("reject-texture.Bad") : TEXT("reject-stream.Bad"),
		bTexture ? TEXT("texture-v2") : TEXT("sound-v1"),
		bTexture ? TEXT("cache/texture/reject") : TEXT("cache/sound/reject"),
		bTexture ? TEXT("wot-texture-v3") : TEXT("wot-sound-v3"));
	return Contract;
}
}  // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureSoundParitySpec,
	"WoT.RetailReader.TextureSound.TypedParity", WoTTextureSoundSpecFlags)

bool FWoTTextureSoundParitySpec::RunTest(const FString&)
{
	FWoTTextureSoundSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = WoTTextureSoundSpecOpen(
		Root, TEXT("cache-v3-importer-input.bin"));
	if (!TestFalse(TEXT("typed fixture package opens"), Opened.IsError()))
	{
		if (Opened.IsError()) AddError(Opened.GetError().ToString());
		return false;
	}
	FWoTTextureSoundSpecWriter Writer;
	FWoTTextureSoundSpecProgress Progress;
	const FWoTRetailContract Contract = WoTTextureSoundSpecValidContract();
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
		ImportTextureSound(Opened.GetValue(), Contract, Writer, Progress);
	if (!TestFalse(TEXT("all five texture/sound facts import"), Imported.IsError()))
	{
		if (Imported.IsError()) AddError(Imported.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("all expected objects are atomically written"), Writer.Objects.Num(), 5);
	TestEqual(TEXT("all expected blobs are returned"), Imported.GetValue().Num(), 5);
	TestEqual(TEXT("progress completed the closed set"), Progress.LastCompleted,
		static_cast<uint64>(5));
	TestEqual(TEXT("progress total is the closed set"), Progress.LastTotal,
		static_cast<uint64>(5));

	struct FExpected
	{
		const TCHAR* LogicalId;
		const TCHAR* Fixture;
	};
	const FExpected Expected[] = {
		{TEXT("cache/texture/synthetic-masked"), TEXT("cache-v3-texture-masked.bin")},
		{TEXT("cache/texture/synthetic-opaque"), TEXT("cache-v3-texture-opaque.bin")},
		{TEXT("cache/sound/synthetic-two-identical-loops"),
			TEXT("cache-v3-sound-two-identical-loops.bin")},
		{TEXT("cache/sound/synthetic-two-distinct-loops"),
			TEXT("cache-v3-sound-two-distinct-loops.bin")},
		{TEXT("cache/sound/synthetic-pcm8-stereo"), TEXT("cache-v3-sound-pcm8-stereo.bin")},
	};
	for (const FExpected& Item : Expected)
	{
		TArray<uint8> Fixture;
		const FWoTTextureSoundSpecWritten* Object = Writer.Find(Item.LogicalId);
		TestTrue(FString::Printf(TEXT("%s fixture loads"), Item.Fixture),
			WoTTextureSoundSpecLoadFixture(Item.Fixture, Fixture));
		TestTrue(FString::Printf(TEXT("%s object was emitted"), Item.LogicalId),
			Object != nullptr);
		if (Object != nullptr)
		{
			TestTrue(FString::Printf(TEXT("%s matches Python bytes"), Item.LogicalId),
				Object->Bytes == Fixture);
			if (Object->Bytes != Fixture)
			{
				auto AsText = [](const TArray<uint8>& Raw)
				{
					FString Text;
					for (int32 Index = 12; Index < Raw.Num() && Index < 700; ++Index)
					{
						Text.AppendChar(static_cast<TCHAR>(Raw[Index]));
					}
					return Text;
				};
				AddError(FString::Printf(TEXT("%s NATIVE(%d): %s"), Item.LogicalId,
					Object->Bytes.Num(), *AsText(Object->Bytes)));
				AddError(FString::Printf(TEXT("%s PYTHON(%d): %s"), Item.LogicalId,
					Fixture.Num(), *AsText(Fixture)));
			}
		}
	}
	FWoTCacheManifestV3 Manifest;
	Manifest.Blobs = MoveTemp(Imported).GetValue();
	TWoTBootstrapResult<TArray<uint8>> ManifestBytes =
		WriteCanonicalCacheManifest(Manifest);
	TestFalse(TEXT("canonical manifest construction succeeds"), ManifestBytes.IsError());
	TArray<uint8> ExpectedManifest;
	TestTrue(TEXT("canonical manifest fixture loads"),
		WoTTextureSoundSpecLoadFixture(TEXT("cache-v3-manifest.bin"), ExpectedManifest));
	if (!ManifestBytes.IsError())
	{
		TestTrue(TEXT("native/Python canonical manifest bytes agree"),
			ManifestBytes.GetValue() == ExpectedManifest);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureAnimationRealRetailCensusSpec,
	"WoT.RetailReader.TextureSound.RealRetailAnimationCensus",
	WoTTextureSoundSpecFlags)

bool FWoTTextureAnimationRealRetailCensusSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(TEXT("real retail data absent: production census not verified"));
		return true;
	}
	TWoTBootstrapResult<FWoTRetailContract> Loaded =
		WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production contract loads"), Loaded.IsError()))
	{
		if (Loaded.IsError()) AddError(Loaded.GetError().ToString());
		return false;
	}
	struct FAnimationRow
	{
		FString Next;
		uint32 MinBits = 0;
		uint32 MaxBits = 0;
		uint32 PrimeCount = 0;
	};
	uint32 TotalChains = 0;
	uint32 TotalFrames = 0;
	for (const TCHAR* SourcePath : {TEXT("System/Angreal.u"), TEXT("System/WOT.u")})
	{
		TArray<uint8> Bytes;
		const FString Relative = TEXT("original/") + FString(SourcePath);
		if (!TestTrue(Relative + TEXT(" loads"), FFileHelper::LoadFileToArray(
			Bytes, *WoTRepositoryOrReferencePath(Relative)))) return false;
		FWoTRetailContract Contract;
		for (const FWoTRequiredFact& Fact : Loaded.GetValue().Facts)
			if (Fact.Domain == EWoTFactDomain::Texture
				&& Fact.SourcePath == SourcePath)
				Contract.Facts.Add(Fact);
		FWoTTextureSoundSpecWriter Writer;
		FWoTTextureSoundSpecProgress Progress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
			ImportTexturePackageBytesForTesting(
				Bytes, SourcePath, Contract, Writer, Progress);
		if (!TestFalse(FString(SourcePath) + TEXT(" textures import"),
			Imported.IsError()))
		{
			if (Imported.IsError()) AddError(Imported.GetError().ToString());
			return false;
		}
		TMap<FString, FAnimationRow> Rows;
		for (const FWoTTextureSoundSpecWritten& Object : Writer.Objects)
		{
			TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
				WoTTextureSoundSpecParseHeader(Object.Bytes);
			if (!TestFalse(Object.LogicalId + TEXT(" header parses"), Header.IsError()))
				continue;
			const FWoTCanonicalJsonValue* Source =
				Header.GetValue().FindMember(TEXT("source"));
			const FWoTCanonicalJsonValue* Export =
				Source != nullptr ? Source->FindMember(TEXT("export")) : nullptr;
			const FWoTCanonicalJsonValue* Next =
				Header.GetValue().FindMember(TEXT("anim_next"));
			const FWoTCanonicalJsonValue* Min =
				Header.GetValue().FindMember(TEXT("min_frame_rate_bits"));
			const FWoTCanonicalJsonValue* Max =
				Header.GetValue().FindMember(TEXT("max_frame_rate_bits"));
			const FWoTCanonicalJsonValue* Prime =
				Header.GetValue().FindMember(TEXT("prime_count"));
			if (!TestTrue(Object.LogicalId + TEXT(" animation header is complete"),
				Export != nullptr && Next != nullptr && Min != nullptr
					&& Max != nullptr && Prime != nullptr))
				continue;
			FAnimationRow& Row = Rows.Add(Export->GetString());
			Row.Next = Next->GetString();
			Row.MinBits = static_cast<uint32>(Min->GetInteger());
			Row.MaxBits = static_cast<uint32>(Max->GetInteger());
			Row.PrimeCount = static_cast<uint32>(Prime->GetInteger());
		}
		TSet<FString> Targets;
		for (const TPair<FString, FAnimationRow>& Pair : Rows)
			if (!Pair.Value.Next.IsEmpty()) Targets.Add(Pair.Value.Next.ToLower());
		TArray<FString> Heads;
		for (const TPair<FString, FAnimationRow>& Pair : Rows)
			if (!Pair.Value.Next.IsEmpty()
				&& !Targets.Contains(Pair.Key.ToLower()))
				Heads.Add(Pair.Key);
		Heads.Sort();
		uint32 PackageFrames = 0;
		for (const FString& Head : Heads)
		{
			TSet<FString> Seen;
			FString Current = Head;
			uint32 Frames = 0;
			for (;;)
			{
				const FAnimationRow* Row = Rows.Find(Current);
				if (!TestNotNull(Current + TEXT(" animation frame is cached"), Row))
					break;
				if (!TestFalse(Current + TEXT(" animation chain is acyclic"),
					Seen.Contains(Current.ToLower())))
					break;
				Seen.Add(Current.ToLower());
				++Frames;
				TestEqual(Current + TEXT(" PrimeCount"), Row->PrimeCount, 0u);
				if (Row->Next.IsEmpty()) break;
				const FString Package = Current.Left(Current.Find(TEXT(".")));
				TestTrue(Current + TEXT(" AnimNext remains in its package"),
					Row->Next.StartsWith(Package + TEXT("."),
						ESearchCase::IgnoreCase));
				Current = Row->Next;
			}
			PackageFrames += Frames;
			if (Head.EndsWith(TEXT(".SG_A00"), ESearchCase::IgnoreCase))
			{
				TestEqual(TEXT("SG_A00 frame count"), Frames, 18u);
				TestEqual(TEXT("SG_A00 minimum rate bits"),
					Rows[Head].MinBits, 0u);
				uint32 FifteenBits = 0;
				const float Fifteen = 15.0f;
				FMemory::Memcpy(&FifteenBits, &Fifteen, sizeof(uint32));
				TestEqual(TEXT("SG_A00 maximum rate bits"),
					Rows[Head].MaxBits, FifteenBits);
			}
			if (Head.EndsWith(TEXT(".BFHand_A01"), ESearchCase::IgnoreCase))
			{
				TestEqual(TEXT("BFHand_A01 frame count"), Frames, 10u);
				TestEqual(TEXT("BFHand_A01 minimum rate bits"),
					Rows[Head].MinBits, 0u);
				uint32 EighteenBits = 0;
				const float Eighteen = 18.0f;
				FMemory::Memcpy(&EighteenBits, &Eighteen, sizeof(uint32));
				TestEqual(TEXT("BFHand_A01 maximum rate bits"),
					Rows[Head].MaxBits, EighteenBits);
			}
		}
		const bool bAngreal = FString(SourcePath).Contains(TEXT("Angreal"));
		TestEqual(FString(SourcePath) + TEXT(" animation chains"),
			Heads.Num(), bAngreal ? 14 : 15);
		TestEqual(FString(SourcePath) + TEXT(" animation frames"),
			PackageFrames, bAngreal ? 182u : 131u);
		AddInfo(FString::Printf(TEXT("%s animation census chains=%d frames=%u"),
			SourcePath, Heads.Num(), PackageFrames));
		TotalChains += static_cast<uint32>(Heads.Num());
		TotalFrames += PackageFrames;
	}
	TestEqual(TEXT("production animation chain total"), TotalChains, 29u);
	TestEqual(TEXT("production animation frame total"), TotalFrames, 313u);

	FWoTRetailContract TextureContract = Loaded.GetValue();
	TextureContract.Facts.Reset();
	struct FTextureSource
	{
		FString Path;
		TArray<uint8> Bytes;
	};
	TArray<FTextureSource> TextureSources;
	TMap<FString, uint64> SourceSizes;
	FWoTSha256Hasher InstallerHasher;
	for (const TCHAR* SourcePath : {TEXT("System/Angreal.u"), TEXT("System/WOT.u")})
	{
		FTextureSource& Source = TextureSources.AddDefaulted_GetRef();
		Source.Path = SourcePath;
		const FString Relative = TEXT("original/") + Source.Path;
		if (!TestTrue(Relative + TEXT(" coordinator source loads"),
			FFileHelper::LoadFileToArray(Source.Bytes,
				*WoTRepositoryOrReferencePath(Relative)))) return false;
		SourceSizes.Add(Source.Path, static_cast<uint64>(Source.Bytes.Num()));
		const FTCHARToUTF8 SourceName(*Source.Path);
		InstallerHasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(SourceName.Get()), SourceName.Length()));
		const uint8 Separator = 0;
		InstallerHasher.Update(MakeArrayView(&Separator, 1));
		InstallerHasher.Update(Source.Bytes);
		for (const FWoTRequiredFact& Fact : Loaded.GetValue().Facts)
		{
			if (Fact.Domain == EWoTFactDomain::Texture
				&& Fact.SourcePath.Equals(Source.Path, ESearchCase::CaseSensitive))
			{
				TextureContract.Facts.Add(Fact);
			}
		}
	}
	if (!TestTrue(TEXT("texture-only contract retains a declared output"),
		!TextureContract.Facts.IsEmpty()
			&& !TextureContract.Facts[0].Outputs.IsEmpty())) return false;
	TextureContract.StartupCriticalLogicalIds = {
		TextureContract.Facts[0].Outputs[0].LogicalId};
	FWoTSha256Hasher StartupClosureHasher;
	const FTCHARToUTF8 StartupLogicalId(
		*TextureContract.StartupCriticalLogicalIds[0]);
	const uint64 StartupLogicalIdLength =
		static_cast<uint64>(StartupLogicalId.Length());
	uint8 StartupLogicalIdLengthPrefix[8];
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(StartupLogicalIdLengthPrefix); ++Index)
	{
		StartupLogicalIdLengthPrefix[Index] = static_cast<uint8>(
			(StartupLogicalIdLength >> (Index * 8)) & 0xff);
	}
	StartupClosureHasher.Update(MakeArrayView(StartupLogicalIdLengthPrefix));
	StartupClosureHasher.Update(TConstArrayView<uint8>(
		reinterpret_cast<const uint8*>(StartupLogicalId.Get()),
		StartupLogicalId.Length()));
	TextureContract.StartupCriticalLogicalIdsSha256 = StartupClosureHasher.Finish();
	TWoTBootstrapResult<TArray<uint8>> Descriptor =
		LoadCacheProductionDescriptorForTesting();
	if (!TestFalse(TEXT("production descriptor loads for texture coordinator"),
		Descriptor.IsError()))
	{
		if (Descriptor.IsError()) AddError(Descriptor.GetError().ToString());
		return false;
	}
	FWoTTextureSoundSpecWriter CoordinatedWriter;
	FWoTTextureSoundSpecProgress CoordinatedProgress;
	TArray<FWoTCacheImporterForTesting> Importers;
	FWoTCacheImporterForTesting& TextureImporter = Importers.AddDefaulted_GetRef();
	TextureImporter.Stage = TEXT("texture-sound");
	TextureImporter.Importers = {TEXT("texture-v2")};
	TextureImporter.Run = [&TextureSources, &TextureContract](
		IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
	{
		using FObjectsResult = TWoTBootstrapResult<TArray<FWoTCacheBlob>>;
		TArray<FWoTCacheBlob> Objects;
		for (const FTextureSource& Source : TextureSources)
		{
			TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
				ImportTexturePackageBytesForTesting(Source.Bytes, Source.Path,
					TextureContract, Writer, Progress);
			if (Imported.IsError())
				return FObjectsResult::Error(MoveTemp(Imported).GetError());
			Objects.Append(MoveTemp(Imported).GetValue());
		}
		return FObjectsResult::Success(MoveTemp(Objects));
	};
	TWoTBootstrapResult<FWoTCacheBuildResult> Coordinated =
		BuildRetailCacheForTesting(InstallerHasher.Finish(), TextureContract,
			CoordinatedWriter, CoordinatedProgress,
			Descriptor.GetValue(), SourceSizes, Importers);
	if (!TestFalse(TEXT("BuildRetailCacheForTesting routes both real texture-v2 packages"),
		Coordinated.IsError()))
	{
		if (Coordinated.IsError()) AddError(Coordinated.GetError().ToString());
		return false;
	}
	TestEqual(TEXT("coordinated manifest covers every texture-v2 write"),
		Coordinated.GetValue().Manifest.Blobs.Num(), CoordinatedWriter.Objects.Num());

	TMap<FString, FAnimationRow> CoordinatedRows;
	for (const FWoTTextureSoundSpecWritten& Object : CoordinatedWriter.Objects)
	{
		if (Object.Schema != TEXT("wot-texture-v3")) continue;
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
			WoTTextureSoundSpecParseHeader(Object.Bytes);
		if (!TestFalse(Object.LogicalId + TEXT(" coordinated header parses"),
			Header.IsError())) continue;
		const FWoTCanonicalJsonValue* Source =
			Header.GetValue().FindMember(TEXT("source"));
		const FWoTCanonicalJsonValue* Export =
			Source ? Source->FindMember(TEXT("export")) : nullptr;
		const FWoTCanonicalJsonValue* Next =
			Header.GetValue().FindMember(TEXT("anim_next"));
		if (Export == nullptr || Next == nullptr) continue;
		FAnimationRow& Row = CoordinatedRows.Add(Export->GetString());
		Row.Next = Next->GetString();
	}
	TSet<FString> CoordinatedTargets;
	for (const TPair<FString, FAnimationRow>& Pair : CoordinatedRows)
		if (!Pair.Value.Next.IsEmpty())
			CoordinatedTargets.Add(Pair.Value.Next.ToLower());
	uint32 CoordinatedChains = 0;
	uint32 CoordinatedFrames = 0;
	for (const TPair<FString, FAnimationRow>& Pair : CoordinatedRows)
	{
		if (Pair.Value.Next.IsEmpty()
			|| CoordinatedTargets.Contains(Pair.Key.ToLower())) continue;
		++CoordinatedChains;
		TSet<FString> Seen;
		FString Current = Pair.Key;
		for (;;)
		{
			const FAnimationRow* Row = CoordinatedRows.Find(Current);
			if (!TestNotNull(Current + TEXT(" coordinated animation frame"), Row)
				|| !TestFalse(Current + TEXT(" coordinated chain is acyclic"),
					Seen.Contains(Current.ToLower()))) break;
			Seen.Add(Current.ToLower());
			++CoordinatedFrames;
			if (Row->Next.IsEmpty()) break;
			Current = Row->Next;
		}
	}
	TestEqual(TEXT("coordinated production animation chain total"),
		CoordinatedChains, 29u);
	TestEqual(TEXT("coordinated production animation frame total"),
		CoordinatedFrames, 313u);
	AddInfo(FString::Printf(
		TEXT("BuildRetailCacheForTesting texture-v2 census chains=%u frames=%u"),
		CoordinatedChains, CoordinatedFrames));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureSoundPublicationReverifySpec,
	"WoT.RetailReader.TextureSound.PerPublicationSnapshotReverification",
	WoTTextureSoundSpecFlags)

bool FWoTTextureSoundPublicationReverifySpec::RunTest(const FString&)
{
	constexpr const TCHAR* FixtureName = TEXT("cache-v3-importer-input.bin");
	FWoTTextureSoundSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened =
		WoTTextureSoundSpecOpen(Root, FixtureName);
	if (!TestFalse(TEXT("shared texture package opens"), Opened.IsError()))
	{
		return false;
	}
	FWoTRetailContract Contract = WoTTextureSoundSpecValidContract();
	Contract.Facts.SetNum(2);
	TArray<uint8> Mutated;
	if (!TestTrue(TEXT("shared texture installer bytes load"),
		WoTTextureSoundSpecLoadFixture(FixtureName, Mutated))
		|| Mutated.IsEmpty())
	{
		return false;
	}
	// Change the length as well as the bytes (same NTFS ChangeTime race as
	// MeshAnimation.PerPublicationSnapshotReverification, fixed in 5921249f6):
	// size is part of the held identity, so detection is deterministic.
	Mutated.Add(0x5a);
	bool bMutationAttempted = false;
	bool bMutationSucceeded = false;
	FWoTTextureSoundSpecWriter Writer;
	Writer.AfterWrite = [&]()
	{
		if (!bMutationAttempted && Writer.Objects.Num() == 1)
		{
			bMutationAttempted = true;
			bMutationSucceeded = WoTNativeHandleTestFactory::WriteFile(
				Root.Path / FixtureName, Mutated);
		}
	};
	FWoTTextureSoundSpecProgress Progress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
		ImportTextureSound(Opened.GetValue(), Contract, Writer, Progress);
	TestTrue(TEXT("the held installer is mutated after the first publication"),
		bMutationAttempted && bMutationSucceeded);
	TestTrue(TEXT("the next shared-package fact fails closed"), Imported.IsError());
	TestEqual(TEXT("the second object is never written"), Writer.Objects.Num(), 1);
	TestEqual(TEXT("per-publication verification performs no second decode"),
		Opened.GetValue().EntryDecodeCountForTesting(), 1ull);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureSoundExportIdentitySpec,
	"WoT.RetailReader.TextureSound.ExportIdentity", WoTTextureSoundSpecFlags)

bool FWoTTextureSoundExportIdentitySpec::RunTest(const FString&)
{
	{
		FWoTTextureSoundSpecRoot GroupedRoot;
		TWoTBootstrapResult<FWoTRetailPackage> GroupedOpened = WoTTextureSoundSpecOpen(
			GroupedRoot, TEXT("cache-v3-importer-input.bin"));
		if (!TestFalse(TEXT("grouped duplicate fixture package opens"),
			GroupedOpened.IsError()))
		{
			return false;
		}
		FWoTRetailContract GroupedContract;
		WoTTextureSoundSpecAddFact(GroupedContract, EWoTFactDomain::Texture,
			TEXT("synthetic.texture.ambiguous"), TEXT("Synthetic/texture.u"),
			TEXT("texture.Stone"), TEXT("texture-v2"),
			TEXT("cache/texture/ambiguous"), TEXT("wot-texture-v3"));
		FWoTTextureSoundSpecWriter GroupedWriter;
		FWoTTextureSoundSpecProgress GroupedProgress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> GroupedImported = ImportTextureSound(
			GroupedOpened.GetValue(), GroupedContract, GroupedWriter, GroupedProgress);
		if (TestTrue(TEXT("a duplicate bare export identity is rejected"),
			GroupedImported.IsError()))
		{
			TestEqual(TEXT("bare ambiguity has the duplicate-field error code"),
				GroupedImported.GetError().Code, EWoTBootstrapError::DuplicateField);
		}
		TestEqual(TEXT("an ambiguous bare export publishes no object"),
			GroupedWriter.Objects.Num(), 0);
	}

	{
		FWoTTextureSoundSpecRoot UniqueRoot;
		TWoTBootstrapResult<FWoTRetailPackage> UniqueOpened = WoTTextureSoundSpecOpen(
			UniqueRoot, TEXT("cache-v3-export-case-unique.bin"));
		if (!TestFalse(TEXT("case-unique fixture package opens"), UniqueOpened.IsError()))
		{
			return false;
		}
		FWoTRetailContract UniqueContract;
		WoTTextureSoundSpecAddFact(UniqueContract, EWoTFactDomain::Texture,
			TEXT("synthetic.texture.case-unique"), TEXT("Synthetic/CaseUnique.u"),
			TEXT("cAsEuNiQuE.cAsEsToNe"), TEXT("texture-v2"),
			TEXT("cache/texture/case-unique"), TEXT("wot-texture-v3"));
		FWoTTextureSoundSpecWriter UniqueWriter;
		FWoTTextureSoundSpecProgress UniqueProgress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> UniqueImported = ImportTextureSound(
			UniqueOpened.GetValue(), UniqueContract, UniqueWriter, UniqueProgress);
		if (!TestFalse(TEXT("a uniquely case-varied identity resolves"),
			UniqueImported.IsError()))
		{
			if (UniqueImported.IsError()) AddError(UniqueImported.GetError().ToString());
		}
		TestEqual(TEXT("the case-varied identity publishes one object"),
			UniqueWriter.Objects.Num(), 1);
	}

	{
		FWoTTextureSoundSpecRoot CollisionRoot;
		TWoTBootstrapResult<FWoTRetailPackage> CollisionOpened = WoTTextureSoundSpecOpen(
			CollisionRoot, TEXT("cache-v3-export-case-collision.bin"));
		if (!TestFalse(TEXT("case-collision fixture package opens"),
			CollisionOpened.IsError()))
		{
			return false;
		}
		FWoTRetailContract CollisionContract;
		WoTTextureSoundSpecAddFact(CollisionContract, EWoTFactDomain::Texture,
			TEXT("synthetic.texture.case-collision"), TEXT("Synthetic/CaseCollision.u"),
			TEXT("casecollision.STONE"), TEXT("texture-v2"),
			TEXT("cache/texture/case-collision"), TEXT("wot-texture-v3"));
		FWoTTextureSoundSpecWriter CollisionWriter;
		FWoTTextureSoundSpecProgress CollisionProgress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> CollisionImported = ImportTextureSound(
			CollisionOpened.GetValue(), CollisionContract, CollisionWriter,
			CollisionProgress);
		if (TestTrue(TEXT("case-only colliding exports are ambiguous"),
			CollisionImported.IsError()))
		{
			TestEqual(TEXT("case collision has the duplicate-field error code"),
				CollisionImported.GetError().Code, EWoTBootstrapError::DuplicateField);
			TestTrue(TEXT("ambiguity names the upper-case-spelled candidate"),
				CollisionImported.GetError().Message.Contains(
					TEXT("CaseCollision.Stone"), ESearchCase::CaseSensitive));
			TestTrue(TEXT("ambiguity names the lower-case-spelled candidate"),
				CollisionImported.GetError().Message.Contains(
					TEXT("CaseCollision.stone"), ESearchCase::CaseSensitive));
		}
		TestEqual(TEXT("a case collision publishes no object"),
			CollisionWriter.Objects.Num(), 0);
	}

	{
		FWoTTextureSoundSpecRoot DottedRoot;
		TWoTBootstrapResult<FWoTRetailPackage> DottedOpened = WoTTextureSoundSpecOpen(
			DottedRoot, TEXT("cache-v3-export-dotted-alias.bin"));
		if (!TestFalse(TEXT("dotted-alias fixture package opens"), DottedOpened.IsError()))
		{
			return false;
		}
		FWoTRetailContract DottedContract;
		WoTTextureSoundSpecAddFact(DottedContract, EWoTFactDomain::Texture,
			TEXT("synthetic.texture.dotted-alias"), TEXT("Synthetic/DottedAlias.u"),
			TEXT("dottedalias.Surface.Stone"), TEXT("texture-v2"),
			TEXT("cache/texture/dotted-alias"), TEXT("wot-texture-v3"));
		FWoTTextureSoundSpecWriter DottedWriter;
		FWoTTextureSoundSpecProgress DottedProgress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> DottedImported = ImportTextureSound(
			DottedOpened.GetValue(), DottedContract, DottedWriter, DottedProgress);
		if (TestTrue(TEXT("a dotted serialized name component is rejected"),
			DottedImported.IsError()))
		{
			TestEqual(TEXT("dotted name rejection has a stable field path"),
				DottedImported.GetError().Path,
				FString(TEXT("source.package.name_table[12]")));
		}
		TestEqual(TEXT("a dotted component publishes no object"),
			DottedWriter.Objects.Num(), 0);
	}

	{
		FWoTTextureSoundSpecRoot EmptyRoot;
		TWoTBootstrapResult<FWoTRetailPackage> EmptyOpened = WoTTextureSoundSpecOpen(
			EmptyRoot, TEXT("cache-v3-export-empty-name.bin"));
		if (!TestFalse(TEXT("empty-name fixture package opens"), EmptyOpened.IsError()))
		{
			return false;
		}
		FWoTRetailContract EmptyContract;
		WoTTextureSoundSpecAddFact(EmptyContract, EWoTFactDomain::Texture,
			TEXT("synthetic.texture.empty-name"), TEXT("Synthetic/EmptyName.u"),
			TEXT("emptyname.Any"), TEXT("texture-v2"),
			TEXT("cache/texture/empty-name"), TEXT("wot-texture-v3"));
		FWoTTextureSoundSpecWriter EmptyWriter;
		FWoTTextureSoundSpecProgress EmptyProgress;
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> EmptyImported = ImportTextureSound(
			EmptyOpened.GetValue(), EmptyContract, EmptyWriter, EmptyProgress);
		if (TestTrue(TEXT("an empty serialized name component is rejected"),
			EmptyImported.IsError()))
		{
			TestEqual(TEXT("empty name rejection has a stable field path"),
				EmptyImported.GetError().Path,
				FString(TEXT("source.package.name_table[11]")));
		}
		TestEqual(TEXT("an empty component publishes no object"),
			EmptyWriter.Objects.Num(), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureSoundCanonicalHeaderSpec,
	"WoT.RetailReader.TextureSound.CanonicalHeaders", WoTTextureSoundSpecFlags)

bool FWoTTextureSoundCanonicalHeaderSpec::RunTest(const FString&)
{
	FWoTCanonicalObjectBuilder RejectedBuilder;
	RejectedBuilder.Add(TEXT("zeta"), FWoTCanonicalJsonValue::MakeInteger(1));
	RejectedBuilder.Add(TEXT("alpha"), FWoTCanonicalJsonValue::MakeInteger(2));
	TestFalse(TEXT("out-of-order canonical member marks the builder incomplete"),
		RejectedBuilder.IsComplete());
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Rejected =
		MoveTemp(RejectedBuilder).Build(TEXT("synthetic guard object"));
	TestTrue(TEXT("out-of-order canonical object returns an error"), Rejected.IsError());
	if (Rejected.IsError())
	{
		TestEqual(TEXT("out-of-order rejection uses the canonical error code"),
			Rejected.GetError().Code, EWoTBootstrapError::NonCanonicalJson);
		TestEqual(TEXT("out-of-order rejection identifies the dropped key"),
			Rejected.GetError().Path, FString(TEXT("alpha")));
		TestTrue(TEXT("out-of-order rejection message names the dropped key"),
			Rejected.GetError().Message.Contains(TEXT("'alpha'"),
				ESearchCase::CaseSensitive));
	}

	FWoTTextureSoundSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = WoTTextureSoundSpecOpen(
		Root, TEXT("cache-v3-importer-input.bin"));
	if (!TestFalse(TEXT("typed fixture package opens for header checks"),
		Opened.IsError()))
	{
		return false;
	}
	FWoTTextureSoundSpecWriter Writer;
	FWoTTextureSoundSpecProgress Progress;
	const FWoTRetailContract Contract = WoTTextureSoundSpecValidContract();
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
		ImportTextureSound(Opened.GetValue(), Contract, Writer, Progress);
	if (!TestFalse(TEXT("all typed objects emit for header checks"), Imported.IsError()))
	{
		return false;
	}

	struct FHeaderExpectation
	{
		const TCHAR* LogicalId;
		const TCHAR* Keys;
	};
	const FHeaderExpectation Expectations[] = {
		{TEXT("cache/texture/synthetic-masked"),
			TEXT("anim_next,color,decoded_layout,encoding,fact_id,importer,importer_version,kind,masked,max_frame_rate_bits,min_frame_rate_bits,payload_byte_length,prime_count,source")},
		{TEXT("cache/texture/synthetic-opaque"),
			TEXT("anim_next,color,decoded_layout,encoding,fact_id,importer,importer_version,kind,masked,max_frame_rate_bits,min_frame_rate_bits,payload_byte_length,prime_count,source")},
		{TEXT("cache/sound/synthetic-two-identical-loops"),
			TEXT("audio,decoded_layout,duration,encoding,fact_id,importer,importer_version,kind,loops,payload_byte_length,source")},
		{TEXT("cache/sound/synthetic-two-distinct-loops"),
			TEXT("audio,decoded_layout,duration,encoding,fact_id,importer,importer_version,kind,loops,payload_byte_length,source")},
		{TEXT("cache/sound/synthetic-pcm8-stereo"),
			TEXT("audio,decoded_layout,duration,encoding,fact_id,importer,importer_version,kind,loops,payload_byte_length,source")},
	};
	for (const FHeaderExpectation& Expectation : Expectations)
	{
		const FWoTTextureSoundSpecWritten* Written = Writer.Find(Expectation.LogicalId);
		if (!TestTrue(FString::Printf(TEXT("%s object exists for header checks"),
			Expectation.LogicalId), Written != nullptr))
		{
			continue;
		}
		TWoTBootstrapResult<FWoTCanonicalJsonValue> Header =
			WoTTextureSoundSpecParseHeader(Written->Bytes);
		if (!TestFalse(FString::Printf(TEXT("%s canonical header parses"),
			Expectation.LogicalId), Header.IsError()))
		{
			continue;
		}
		TArray<FString> ExpectedKeys;
		FString(Expectation.Keys).ParseIntoArray(ExpectedKeys, TEXT(","), true);
		TestEqual(FString::Printf(TEXT("%s header has the exact member count"),
			Expectation.LogicalId), Header.GetValue().Num(), ExpectedKeys.Num());
		for (const FString& ExpectedKey : ExpectedKeys)
		{
			TestTrue(FString::Printf(TEXT("%s header contains member %s"),
				Expectation.LogicalId, *ExpectedKey),
				Header.GetValue().FindMember(ExpectedKey) != nullptr);
		}
		if (FStringView(Expectation.LogicalId)
			== TEXTVIEW("cache/texture/synthetic-masked"))
		{
			TestEqual(TEXT("typed AnimNext canonical identity"),
				Header.GetValue().FindMember(TEXT("anim_next"))->GetString(),
				FString(TEXT("texture.SurfaceFloor.Stone")));
			TestEqual(TEXT("typed MinFrameRate bits"),
				Header.GetValue().FindMember(TEXT("min_frame_rate_bits"))->GetInteger(),
				static_cast<int64>(0x40000000u));
			TestEqual(TEXT("typed MaxFrameRate bits"),
				Header.GetValue().FindMember(TEXT("max_frame_rate_bits"))->GetInteger(),
				static_cast<int64>(0x40e00000u));
			TestEqual(TEXT("typed PrimeCount"),
				Header.GetValue().FindMember(TEXT("prime_count"))->GetInteger(),
				static_cast<int64>(3));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureSoundRejectSpec,
	"WoT.RetailReader.TextureSound.RejectionMatrix", WoTTextureSoundSpecFlags)

bool FWoTTextureSoundRejectSpec::RunTest(const FString&)
{
	struct FCase
	{
		const TCHAR* Fixture;
		EWoTFactDomain Domain;
	};
	const FCase Cases[] = {
		{TEXT("cache-v3-texture-invalid-dimension.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-zero-dimension.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-invalid-format.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-invalid-palette.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-invalid-bits.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-invalid-successor.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-invalid-lazy-end.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-bool-anim-next.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-bool-min-frame-rate.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-bool-max-frame-rate.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-texture-bool-prime-count.bin"), EWoTFactDomain::Texture},
		{TEXT("cache-v3-sound-invalid-rate.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-invalid-channels.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-invalid-block-align.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-invalid-byte-rate.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-invalid-bit-depth.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-invalid-format-tag.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-invalid-loop.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-over-loop-cap.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-music-class-rejected.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-bomb-size.bin"), EWoTFactDomain::Sound},
		{TEXT("cache-v3-sound-hash-mismatch.bin"), EWoTFactDomain::Sound},
	};
	for (const FCase& Case : Cases)
	{
		FWoTTextureSoundSpecRoot Root;
		TWoTBootstrapResult<FWoTRetailPackage> Opened =
			WoTTextureSoundSpecOpen(Root, Case.Fixture);
		if (!TestFalse(FString::Printf(TEXT("%s package index opens"), Case.Fixture),
			Opened.IsError()))
		{
			if (Opened.IsError()) AddError(Opened.GetError().ToString());
			continue;
		}
		FWoTTextureSoundSpecWriter Writer;
		FWoTTextureSoundSpecProgress Progress;
		FWoTRetailContract Contract = WoTTextureSoundSpecRejectContract(Case.Domain);
		TWoTBootstrapResult<TArray<FWoTCacheBlob>> Imported =
			ImportTextureSound(Opened.GetValue(), Contract, Writer, Progress);
		TestTrue(FString::Printf(TEXT("%s is rejected"), Case.Fixture), Imported.IsError());
		TestEqual(FString::Printf(TEXT("%s publishes no object"), Case.Fixture),
			Writer.Objects.Num(), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTTextureSoundAtomicSpec,
	"WoT.RetailReader.TextureSound.InterruptionAndBounds", WoTTextureSoundSpecFlags)

bool FWoTTextureSoundAtomicSpec::RunTest(const FString&)
{
	FWoTTextureSoundSpecRoot Root;
	TWoTBootstrapResult<FWoTRetailPackage> Opened = WoTTextureSoundSpecOpen(
		Root, TEXT("cache-v3-importer-input.bin"));
	if (!TestFalse(TEXT("typed fixture package opens"), Opened.IsError())) return false;

	FWoTRetailContract Contract = WoTTextureSoundSpecValidContract();
	FWoTTextureSoundSpecWriter InterruptedWriter;
	FWoTTextureSoundSpecProgress InterruptedProgress;
	InterruptedProgress.CancelAtCompleted = 1;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Interrupted = ImportTextureSound(
		Opened.GetValue(), Contract, InterruptedWriter, InterruptedProgress);
	TestTrue(TEXT("interruption prevents a completed manifest result"), Interrupted.IsError());
	TestEqual(TEXT("only one complete atomic object preceded interruption"),
		InterruptedWriter.Objects.Num(), 1);
	TArray<uint8> ExpectedFirst;
	WoTTextureSoundSpecLoadFixture(TEXT("cache-v3-texture-masked.bin"), ExpectedFirst);
	TestTrue(TEXT("the pre-interruption object is complete, never partial"),
		InterruptedWriter.Objects.Num() == 1
		&& InterruptedWriter.Objects[0].Bytes == ExpectedFirst);

	FWoTRetailContract Small = WoTTextureSoundSpecValidContract();
	Small.Facts.SetNum(1);
	Small.Facts[0].Outputs[0].MaxDecodedSize = 1;
	FWoTTextureSoundSpecWriter SmallWriter;
	FWoTTextureSoundSpecProgress SmallProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Oversized = ImportTextureSound(
		Opened.GetValue(), Small, SmallWriter, SmallProgress);
	TestTrue(TEXT("contract maximum rejects before publication"), Oversized.IsError());
	TestEqual(TEXT("oversized object never reaches the writer"), SmallWriter.Objects.Num(), 0);

	FWoTRetailContract One = WoTTextureSoundSpecValidContract();
	One.Facts.SetNum(1);
	FWoTTextureSoundSpecWriter RefusingWriter;
	RefusingWriter.bRejectNext = true;
	FWoTTextureSoundSpecProgress RefusingProgress;
	TWoTBootstrapResult<TArray<FWoTCacheBlob>> Refused = ImportTextureSound(
		Opened.GetValue(), One, RefusingWriter, RefusingProgress);
	TestTrue(TEXT("atomic writer refusal aborts the import"), Refused.IsError());
	TestEqual(TEXT("refused write leaves no partial object"), RefusingWriter.Objects.Num(), 0);
	return !HasAnyErrors();
}

#endif  // WITH_AUTOMATION_TESTS
