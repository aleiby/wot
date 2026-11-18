#include "WoTUpdaterCore/ReleaseStaging.h"
#include "WoTBootstrapCore/Announcement.h"
// Socket-free production-worker specifications. Every helper is spec-prefixed
// because WoT test sources share unity translation units.
#if WITH_AUTOMATION_TESTS

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/Event.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#if !PLATFORM_WINDOWS
#include <errno.h>
#include <string.h>
#else
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
// <windows.h> maps FormatMessage to FormatMessageW. In a unity build that
// macro leaks into later specs and renames AWoTGameMode::FormatMessage
// (LNK2019 on MSVC only; Clang never sees it).
#undef FormatMessage
#endif
#include "WoTBootstrapCore/Ed25519.h"
#include "WoTBootstrapCore/Generated/BootstrapTrust.generated.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/InstalledTree.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ReleaseManifest.h"
#include "WoTUpdaterCore/UpdateFailureNotice.h"
#include "WoTUpdaterCore/UpdateState.h"
#include "WoTUpdaterCore/UpdaterPaths.h"
#include "WoTRetailReader/CacheManifest.h"
#include "WoTRetailReader/FrontEndDescriptorSidecar.h"
#include "WoTRetailReader/Package.h"
#include "WoTRetailReader/ProductionContract.h"

#include "../Payload/WoTCacheStore.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTVerifiedBlobReader.h"
#include "../Updater/WoTGameBootstrapWorker.h"
#include "../Updater/WoTRememberedInstaller.h"
#include "../WoTGameInstance.h"
#include "../WoTPreferencesSubsystem.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTBootstrapSubsystem.h"
#include "../WoTBootstrapViewModel.h"
#include "WoTEarlyUI.h"
#include "WoTLaunchEngineHooks.h"
#include "WoTTestWorld.h"
#include "WoTWorldMaterializerTestData.h"
#include "WoTFrontEndTestData.h"
#include "WoTInstallerReceiverTestSupport.h"
#include "WoTBootstrapCore/ProfileSession.h"

#include "../WoTPayloadSubsystem.h"
#include "../WoTSessionSubsystem.h"
#include "../World/WoTWorldMaterializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

#if PLATFORM_MAC
extern "C" int link(const char* ExistingPath, const char* LinkPath);
#endif

extern "C"
{
struct evp_pkey_st;
struct evp_md_ctx_st;
evp_pkey_st* EVP_PKEY_new_raw_private_key(
	int Type, void* Engine, const unsigned char* Key, size_t KeyLength);
int EVP_PKEY_get_raw_public_key(
	const evp_pkey_st* Key, unsigned char* Out, size_t* OutLength);
void EVP_PKEY_free(evp_pkey_st* Key);
evp_md_ctx_st* EVP_MD_CTX_new();
void EVP_MD_CTX_free(evp_md_ctx_st* Context);
int EVP_DigestSignInit(evp_md_ctx_st* Context, void** KeyContext,
	const void* Digest, void* Engine, evp_pkey_st* Key);
int EVP_DigestSign(evp_md_ctx_st* Context, unsigned char* Signature,
	size_t* SignatureLength, const unsigned char* Message,
	size_t MessageLength);
}

namespace
{
constexpr EAutomationTestFlags WoTGameBootstrapWorkerSpecFlags =
	EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ProductFilter;
constexpr TCHAR WoTGameBootstrapWorkerSpecAnnouncementUrl[] =
	TEXT("https://github.com/aleiby/wot/releases/download/worker/announcement.json");
// The URL SpecBuildRelease authors into the signed manifest's archive ref.
constexpr TCHAR WoTGameBootstrapWorkerSpecArchiveUrl[] =
	TEXT("https://github.com/aleiby/wot/releases/download/worker/release.zip");
constexpr TCHAR WoTGameBootstrapWorkerSpecReleaseKeyId[] =
	TEXT("worker-test-release-a");
// Every wait below is for an observable condition (an event, a counter, a
// phase). This deadline only bounds a genuine hang: a loaded machine can be
// slow, but a passing condition never waits it out.
constexpr double WoTGameBootstrapWorkerSpecHangSeconds = 60.0;

FString WoTGameBootstrapWorkerSpecCanonicalOwnedRoot(const FString& Root)
{
	if (IFileManager::Get().DirectoryExists(*Root))
	{
		TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Opened =
			OpenCanonicalNativeDirectoryNoFollow(Root);
		return Opened.IsError() ? FString()
			: MoveTemp(Opened).GetValue().CanonicalPath;
	}
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Parent =
		OpenCanonicalNativeDirectoryFollowing(FPaths::GetPath(Root));
	return Parent.IsError() ? FString()
		: FPaths::Combine(MoveTemp(Parent).GetValue().CanonicalPath,
			FPaths::GetCleanFilename(Root));
}

#if PLATFORM_MAC
constexpr TCHAR WoTGameBootstrapWorkerSpecPlatform[] = TEXT("Mac");
constexpr TCHAR WoTGameBootstrapWorkerSpecRootLeaf[] = TEXT("WoT.app");
constexpr TCHAR WoTGameBootstrapWorkerSpecMainRelative[] =
	TEXT("Contents/MacOS/WoT");
constexpr TCHAR WoTGameBootstrapWorkerSpecCompanionRelative[] =
	TEXT("Contents/MacOS/Companion.bin");
constexpr TCHAR WoTGameBootstrapWorkerSpecIdentityCompanionRelative[] =
	TEXT("Contents/MacOS/Companion.bin");
constexpr TCHAR WoTGameBootstrapWorkerSpecStampRelative[] =
	TEXT("Contents/UE/WoT/Build/WoTRelease/release-identity.json");
constexpr uint32 WoTGameBootstrapWorkerSpecExecutableMode = 0755u;
#elif PLATFORM_WINDOWS
constexpr TCHAR WoTGameBootstrapWorkerSpecPlatform[] = TEXT("Win64");
constexpr TCHAR WoTGameBootstrapWorkerSpecRootLeaf[] =
	TEXT("Game");
constexpr TCHAR WoTGameBootstrapWorkerSpecMainRelative[] = TEXT("WoT.exe");
constexpr TCHAR WoTGameBootstrapWorkerSpecCompanionRelative[] =
	TEXT("Companion.bin");
constexpr TCHAR WoTGameBootstrapWorkerSpecIdentityCompanionRelative[] =
	TEXT("InternalTools/Companion.bin");
constexpr TCHAR WoTGameBootstrapWorkerSpecStampRelative[] =
	TEXT("WoT/Build/WoTRelease/release-identity.json");
constexpr uint32 WoTGameBootstrapWorkerSpecExecutableMode = 0644u;
#else
#error "game bootstrap worker specs run only on Win64 and Mac"
#endif

bool WoTGameBootstrapWorkerSpecCreateHardLink(
	const FString& LinkPath, const FString& ExistingPath)
{
#if PLATFORM_WINDOWS
	return ::CreateHardLinkW(*LinkPath, *ExistingPath, nullptr) != 0;
#else
	// task5-mac: exercise the same single-link companion refusal with link(2).
	const FTCHARToUTF8 LinkUtf8(*LinkPath);
	const FTCHARToUTF8 ExistingUtf8(*ExistingPath);
	return link(ExistingUtf8.Get(), LinkUtf8.Get()) == 0;
#endif
}

TArray<uint8> WoTGameBootstrapWorkerSpecAscii(FStringView Text)
{
	const FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
	TArray<uint8> Bytes;
	Bytes.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

TArray<uint8> WoTGameBootstrapWorkerSpecInstallerBytes()
{
	return WoTGameBootstrapWorkerSpecAscii(TEXT("abcdefgh"));
}

TArray<uint8> WoTGameBootstrapWorkerSpecBadInstallerBytes()
{
	return WoTGameBootstrapWorkerSpecAscii(TEXT("ABCDEFGH"));
}

FWoTInstallerAcquisitionPins WoTGameBootstrapWorkerSpecInstallerPins()
{
	FWoTInstallerAcquisitionPins Pins;
	Pins.Name = TEXT("fixture-installer.exe");
	const TArray<uint8> Bytes =
		WoTGameBootstrapWorkerSpecInstallerBytes();
	Pins.Size = static_cast<uint64>(Bytes.Num());
	Pins.Sha256 = Sha256(Bytes);
	return Pins;
}

bool WoTGameBootstrapWorkerSpecLoadInnoInstaller(TArray<uint8>& Out)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::GetPath(
		FString(ANSI_TO_TCHAR(__FILE__)))
		/ TEXT("../../../Tools/dist/fixtures/innoextract-5.5.6-standard.bin"));
	return FFileHelper::LoadFileToArray(Out, *Path);
}

FWoTInstallerAcquisitionPins WoTGameBootstrapWorkerSpecInnoPins(
	TConstArrayView<uint8> Bytes)
{
	FWoTInstallerAcquisitionPins Pins;
	Pins.Name = WoTGameBootstrapWorkerSpecInstallerPins().Name;
	Pins.Size = static_cast<uint64>(Bytes.Num());
	Pins.Sha256 = Sha256(Bytes);
	return Pins;
}

FWoTRetailContract WoTGameBootstrapWorkerSpecContentContract()
{
	FWoTRetailContract Contract;
	Contract.Format = TEXT("wot-cache-contract-v3");
	Contract.ContractVersion = 3;
	Contract.ContractId = TEXT("worker-content-operation");
	Contract.KeyId = TEXT("worker-content-key");
	Contract.ReleaseSequence = 6;
	Contract.AuditSet.Add({TEXT("UWoTPayloadSubsystem"),
		TEXT("audit.texture-payload-v1")});
	FWoTRequiredFact& Fact = Contract.Facts.AddDefaulted_GetRef();
	Fact.Domain = EWoTFactDomain::Texture;
	Fact.FactId = TEXT("worker.texture");
	Fact.SourcePath = TEXT("Data/Stored.txt");
	Fact.SourceExport = TEXT("Synthetic.Texture");
	Fact.Importer = TEXT("texture-v2");
	Fact.RuntimeConsumer = TEXT("UWoTPayloadSubsystem");
	Fact.AuditRule = TEXT("audit.texture-payload-v1");
	FWoTRequiredFactOutput& Output = Fact.Outputs.AddDefaulted_GetRef();
	Output.LogicalId = TEXT("cache/texture/worker");
	Output.Schema = TEXT("wot-texture-v3");
	Output.MaxDecodedSize = 268435456ull;
	Contract.StartupCriticalLogicalIds = {Output.LogicalId};
	TWoTBootstrapResult<FWoTSha256> CriticalDigest = FWoTSha256::FromHex(
		TEXT("b360982c94bafa0b1b929a6d921b1ef81f070eb2f133602afcbf2bf0f2ac5b2d"));
	if (!CriticalDigest.IsError())
	{
		Contract.StartupCriticalLogicalIdsSha256 = CriticalDigest.GetValue();
	}
	return Contract;
}

struct FWoTGameBootstrapWorkerSpecContractScope
{
	explicit FWoTGameBootstrapWorkerSpecContractScope(
		const FWoTRetailContract& Contract)
	{
		WoTSetProductionRetailContractForTesting(&Contract);
	}
	~FWoTGameBootstrapWorkerSpecContractScope()
	{
		WoTSetProductionRetailContractForTesting(nullptr);
	}
};

TWoTBootstrapResult<FWoTCacheBuildResult>
WoTGameBootstrapWorkerSpecContentBuild(FWoTRetailPackage& Package,
	const FWoTRetailContract& Contract, IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	using FResult = TWoTBootstrapResult<FWoTCacheBuildResult>;
	const FWoTRequiredFact& Fact = Contract.Facts[0];
	const FWoTRequiredFactOutput& Output = Fact.Outputs[0];
	FWoTBootstrapStatus Reported =
		Progress.ReportProgress(TEXT("texture-sound"), 0, 1);
	if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
	const TArray<uint8> Bytes =
		WoTGameBootstrapWorkerSpecAscii(TEXT("worker-cache-object"));
	FWoTBootstrapStatus Written = Writer.WriteObject(
		Output.LogicalId, Output.Schema, Bytes, Sha256(Bytes), Output.MaxDecodedSize);
	if (Written.IsError()) return FResult::Error(MoveTemp(Written).GetError());
	Reported = Progress.ReportProgress(TEXT("texture-sound"), 1, 1);
	if (Reported.IsError()) return FResult::Error(MoveTemp(Reported).GetError());
	TWoTBootstrapResult<FWoTSha256> ContentLabel = GetCacheContentLabel();
	if (ContentLabel.IsError()) return FResult::Error(MoveTemp(ContentLabel).GetError());
	FWoTCacheBuildResult Build;
	Build.Manifest.bHasBuildIdentity = true;
	Build.Manifest.InstallerSha256 = Package.CachedInputSha256();
	Build.Manifest.ContractSha256 = ComputeRetailContractIdentity(Contract);
	Build.Manifest.ProductionDescriptorSha256 = ContentLabel.GetValue();
	FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
	Blob.LogicalId = Output.LogicalId;
	Blob.Schema = Output.Schema;
	Blob.Size = static_cast<uint64>(Bytes.Num());
	Blob.Sha256 = Sha256(Bytes);
	Blob.Satisfies.Add(FWoTRequiredFactId{Fact.FactId});
	TWoTBootstrapResult<TArray<uint8>> Manifest =
		WriteCanonicalCacheManifest(Build.Manifest);
	if (Manifest.IsError()) return FResult::Error(MoveTemp(Manifest).GetError());
	Build.CanonicalManifest = MoveTemp(Manifest).GetValue();
	Build.ManifestSha256 = Sha256(Build.CanonicalManifest);
	Build.Progress = FWoTCacheProgressSnapshot(TEXT("cache-builder"), 1, 1);
	return FResult::Success(MoveTemp(Build));
}

FString WoTGameBootstrapWorkerSpecInstallerPath(
	const FString& InstallRoot)
{
	return FPaths::Combine(InstallRoot,
		WoTGameBootstrapWorkerSpecInstallerPins().Name);
}

FString WoTGameBootstrapWorkerSpecCacheRoot(
	const FString& InstallRoot)
{
	return FPaths::Combine(InstallRoot + TEXT("-user"), TEXT("ContentCache"));
}

bool WoTGameBootstrapWorkerSpecSave(
	FStringView Path, TConstArrayView<uint8> Bytes)
{
	TArray<uint8> Owned;
	Owned.Append(Bytes.GetData(), Bytes.Num());
	return FFileHelper::SaveArrayToFile(Owned, *FString(Path));
}

void WoTGameBootstrapWorkerSpecAppend16(
	TArray<uint8>& Out, uint16 Value)
{
	Out.Add(static_cast<uint8>(Value & 0xffu));
	Out.Add(static_cast<uint8>((Value >> 8) & 0xffu));
}

void WoTGameBootstrapWorkerSpecAppend32(
	TArray<uint8>& Out, uint32 Value)
{
	for (int32 Shift = 0; Shift < 32; Shift += 8)
	{
		Out.Add(static_cast<uint8>((Value >> Shift) & 0xffu));
	}
}

enum class EWoTGameBootstrapWorkerSpecZipType : uint8
{
	File,
	Directory,
};

struct FWoTGameBootstrapWorkerSpecZipInput
{
	FString Path;
	TArray<uint8> Bytes;
	EWoTGameBootstrapWorkerSpecZipType Type =
		EWoTGameBootstrapWorkerSpecZipType::File;
	uint32 Mode = 0644u;
};

struct FWoTGameBootstrapWorkerSpecZipCentral
{
	TArray<uint8> Name;
	uint32 Crc = 0;
	uint32 Size = 0;
	uint32 External = 0;
	uint32 LocalOffset = 0;
};

TArray<uint8> WoTGameBootstrapWorkerSpecZip(
	const TArray<FWoTGameBootstrapWorkerSpecZipInput>& Inputs)
{
	TArray<uint8> Archive;
	TArray<FWoTGameBootstrapWorkerSpecZipCentral> Central;
	for (const FWoTGameBootstrapWorkerSpecZipInput& Input : Inputs)
	{
		FWoTGameBootstrapWorkerSpecZipCentral Record;
		FString StoredPath = Input.Path;
		if (Input.Type == EWoTGameBootstrapWorkerSpecZipType::Directory
			&& !StoredPath.EndsWith(TEXT("/")))
		{
			StoredPath += TEXT("/");
		}
		Record.Name = WoTGameBootstrapWorkerSpecAscii(StoredPath);
		Record.Crc = FCrc::MemCrc32(
			Input.Bytes.GetData(), Input.Bytes.Num());
		Record.Size = static_cast<uint32>(Input.Bytes.Num());
		Record.LocalOffset = static_cast<uint32>(Archive.Num());
		Record.External =
			(Input.Type == EWoTGameBootstrapWorkerSpecZipType::Directory
				? (0040000u | Input.Mode)
				: (0100000u | Input.Mode)) << 16;

		WoTGameBootstrapWorkerSpecAppend32(Archive, 0x04034b50u);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 20);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0x0800u);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.Crc);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.Size);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.Size);
		WoTGameBootstrapWorkerSpecAppend16(
			Archive, static_cast<uint16>(Record.Name.Num()));
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		Archive.Append(Record.Name);
		Archive.Append(Input.Bytes);
		Central.Add(MoveTemp(Record));
	}
	const uint32 CentralOffset = static_cast<uint32>(Archive.Num());
	for (const FWoTGameBootstrapWorkerSpecZipCentral& Record : Central)
	{
		WoTGameBootstrapWorkerSpecAppend32(Archive, 0x02014b50u);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0x033fu);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 20);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0x0800u);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.Crc);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.Size);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.Size);
		WoTGameBootstrapWorkerSpecAppend16(
			Archive, static_cast<uint16>(Record.Name.Num()));
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.External);
		WoTGameBootstrapWorkerSpecAppend32(Archive, Record.LocalOffset);
		Archive.Append(Record.Name);
	}
	const uint32 CentralSize =
		static_cast<uint32>(Archive.Num()) - CentralOffset;
	WoTGameBootstrapWorkerSpecAppend32(Archive, 0x06054b50u);
	WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
	WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
	WoTGameBootstrapWorkerSpecAppend16(
		Archive, static_cast<uint16>(Central.Num()));
	WoTGameBootstrapWorkerSpecAppend16(
		Archive, static_cast<uint16>(Central.Num()));
	WoTGameBootstrapWorkerSpecAppend32(Archive, CentralSize);
	WoTGameBootstrapWorkerSpecAppend32(Archive, CentralOffset);
	WoTGameBootstrapWorkerSpecAppend16(Archive, 0);
	return Archive;
}

struct FWoTGameBootstrapWorkerSpecKey final
{
	FWoTEd25519PublicKey Public;
	evp_pkey_st* Private = nullptr;

	~FWoTGameBootstrapWorkerSpecKey()
	{
		if (Private != nullptr)
		{
			EVP_PKEY_free(Private);
		}
	}
	FWoTGameBootstrapWorkerSpecKey() = default;
	FWoTGameBootstrapWorkerSpecKey(
		const FWoTGameBootstrapWorkerSpecKey&) = delete;
	FWoTGameBootstrapWorkerSpecKey& operator=(
		const FWoTGameBootstrapWorkerSpecKey&) = delete;

	bool Initialize(uint8 Start)
	{
		uint8 Seed[32];
		for (int32 Index = 0;
			Index < static_cast<int32>(UE_ARRAY_COUNT(Seed)); ++Index)
		{
			Seed[Index] = static_cast<uint8>(Start + Index);
		}
		Private = EVP_PKEY_new_raw_private_key(
			1087, nullptr, Seed, sizeof(Seed));
		if (Private == nullptr)
		{
			return false;
		}
		size_t Length = FWoTEd25519PublicKey::ByteCount;
		return EVP_PKEY_get_raw_public_key(
			Private, Public.Bytes, &Length) == 1
			&& Length == FWoTEd25519PublicKey::ByteCount;
	}
};

TArray<uint8> WoTGameBootstrapWorkerSpecSign(
	FWoTGameBootstrapWorkerSpecKey& Key,
	EWoTSignatureContext Context, TConstArrayView<uint8> Payload)
{
	const TArray<uint8> Framed = FrameSignatureInput(Context, Payload);
	TArray<uint8> Signature;
	Signature.SetNumZeroed(FWoTEd25519Signature::ByteCount);
	evp_md_ctx_st* Signer = EVP_MD_CTX_new();
	size_t Length = Signature.Num();
	if (Signer == nullptr
		|| EVP_DigestSignInit(
			Signer, nullptr, nullptr, nullptr, Key.Private) != 1
		|| EVP_DigestSign(Signer, Signature.GetData(), &Length,
			Framed.GetData(), Framed.Num()) != 1
		|| Length != FWoTEd25519Signature::ByteCount)
	{
		Signature.Reset();
	}
	if (Signer != nullptr)
	{
		EVP_MD_CTX_free(Signer);
	}
	return Signature;
}

FWoTInstalledTreeEntry WoTGameBootstrapWorkerSpecDirectory(
	const TCHAR* Path, uint32 Mode = 0755u)
{
	FWoTInstalledTreeEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::Directory;
	Entry.Mode = Mode;
	return Entry;
}

FWoTInstalledTreeEntry WoTGameBootstrapWorkerSpecFile(
	const TCHAR* Path, TConstArrayView<uint8> Bytes, uint32 Mode)
{
	FWoTInstalledTreeEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::File;
	Entry.Mode = Mode;
	Entry.Size = static_cast<uint64>(Bytes.Num());
	Entry.Sha256 = Sha256(Bytes);
	return Entry;
}

struct FWoTGameBootstrapWorkerSpecTree
{
	TArray<uint8> Payload;
	TArray<uint8> Main;
	TArray<uint8> Updater;
	TArray<uint8> Stamp;
	TArray<uint8> Padding;
	FString UpdaterRelativePath = WoTGameBootstrapWorkerSpecCompanionRelative;
	FWoTInstalledTreeManifest Inventory;
	TArray<uint8> InventoryBytes;
	FWoTSha256 RootDigest;
};

FWoTGameBootstrapWorkerSpecTree WoTGameBootstrapWorkerSpecBuildTree(
	uint64 Sequence, FStringView MarkerPath, int32 PaddingBytes = 0,
	bool bIncludeIdentity = false)
{
	FWoTGameBootstrapWorkerSpecTree Tree;
	Tree.Padding.SetNumUninitialized(PaddingBytes);
	for (int32 Index = 0; Index < Tree.Padding.Num(); ++Index)
	{
		Tree.Padding[Index] = static_cast<uint8>((Index * 131 + 23) & 0xff);
	}
	Tree.Payload = WoTGameBootstrapWorkerSpecAscii(
		FString::Printf(TEXT("worker-release-%llu\n"), Sequence));
#if PLATFORM_MAC
	FString Marker(MarkerPath);
	Marker.ReplaceInline(TEXT("\""), TEXT("\\\""));
	Tree.Main = WoTGameBootstrapWorkerSpecAscii(FString::Printf(
		TEXT("#!/bin/sh\nprintf 'release-%llu\\n' > \"%s\"\nexit 0\n"),
		Sequence, *Marker));
	Tree.Updater =
		WoTGameBootstrapWorkerSpecAscii(TEXT("#!/bin/sh\nexit 0\n"));
#else
	Tree.Updater = WoTGameBootstrapWorkerSpecAscii(TEXT("synthetic companion bytes"));
	Tree.Main = Tree.Updater;
	if (bIncludeIdentity)
	{
		Tree.UpdaterRelativePath =
			WoTGameBootstrapWorkerSpecIdentityCompanionRelative;
		Tree.Stamp = WoTGameBootstrapWorkerSpecAscii(FString::Printf(
			TEXT("{\"format\":\"wot-release-identity-v1\",\"release_commit\":")
			TEXT("\"worker-test\",\"release_sequence\":%llu,\"version\":1}"),
			Sequence));
	}
#endif
	Tree.Inventory.Format = WoTInstalledTreeV1Format;
	Tree.Inventory.Version = WoTInstalledTreeVersion;
	Tree.Inventory.Platform = WoTGameBootstrapWorkerSpecPlatform;
	Tree.Inventory.Entries.Add(WoTGameBootstrapWorkerSpecFile(
		TEXT("Content.bin"), Tree.Payload, 0644u));
#if PLATFORM_MAC
	Tree.Inventory.Entries.Add(
		WoTGameBootstrapWorkerSpecDirectory(TEXT("Contents")));
	Tree.Inventory.Entries.Add(
		WoTGameBootstrapWorkerSpecDirectory(TEXT("Contents/MacOS")));
#endif
	Tree.Inventory.Entries.Add(WoTGameBootstrapWorkerSpecFile(
		WoTGameBootstrapWorkerSpecMainRelative, Tree.Main,
		WoTGameBootstrapWorkerSpecExecutableMode));
	Tree.Inventory.Entries.Add(WoTGameBootstrapWorkerSpecFile(
		*Tree.UpdaterRelativePath, Tree.Updater,
		WoTGameBootstrapWorkerSpecExecutableMode));
	if (bIncludeIdentity)
	{
		for (const TCHAR* Directory : {
				TEXT("InternalTools"), TEXT("WoT"), TEXT("WoT/Build"),
				TEXT("WoT/Build/WoTRelease")})
		{
			Tree.Inventory.Entries.Add(
				WoTGameBootstrapWorkerSpecDirectory(Directory));
		}
		Tree.Inventory.Entries.Add(WoTGameBootstrapWorkerSpecFile(
			WoTGameBootstrapWorkerSpecStampRelative, Tree.Stamp, 0644u));
	}
	if (!Tree.Padding.IsEmpty())
	{
		Tree.Inventory.Entries.Add(WoTGameBootstrapWorkerSpecFile(
			TEXT("Padding.bin"), Tree.Padding, 0644u));
	}
	Tree.Inventory.Entries.Sort(
		[](const FWoTInstalledTreeEntry& Left,
			const FWoTInstalledTreeEntry& Right)
		{
			return Left.Path.Compare(
				Right.Path, ESearchCase::CaseSensitive) < 0;
		});
	TWoTBootstrapResult<FWoTSha256> Root =
		ComputeInstalledTreeRoot(Tree.Inventory);
	check(!Root.IsError());
	Tree.RootDigest = MoveTemp(Root).GetValue();

	FString Entries;
	for (int32 Index = 0;
		Index < Tree.Inventory.Entries.Num(); ++Index)
	{
		const FWoTInstalledTreeEntry& Entry =
			Tree.Inventory.Entries[Index];
		if (Index > 0)
		{
			Entries += TEXT(",");
		}
		Entries += FString::Printf(
			TEXT("{\"link_target\":\"\",\"mode\":%u,\"path\":\"%s\",")
			TEXT("\"sha256\":\"%s\",\"size\":%llu,\"type\":\"%s\"}"),
			Entry.Mode, *Entry.Path,
			Entry.Type == EWoTTreeEntryType::File
				? *Entry.Sha256.ToHex() : TEXT(""),
			Entry.Size,
			Entry.Type == EWoTTreeEntryType::File
				? TEXT("file") : TEXT("directory"));
	}
	Tree.InventoryBytes = WoTGameBootstrapWorkerSpecAscii(
		FString::Printf(
			TEXT("{\"entries\":[%s],\"format\":\"wot-installed-tree-v1\",")
			TEXT("\"platform\":\"%s\",\"version\":1}"),
			*Entries, WoTGameBootstrapWorkerSpecPlatform));
	return Tree;
}

struct FWoTGameBootstrapWorkerSpecRelease
{
	FWoTGameBootstrapWorkerSpecTree Tree;
	TArray<uint8> Archive;
	TArray<uint8> AnnouncementBytes;
	TArray<uint8> AnnouncementSignature;
	FWoTAnnouncementV1 Announcement;
	FWoTReleaseStagingMetadata Metadata;
};

FWoTGameBootstrapWorkerSpecRelease WoTGameBootstrapWorkerSpecBuildRelease(
	uint64 Sequence, FStringView MarkerPath,
	FWoTGameBootstrapWorkerSpecKey& AnnouncementKey,
	FWoTGameBootstrapWorkerSpecKey& ReleaseKey,
	FWoTGameBootstrapWorkerSpecKey& RootKey,
	uint64 RingEpoch = 1, int32 PaddingBytes = 0,
	bool bIncludeIdentity = false)
{
	FWoTGameBootstrapWorkerSpecRelease Release;
	Release.Tree = WoTGameBootstrapWorkerSpecBuildTree(
		Sequence, MarkerPath, PaddingBytes, bIncludeIdentity);
	const FString Root = WoTGameBootstrapWorkerSpecRootLeaf;
	TArray<FWoTGameBootstrapWorkerSpecZipInput> Inputs;
	Inputs.Add({Root, {},
		EWoTGameBootstrapWorkerSpecZipType::Directory, 0755u});
	for (const FWoTInstalledTreeEntry& Entry
		: Release.Tree.Inventory.Entries)
	{
		TArray<uint8> Bytes;
		if (Entry.Path == TEXT("Content.bin"))
		{
			Bytes = Release.Tree.Payload;
		}
		else if (Entry.Path == WoTGameBootstrapWorkerSpecMainRelative)
		{
			Bytes = Release.Tree.Main;
		}
		else if (Entry.Path == Release.Tree.UpdaterRelativePath)
		{
			Bytes = Release.Tree.Updater;
		}
		else if (Entry.Path == WoTGameBootstrapWorkerSpecStampRelative)
		{
			Bytes = Release.Tree.Stamp;
		}
		else if (Entry.Path == TEXT("Padding.bin"))
		{
			Bytes = Release.Tree.Padding;
		}
		Inputs.Add({Root + TEXT("/") + Entry.Path, MoveTemp(Bytes),
			Entry.Type == EWoTTreeEntryType::Directory
				? EWoTGameBootstrapWorkerSpecZipType::Directory
				: EWoTGameBootstrapWorkerSpecZipType::File,
			Entry.Mode});
	}
	Inputs.Add({Root + TEXT("/installed-tree-v1.json"),
		Release.Tree.InventoryBytes,
		EWoTGameBootstrapWorkerSpecZipType::File, 0644u});
	Release.Archive = WoTGameBootstrapWorkerSpecZip(Inputs);

	Release.Metadata.KeyRingBytes =
		WoTGameBootstrapWorkerSpecAscii(FString::Printf(
			TEXT("{\"epoch\":%llu,\"format\":\"wot-release-key-ring-v1\",")
			TEXT("\"keys\":[{\"first_sequence\":1,\"key_id\":\"%s\",")
			TEXT("\"last_sequence\":1000,\"public_key\":\"%s\"}],")
			TEXT("\"revoked_key_ids\":[]}"),
			static_cast<unsigned long long>(RingEpoch),
			WoTGameBootstrapWorkerSpecReleaseKeyId,
			*ReleaseKey.Public.ToHex()));
	Release.Metadata.KeyRingSignature =
		WoTGameBootstrapWorkerSpecSign(
			RootKey, EWoTSignatureContext::ReleaseKeyRingV1,
			Release.Metadata.KeyRingBytes);

	const FWoTSha256 ArchiveDigest = Sha256(Release.Archive);
	Release.Metadata.ReleaseManifestBytes =
		WoTGameBootstrapWorkerSpecAscii(FString::Printf(
			TEXT("{\"announcement_domain\":\"com.aleiby.wot.release.v1\",")
			TEXT("\"archive\":{\"name\":\"release-%llu.zip\",\"sha256\":\"%s\",")
			TEXT("\"size\":%d,\"url\":\"https://github.com/aleiby/wot/")
			TEXT("releases/download/worker/release.zip\"},")
			TEXT("\"archive_root_id\":\"wot-archive-root-v1\",")
			TEXT("\"build_commit\":\"worker-test\",")
			TEXT("\"cache_contract_id\":\"wot-cache-contract-1\",")
			TEXT("\"cache_contract_version\":3,")
			TEXT("\"format\":\"wot-release-manifest-v2\",")
			TEXT("\"generated_utc\":\"2026-07-28T00:00:00+00:00\",")
			TEXT("\"human_version\":\"worker-%llu\",")
			TEXT("\"installed_tree_root\":{\"name\":\"%s\",\"sha256\":\"%s\",")
			TEXT("\"size\":%d},\"installed_tree_version\":1,\"key_id\":\"%s\",")
			TEXT("\"main_executable\":{\"name\":\"%s\",\"sha256\":\"%s\",")
			TEXT("\"size\":%d},\"manifest_version\":2,")
			TEXT("\"minimum_commit_service_protocol\":1,")
			TEXT("\"minimum_gate_protocol\":1,\"minimum_updater_protocol\":1,")
			TEXT("\"platform\":\"%s\",\"release_sequence\":%llu,")
			TEXT("\"stripped_audit_verdict\":\"pass\",")
			TEXT("\"target_attestation\":{\"configuration\":\"Shipping\",")
			TEXT("\"dedicated_server_startup_available\":false,")
			TEXT("\"launch_module\":\"WoTLaunch\",\"listen_server_present\":true,")
			TEXT("\"release_gate_enabled\":false,\"target_type\":\"Game\"}")
			TEXT("}"),
			Sequence, *ArchiveDigest.ToHex(), Release.Archive.Num(),
			Sequence, WoTGameBootstrapWorkerSpecRootLeaf,
			*Release.Tree.RootDigest.ToHex(),
			Release.Tree.InventoryBytes.Num(),
			WoTGameBootstrapWorkerSpecReleaseKeyId,
			WoTGameBootstrapWorkerSpecMainRelative,
			*Sha256(Release.Tree.Main).ToHex(),
			Release.Tree.Main.Num(),
			WoTGameBootstrapWorkerSpecPlatform, Sequence));
	Release.Metadata.ReleaseManifestSignature =
		WoTGameBootstrapWorkerSpecSign(
			ReleaseKey, EWoTSignatureContext::ReleaseManifestV2,
			Release.Metadata.ReleaseManifestBytes);

	const FWoTSha256 RingDigest =
		Sha256(Release.Metadata.KeyRingBytes);
	const FWoTSha256 ManifestDigest =
		Sha256(Release.Metadata.ReleaseManifestBytes);
	Release.AnnouncementBytes =
		WoTGameBootstrapWorkerSpecAscii(FString::Printf(
			TEXT("{\"announcement_domain\":\"com.aleiby.wot.release.v1\",")
			TEXT("\"format\":\"wot-announcement-v1\",\"key_id\":\"%s\",")
			TEXT("\"key_ring\":{\"sha256\":\"%s\",\"size\":%d,")
			TEXT("\"url\":\"https://github.com/aleiby/wot/releases/download/")
			TEXT("worker/key-ring.json\"},\"key_ring_epoch\":%llu,")
			TEXT("\"manifest\":{\"sha256\":\"%s\",\"size\":%d,")
			TEXT("\"url\":\"https://github.com/aleiby/wot/releases/download/")
			TEXT("worker/manifest.json\"},\"minimum_gate_protocol\":1,")
			TEXT("\"platform\":\"%s\",\"release_sequence\":%llu}"),
			WoTGameBootstrapWorkerSpecReleaseKeyId,
			*RingDigest.ToHex(), Release.Metadata.KeyRingBytes.Num(),
			static_cast<unsigned long long>(RingEpoch),
			*ManifestDigest.ToHex(),
			Release.Metadata.ReleaseManifestBytes.Num(),
			WoTGameBootstrapWorkerSpecPlatform, Sequence));
	Release.AnnouncementSignature =
		WoTGameBootstrapWorkerSpecSign(
			AnnouncementKey, EWoTSignatureContext::AnnouncementV1,
			Release.AnnouncementBytes);
	Release.Metadata.AnnouncementBytes = Release.AnnouncementBytes;
	Release.Metadata.AnnouncementSignature =
		Release.AnnouncementSignature;

	FWoTEd25519Signature Signature;
	FMemory::Memcpy(Signature.Bytes,
		Release.AnnouncementSignature.GetData(),
		FWoTEd25519Signature::ByteCount);
	TWoTBootstrapResult<FWoTAnnouncementV1> Verified =
		VerifyAnnouncementV1(
			Release.AnnouncementBytes, Signature,
			WoTGameBootstrapWorkerSpecPlatform,
			WoT::Bootstrap::AnnouncementDomain,
			AnnouncementKey.Public);
	check(!Verified.IsError());
	Release.Announcement = MoveTemp(Verified).GetValue();
	return Release;
}

struct FWoTGameBootstrapWorkerSpecHopStep
{
	FWoTHttpHopResponse Response;
	bool bFailure = false;
	// When non-empty, the executed request must carry EXACTLY this URL (and
	// this Range value); any mismatch fails the hop and is counted, so a
	// worker wired to the wrong logical object cannot pass on counts alone.
	FString ExpectedUrl;
	FString ExpectedRange;
	TArray<TArray<uint8>> StreamChunks;
	int32 FailAfterStreamWrites = INDEX_NONE;
};

struct FWoTGameBootstrapWorkerSpecHopState
{
	TArray<FWoTGameBootstrapWorkerSpecHopStep> Steps;
	TArray<FWoTHttpHopRequest> Requests;
	TAtomic<int32> RequestCount{0};
	int32 Cursor = 0;
	int32 MismatchCount = 0;
	int32 BlockAtRequest = INDEX_NONE;
	int32 BlockAfterStreamWriteAtRequest = INDEX_NONE;
	TAtomic<int32> CancelCalls{0};
	FEvent* BlockEntered =
		FPlatformProcess::GetSynchEventFromPool(false);
	FEvent* BlockRelease =
		FPlatformProcess::GetSynchEventFromPool(false);
	FEvent* RequestEntered =
		FPlatformProcess::GetSynchEventFromPool(false);

	~FWoTGameBootstrapWorkerSpecHopState()
	{
		FPlatformProcess::ReturnSynchEventToPool(BlockEntered);
		FPlatformProcess::ReturnSynchEventToPool(BlockRelease);
		FPlatformProcess::ReturnSynchEventToPool(RequestEntered);
	}
};

class FWoTGameBootstrapWorkerSpecExecutor final
	: public IWoTHttpHopExecutor
{
public:
	explicit FWoTGameBootstrapWorkerSpecExecutor(
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> InState)
		: State(InState)
	{
	}

	virtual FWoTBootstrapStatus ExecuteHop(
		const FWoTHttpHopRequest& Request,
		FWoTHttpHopResponse& OutResponse) override
	{
		const int32 RequestIndex = State->Requests.Num();
		State->Requests.Add(Request);
		++State->RequestCount;
		State->RequestEntered->Trigger();
		if (RequestIndex == State->BlockAtRequest)
		{
			State->BlockEntered->Trigger();
			State->BlockRelease->Wait();
		}
		if (!State->Steps.IsValidIndex(State->Cursor))
		{
			OutResponse = FWoTHttpHopResponse();
			return FWoTBootstrapStatus::Error(
				MakeWoTBootstrapError(
					EWoTBootstrapError::ServiceUnavailable,
					TEXT("worker spec hop script exhausted")));
		}
		const FWoTGameBootstrapWorkerSpecHopStep& Step =
			State->Steps[State->Cursor++];
		if (!Step.ExpectedUrl.IsEmpty()
			&& (Request.Url != Step.ExpectedUrl
				))
		{
			++State->MismatchCount;
			OutResponse = FWoTHttpHopResponse();
			return FWoTBootstrapStatus::Error(
				MakeWoTBootstrapError(
					EWoTBootstrapError::ServiceUnavailable,
					TEXT("worker spec hop URL/Range mismatch")));
		}
		OutResponse = Step.Response;
		if (Step.bFailure)
		{
			return FWoTBootstrapStatus::Error(
				MakeWoTBootstrapError(
					EWoTBootstrapError::ServiceUnavailable,
					TEXT("scripted worker transport failure")));
		}
		if (!Step.StreamChunks.IsEmpty())
		{
			if (Request.BodySink == nullptr)
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("scripted stream had no response body sink")));
			}
			if (!Request.BodySink->ShouldStreamResponseBody(OutResponse.Status))
			{
				for (const TArray<uint8>& Chunk : Step.StreamChunks)
				{
					OutResponse.Body.Append(Chunk);
				}
				return FWoTBootstrapStatus::Success();
			}
			OutResponse.Body.Reset();
			FWoTBootstrapStatus Started =
				Request.BodySink->OnResponseStart(OutResponse);
			if (Started.IsError())
			{
				return Started;
			}
			int32 Writes = 0;
			for (const TArray<uint8>& Chunk : Step.StreamChunks)
			{
				FWoTBootstrapStatus Written =
					Request.BodySink->Write(Chunk);
				if (Written.IsError())
				{
					return Written;
				}
				OutResponse.StreamedBodyLength +=
					static_cast<uint64>(Chunk.Num());
				++Writes;
				if (Writes == 1 && RequestIndex
						== State->BlockAfterStreamWriteAtRequest)
				{
					State->BlockEntered->Trigger();
					State->BlockRelease->Wait();
				}
				if (Writes == Step.FailAfterStreamWrites)
				{
					return FWoTBootstrapStatus::Error(
						MakeWoTBootstrapError(
							EWoTBootstrapError::ServiceUnavailable,
							TEXT("scripted stream ended mid-body")));
				}
			}
		}
		return FWoTBootstrapStatus::Success();
	}

	virtual void CancelCurrentRequest() override
	{
		++State->CancelCalls;
		State->BlockRelease->Trigger();
	}

private:
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> State;
};

FWoTGameBootstrapWorkerSpecHopStep WoTGameBootstrapWorkerSpecHop(
	TConstArrayView<uint8> Body, int32 Status = 200,
	const FString& ExpectedUrl = FString(),
	const FString& ExpectedRange = FString())
{
	FWoTGameBootstrapWorkerSpecHopStep Step;
	Step.Response.Status = Status;
	Step.Response.Body.Append(Body.GetData(), Body.Num());
	Step.Response.BodyLength = static_cast<uint64>(Body.Num());
	Step.ExpectedUrl = ExpectedUrl;
	Step.ExpectedRange = ExpectedRange;
	return Step;
}

FWoTGameBootstrapWorkerSpecHopStep
WoTGameBootstrapWorkerSpecFailureHop()
{
	FWoTGameBootstrapWorkerSpecHopStep Step;
	Step.bFailure = true;
	return Step;
}

void WoTGameBootstrapWorkerSpecAppendAnnouncementScript(
	FWoTGameBootstrapWorkerSpecHopState& State,
	const FWoTGameBootstrapWorkerSpecRelease& Release,
	const FString& ExpectedAnnouncementUrl =
		WoTGameBootstrapWorkerSpecAnnouncementUrl)
{
	State.Steps.Add(WoTGameBootstrapWorkerSpecHop(
		Release.AnnouncementBytes, 200, ExpectedAnnouncementUrl));
	State.Steps.Add(WoTGameBootstrapWorkerSpecHop(
		Release.AnnouncementSignature, 200,
		ExpectedAnnouncementUrl + TEXT(".sig")));
}

void WoTGameBootstrapWorkerSpecAppendMetadataScript(
	FWoTGameBootstrapWorkerSpecHopState& State,
	const FWoTGameBootstrapWorkerSpecRelease& Release,
	bool bTamperManifest = false)
{
	State.Steps.Add(WoTGameBootstrapWorkerSpecHop(
		Release.Metadata.KeyRingBytes, 200,
		Release.Announcement.KeyRing.Url));
	State.Steps.Add(WoTGameBootstrapWorkerSpecHop(
		Release.Metadata.KeyRingSignature, 200,
		Release.Announcement.KeyRing.Url + TEXT(".sig")));
	TArray<uint8> Manifest = Release.Metadata.ReleaseManifestBytes;
	if (bTamperManifest && !Manifest.IsEmpty())
	{
		Manifest[0] ^= 0x01u;
	}
	State.Steps.Add(WoTGameBootstrapWorkerSpecHop(Manifest, 200,
		Release.Announcement.Manifest.Url));
	State.Steps.Add(WoTGameBootstrapWorkerSpecHop(
		Release.Metadata.ReleaseManifestSignature, 200,
		Release.Announcement.Manifest.Url + TEXT(".sig")));
}

bool WoTGameBootstrapWorkerSpecSetMode(
	FStringView Path, uint32 Mode)
{
	TWoTBootstrapResult<FWoTNativeFileHandle> File =
		OpenNativeFileReadNoFollow(Path);
	if (File.IsError())
	{
		return false;
	}
	FWoTNativeFileHandle Opened = MoveTemp(File).GetValue();
	return !SetNativeMode(Opened, Mode).IsError();
}

class FWoTGameBootstrapWorkerSpecFixture final
{
public:
	FWoTGameBootstrapWorkerSpecFixture()
	{
		// ResolveUpdaterStateRoot refuses relative roots (UnsafePath), and
		// AutomationTransientDir is project-relative — make it absolute first.
		InstallRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::AutomationTransientDir(),
			TEXT("wot-game-bootstrap-worker-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower()));
		UserStateRoot = InstallRoot + TEXT("-user");
		MarkerPath = FPaths::Combine(
			InstallRoot, TEXT("worker-relaunch.marker"));
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_UPDATER_SPEC_RELAUNCH_MARKER"), *MarkerPath);
		PreviousAnnouncementOverride = FPlatformMisc::GetEnvironmentVariable(
			TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"));
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"),
			TEXT("http://127.0.0.1:38431/announcement.json"));
		// An unrouted update-failed notice resolves to one per-process
		// directory keyed by PID that nothing clears, so a stale "failed=2"
		// left there (or by an earlier process with a reused PID) turned
		// every sequence-2 background check into KnownBad. Each fixture
		// reads and writes its own, initially absent, notice directory.
		NoticeDirectory = InstallRoot + TEXT("-notices");
		WoTSetUpdateFailureNoticeDirectoryForTesting(NoticeDirectory);
	}

	~FWoTGameBootstrapWorkerSpecFixture()
	{
		WoTSetUpdateFailureNoticeDirectoryForTesting(FString());
		IFileManager::Get().DeleteDirectory(
			*NoticeDirectory, false, true);
		WoTResetProcessProfileSessionForTesting();
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_UPDATER_SPEC_RELAUNCH_MARKER"), TEXT(""));
		FPlatformMisc::SetEnvironmentVar(
			TEXT("WOT_BOOTSTRAP_TEST_ANNOUNCEMENT_URL"),
			*PreviousAnnouncementOverride);
		IFileManager::Get().DeleteDirectory(
			*InstallRoot, false, true);
		IFileManager::Get().DeleteDirectory(
			*UserStateRoot, false, true);
		IFileManager::Get().DeleteDirectory(
			*(InstallRoot + TEXT("-downloads")), false, true);
	}

	bool Initialize(FAutomationTestBase& Test,
		bool bIncludeIdentity = false)
	{
		WoTResetProcessProfileSessionForTesting();
		if (!AnnouncementKey.Initialize(0x11)
			|| !ReleaseKey.Initialize(0x51)
			|| !RootKey.Initialize(0x91))
		{
			Test.AddError(TEXT("worker fixture could not derive role keys"));
			return false;
		}
		V1 = WoTGameBootstrapWorkerSpecBuildRelease(
			1, MarkerPath, AnnouncementKey, ReleaseKey, RootKey,
			1, 0, bIncludeIdentity);
		V2 = WoTGameBootstrapWorkerSpecBuildRelease(
			2, MarkerPath, AnnouncementKey, ReleaseKey, RootKey,
			1, 0, bIncludeIdentity);
		V3 = WoTGameBootstrapWorkerSpecBuildRelease(
			3, MarkerPath, AnnouncementKey, ReleaseKey, RootKey,
			2, 0, bIncludeIdentity);
		if (!IFileManager::Get().MakeDirectory(*InstallRoot, true)
			|| !IFileManager::Get().MakeDirectory(*UserStateRoot, true))
		{
			return false;
		}
		FWoTBootstrapStatus ProcessSession =
			WoTAcquireProcessProfileSessionShared(UserStateRoot);
		if (ProcessSession.IsError())
		{
			Test.AddError(MoveTemp(ProcessSession).GetError().ToString());
			return false;
		}
#if PLATFORM_WINDOWS
		for (const TCHAR* Dll
			: {TEXT("tbbmalloc.dll"), TEXT("tbb12.dll")})
		{
			const FString Source = FPaths::Combine(
				FPaths::GetPath(FPlatformProcess::ExecutablePath()),
				Dll);
			if (!IFileManager::Get().FileExists(*Source))
			{
				continue;
			}
			if (IFileManager::Get().Copy(
				*FPaths::Combine(InstallRoot, Dll),
				*Source, true, true) != COPY_OK)
			{
				Test.AddError(
					TEXT("worker fixture could not colocate ")
					+ FString(Dll));
				return false;
			}
		}
#endif
		TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved =
			ResolveUpdaterStateRoot(UserStateRoot, InstallRoot);
		if (Resolved.IsError())
		{
			Test.AddError(MoveTemp(Resolved).GetError().ToString());
			return false;
		}
		Root = MoveTemp(Resolved).GetValue();
		FWoTUpdateStateRecord Installed;
		Installed.KeyRevocationFloor = 1;
		Installed.NewestAnnouncementSequence = 1;
		FWoTBootstrapStatus Seeded =
			MergeAndWriteWoTUpdateState(Root, Installed);
		if (Seeded.IsError())
		{
			Test.AddError(MoveTemp(Seeded).GetError().ToString());
			return false;
		}
		if (!MaterializeV1())
		{
			Test.AddError(TEXT("worker fixture could not materialize v1"));
			return false;
		}
		return true;
	}

	bool SeedUpdateState(FAutomationTestBase& Test)
	{
		FWoTUpdateStateRecord Next;
		Next.NewestAnnouncementSequence = 2;
		FWoTBootstrapStatus Raised =
			MergeAndWriteWoTUpdateState(Root, Next);
		if (Raised.IsError())
		{
			Test.AddError(MoveTemp(Raised).GetError().ToString());
			return false;
		}
		return true;
	}

	FWoTGameBootstrapWorkerSpecRelease BuildRelease(
		uint64 Sequence, uint64 RingEpoch, int32 PaddingBytes)
	{
		return WoTGameBootstrapWorkerSpecBuildRelease(
			Sequence, MarkerPath, AnnouncementKey, ReleaseKey, RootKey,
			RingEpoch, PaddingBytes);
	}

	bool WriteInstaller(TConstArrayView<uint8> Bytes) const
	{
		return WoTGameBootstrapWorkerSpecSave(
			WoTGameBootstrapWorkerSpecInstallerPath(InstallRoot), Bytes);
	}

	bool PublishActiveCache(FAutomationTestBase& Test,
		TOptional<FWoTSha256> InstallerShaOverride = {},
		TOptional<FWoTSha256> DescriptorShaOverride = {},
		TOptional<FWoTSha256> ContractShaOverride = {},
		FString* OutLogicalId = nullptr) const
	{
		TWoTBootstrapResult<FWoTCacheStore> Opened =
			FWoTCacheStore::ForTesting(
				WoTGameBootstrapWorkerSpecCacheRoot(InstallRoot));
		if (Opened.IsError())
		{
			Test.AddError(MoveTemp(Opened).GetError().ToString());
			return false;
		}
		FWoTCacheStore Cache = MoveTemp(Opened).GetValue();
		TUniquePtr<FWoTProfileSession> Session =
			WoTTakeProcessProfileSession();
		if (!Session.IsValid())
		{
			Test.AddError(TEXT("worker fixture has no process profile session"));
			return false;
		}
		TWoTBootstrapResult<bool> Exclusive = Session->TryAcquireExclusive();
		if (Exclusive.IsError() || !Exclusive.GetValue())
		{
			Test.AddError(TEXT("worker fixture profile session is contended"));
			return false;
		}
		TWoTBootstrapResult<FWoTSha256> CompiledInstallerSha =
			TWoTBootstrapResult<FWoTSha256>::Success(
				WoTGameBootstrapWorkerSpecInstallerPins().Sha256);
		TWoTBootstrapResult<FWoTSha256> CompiledContentLabel =
			GetCacheContentLabel();
		TWoTBootstrapResult<FWoTRetailContract> CompiledContract =
			WoTLoadProductionRetailContract();
		if (CompiledInstallerSha.IsError()
			|| CompiledContentLabel.IsError() || CompiledContract.IsError()
			|| CompiledContract.GetValue().StartupCriticalLogicalIds.Num() != 48)
		{
			Test.AddError(TEXT("worker fixture could not parse compiled cache pins"));
			return false;
		}
		const FString LogicalId =
			CompiledContract.GetValue().StartupCriticalLogicalIds[0];
		FWoTBootstrapStatus Begun = Cache.BeginBuild(*Session);
		if (Begun.IsError())
		{
			Test.AddError(MoveTemp(Begun).GetError().ToString());
			return false;
		}
		FWoTCacheBuildResult Build;
		for (const FString& CriticalLogicalId :
			CompiledContract.GetValue().StartupCriticalLogicalIds)
		{
			const FWoTRequiredFact* RequiredFact = nullptr;
			const FWoTRequiredFactOutput* RequiredOutput = nullptr;
			for (const FWoTRequiredFact& Fact : CompiledContract.GetValue().Facts)
			{
				for (const FWoTRequiredFactOutput& Output : Fact.Outputs)
				{
					if (Output.LogicalId == CriticalLogicalId)
					{
						RequiredFact = &Fact;
						RequiredOutput = &Output;
						break;
					}
				}
				if (RequiredOutput != nullptr) break;
			}
			if (RequiredFact == nullptr || RequiredOutput == nullptr)
			{
				Test.AddError(TEXT("worker fixture could not resolve a critical output"));
				return false;
			}
			const TArray<uint8> Bytes =
				WoTGameBootstrapWorkerSpecAscii(CriticalLogicalId);
			FWoTBootstrapStatus Written = Cache.WriteObject(
				CriticalLogicalId, RequiredOutput->Schema, Bytes, Sha256(Bytes),
				RequiredOutput->MaxDecodedSize);
			if (Written.IsError())
			{
				Test.AddError(MoveTemp(Written).GetError().ToString());
				return false;
			}
			FWoTCacheBlob& Blob = Build.Manifest.Blobs.AddDefaulted_GetRef();
			Blob.Satisfies.Add(FWoTRequiredFactId{RequiredFact->FactId});
			Blob.LogicalId = CriticalLogicalId;
			Blob.Schema = RequiredOutput->Schema;
			Blob.Size = static_cast<uint64>(Bytes.Num());
			Blob.Sha256 = Sha256(Bytes);
		}
		Build.Manifest.bHasBuildIdentity = true;
		Build.Manifest.InstallerSha256 = InstallerShaOverride.IsSet()
			? InstallerShaOverride.GetValue()
			: CompiledInstallerSha.GetValue();
		Build.Manifest.ContractSha256 = ContractShaOverride.IsSet()
			? ContractShaOverride.GetValue()
			: ComputeRetailContractIdentity(CompiledContract.GetValue());
		Build.Manifest.ProductionDescriptorSha256 =
			DescriptorShaOverride.IsSet()
			? DescriptorShaOverride.GetValue()
			: CompiledContentLabel.GetValue();
		auto FrontEndDescriptor = WoTFrontEndTest_MakeDescriptor(
			CompiledContract.GetValue());
		if (FrontEndDescriptor.IsError())
		{
			Test.AddError(MoveTemp(FrontEndDescriptor).GetError().ToString());
			return false;
		}
		auto FrontEndSidecar = WriteSealedFrontEndDescriptorSidecar(
			FrontEndDescriptor.GetValue(), Build.Manifest.InstallerSha256,
			Build.Manifest.ContractSha256,
			Build.Manifest.ProductionDescriptorSha256);
		if (FrontEndSidecar.IsError())
		{
			Test.AddError(MoveTemp(FrontEndSidecar).GetError().ToString());
			return false;
		}
		constexpr TCHAR FrontEndLogicalId[] =
			TEXT("cache/front_end_world/WheelOfTime");
		FWoTBootstrapStatus FrontEndWritten = Cache.WriteObject(
			FrontEndLogicalId, TEXT("wot-front-end-world-v3"),
			FrontEndSidecar.GetValue(), Sha256(FrontEndSidecar.GetValue()),
			4194304);
		if (FrontEndWritten.IsError())
		{
			Test.AddError(MoveTemp(FrontEndWritten).GetError().ToString());
			return false;
		}
		FWoTCacheBlob& FrontEndBlob = Build.Manifest.Blobs.AddDefaulted_GetRef();
		FrontEndBlob.Satisfies.Add(FWoTRequiredFactId{
			TEXT("retail.front_end_world.WheelOfTime")});
		FrontEndBlob.LogicalId = FrontEndLogicalId;
		FrontEndBlob.Schema = TEXT("wot-front-end-world-v3");
		FrontEndBlob.Size = static_cast<uint64>(FrontEndSidecar.GetValue().Num());
		FrontEndBlob.Sha256 = Sha256(FrontEndSidecar.GetValue());
		Build.Manifest.FrontEndDescriptorRoot =
			FrontEndDescriptor.GetValue().DescriptorRoot;
		TWoTBootstrapResult<TArray<uint8>> Manifest =
			WriteCanonicalCacheManifest(Build.Manifest);
		if (Manifest.IsError())
		{
			Test.AddError(MoveTemp(Manifest).GetError().ToString());
			return false;
		}
		Build.CanonicalManifest = MoveTemp(Manifest).GetValue();
		Build.ManifestSha256 = Sha256(Build.CanonicalManifest);
		FWoTBootstrapStatus Published = Cache.Publish(Build);
		if (Published.IsError())
		{
			Test.AddError(MoveTemp(Published).GetError().ToString());
			return false;
		}
		FWoTBootstrapStatus Restored = Session->RestoreShared();
		if (Restored.IsError())
		{
			Test.AddError(MoveTemp(Restored).GetError().ToString());
			return false;
		}
		if (!WoTRestoreProcessProfileSession(MoveTemp(Session)))
		{
			Test.AddError(TEXT("worker fixture could not restore its process session"));
			return false;
		}
		if (OutLogicalId != nullptr)
		{
			*OutLogicalId = LogicalId;
		}
		return true;
	}

	FString InstallRoot;
	FString UserStateRoot;
	FString MarkerPath;
	FWoTUpdaterStateRoot Root;
	FWoTGameBootstrapWorkerSpecRelease V1;
	FWoTGameBootstrapWorkerSpecRelease V2;
	FWoTGameBootstrapWorkerSpecRelease V3;

private:
	bool MaterializeV1()
	{
		const FString Active = FPaths::Combine(
			InstallRoot, WoTGameBootstrapWorkerSpecRootLeaf);
		if (!IFileManager::Get().MakeDirectory(*Active, true))
		{
			return false;
		}
		for (const FWoTInstalledTreeEntry& Entry
			: V1.Tree.Inventory.Entries)
		{
			const FString Path = FPaths::Combine(Active, Entry.Path);
			if (Entry.Type == EWoTTreeEntryType::Directory)
			{
				if (!IFileManager::Get().MakeDirectory(*Path, true))
				{
					return false;
				}
				TWoTBootstrapResult<FWoTNativeDirectoryHandle> Directory =
					OpenNativeDirectoryNoFollow(Path);
				if (Directory.IsError())
				{
					return false;
				}
				FWoTNativeDirectoryHandle OpenedDirectory =
					MoveTemp(Directory).GetValue();
				if (SetNativeMode(
					OpenedDirectory, Entry.Mode).IsError())
				{
					return false;
				}
				continue;
			}
			IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(Path), true);
			const TArray<uint8>* Bytes = nullptr;
			if (Entry.Path == TEXT("Content.bin"))
			{
				Bytes = &V1.Tree.Payload;
			}
			else if (Entry.Path
				== WoTGameBootstrapWorkerSpecMainRelative)
			{
				Bytes = &V1.Tree.Main;
			}
			else if (Entry.Path == V1.Tree.UpdaterRelativePath)
			{
				Bytes = &V1.Tree.Updater;
			}
			else if (Entry.Path == WoTGameBootstrapWorkerSpecStampRelative)
			{
				Bytes = &V1.Tree.Stamp;
			}
			if (Bytes == nullptr
				|| !WoTGameBootstrapWorkerSpecSave(Path, *Bytes)
				|| !WoTGameBootstrapWorkerSpecSetMode(
					Path, Entry.Mode))
			{
				return false;
			}
		}
		return true;
	}

	FWoTGameBootstrapWorkerSpecKey AnnouncementKey;
	FWoTGameBootstrapWorkerSpecKey ReleaseKey;
	FWoTGameBootstrapWorkerSpecKey RootKey;
	FString PreviousAnnouncementOverride;
	FString NoticeDirectory;
};

TUniquePtr<FWoTGameBootstrapWorker>
WoTGameBootstrapWorkerSpecMakeWorker(
	const FString& InstallRoot,
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops,
	TFunction<void()> RestartExit = TFunction<void()>(),
	FString AnnouncementUrl =
		WoTGameBootstrapWorkerSpecAnnouncementUrl,
	TOptional<FWoTInstallerAcquisitionPins> InstallerPinsOverride = {},
	FWoTCacheBootstrapOperation::FBuildForTesting CacheBuildForTesting = {},
	FString InstalledTreeRoot = FString(),
	TFunction<double()> NowSeconds = {},
	FWoTInstallerAcquisitionTestHooks InstallerHooksForTesting = {},
	TFunction<FWoTBootstrapStatus()> InstallWritabilityProbeForTesting = {},
	TFunction<FWoTBootstrapStatus()> ActivationRecoveryForTesting = {},
	FString UserStateRootOverride = FString(),
	FString UpdateDisableReason = FString(),
	TFunction<bool(const FString&)> RunningTreeTranslocatedForTesting = {},
	TOptional<FWoTNativeFileIdentity> InstallRootIdentityOverride = {},
	TFunction<void()> BeforeStagingLeaseForTesting = {},
	TFunction<TWoTBootstrapResult<bool>(const FString&, bool)>
		AnyProcessRunsFromTreeForTesting = {},
	TFunction<bool()> SkipAutoInstallForTesting =
		[]() { return true; },
	TFunction<TWoTBootstrapResult<bool>()>
		AdjacentParentWritableForTesting = {},
	TFunction<TOptional<FWoTBootstrapError>(
		EWoTInstallerSource, const FString&)>
		InstallerLocationFaultForTesting = {},
	FString RunningConfiguration = FString())
{
	FWoTGameBootstrapWorker::FDependencies Deps;
	Deps.Hops =
		MakeUnique<FWoTGameBootstrapWorkerSpecExecutor>(Hops);
	Deps.InstallRoot = InstallRoot;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> HeldInstallRoot =
		OpenNativeDirectoryNoFollow(InstallRoot);
	if (InstallRootIdentityOverride.IsSet())
	{
		Deps.InstallRootIdentity = InstallRootIdentityOverride.GetValue();
		Deps.bHasInstallRootIdentity = true;
		Deps.AdjacentInstallerParentIdentity =
			InstallRootIdentityOverride.GetValue();
		Deps.bHasAdjacentInstallerParentIdentity = true;
	}
	else if (!HeldInstallRoot.IsError())
	{
		Deps.InstallRootIdentity = HeldInstallRoot.GetValue().GetIdentity();
		Deps.bHasInstallRootIdentity = true;
		Deps.AdjacentInstallerParentIdentity =
			HeldInstallRoot.GetValue().GetIdentity();
		Deps.bHasAdjacentInstallerParentIdentity = true;
	}
	Deps.UserStateRoot = UserStateRootOverride.IsEmpty()
		? InstallRoot + TEXT("-user") : MoveTemp(UserStateRootOverride);
	Deps.InstalledTreeRoot = InstalledTreeRoot.IsEmpty()
		? FPaths::Combine(InstallRoot, WoTGameBootstrapWorkerSpecRootLeaf)
		: MoveTemp(InstalledTreeRoot);
	Deps.LiveLeaf = FPaths::GetCleanFilename(Deps.InstalledTreeRoot);
	Deps.CanonicalAdjacentInstallerPath =
		WoTGameBootstrapWorkerSpecInstallerPath(InstallRoot);
	Deps.DownloadsDirectory = InstallRoot + TEXT("-downloads");
	IFileManager::Get().MakeDirectory(*Deps.DownloadsDirectory, true);
	Deps.ContentCacheRoot = FPaths::Combine(
		Deps.UserStateRoot, TEXT("ContentCache"));
	Deps.UpdateDisableReason = MoveTemp(UpdateDisableReason);
	Deps.AnnouncementJsonUrl = MoveTemp(AnnouncementUrl);
	Deps.RunningIdentity.ReleaseSequence = 1;
	Deps.RunningIdentity.ReleaseCommit = TEXT("worker-spec-release-1");
	Deps.RunningIdentity.bStamped = true;
	Deps.NowSeconds = NowSeconds ? MoveTemp(NowSeconds)
		: TFunction<double()>([]() { return 100.0; });
	Deps.AnnouncementDeadlineSeconds = 15.0;
	Deps.DownloadOperationDeadlineSeconds = 0.0;
	Deps.OriginalGameProbeIntervalSeconds = 0.01;
	Deps.RequestRestartExit = MoveTemp(RestartExit);
	Deps.InstallerPinsForTesting = InstallerPinsOverride.IsSet()
		? InstallerPinsOverride
		: TOptional<FWoTInstallerAcquisitionPins>(
			WoTGameBootstrapWorkerSpecInstallerPins());
	Deps.CacheBuildForTesting = MoveTemp(CacheBuildForTesting);
	Deps.bUseSemanticCacheFixture = true;
	Deps.InstallerHooksForTesting = MoveTemp(InstallerHooksForTesting);
	Deps.InstallWritabilityProbeForTesting =
		MoveTemp(InstallWritabilityProbeForTesting);
	Deps.RunningTreeTranslocatedForTesting =
		MoveTemp(RunningTreeTranslocatedForTesting);
	Deps.AdjacentParentWritableForTesting =
		MoveTemp(AdjacentParentWritableForTesting);
	Deps.InstallerLocationFaultForTesting =
		MoveTemp(InstallerLocationFaultForTesting);
	Deps.RunningConfiguration = MoveTemp(RunningConfiguration);
	return MakeUnique<FWoTGameBootstrapWorker>(MoveTemp(Deps));
}

bool WoTGameBootstrapWorkerSpecWaitForAtLeast(
	const TAtomic<int32>& Counter, int32 Expected)
{
	const double Deadline =
		FPlatformTime::Seconds() + WoTGameBootstrapWorkerSpecHangSeconds;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (Counter.Load() >= Expected)
		{
			return true;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	return Counter.Load() >= Expected;
}

int32 WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
	const FString& UserStateRoot, const FString& Leaf)
{
	FString Heartbeat;
	if (!FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
		UserStateRoot, TEXT("UpdaterState"),
		WoTBootstrapHeartbeatFileName())))
	{
		return 0;
	}
	TArray<FString> Lines;
	Heartbeat.ParseIntoArrayLines(Lines, false);
	const FString Detail = FString::Printf(
		TEXT("source=downloads leaf=%s detail=Io"), *Leaf);
	int32 Count = 0;
	for (const FString& Line : Lines)
	{
		if (Line.Contains(
				TEXT("\"event\":\"installer-location-unavailable\""),
				ESearchCase::CaseSensitive)
			&& Line.Contains(Detail, ESearchCase::CaseSensitive))
		{
			++Count;
		}
	}
	return Count;
}

bool WoTGameBootstrapWorkerSpecWaitForUnavailableHeartbeats(
	const FString& UserStateRoot, const FString& Leaf, int32 Expected)
{
	const double Deadline =
		FPlatformTime::Seconds() + WoTGameBootstrapWorkerSpecHangSeconds;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
			UserStateRoot, Leaf) >= Expected)
		{
			return true;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	return WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
		UserStateRoot, Leaf) >= Expected;
}

FWoTBootstrapWorkerRequest WoTGameBootstrapWorkerSpecRequest(
	EWoTBootstrapPhase Phase, uint64 Epoch)
{
	FWoTBootstrapWorkerRequest Request;
	Request.Phase = Phase;
	Request.OperationEpoch = Epoch;
	if (Phase == EWoTBootstrapPhase::CheckingGameUpdates)
	{
		Request.AnnouncementLogicalObjects = {
			WoTGameBootstrapWorkerSpecAnnouncementUrl,
			FString(WoTGameBootstrapWorkerSpecAnnouncementUrl)
				+ TEXT(".sig")};
	}
	return Request;
}

bool WoTGameBootstrapWorkerSpecRunPhase(
	IWoTBootstrapWorker& Worker,
	EWoTBootstrapPhase Phase, uint64 Epoch,
	FWoTBootstrapWorkerCompletion& OutCompletion,
	TArray<TPair<uint64, uint64>>* OutByteProgress = nullptr,
	double TimeoutSeconds = WoTGameBootstrapWorkerSpecHangSeconds,
	TArray<uint64>* OutProgressEpochs = nullptr,
	TArray<TPair<uint64, uint64>>* OutItemProgress = nullptr,
	TArray<FWoTBootstrapProgressEvent>* OutProgressEvents = nullptr)
{
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
	bool bCompleted = false;
	Worker.StartPhase(
		WoTGameBootstrapWorkerSpecRequest(Phase, Epoch),
		[OutByteProgress, OutProgressEpochs, OutItemProgress,
			OutProgressEvents](FWoTBootstrapProgressEvent&& Event)
		{
			if (OutProgressEvents != nullptr)
			{
				OutProgressEvents->Add(Event);
			}
			if (OutItemProgress != nullptr)
			{
				OutItemProgress->Add({Event.CompletedUnits, Event.TotalUnits});
			}
			if (OutByteProgress != nullptr
				&& Event.CompletedBytes.IsSet() && Event.TotalBytes.IsSet())
			{
				OutByteProgress->Add(
					{Event.CompletedBytes.GetValue(),
						Event.TotalBytes.GetValue()});
				if (OutProgressEpochs != nullptr)
				{
					OutProgressEpochs->Add(Event.OperationEpoch);
				}
			}
		},
		[&OutCompletion, &bCompleted, Done](
			FWoTBootstrapWorkerCompletion&& Completion)
		{
			OutCompletion = MoveTemp(Completion);
			bCompleted = true;
			Done->Trigger();
		});
	const bool bSignaled = Done->Wait(
		FTimespan::FromSeconds(TimeoutSeconds));
	if (!bSignaled)
	{
		// The still-armed completion callback holds the addresses of this
		// frame's locals and the pooled event; join the worker so it can
		// never fire into dead stack state or a recycled event.
		Worker.CancelAndJoin();
	}
	FPlatformProcess::ReturnSynchEventToPool(Done);
	return bSignaled && bCompleted;
}

bool WoTGameBootstrapWorkerSpecEstablish(
	FAutomationTestBase& Test, IWoTBootstrapWorker& Worker,
	uint64 Epoch = 1)
{
	FWoTBootstrapWorkerCompletion Completion;
	if (!WoTGameBootstrapWorkerSpecRunPhase(
		Worker, EWoTBootstrapPhase::EstablishingIdentity,
		Epoch, Completion))
	{
		Test.AddError(TEXT("worker EstablishingIdentity timed out"));
		return false;
	}
	return Test.TestTrue(
		TEXT("identity establishment succeeds"),
		Completion.bSucceeded)
		&& Test.TestEqual(
			TEXT("identity routes to game update assessment"),
			static_cast<int32>(Completion.NextPhase),
			static_cast<int32>(
				EWoTBootstrapPhase::CheckingGameUpdates));
}

FWoTSha256 WoTGameBootstrapWorkerSpecDigest(uint8 Seed)
{
	FWoTSha256 Digest;
	for (int32 Index = 0; Index < FWoTSha256::ByteCount; ++Index)
	{
		Digest.Bytes[Index] = static_cast<uint8>(Seed + Index);
	}
	return Digest;
}

bool WoTGameBootstrapWorkerSpecPublishEarlyUI()
{
	FWoTEarlyUIAuthority& Authority = GetEarlyUIAuthority();
	Authority.ResetForTesting();
	const ANSICHAR Source[] =
		"Schema=EarlyUI-v1\nUIScale=1.0\nLocale=en-US\n"
		"ReducedMotion=1\nHighContrast=1\nScreenReader=1\n"
		"BootstrapVolume=1.0\nInputSensitivity=1.0\n";
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Source),
		static_cast<int32>(UE_ARRAY_COUNT(Source) - 1));
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed =
		ParseEarlyUIV1(Bytes, FWoTEarlyUILimits::Production(),
			FWoTInventoryLocaleSet::Production());
	if (Parsed.IsError()
		|| Authority.PublishOnce(
			MoveTemp(Parsed).GetValue()).IsError())
	{
		return false;
	}
	auto Apply = [](uint16, uint16, EWoTWindowMode)
		-> FWoTBootstrapStatus
	{
		return FWoTBootstrapStatus::Success();
	};
	const FWoTEarlyUINativeProjectionSink Sink{Apply};
	FString Error;
	return
		WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
			Sink, &Error);
}

class FWoTGameBootstrapWorkerSpecFreeSpace final
	: public IWoTFreeSpaceProbe
{
public:
	virtual bool QueryAvailableBytes(
		EWoTBootstrapPhase, uint64& OutAvailableBytes) override
	{
		OutAvailableBytes = MAX_uint64;
		return true;
	}
};

class FWoTGameBootstrapWorkerSpecCacheProgress final
	: public IWoTProgressSink
{
public:
	virtual FWoTBootstrapStatus ReportProgress(
		FStringView, uint64, uint64) override
	{
		return FWoTBootstrapStatus::Success();
	}
	virtual bool IsCancellationRequested() const override { return false; }
};

TWoTBootstrapResult<FWoTVerifiedInstaller>
WoTGameBootstrapWorkerSpecVerifiedInstaller(const FString& Path)
{
	using FResult = TWoTBootstrapResult<FWoTVerifiedInstaller>;
	TWoTBootstrapResult<FWoTNativeFileHandle> Opened =
		OpenNativeFileReadNoFollow(Path);
	if (Opened.IsError())
	{
		return FResult::Error(MoveTemp(Opened).GetError());
	}
	TWoTBootstrapResult<TArray<uint8>> Bytes = Opened.GetValue().ReadAll();
	if (Bytes.IsError())
	{
		return FResult::Error(MoveTemp(Bytes).GetError());
	}
	FWoTVerifiedInstaller Installer;
	Installer.Source = EWoTInstallerSource::Adjacent;
	Installer.Snapshot.Handle = MoveTemp(Opened).GetValue();
	Installer.Snapshot.Size = static_cast<uint64>(Bytes.GetValue().Num());
	Installer.Snapshot.Sha256 = Sha256(Bytes.GetValue());
	Installer.Snapshot.Identity = Installer.Snapshot.Handle.GetIdentity();
	return FResult::Success(MoveTemp(Installer));
}

struct FWoTGameBootstrapWorkerSpecGlobalCleanup
{
	~FWoTGameBootstrapWorkerSpecGlobalCleanup()
	{
		UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
		UWoTPreferencesSubsystem::SetCreationEnabledForTesting(false);
		UWoTPreferencesSubsystem::SetDelayEarlyUIAdoptionForTesting(false);
		GetEarlyUIAuthority().ResetForTesting();
		ResetWoTPayloadActivationForTesting();
		FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
		WoTSetPayloadLeaseAdoptionObserverForTesting({});
		WoTSetWorldLeaseAdoptionObserverForTesting({});
		WoTSetVerifiedBlobBodyReadObserverForTesting({});
		UWoTBootstrapSubsystem::SetBeforeCacheActivationObserverForTesting({});
	}
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecBootstrapNeverFetches,
	"WoT.Update.Worker.BootstrapNeverFetches",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecBootstrapNeverFetches::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}

	auto CheckLocalOnly = [this, &Fixture](const TCHAR* Label, uint64 Epoch)
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker, Epoch))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Checked;
		if (!TestTrue(FString(Label) + TEXT(" completes locally"),
				WoTGameBootstrapWorkerSpecRunPhase(*Worker,
					EWoTBootstrapPhase::CheckingGameUpdates, Epoch + 1, Checked)))
		{
			return false;
		}
		TestTrue(FString(Label) + TEXT(" stays playable"), Checked.bSucceeded);
		TestEqual(FString(Label) + TEXT(" routes to original-game data"),
			static_cast<int32>(Checked.NextPhase),
			static_cast<int32>(EWoTBootstrapPhase::CheckingOriginalGameData));
		TestEqual(FString(Label) + TEXT(" performs zero announcement calls"),
			Hops->Requests.Num(), 0);
		return true;
	};

	if (!CheckLocalOnly(TEXT("fresh install without cache or installer"), 1))
	{
		return false;
	}
	if (!Fixture.SeedUpdateState(*this))
	{
		return false;
	}
	CheckLocalOnly(TEXT("durable pending-newer install"), 10);
	return !HasAnyErrors();
}

bool WoTGameBootstrapWorkerSpecRunBackground(
	IWoTBootstrapWorker& Worker, FWoTUpdateState& OutTerminal,
	TArray<FWoTUpdateState>* OutStates = nullptr,
	double TimeoutSeconds = WoTGameBootstrapWorkerSpecHangSeconds)
{
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
	TAtomic<bool> bTerminal{false};
	Worker.StartBackgroundUpdate(
		[&OutTerminal, OutStates, Done, &bTerminal](FWoTUpdateState&& State)
		{
			if (OutStates != nullptr)
			{
				OutStates->Add(State);
			}
			if (State.Status != EWoTUpdateStatus::Checking
				&& State.Status != EWoTUpdateStatus::Downloading)
			{
				OutTerminal = MoveTemp(State);
				bTerminal.Store(true);
				Done->Trigger();
			}
		});
	const bool bSignaled = Done->Wait(FTimespan::FromSeconds(TimeoutSeconds));
	if (!bSignaled)
	{
		Worker.CancelAndJoin();
	}
	FPlatformProcess::ReturnSynchEventToPool(Done);
	return bSignaled && bTerminal.Load();
}

bool WoTGameBootstrapWorkerSpecRunReleaseNotes(
	IWoTBootstrapWorker& Worker, bool bSeen,
	FWoTReleaseNotesResult& OutResult,
	double TimeoutSeconds = WoTGameBootstrapWorkerSpecHangSeconds)
{
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
	TAtomic<bool> bCompleted{false};
	Worker.StartReleaseNotes(bSeen,
		[&OutResult, Done, &bCompleted](FWoTReleaseNotesResult&& Result)
		{
			OutResult = MoveTemp(Result);
			bCompleted.Store(true);
			Done->Trigger();
		});
	const bool bSignaled = Done->Wait(FTimespan::FromSeconds(TimeoutSeconds));
	if (!bSignaled)
	{
		Worker.CancelAndJoin();
	}
	FPlatformProcess::ReturnSynchEventToPool(Done);
	return bSignaled && bCompleted.Load();
}

FWoTGameBootstrapWorkerSpecHopStep WoTGameBootstrapWorkerSpecNotesHop(
	const FString& Json)
{
	FWoTGameBootstrapWorkerSpecHopStep Step;
	Step.Response.Status = 200;
	Step.ExpectedUrl = WoTReleaseNotes::LatestUrl;
	Step.StreamChunks = {WoTGameBootstrapWorkerSpecAscii(Json)};
	Step.Response.BodyLength = static_cast<uint64>(Step.StreamChunks[0].Num());
	return Step;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReleaseNotes,
	"WoT.Update.Worker.ReleaseNotes",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReleaseNotes::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	const FString Asset = TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[{\"release_sequence\":1,\"text\":\"worker notes\"}]}");
	auto MakeEstablished = [this, &Fixture](
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops)
	{
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			Worker.Reset();
		}
		return Worker;
	};
	auto TestGate = [this, &MakeEstablished](const TCHAR* Label,
		TFunction<void(FWoTGameBootstrapWorker&)>&& Configure, bool bSeen)
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker = MakeEstablished(Hops);
		if (!Worker.IsValid()) return false;
		Configure(*Worker);
		FWoTReleaseNotesResult Result;
		if (!TestTrue(FString(Label) + TEXT(" completes"),
				WoTGameBootstrapWorkerSpecRunReleaseNotes(
					*Worker, bSeen, Result))) return false;
		TestEqual(FString(Label) + TEXT(" is not attempted"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::NotAttempted));
		TestEqual(FString(Label) + TEXT(" performs no request"),
			Hops->Requests.Num(), 0);
		return true;
	};
	if (!TestGate(TEXT("sequence zero"),
		[](FWoTGameBootstrapWorker& Worker)
		{
			Worker.SetRunningReleaseSequenceForTesting(0);
		}, false)) return false;
	if (!TestGate(TEXT("unstamped identity"),
		[](FWoTGameBootstrapWorker& Worker)
		{
			Worker.SetRunningIdentityForTesting(false, 1, TEXT("worker-spec-release-1"));
		}, false)) return false;
	if (!TestGate(TEXT("seen release"),
		[](FWoTGameBootstrapWorker&) {}, true)) return false;

	{
		// A notify-only launch still fetches notes. The first launch after an
		// update is notify-only (activation-recovery-pending), and notes were
		// once skipped on exactly that launch. A disable
		// reason set at identity time also leaves the updater state root
		// unresolved, so this covers the read-only and translocated launches
		// too.
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecNotesHop(Asset));
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops,
				TFunction<void()>(), WoTGameBootstrapWorkerSpecAnnouncementUrl,
				{}, {}, FString(), {}, {}, {}, {}, FString(),
				TEXT("activation-recovery-pending"));
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTReleaseNotesResult Result;
		TestTrue(TEXT("notify-only notes operation completes"),
			WoTGameBootstrapWorkerSpecRunReleaseNotes(*Worker, false, Result));
		TestEqual(TEXT("notify-only launch still selects its notes"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::Selected));
		TestEqual(TEXT("notify-only launch makes exactly the notes request"),
			Hops->Requests.Num(), 1);
	}
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecNotesHop(Asset));
		TUniquePtr<FWoTGameBootstrapWorker> Worker = MakeEstablished(Hops);
		if (!Worker.IsValid()) return false;
		FWoTReleaseNotesResult Result;
		TestTrue(TEXT("selected notes complete"),
			WoTGameBootstrapWorkerSpecRunReleaseNotes(*Worker, false, Result));
		TestEqual(TEXT("valid matching notes are selected"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::Selected));
		TestEqual(TEXT("selected notes carry the authoritative commit"),
			Result.ReleaseCommit, FString(TEXT("worker-spec-release-1")));
		TestEqual(TEXT("selected notes carry the authoritative sequence"),
			Result.ReleaseSequence, uint64(1));
		TestEqual(TEXT("selected notes carry text"), Result.Text,
			FString(TEXT("worker notes")));
		TestEqual(TEXT("notes request has one 15-second deadline"),
			Hops->Requests[0].AbsoluteDeadlineSeconds, 115.0);

		TAtomic<int32> DuplicateCallbacks{0};
		Worker->StartReleaseNotes(false,
			[&DuplicateCallbacks](FWoTReleaseNotesResult&&)
			{
				++DuplicateCallbacks;
			});
		FPlatformProcess::Sleep(0.05f);
		TestEqual(TEXT("release notes run at most once per worker"),
			Hops->Requests.Num(), 1);
		TestEqual(TEXT("duplicate enqueue has no second outcome"),
			DuplicateCallbacks.Load(), 0);
	}
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecNotesHop(
			TEXT("{\"format\":\"wot-release-notes-v1\",\"notes\":[]}")));
		TUniquePtr<FWoTGameBootstrapWorker> Worker = MakeEstablished(Hops);
		if (!Worker.IsValid()) return false;
		FWoTReleaseNotesResult Result;
		TestTrue(TEXT("missing-entry notes complete"),
			WoTGameBootstrapWorkerSpecRunReleaseNotes(*Worker, false, Result));
		TestEqual(TEXT("valid asset without the sequence reports no-entry"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::NoEntry));
	}
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		FWoTGameBootstrapWorkerSpecHopStep Oversize;
		Oversize.Response.Status = 200;
		Oversize.Response.BodyLength = 0;
		Oversize.ExpectedUrl = WoTReleaseNotes::LatestUrl;
		Oversize.StreamChunks = {
			TArray<uint8>(), TArray<uint8>()};
		Oversize.StreamChunks[0].Init(static_cast<uint8>('x'),
			WoTReleaseNotes::MaximumFileBytes);
		Oversize.StreamChunks[1].Add(static_cast<uint8>('x'));
		Hops->Steps.Add(MoveTemp(Oversize));
		TUniquePtr<FWoTGameBootstrapWorker> Worker = MakeEstablished(Hops);
		if (!Worker.IsValid()) return false;
		FWoTReleaseNotesResult Result;
		TestTrue(TEXT("streaming-cap notes complete"),
			WoTGameBootstrapWorkerSpecRunReleaseNotes(*Worker, false, Result));
		TestEqual(TEXT("unknown-length streaming cap reports invalid"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::Invalid));
		TestEqual(TEXT("streaming cap aborts in one request"),
			Hops->Requests.Num(), 1);
	}
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		FWoTGameBootstrapWorkerSpecHopStep OversizeError;
		OversizeError.Response.Status = 404;
		OversizeError.Response.BodyLength = 0;
		OversizeError.ExpectedUrl = WoTReleaseNotes::LatestUrl;
		OversizeError.StreamChunks = {TArray<uint8>(), TArray<uint8>()};
		OversizeError.StreamChunks[0].Init(static_cast<uint8>('x'),
			WoTReleaseNotes::MaximumFileBytes);
		OversizeError.StreamChunks[1].Add(static_cast<uint8>('x'));
		Hops->Steps.Add(MoveTemp(OversizeError));
		TUniquePtr<FWoTGameBootstrapWorker> Worker = MakeEstablished(Hops);
		if (!Worker.IsValid()) return false;
		FWoTReleaseNotesResult Result;
		TestTrue(TEXT("error-body streaming-cap notes complete"),
			WoTGameBootstrapWorkerSpecRunReleaseNotes(*Worker, false, Result));
		TestEqual(TEXT("error bodies are capped while streaming"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::Invalid));
	}
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		TUniquePtr<FWoTGameBootstrapWorker> Worker = MakeEstablished(Hops);
		if (!Worker.IsValid()) return false;
		FWoTReleaseNotesResult Result;
		TestTrue(TEXT("failed notes fetch completes"),
			WoTGameBootstrapWorkerSpecRunReleaseNotes(*Worker, false, Result));
		TestEqual(TEXT("transport failure reports unavailable"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::Unavailable));
		TestEqual(TEXT("transport failure is not retried"),
			Hops->Requests.Num(), 1);
	}
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TSharedRef<TAtomic<int32>> ClockCalls = MakeShared<TAtomic<int32>>(0);
		TFunction<double()> Clock = [ClockCalls]()
		{
			return ++(*ClockCalls) >= 3 ? 116.0 : 100.0;
		};
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, FString(), Clock);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		ClockCalls->Store(0);
		FWoTReleaseNotesResult Result;
		TestTrue(TEXT("expired-deadline notes complete"),
			WoTGameBootstrapWorkerSpecRunReleaseNotes(*Worker, false, Result));
		TestEqual(TEXT("deadline exhaustion reports unavailable"),
			static_cast<int32>(Result.Outcome),
			static_cast<int32>(EWoTReleaseNotesOutcome::Unavailable));
		TestEqual(TEXT("expired deadline performs no request"),
			Hops->Requests.Num(), 0);
	}
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecNotesHop(Asset));
		Hops->BlockAtRequest = 0;
		TUniquePtr<FWoTGameBootstrapWorker> Worker = MakeEstablished(Hops);
		if (!Worker.IsValid()) return false;
		TAtomic<int32> CompletionCount{0};
		Worker->StartReleaseNotes(false,
			[&CompletionCount](FWoTReleaseNotesResult&&) { ++CompletionCount; });
		TestTrue(TEXT("notes fetch reaches the blocked request"),
			Hops->BlockEntered->Wait(FTimespan::FromSeconds(
				WoTGameBootstrapWorkerSpecHangSeconds)));
		// Only CancelCurrentRequest (or this spec) releases the blocked hop, so
		// a join that finishes before the spec releases it proves the abort.
		TFuture<void> Cancelling = Async(EAsyncExecution::Thread,
			[&Worker]() { Worker->CancelAndJoin(); });
		const bool bCancelledWithoutRelease = Cancelling.WaitFor(
			FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds));
		if (!bCancelledWithoutRelease)
		{
			Hops->BlockRelease->Trigger();
		}
		Cancelling.Wait();
		TestTrue(TEXT("notes cancellation aborts the request promptly"),
			bCancelledWithoutRelease && Hops->CancelCalls.Load() > 0);
		TestEqual(TEXT("notes cancellation suppresses delivery"),
			CompletionCount.Load(), 0);
	}
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecRouting,
	"WoT.Update.Worker.Routing",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecRouting::RunTest(const FString&)
{
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		WoTGameBootstrapWorkerSpecAppendAnnouncementScript(
			*Hops, Fixture.V1);
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		TestTrue(TEXT("clean recovered state is persistable"),
			Worker->PersistVerifiedResumeState());
		TestTrue(TEXT("identity recovery preserves the canonical live tree"),
			IFileManager::Get().FileExists(*FPaths::Combine(
				Fixture.Root.InstallRoot,
				WoTGameBootstrapWorkerSpecRootLeaf,
				WoTGameBootstrapWorkerSpecMainRelative)));

		FWoTBootstrapWorkerCompletion Current;
		TestTrue(TEXT("Current assessment completes"),
			WoTGameBootstrapWorkerSpecRunPhase(
				*Worker, EWoTBootstrapPhase::CheckingGameUpdates,
				2, Current));
		TestTrue(TEXT("Current succeeds"), Current.bSucceeded);
		TestEqual(TEXT("Current routes to original-game checks"),
			static_cast<int32>(Current.NextPhase),
			static_cast<int32>(
				EWoTBootstrapPhase::CheckingOriginalGameData));
		TestFalse(TEXT("Current is not mandatory"),
			Current.bMandatoryWork);
		TestEqual(TEXT("bootstrap Current performs no announcement request"),
			Hops->Requests.Num(), 0);
		FWoTUpdateState BackgroundCurrent;
		TestTrue(TEXT("background Current completes"),
			WoTGameBootstrapWorkerSpecRunBackground(
				*Worker, BackgroundCurrent));
		TestEqual(TEXT("background Current publishes None"),
			static_cast<int32>(BackgroundCurrent.Status),
			static_cast<int32>(EWoTUpdateStatus::None));
		TestEqual(TEXT("background Current fetches the announcement pair"),
			Hops->Requests.Num(), 2);

		FWoTBootstrapWorkerCompletion StartingWithoutVerification;
		TestTrue(TEXT("unverified StartingGame completion arrives"),
			WoTGameBootstrapWorkerSpecRunPhase(
				*Worker, EWoTBootstrapPhase::StartingGame, 3,
				StartingWithoutVerification));
		TestFalse(TEXT("StartingGame is no longer a pass-through"),
			StartingWithoutVerification.bSucceeded);
		TestEqual(TEXT("unverified StartingGame fails closed"),
			static_cast<int32>(StartingWithoutVerification.NextPhase),
			static_cast<int32>(EWoTBootstrapPhase::Fatal));
	}

	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Unavailable;
		TestTrue(TEXT("unavailable assessment completes"),
			WoTGameBootstrapWorkerSpecRunPhase(
				*Worker, EWoTBootstrapPhase::CheckingGameUpdates,
				2, Unavailable));
		TestTrue(TEXT("unavailable-before-knowledge fails open"),
			Unavailable.bSucceeded);
		TestFalse(TEXT("bootstrap does not classify network availability"),
			Unavailable.bAnnouncementUnavailable);
		TestEqual(TEXT("unavailable continues to original-game checks"),
			static_cast<int32>(Unavailable.NextPhase),
			static_cast<int32>(
				EWoTBootstrapPhase::CheckingOriginalGameData));
		FWoTUpdateState BackgroundOffline;
		TestTrue(TEXT("offline background check completes"),
			WoTGameBootstrapWorkerSpecRunBackground(
				*Worker, BackgroundOffline));
		TestEqual(TEXT("offline background check stays None"),
			static_cast<int32>(BackgroundOffline.Status),
			static_cast<int32>(EWoTUpdateStatus::None));
	}

	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)
			|| !Fixture.SeedUpdateState(*this)
			|| !Fixture.WriteInstaller(
				WoTGameBootstrapWorkerSpecInstallerBytes()))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Blocked;
		TestTrue(TEXT("blocked assessment completes"),
			WoTGameBootstrapWorkerSpecRunPhase(
				*Worker, EWoTBootstrapPhase::CheckingGameUpdates,
				2, Blocked));
		TestTrue(TEXT("known-newer bootstrap remains playable"),
			Blocked.bSucceeded);
		TestEqual(TEXT("pending-newer knowledge never forces bootstrap network"),
			Hops->Requests.Num(), 0);
		TestEqual(TEXT("local bootstrap route has no diagnostic"),
			static_cast<int32>(Blocked.DiagnosticCode),
			static_cast<int32>(EWoTBootstrapDiagnosticCode::None));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecTranslocatedNotifyOnly,
	"WoT.Update.Worker.NotifyOnly.TranslocatedSkipsRecovery",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecTranslocatedNotifyOnly::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
	int32 RecoveryCalls = 0;
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {},
			FPaths::Combine(Fixture.InstallRoot,
				WoTGameBootstrapWorkerSpecRootLeaf), {}, {}, {},
			[&RecoveryCalls]()
			{
				++RecoveryCalls;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("translocated startup must not recover activation")));
			}, Fixture.UserStateRoot, {},
			[](const FString&) { return true; });
	// The held update store this once timed against is gone and nothing on
	// this path waits with a timeout any more, so a short wall-clock budget
	// measured machine load, not skipping; only a hang exceeds this bound.
	const double EstablishStarted = FPlatformTime::Seconds();
	TestTrue(TEXT("translocated identity establishment remains playable"),
		WoTGameBootstrapWorkerSpecEstablish(*this, *Worker));
	TestTrue(TEXT("translocated notify-only skips the held store promptly"),
		FPlatformTime::Seconds() - EstablishStarted
			< WoTGameBootstrapWorkerSpecHangSeconds);
	TestEqual(TEXT("translocated identity establishment skips activation recovery"),
		RecoveryCalls, 0);
	FWoTBootstrapWorkerCompletion Checked;
	TestTrue(TEXT("translocated update assessment completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, Checked));
	TestTrue(TEXT("translocated update assessment is notify-only"),
		Checked.bSucceeded && Checked.bUpdateNotifyOnly);
	TestEqual(TEXT("translocated notify-only reason is preserved"),
		Checked.NotifyOnlyReason, FString(TEXT("translocated")));
	TestEqual(TEXT("translocated update assessment still skips recovery"),
		RecoveryCalls, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecConfigurationDiffers,
	"WoT.Update.Worker.NotifyOnly.ReleaseConfigurationDiffers",
	WoTGameBootstrapWorkerSpecFlags)

// A Test build offered the published Shipping release (the fixture manifest
// attests "Shipping") must not stage it: the Test launcher would discard the
// tree as torn and the next session would download it again.
bool FWoTGameBootstrapWorkerSpecConfigurationDiffers::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
	// The configuration is attested in the signed manifest, so the worker
	// reads the metadata; the archive is never requested.
	WoTGameBootstrapWorkerSpecAppendMetadataScript(*Hops, Fixture.V2);
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
			{}, {}, {}, {}, {}, {}, {}, {},
			[]() { return true; }, {}, {}, TEXT("Test"));
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Bootstrap;
	TestTrue(TEXT("bootstrap local check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, Bootstrap));
	TestTrue(TEXT("bootstrap local check succeeds"), Bootstrap.bSucceeded);
	FWoTUpdateState Completion;
	TestTrue(TEXT("the background assessment completes"),
		WoTGameBootstrapWorkerSpecRunBackground(*Worker, Completion));
	TestEqual(TEXT("a release of another configuration is only announced"),
		static_cast<int32>(Completion.Status),
		static_cast<int32>(EWoTUpdateStatus::Available));
	TestEqual(TEXT("the notify-only reason names the configuration"),
		Completion.Reason,
		FString(TEXT("release-configuration-differs")));
	TestEqual(TEXT("the announced release is still the authenticated one"),
		Completion.ReleaseSequence, uint64(2));
	TestEqual(TEXT("announcement and signed metadata only; no archive download"),
		Hops->Requests.Num(), 6);
	TestFalse(TEXT("nothing is staged for a configuration this copy can't run"),
		IFileManager::Get().DirectoryExists(*FPaths::Combine(
			Fixture.Root.StagingDir, TEXT("staged-2"))));

	FString Heartbeat;
	TestTrue(TEXT("the heartbeat is readable"),
		FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
			Fixture.UserStateRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName())));
	TestTrue(TEXT("the heartbeat names both configurations"),
		Heartbeat.Contains(
			TEXT("notify-only reason=release-configuration-differs detail=running=Test release=Shipping"),
			ESearchCase::CaseSensitive));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecAnnouncementRetry,
	"WoT.Update.Worker.AnnouncementReadRetriedOnce",
	WoTGameBootstrapWorkerSpecFlags)

// A single failed announcement read (seen on the Deck under Proton) is retried
// once within the same deadline; a second failure is not retried again.
bool FWoTGameBootstrapWorkerSpecAnnouncementRetry::RunTest(const FString&)
{
	auto ForcedNotWritable = []()
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::Io, TEXT("forced install-root probe refusal")));
	};
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this))
		{
			return false;
		}
		SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
		ON_SCOPE_EXIT
		{
			SetWoTBootstrapHeartbeatRootForTesting(FString());
		};
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				ForcedNotWritable);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Bootstrap;
		TestTrue(TEXT("bootstrap local check completes"),
			WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Bootstrap));
		FWoTUpdateState Completion;
		TestTrue(TEXT("the background check completes"),
			WoTGameBootstrapWorkerSpecRunBackground(*Worker, Completion));
		TestEqual(TEXT("the retried check still finds the release"),
			static_cast<int32>(Completion.Status),
			static_cast<int32>(EWoTUpdateStatus::Available));
		TestEqual(TEXT("the retried check reads the authenticated release"),
			Completion.ReleaseSequence, uint64(2));
		TestEqual(TEXT("one failed read plus the announcement pair"),
			Hops->Requests.Num(), 3);
		FString Heartbeat;
		TestTrue(TEXT("the heartbeat is readable"),
			FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
				Fixture.UserStateRoot, TEXT("UpdaterState"),
				WoTBootstrapHeartbeatFileName())));
		TestTrue(TEXT("the retry is recorded with the failed read"),
			Heartbeat.Contains(TEXT("\"event\":\"update-check-retry\""),
				ESearchCase::CaseSensitive)
			&& Heartbeat.Contains(TEXT("scripted worker transport failure"),
				ESearchCase::CaseSensitive));
	}
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				ForcedNotWritable);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Bootstrap;
		TestTrue(TEXT("bootstrap local check completes"),
			WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Bootstrap));
		FWoTUpdateState Completion;
		TestTrue(TEXT("the background check completes"),
			WoTGameBootstrapWorkerSpecRunBackground(*Worker, Completion));
		TestEqual(TEXT("two failed reads leave no update known"),
			static_cast<int32>(Completion.Status),
			static_cast<int32>(EWoTUpdateStatus::None));
		TestEqual(TEXT("exactly one retry: two requests, not three"),
			Hops->Requests.Num(), 2);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecNotifyOnly,
	"WoT.Update.Worker.NotifyOnly.InstallRootNotWritable",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecNotifyOnly::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
	int32 ProbeCalls = 0;
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
			[&ProbeCalls]()
			{
				++ProbeCalls;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("forced install-root probe refusal")));
			});
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	TestTrue(TEXT("read-only recovery is clean before an update is known"),
		Worker->PersistVerifiedResumeState());
	TestEqual(TEXT("identity establishment makes no writability probe"),
		ProbeCalls, 0);
	FWoTBootstrapWorkerCompletion Bootstrap;
	TestTrue(TEXT("bootstrap local check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, Bootstrap));
	TestTrue(TEXT("bootstrap local check succeeds"), Bootstrap.bSucceeded);
	TestEqual(TEXT("bootstrap local check stays zero-network"),
		Hops->Requests.Num(), 0);
	FWoTUpdateState Completion;
	TestTrue(TEXT("notify-only background assessment completes"),
		WoTGameBootstrapWorkerSpecRunBackground(*Worker, Completion));
	TestEqual(TEXT("the authenticated newer release triggers one probe"),
		ProbeCalls, 1);
	TestEqual(TEXT("notify-only state is Available"),
		static_cast<int32>(Completion.Status),
		static_cast<int32>(EWoTUpdateStatus::Available));
	TestEqual(TEXT("notify-only reason is exposed"),
		Completion.Reason,
		FString(TEXT("install-root-not-writable")));
	TestEqual(TEXT("notify-only sequence is the authenticated release"),
		Completion.ReleaseSequence, uint64(2));
	TestEqual(TEXT("notify-only release page link is exposed"),
		Completion.ReleasePageUrl,
		FString(TEXT("https://github.com/aleiby/wot/releases/tag/worker")));
	TestFalse(TEXT("notify-only creates no staged release container"),
		IFileManager::Get().DirectoryExists(*FPaths::Combine(
			Fixture.Root.StagingDir, TEXT("staged-2"))));

	FString Heartbeat;
	TestTrue(TEXT("notify-only heartbeat is readable"),
		FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
			Fixture.UserStateRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName())));
	TestTrue(TEXT("notify-only heartbeat names its reason"),
		Heartbeat.Contains(
			TEXT("notify-only reason=install-root-not-writable"),
			ESearchCase::CaseSensitive));
	TestTrue(TEXT("notify-only result heartbeat carries the published state"),
		Heartbeat.Contains(
			TEXT("\"detail\":\"available status=Available sequence=2 reason=install-root-not-writable\""),
			ESearchCase::CaseSensitive));
	Worker.Reset();
	const FWoTUpdateStateRecordReadResult State =
		ReadWoTUpdateState(Fixture.Root);
	TestEqual(TEXT("notify-only records the authenticated accepted sequence"),
		State.State.NewestAnnouncementSequence, uint64(2));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecDisabledPendingOffline,
	"WoT.Update.Worker.NotifyOnly.PendingOffline",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecDisabledPendingOffline::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this) || !Fixture.SeedUpdateState(*this))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> OfflineHops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, OfflineHops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
			{}, {}, Fixture.UserStateRoot, TEXT("reserved-live-leaf"));
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Checked;
	TestTrue(TEXT("disabled pending offline update check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, Checked));
	TestTrue(TEXT("disabled pending offline update check succeeds"),
		Checked.bSucceeded);
	TestTrue(TEXT("offline result remains notify-only"),
		Checked.bUpdateNotifyOnly);
	TestEqual(TEXT("offline result retains the disable reason"),
		Checked.NotifyOnlyReason, FString(TEXT("reserved-live-leaf")));
	TestTrue(TEXT("offline announcement is reported unavailable"),
		Checked.bAnnouncementUnavailable);
	TestEqual(TEXT("offline result advances to content checks"),
		Checked.NextPhase, EWoTBootstrapPhase::CheckingOriginalGameData);

	FWoTBootstrapWorkerCompletion ContentChecked;
	TestTrue(TEXT("disabled pending offline copy enters content work"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 3, ContentChecked));
	TestTrue(TEXT("content check remains playable"),
		ContentChecked.bSucceeded);
	TestEqual(TEXT("content check selects the original-game-needed phase"),
		ContentChecked.NextPhase,
		EWoTBootstrapPhase::OriginalGameNeeded);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecNotifyOnlyPublication,
	"WoT.Update.Worker.NotifyOnly.EverySuccessfulExit",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecNotifyOnlyPublication::RunTest(
	const FString&)
{
	auto CheckPublished = [this](const TCHAR* Label,
		const FWoTBootstrapWorkerCompletion& Completion)
	{
		TestTrue(FString::Printf(TEXT("%s succeeds"), Label),
			Completion.bSucceeded);
		TestTrue(FString::Printf(TEXT("%s publishes notify-only"), Label),
			Completion.bUpdateNotifyOnly);
		TestEqual(FString::Printf(TEXT("%s publishes its reason"), Label),
			Completion.NotifyOnlyReason,
			FString(TEXT("reserved-live-leaf")));
		TestEqual(FString::Printf(TEXT("%s enters content checks"), Label),
			Completion.NextPhase,
			EWoTBootstrapPhase::CheckingOriginalGameData);
	};

	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V1);
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				{}, {}, Fixture.UserStateRoot, TEXT("reserved-live-leaf"));
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Completion;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Completion))
		{
			return false;
		}
		CheckPublished(TEXT("current announcement exit"), Completion);
	}

	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)
			|| !Fixture.WriteInstaller(WoTGameBootstrapWorkerSpecInstallerBytes()))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				{}, {}, Fixture.UserStateRoot, TEXT("reserved-live-leaf"));
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Completion;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Completion))
		{
			return false;
		}
		CheckPublished(TEXT("adjacent installer exit"), Completion);
		TestEqual(TEXT("adjacent installer exit is offline"),
			Hops->Requests.Num(), 0);
	}

	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				{}, {}, Fixture.UserStateRoot, TEXT("reserved-live-leaf"));
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Completion;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Completion))
		{
			return false;
		}
		CheckPublished(TEXT("cached generation exit"), Completion);
		TestEqual(TEXT("cached generation exit is offline"),
			Hops->Requests.Num(), 0);
	}
	return !HasAnyErrors();
}

namespace
{
	/** A phase started without waiting: the completion lands in shared state,
	 * never in a stack frame the spec may already have left. */
	struct FWoTGameBootstrapWorkerSpecPendingPhase
	{
		FEvent* Done = FPlatformProcess::GetSynchEventFromPool(false);
		TAtomic<bool> bCompleted{false};
		FWoTBootstrapWorkerCompletion Completion;
		~FWoTGameBootstrapWorkerSpecPendingPhase()
		{
			FPlatformProcess::ReturnSynchEventToPool(Done);
		}
	};

	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase>
	WoTGameBootstrapWorkerSpecStartPhase(IWoTBootstrapWorker& Worker,
		EWoTBootstrapPhase Phase, uint64 Epoch)
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			MakeShared<FWoTGameBootstrapWorkerSpecPendingPhase>();
		Worker.StartPhase(WoTGameBootstrapWorkerSpecRequest(Phase, Epoch),
			[](FWoTBootstrapProgressEvent&&) {},
			[Pending](FWoTBootstrapWorkerCompletion&& Completion)
			{
				Pending->Completion = MoveTemp(Completion);
				Pending->bCompleted.Store(true);
				Pending->Done->Trigger();
			});
		return Pending;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecNotifyOnlyTerminalEarly,
	"WoT.Update.Worker.NotifyOnly.TerminalBeforeStoreAndValidation",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecNotifyOnlyTerminalEarly::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	int32 RecoveryCalls = 0;
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {}, {},
			[&RecoveryCalls]()
			{
				++RecoveryCalls;
				return FWoTBootstrapStatus::Success();
			}, {}, TEXT("preexisting-notify-only"));
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	TestEqual(TEXT("terminal notify-only skips real and injected recovery"),
		RecoveryCalls, 0);

	FWoTBootstrapWorkerRequest Malformed;
	Malformed.Phase = EWoTBootstrapPhase::CheckingGameUpdates;
	Malformed.OperationEpoch = 2;
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		MakeShared<FWoTGameBootstrapWorkerSpecPendingPhase>();
	Worker->StartPhase(Malformed, [](FWoTBootstrapProgressEvent&&) {},
		[Pending](FWoTBootstrapWorkerCompletion&& Completion)
		{
			Pending->Completion = MoveTemp(Completion);
			Pending->bCompleted.Store(true);
			Pending->Done->Trigger();
		});
	TestTrue(TEXT("notify-only precedes malformed update-request validation"),
		Pending->Done->Wait(FTimespan::FromSeconds(
			WoTGameBootstrapWorkerSpecHangSeconds))
			&& Pending->bCompleted.Load()
			&& Pending->Completion.bSucceeded
			&& Pending->Completion.bUpdateNotifyOnly);
	TestEqual(TEXT("terminal notify-only preserves its first reason"),
		Pending->Completion.NotifyOnlyReason,
		FString(TEXT("preexisting-notify-only")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecInstallRootIdentityMismatch,
	"WoT.Update.Worker.NotifyOnly.InstallRootIdentityMismatch",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecInstallRootIdentityMismatch::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	const FString ForeignRoot = Fixture.InstallRoot + TEXT("-foreign-identity");
	if (!IFileManager::Get().MakeDirectory(*ForeignRoot, true)) return false;
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*ForeignRoot, false, true);
	};
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> HeldForeign =
		OpenNativeDirectoryNoFollow(ForeignRoot);
	if (!TestFalse(TEXT("foreign install-root identity can be captured"),
		HeldForeign.IsError())) return false;

	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {}, {},
			{}, {}, {}, {}, HeldForeign.GetValue().GetIdentity());
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTBootstrapWorkerCompletion Checked;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, Checked)) return false;
	TestTrue(TEXT("an InstallRoot identity change preserves game startup"),
		Checked.bSucceeded && Checked.bUpdateNotifyOnly);
	TestEqual(TEXT("an InstallRoot identity change has the precise reason"),
		Checked.NotifyOnlyReason,
		FString(TEXT("install-root-identity-changed")));
	TestEqual(TEXT("an InstallRoot identity change performs no network I/O"),
		Hops->Requests.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecContentRouting,
	"WoT.Update.Worker.ContentRouting",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecContentRouting::RunTest(const FString&)
{
	// A valid current snapshot survives the update and content checks and
	// supplies the same activation identity to both cache views.
	{
		FWoTGameBootstrapWorkerSpecFixture Warm;
		if (!Warm.Initialize(*this) || !Warm.PublishActiveCache(*this))
			return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> WarmHops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> WarmWorker =
			WoTGameBootstrapWorkerSpecMakeWorker(Warm.InstallRoot, WarmHops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *WarmWorker))
			return false;
		int32 CriticalReads = 0;
		auto ReadObserver = WoTObserveCacheSnapshotPhysicalReadsForTesting(
			[&](FStringView) { ++CriticalReads; });
		FWoTBootstrapWorkerCompletion Update, Content, Verified, Started;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*WarmWorker,
				EWoTBootstrapPhase::CheckingGameUpdates, 1, Update)) return false;
		TestEqual(TEXT("critical hashes do not precede the warm timing interval"), CriticalReads, 0);
		if (!WoTGameBootstrapWorkerSpecRunPhase(*WarmWorker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Content)) return false;
		TestTrue(TEXT("critical hashes occur inside the warm content timing interval"), CriticalReads > 0);
		if (!WoTGameBootstrapWorkerSpecRunPhase(*WarmWorker,
				EWoTBootstrapPhase::VerifyingContent, 3, Verified)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*WarmWorker,
				EWoTBootstrapPhase::StartingGame, 4, Started))
			return false;
		TestTrue(TEXT("warm current bypasses installer"), Content.bSucceeded
			&& Content.NextPhase == EWoTBootstrapPhase::VerifyingContent);
		TestTrue(TEXT("warm current activates"), Started.bSucceeded
			&& Started.CacheSnapshot.IsSet());
		TestEqual(TEXT("warm current is zero-network"), WarmHops->Requests.Num(), 0);
		if (Started.CacheSnapshot.IsSet())
		{
			TestTrue(TEXT("activation owns a live verified snapshot"),
				Started.CacheSnapshot->IsValid());
		}
	}
	// A cache recorded under the first public release's label stays current
	// while this binary is at content revision 1, even with the installer
	// beside the game; any later revision rebuilds it from the installer.
	{
		const TWoTBootstrapResult<FWoTSha256> FirstReleaseLabel = FWoTSha256::FromHex(
			TEXT("1a7e19b367523c11d9ad6c7fda42a39472a68d4b8c52233b6b2d568ca3daa58e"));
		if (!TestFalse(TEXT("first public release label parses"),
				FirstReleaseLabel.IsError()))
			return false;
		FWoTGameBootstrapWorkerSpecFixture Released;
		if (!Released.Initialize(*this)
			|| !Released.PublishActiveCache(*this, TOptional<FWoTSha256>(),
				TOptional<FWoTSha256>(FirstReleaseLabel.GetValue()))
			|| !Released.WriteInstaller(WoTGameBootstrapWorkerSpecInstallerBytes()))
			return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> ReleasedHops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> ReleasedWorker =
			WoTGameBootstrapWorkerSpecMakeWorker(Released.InstallRoot, ReleasedHops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *ReleasedWorker))
			return false;
		FWoTBootstrapWorkerCompletion Update, Content;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*ReleasedWorker,
				EWoTBootstrapPhase::CheckingGameUpdates, 1, Update)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*ReleasedWorker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Content))
			return false;
		if (GetCacheContentRevision() == 1)
		{
			FWoTBootstrapWorkerCompletion Verified, Started;
			if (!WoTGameBootstrapWorkerSpecRunPhase(*ReleasedWorker,
					EWoTBootstrapPhase::VerifyingContent, 3, Verified)
				|| !WoTGameBootstrapWorkerSpecRunPhase(*ReleasedWorker,
					EWoTBootstrapPhase::StartingGame, 4, Started))
				return false;
			TestTrue(TEXT("first-release cache bypasses installer at revision 1"),
				Content.bSucceeded
				&& Content.NextPhase == EWoTBootstrapPhase::VerifyingContent);
			TestTrue(TEXT("first-release cache activates at revision 1"),
				Started.bSucceeded && Started.CacheSnapshot.IsSet());
			TestEqual(TEXT("first-release cache is zero-network at revision 1"),
				ReleasedHops->Requests.Num(), 0);
		}
		else
		{
			TestTrue(TEXT("first-release cache selects installer after a revision bump"),
				Content.bSucceeded
				&& Content.NextPhase == EWoTBootstrapPhase::ExtractingOriginalGame);
		}
	}
	for (int32 Mismatch = 0; Mismatch < 3; ++Mismatch)
	{
		FWoTGameBootstrapWorkerSpecFixture Stale;
		const FWoTSha256 Wrong = WoTGameBootstrapWorkerSpecDigest(
			static_cast<uint8>(0x44 + Mismatch));
		if (!Stale.Initialize(*this)
			|| !Stale.PublishActiveCache(*this,
				Mismatch == 0 ? TOptional<FWoTSha256>(Wrong)
					: TOptional<FWoTSha256>(),
				Mismatch == 1 ? TOptional<FWoTSha256>(Wrong)
					: TOptional<FWoTSha256>(),
				Mismatch == 2 ? TOptional<FWoTSha256>(Wrong)
					: TOptional<FWoTSha256>())
			|| !Stale.WriteInstaller(WoTGameBootstrapWorkerSpecInstallerBytes()))
			return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Stale.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData,
				20 + Mismatch, Checked)) return false;
		TestTrue(TEXT("stale cache selects installer"), Checked.bSucceeded
			&& Checked.NextPhase == EWoTBootstrapPhase::ExtractingOriginalGame);
	}
	// The damaged marker written by a use-time read excludes current at start.
	{
		FWoTGameBootstrapWorkerSpecFixture Damaged;
		if (!Damaged.Initialize(*this)
			|| !Damaged.PublishActiveCache(*this)
			|| !Damaged.WriteInstaller(WoTGameBootstrapWorkerSpecInstallerBytes()))
			return false;
		const FString Marker = FPaths::Combine(
			WoTGameBootstrapWorkerSpecCacheRoot(Damaged.InstallRoot),
			TEXT("current"), TEXT("damaged"));
		const TArray<uint8> EmptyMarker;
		if (!TestTrue(TEXT("damage marker is written"),
			FFileHelper::SaveArrayToFile(EmptyMarker, *Marker))) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Damaged.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 30, Checked))
			return false;
		TestTrue(TEXT("marked cache selects installer"), Checked.bSucceeded
			&& Checked.NextPhase == EWoTBootstrapPhase::ExtractingOriginalGame);
	}
	// An empty current cache selects the pinned adjacent installer locally.
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)
		|| !Fixture.WriteInstaller(WoTGameBootstrapWorkerSpecInstallerBytes()))
		return false;
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTBootstrapWorkerCompletion UpdateChecked, Checked;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingGameUpdates, 10, UpdateChecked)
		|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 11, Checked))
		return false;
	TestTrue(TEXT("fresh cache selects the installer"),
		Checked.bSucceeded
			&& Checked.NextPhase == EWoTBootstrapPhase::ExtractingOriginalGame);
	TestEqual(TEXT("fresh cache makes no network requests"),
		Hops->Requests.Num(), 0);

	// A failed publication cannot re-enter installer selection indefinitely.
	FWoTBootstrapWorkerCompletion Verified;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::VerifyingContent, 12, Verified))
		return false;
	TestFalse(TEXT("missing published current fails verification"),
		Verified.bSucceeded);
	TestFalse(TEXT("missing current does not request another rebuild"),
		Verified.NextPhase == EWoTBootstrapPhase::CheckingOriginalGameData);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecCacheFailures,
	"WoT.Update.Worker.CacheFailures",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecCacheFailures::RunTest(const FString&)
{
	// Another shared profile owner no longer contends with immutable snapshot
	// discovery. Cache mutation takes the exclusive process session separately.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		auto Held = AcquireWoTProfileSessionShared(Fixture.UserStateRoot);
		if (!TestFalse(TEXT("additional shared session opens"), Held.IsError()))
			return false;
		auto Hops = MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		auto Worker = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Update, Content;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, Update)) return false;
		TestTrue(TEXT("update check performs no cache open or critical hashing"), Update.bSucceeded);
		// Profile locks are taken without waiting (contention fails at once,
		// which the assertions below catch) and nothing else here waits with
		// a timeout, so completing while the other session is still held
		// proves discovery nonblocking; a one-second budget measured load.
		const bool bDiscovered = WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 3, Content);
		TestTrue(TEXT("shared snapshot discovery is nonblocking"), bDiscovered);
		if (!bDiscovered) return false;
		TestTrue(TEXT("shared session permits normal content discovery"),
			Content.bSucceeded);
		TestEqual(TEXT("missing cache still requests original game data"),
			Content.NextPhase, EWoTBootstrapPhase::OriginalGameNeeded);
		TestTrue(TEXT("shared session produces no cache failure message"),
			Content.FailureMessage.IsEmpty());
		TestTrue(TEXT("shared session produces no cache failure detail"),
			Content.FailureDetail.IsEmpty());
	}

	TArray<uint8> Installer;
	if (!TestTrue(TEXT("build-failure Inno fixture loads"),
		WoTGameBootstrapWorkerSpecLoadInnoInstaller(Installer))) return false;
	const FWoTRetailContract Contract = WoTGameBootstrapWorkerSpecContentContract();
	FWoTGameBootstrapWorkerSpecContractScope ContractScope(Contract);
#if PLATFORM_WINDOWS
	const uint32 NativeErrors[] = {112, 5};
#else
	const uint32 NativeErrors[] = {ENOSPC, EACCES};
#endif
	for (int32 Case = 0; Case < 2; ++Case)
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		const FString CacheRoot = WoTGameBootstrapWorkerSpecCacheRoot(Fixture.InstallRoot);
		int32 BuildAttempts = 0;
		auto Hops = MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		auto Worker = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, WoTGameBootstrapWorkerSpecInnoPins(Installer),
			[&](FWoTRetailPackage&, const FWoTRetailContract& InContract,
				IWoTObjectWriter& Writer, IWoTProgressSink&)
			{
				++BuildAttempts;
				const auto& Output = InContract.Facts[0].Outputs[0];
				const TArray<uint8> Bytes = WoTGameBootstrapWorkerSpecAscii(TEXT("partial-build"));
				auto Written = Writer.WriteObject(Output.LogicalId, Output.Schema,
					Bytes, Sha256(Bytes), Output.MaxDecodedSize);
				if (Written.IsError())
					return TWoTBootstrapResult<FWoTCacheBuildResult>::Error(MoveTemp(Written).GetError());
				FWoTBootstrapError Error = MakeWoTBootstrapError(EWoTBootstrapError::Io,
					TEXT("injected cache destination I/O failure"), TEXT("building/blobs"));
				Error.NativeCode = NativeErrors[Case];
				return TWoTBootstrapResult<FWoTCacheBuildResult>::Error(MoveTemp(Error));
			});
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Content, Extracted, Built;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Content)
			|| !TestTrue(TEXT("missing current selects extraction"), Content.bSucceeded
				&& Content.NextPhase == EWoTBootstrapPhase::ExtractingOriginalGame)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::ExtractingOriginalGame, 3, Extracted)
			|| !TestTrue(TEXT("the real package prepares a build"), Extracted.bSucceeded
				&& Extracted.NextPhase == EWoTBootstrapPhase::BuildingContent)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::BuildingContent, 4, Built)) return false;
		TestFalse(TEXT("cache I/O failure does not publish"), Built.bSucceeded);
		TestEqual(TEXT("cache I/O failure does not retry"), Built.NextPhase, EWoTBootstrapPhase::Fatal);
		TestEqual(TEXT("only one build was attempted"), BuildAttempts, 1);
		TestTrue(TEXT("cache I/O failure gives actionable instructions"),
			Built.FailureMessage.Contains(Case == 0 ? TEXT("Free space") : TEXT("permission")));
		TestTrue(TEXT("cache I/O message names the absolute cache folder"),
			Built.FailureMessage.Contains(FPaths::ConvertRelativePathToFull(CacheRoot)));
		TestTrue(TEXT("cache I/O retains diagnostic detail"),
			Built.FailureDetail.Contains(TEXT("injected cache destination I/O failure")));
		TestFalse(TEXT("an incomplete build never creates current"),
			IFileManager::Get().DirectoryExists(*FPaths::Combine(CacheRoot, TEXT("current"))));
		auto Store = FWoTCacheStore::ForTesting(CacheRoot);
		if (!TestFalse(TEXT("failed build releases the exclusive lock"), Store.IsError())) return false;
		auto Current = Store.GetValue().TryOpenCurrent({}, nullptr);
		if (!TestFalse(TEXT("failed build permits a new read"), Current.IsError())) return false;
		TestFalse(TEXT("failed build cannot be opened as current"), Current.GetValue().IsSet());
	}

	// A build refused because another copy holds the shared profile lock must
	// tell the player to close it, not blame an unwritable folder.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		auto OtherCopy = AcquireWoTProfileSessionShared(Fixture.UserStateRoot);
		if (!TestFalse(TEXT("another copy holds the shared lock"), OtherCopy.IsError()))
			return false;
		int32 BuildAttempts = 0;
		auto Hops = MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		auto Worker = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, WoTGameBootstrapWorkerSpecInnoPins(Installer),
			[&](FWoTRetailPackage&, const FWoTRetailContract&, IWoTObjectWriter&,
				IWoTProgressSink&)
			{
				++BuildAttempts;
				return TWoTBootstrapResult<FWoTCacheBuildResult>::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("a contended build must never start")));
			});
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Content, Extracted, Built;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Content)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::ExtractingOriginalGame, 3, Extracted)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::BuildingContent, 4, Built)) return false;
		TestFalse(TEXT("a contended build does not publish"), Built.bSucceeded);
		TestEqual(TEXT("a contended build never starts writing"), BuildAttempts, 0);
		TestTrue(TEXT("a contended build tells the player to close other copies"),
			Built.FailureMessage.Contains(TEXT("Close other copies of WoT Reborn")));
	}
	return !HasAnyErrors();
}

namespace
{
/** Changes the installer's last byte in place after the worker checked and
 * holds it, through a handle that shares with that read handle, until a fresh
 * open sees the file's change identity move. */
bool WoTGameBootstrapWorkerSpecChangeHeldInstaller(
	const FString& Path, TConstArrayView<uint8> Original)
{
	if (Original.Num() == 0)
	{
		return false;
	}
	FWoTNativeFileIdentity Checked;
	{
		TWoTBootstrapResult<FWoTNativeFileHandle> Before =
			OpenNativeFileReadNoFollow(Path);
		if (Before.IsError())
		{
			return false;
		}
		Checked = Before.GetValue().GetIdentity();
	}
	const uint8 Changed = Original[Original.Num() - 1] ^ 0x5a;
	for (int32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		// Step past the file-time granularity (<= 15.6 ms on Windows).
		FPlatformProcess::Sleep(0.02f);
		{
			TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get()
				.GetPlatformFile().OpenWrite(*Path, true, true));
			if (!Handle.IsValid() || !Handle->Seek(Original.Num() - 1)
				|| !Handle->Write(&Changed, 1))
			{
				return false;
			}
		}
		TWoTBootstrapResult<FWoTNativeFileHandle> After =
			OpenNativeFileReadNoFollow(Path);
		if (!After.IsError() && After.GetValue().GetIdentity() != Checked)
		{
			return true;
		}
	}
	return false;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecContentFailureMessages,
	"WoT.Update.Worker.ContentFailureMessages",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecContentFailureMessages::RunTest(const FString&)
{
	// The fatal screen shows FailureMessage. An installer that stops matching
	// is told apart from a cache problem by where the error came from, not by
	// its code: the reader and the cache store both report CorruptInstallation.
	// (Disk full, permission and lock contention: WoT.Update.Worker.CacheFailures.)
	TArray<uint8> Installer;
	if (!TestTrue(TEXT("Inno fixture loads"),
		WoTGameBootstrapWorkerSpecLoadInnoInstaller(Installer))) return false;
	const FWoTRetailContract Contract = WoTGameBootstrapWorkerSpecContentContract();
	FWoTGameBootstrapWorkerSpecContractScope ContractScope(Contract);
	const FWoTInstallerAcquisitionPins Pins =
		WoTGameBootstrapWorkerSpecInnoPins(Installer);
	const FString InstallerAdvice = TEXT(
		"WoT Reborn could not read the original game's installer, or the installer "
		"no longer matched the one it checked. Relaunch WoT Reborn to try again. "
		"If it asks for the installer, choose it again with Browse.");
	const FString CacheAdvice = TEXT("could not prepare its content cache");
	using FBuildResult = TWoTBootstrapResult<FWoTCacheBuildResult>;

	// One fresh install up to the end of BuildingContent, Build standing in for
	// the retail reader (it gets the real held package and cache writer).
	auto BuildWith = [this, &Pins](const FString& What,
		const FWoTGameBootstrapWorkerSpecFixture& Fixture,
		FWoTCacheBootstrapOperation::FBuildForTesting Build,
		FWoTBootstrapWorkerCompletion& Built) -> bool
	{
		auto Hops = MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		auto Worker = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, Pins, MoveTemp(Build));
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Content, Extracted;
		return WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Content)
			&& TestTrue(What + TEXT(": the installer is found"), Content.bSucceeded
				&& Content.NextPhase == EWoTBootstrapPhase::ExtractingOriginalGame)
			&& WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::ExtractingOriginalGame, 3, Extracted)
			&& TestTrue(What + TEXT(": the real package prepares a build"),
				Extracted.bSucceeded
				&& Extracted.NextPhase == EWoTBootstrapPhase::BuildingContent)
			&& WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::BuildingContent, 4, Built)
			&& TestFalse(What + TEXT(": the build fails"), Built.bSucceeded)
			&& TestEqual(What + TEXT(": the failure is fatal"),
				Built.NextPhase, EWoTBootstrapPhase::Fatal);
	};

	// The v0.1.1 report: the held installer changes while content builds, and
	// the package's own held-snapshot check refuses it.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		const FString InstallerPath =
			WoTGameBootstrapWorkerSpecInstallerPath(Fixture.InstallRoot);
		bool bChanged = false;
		FWoTBootstrapWorkerCompletion Built;
		if (!BuildWith(TEXT("changed installer"), Fixture,
			[&](FWoTRetailPackage& Package, const FWoTRetailContract&,
				IWoTObjectWriter&, IWoTProgressSink&)
			{
				bChanged = WoTGameBootstrapWorkerSpecChangeHeldInstaller(
					InstallerPath, Installer);
				FWoTBootstrapStatus Reverified =
					Package.ReverifyHeldSnapshotAfterExtraction();
				return Reverified.IsError()
					? FBuildResult::Error(MoveTemp(Reverified).GetError())
					: FBuildResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
						TEXT("the changed installer still verified")));
			}, Built)) return false;
		TestTrue(TEXT("the held installer changed during the build"), bChanged);
		TestTrue(TEXT("the package's held-snapshot check refused it"),
			Built.FailureDetail.StartsWith(TEXT("CorruptInstallation:"))
			&& Built.FailureDetail.Contains(TEXT("(held-installer-snapshot)")));
		TestEqual(TEXT("a changed installer gets the installer advice"),
			Built.FailureMessage, InstallerAdvice);
		TestEqual(TEXT("the diagnostic code is unchanged"),
			Built.DiagnosticCode, EWoTBootstrapDiagnosticCode::BootstrapFailure);
		TestTrue(TEXT("the detail keeps its native suffix"),
			Built.FailureDetail.EndsWith(TEXT(" native=0")));
	}

	// The reader can surface another error first (here a read error); the
	// installer still takes the blame once it no longer verifies...
	for (const bool bChangeInstaller : {true, false})
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		const FString InstallerPath =
			WoTGameBootstrapWorkerSpecInstallerPath(Fixture.InstallRoot);
		const FString What = bChangeInstaller
			? TEXT("read error on a changed installer")
			: TEXT("read error on an unchanged installer");
		bool bChanged = false;
		FWoTBootstrapWorkerCompletion Built;
		if (!BuildWith(What, Fixture,
			[&](FWoTRetailPackage&, const FWoTRetailContract&,
				IWoTObjectWriter&, IWoTProgressSink&)
			{
				bChanged = bChangeInstaller
					&& WoTGameBootstrapWorkerSpecChangeHeldInstaller(InstallerPath, Installer);
				return FBuildResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
					TEXT("an installer read stopped before any corruption check")));
			}, Built)) return false;
		TestEqual(What + TEXT(": the installer change happened as arranged"),
			bChanged, bChangeInstaller);
		if (bChangeInstaller)
		{
			TestEqual(What + TEXT(": gets the installer advice"),
				Built.FailureMessage, InstallerAdvice);
		}
		else
		{
			// ...and an installer that still verifies is not blamed.
			TestTrue(What + TEXT(": keeps the cache advice"),
				Built.FailureMessage.Contains(CacheAdvice)
				&& !Built.FailureMessage.Contains(TEXT("original game's installer")));
		}
	}

	// The cache store refuses a write with CorruptInstallation: a cache problem,
	// not an installer one, though the code is the reader's too.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		FWoTBootstrapWorkerCompletion Built;
		if (!BuildWith(TEXT("refused cache write"), Fixture,
			[](FWoTRetailPackage&, const FWoTRetailContract& InContract,
				IWoTObjectWriter& Writer, IWoTProgressSink&)
			{
				const FWoTRequiredFactOutput& Output = InContract.Facts[0].Outputs[0];
				const TArray<uint8> Bytes =
					WoTGameBootstrapWorkerSpecAscii(TEXT("worker-cache-object"));
				const TArray<uint8> Other =
					WoTGameBootstrapWorkerSpecAscii(TEXT("other-bytes"));
				FWoTBootstrapStatus Written = Writer.WriteObject(Output.LogicalId,
					Output.Schema, Bytes, Sha256(Other), Output.MaxDecodedSize);
				return Written.IsError()
					? FBuildResult::Error(MoveTemp(Written).GetError())
					: FBuildResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io,
						TEXT("the cache store accepted a wrong digest")));
			}, Built)) return false;
		TestTrue(TEXT("the cache store raised CorruptInstallation"),
			Built.FailureDetail.StartsWith(TEXT("CorruptInstallation:"))
			&& Built.FailureDetail.Contains(TEXT("invalid digest")));
		TestTrue(TEXT("a refused cache write keeps the cache advice and folder"),
			Built.FailureMessage.Contains(CacheAdvice)
			&& Built.FailureMessage.Contains(FPaths::ConvertRelativePathToFull(
				WoTGameBootstrapWorkerSpecCacheRoot(Fixture.InstallRoot))));
		TestFalse(TEXT("a refused cache write never blames the installer"),
			Built.FailureMessage.Contains(TEXT("original game's installer")));
	}

	// The cache store rejects the finished build at publication.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		FWoTBootstrapWorkerCompletion Built;
		if (!BuildWith(TEXT("rejected publication"), Fixture,
			[](FWoTRetailPackage& Package, const FWoTRetailContract& InContract,
				IWoTObjectWriter& Writer, IWoTProgressSink& Progress)
			{
				FBuildResult Build = WoTGameBootstrapWorkerSpecContentBuild(
					Package, InContract, Writer, Progress);
				if (Build.IsError()) return Build;
				FWoTCacheBuildResult Result = MoveTemp(Build).GetValue();
				Result.ManifestSha256 = FWoTSha256();
				return FBuildResult::Success(MoveTemp(Result));
			}, Built)) return false;
		TestTrue(TEXT("publication raised CorruptInstallation"),
			Built.FailureDetail.StartsWith(TEXT("CorruptInstallation:"))
			&& Built.FailureDetail.Contains(TEXT("does not describe the completed build")));
		TestTrue(TEXT("a rejected publication keeps the cache advice"),
			Built.FailureMessage.Contains(CacheAdvice)
			&& !Built.FailureMessage.Contains(TEXT("original game's installer")));
	}

#if PLATFORM_WINDOWS
	// Another program keeps a folder below building/ open (a scanner or an
	// Explorer window), so publishing building -> current is refused with
	// access denied until the rename retry gives up: not a permission problem.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		const FString Blobs = FPaths::Combine(
			WoTGameBootstrapWorkerSpecCacheRoot(Fixture.InstallRoot),
			TEXT("building"), TEXT("blobs"));
		WoTSetNativeDirectoryRenameRetryBudgetForTesting(0.3);
		ON_SCOPE_EXIT { WoTSetNativeDirectoryRenameRetryBudgetForTesting(-1.0); };
		FWoTNativeDirectoryHandle Holder;
		FWoTBootstrapWorkerCompletion Built;
		if (!BuildWith(TEXT("publication held by another program"), Fixture,
			[&Blobs, &Holder](FWoTRetailPackage& Package,
				const FWoTRetailContract& InContract, IWoTObjectWriter& Writer,
				IWoTProgressSink& Progress)
			{
				FBuildResult Build = WoTGameBootstrapWorkerSpecContentBuild(
					Package, InContract, Writer, Progress);
				TWoTBootstrapResult<FWoTNativeDirectoryHandle> Held =
					OpenNativeDirectoryNoFollow(Blobs);
				if (!Held.IsError())
				{
					Holder = MoveTemp(Held).GetValue();
				}
				return Build;
			}, Built)) return false;
		TestTrue(TEXT("another holder kept a folder below building open"),
			Holder.IsValid());
		Holder = FWoTNativeDirectoryHandle();
		TestTrue(TEXT("the publish rename was refused after its retries"),
			Built.FailureDetail.Contains(TEXT("NTSTATUS 0xC0000022"))
			&& Built.FailureDetail.EndsWith(TEXT(" native=5")));
		TestTrue(TEXT("a held publication says another program may be using the files"),
			Built.FailureMessage.StartsWith(TEXT("Another program, "))
			&& Built.FailureMessage.Contains(TEXT("may be using WoT Reborn's files")));
		TestFalse(TEXT("a held publication is not called a permission problem"),
			Built.FailureMessage.Contains(TEXT("permission")));
	}
#endif

	// The installer changes after it was checked but before extraction opens
	// it: the same advice, with the extraction diagnostic code unchanged.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this) || !Fixture.WriteInstaller(Installer)) return false;
		auto Hops = MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		auto Worker = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, Pins,
			WoTGameBootstrapWorkerSpecContentBuild);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Content, Extracted;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Content)
			|| !TestEqual(TEXT("the installer is found and held"),
				Content.NextPhase, EWoTBootstrapPhase::ExtractingOriginalGame)
			|| !TestTrue(TEXT("the held installer changes before extraction"),
				WoTGameBootstrapWorkerSpecChangeHeldInstaller(
					WoTGameBootstrapWorkerSpecInstallerPath(Fixture.InstallRoot),
					Installer))
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::ExtractingOriginalGame, 3, Extracted)) return false;
		TestEqual(TEXT("extraction of a changed installer is fatal"),
			Extracted.NextPhase, EWoTBootstrapPhase::Fatal);
		TestEqual(TEXT("extraction keeps its installer diagnostic code"),
			Extracted.DiagnosticCode, EWoTBootstrapDiagnosticCode::InstallerInvalid);
		TestEqual(TEXT("extraction of a changed installer gets the installer advice"),
			Extracted.FailureMessage, InstallerAdvice);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecOriginalGameNeeded,
	"WoT.Update.Worker.OriginalGameNeeded",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecOriginalGameNeeded::RunTest(const FString&)
{
	// Every sweep of the waiting phase publishes one progress event, so
	// Sweeps counts completed polls (entry included).
	auto StartWaitingPhase = [](IWoTBootstrapWorker& Worker, uint64 Epoch,
		FEvent* Entered, TAtomic<int32>* LastReason = nullptr,
		TAtomic<bool>* SawNotTheInstaller = nullptr,
		TAtomic<int32>* Sweeps = nullptr)
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			MakeShared<FWoTGameBootstrapWorkerSpecPendingPhase>();
		Worker.StartPhase(
			WoTGameBootstrapWorkerSpecRequest(
				EWoTBootstrapPhase::OriginalGameNeeded, Epoch),
			[Entered, LastReason, SawNotTheInstaller, Sweeps](
				FWoTBootstrapProgressEvent&& Event)
			{
				if (Sweeps != nullptr)
				{
					++(*Sweeps);
				}
				if (Event.OriginalGameNeededReason.IsSet())
				{
					const EWoTOriginalGameNeededReason Reason =
						Event.OriginalGameNeededReason.GetValue();
					if (LastReason != nullptr)
					{
						LastReason->Store(static_cast<int32>(Reason));
					}
					if (SawNotTheInstaller != nullptr
						&& Reason
							== EWoTOriginalGameNeededReason::NotTheInstaller)
					{
						SawNotTheInstaller->Store(true);
					}
				}
				Entered->Trigger();
			},
			[Pending](FWoTBootstrapWorkerCompletion&& Completion)
			{
				Pending->Completion = MoveTemp(Completion);
				Pending->bCompleted.Store(true);
				Pending->Done->Trigger();
			});
		return Pending;
	};
	auto WaitForReason = [](const TAtomic<int32>& LastReason,
		EWoTOriginalGameNeededReason Expected)
	{
		const double Deadline =
			FPlatformTime::Seconds() + WoTGameBootstrapWorkerSpecHangSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if (LastReason.Load() == static_cast<int32>(Expected))
			{
				return true;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return false;
	};
	// The waiting phase polls every OriginalGameProbeIntervalSeconds (0.01 s
	// in these specs); "not rehashed" used to sleep 0.1 s, about ten polls,
	// which a loaded machine may not run. Wait for the polls themselves.
	constexpr int32 NegativeCheckSweeps = 10;

	// The long-lived phase observes a copy made after entry, requires two
	// identical full native identities, hashes once, and hands that exact held
	// snapshot to extraction without any transport request or process restart.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TAtomic<int32> Hashes{0};
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks,
				{}, {}, {}, {}, {}, {}, {}, {},
				[]() { return true; }, []()
				{
					return TWoTBootstrapResult<bool>::Success(true);
				});
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!TestTrue(TEXT("missing installer check completes"),
			WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 80, Checked)))
		{
			return false;
		}
		TestEqual(TEXT("missing installer waits"), Checked.NextPhase,
			EWoTBootstrapPhase::OriginalGameNeeded);
		FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			StartWaitingPhase(*Worker, 81, Entered);
		const bool bEntered = Entered->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds));
		if (!TestTrue(TEXT("long-lived waiting phase publishes entry"), bEntered)
			|| !TestTrue(TEXT("installer is copied after entry"),
				Fixture.WriteInstaller(
					WoTGameBootstrapWorkerSpecInstallerBytes())))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		Worker->RequestOriginalGameRecheck();
		if (!TestTrue(TEXT("post-entry copy is auto-detected"),
			Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds))))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		TestTrue(TEXT("auto-detect succeeds"),
			Pending->Completion.bSucceeded);
		TestEqual(TEXT("auto-detect routes directly to extraction"),
			Pending->Completion.NextPhase,
			EWoTBootstrapPhase::ExtractingOriginalGame);
		TestEqual(TEXT("stable copy is hashed exactly once"),
			Hashes.Load(), 1);
		TestEqual(TEXT("waiting path is zero-network"),
			Hops->Requests.Num(), 0);
	}

	// A known-wrong native identity is not rehashed every timer tick. Replacing
	// it produces a new identity, one fresh hash and acceptance in the same
	// worker process.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)
			|| !Fixture.WriteInstaller(
				WoTGameBootstrapWorkerSpecBadInstallerBytes()))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TAtomic<int32> Hashes{0};
		TAtomic<int32> Sweeps{0};
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!TestTrue(TEXT("wrong installer check completes"),
			WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 82, Checked)))
		{
			return false;
		}
		TestEqual(TEXT("wrong identity is hashed once during check"),
			Hashes.Load(), 1);
		TestEqual(TEXT("wrong installer reason is preserved"),
			Checked.OriginalGameNeededReason,
			EWoTOriginalGameNeededReason::NotTheInstaller);
		FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			StartWaitingPhase(*Worker, 83, Entered, nullptr, nullptr, &Sweeps);
		const bool bEntered = Entered->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds));
		if (!TestTrue(TEXT("wrong-installer wait publishes entry"), bEntered)
			|| !TestTrue(TEXT("the wrong installer is re-polled while waiting"),
				WoTGameBootstrapWorkerSpecWaitForAtLeast(
					Sweeps, Sweeps.Load() + NegativeCheckSweeps)))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		TestEqual(TEXT("known-wrong identity is not rehashed"),
			Hashes.Load(), 1);
		const FString InstallerPath =
			WoTGameBootstrapWorkerSpecInstallerPath(Fixture.InstallRoot);
		if (!TestTrue(TEXT("known-wrong identity can be removed"),
			IFileManager::Get().Delete(*InstallerPath, false, true, true))
			|| !TestTrue(TEXT("replacement installer is written"),
				Fixture.WriteInstaller(
					WoTGameBootstrapWorkerSpecInstallerBytes())))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		Worker->RequestOriginalGameRecheck();
		if (!TestTrue(TEXT("replacement is accepted without restart"),
			Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds))))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		TestEqual(TEXT("replacement is hashed exactly once"),
			Hashes.Load(), 2);
		TestEqual(TEXT("replacement routes to extraction"),
			Pending->Completion.NextPhase,
			EWoTBootstrapPhase::ExtractingOriginalGame);
		TestEqual(TEXT("replacement path is zero-network"),
			Hops->Requests.Num(), 0);
	}

	// A translocated launch skips adjacency but keeps polling Downloads.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)
			|| !Fixture.WriteInstaller(
				WoTGameBootstrapWorkerSpecInstallerBytes())) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TAtomic<int32> Hashes{0};
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks,
				{}, {}, {}, {}, [](const FString&) { return true; });
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!TestTrue(TEXT("translocated missing check completes"),
			WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 84, Checked)))
		{
			return false;
		}
		TestEqual(TEXT("translocated check skips a valid adjacent installer"),
			Checked.NextPhase, EWoTBootstrapPhase::OriginalGameNeeded);
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			WoTGameBootstrapWorkerSpecStartPhase(*Worker,
				EWoTBootstrapPhase::OriginalGameNeeded, 85);
		const FString DownloadsPath = FPaths::Combine(
			Fixture.InstallRoot + TEXT("-downloads"),
			WoT::Bootstrap::InstallerName);
		TestTrue(TEXT("translocated Downloads installer is written"),
			FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecInstallerBytes(), *DownloadsPath));
		Worker->RequestOriginalGameRecheck();
		TestTrue(TEXT("translocated Downloads copy completes the phase"),
			Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)));
		TestEqual(TEXT("translocated Downloads copy is hashed once"),
			Hashes.Load(), 1);
		TestEqual(TEXT("translocated Downloads routes to extraction"),
			Pending->Completion.NextPhase,
			EWoTBootstrapPhase::ExtractingOriginalGame);
		TestEqual(TEXT("translocated wait is zero-network"),
			Hops->Requests.Num(), 0);
	}

	// A partial final-name download is waiting, never wrong, and completing the
	// same pathname is picked up without restarting the phase.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TAtomic<int32> Hashes{0};
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 88, Checked)) return false;
		FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
		TAtomic<int32> LastReason{
			static_cast<int32>(EWoTOriginalGameNeededReason::Missing)};
		TAtomic<bool> SawWrong{false};
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			StartWaitingPhase(*Worker, 89, Entered, &LastReason, &SawWrong);
		if (!Entered->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		const FString DownloadsPath = FPaths::Combine(
			Fixture.InstallRoot + TEXT("-downloads"),
			WoT::Bootstrap::InstallerName);
		TArray<uint8> Partial;
		Partial.Append(WoTGameBootstrapWorkerSpecInstallerBytes().GetData(), 4);
		TestTrue(TEXT("partial Downloads final name is written"),
			FFileHelper::SaveArrayToFile(Partial, *DownloadsPath));
		Worker->RequestOriginalGameRecheck();
		TestTrue(TEXT("partial Downloads reports downloading"),
			WaitForReason(LastReason,
				EWoTOriginalGameNeededReason::Downloading));
		TestFalse(TEXT("partial Downloads never reports wrong"), SawWrong.Load());
		TestEqual(TEXT("partial Downloads is never hashed"), Hashes.Load(), 0);
		TestTrue(TEXT("partial Downloads can finish in place"),
			FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecInstallerBytes(), *DownloadsPath));
		Worker->RequestOriginalGameRecheck();
		TestTrue(TEXT("completed Downloads copy is acquired"),
			Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)));
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		TestEqual(TEXT("completed partial is hashed once"), Hashes.Load(), 1);
		TestFalse(TEXT("completion never passed through wrong"), SawWrong.Load());
	}

	// A player-picked wrong file is classified distinctly, remembered across
	// polls, and a later valid choice succeeds in the same worker.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TAtomic<int32> Hashes{0};
		TAtomic<int32> Sweeps{0};
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 90, Checked)) return false;
		FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
		TAtomic<int32> LastReason{
			static_cast<int32>(EWoTOriginalGameNeededReason::Missing)};
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			StartWaitingPhase(*Worker, 91, Entered, &LastReason, nullptr,
				&Sweeps);
		if (!Entered->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		const FString WrongPath = FPaths::Combine(
			Fixture.InstallRoot, TEXT("renamed-wrong.exe"));
		const FString ValidPath = FPaths::Combine(
			Fixture.InstallRoot, TEXT("renamed-valid.exe"));
		TestTrue(TEXT("wrong picked fixture is written"),
			FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecBadInstallerBytes(), *WrongPath));
		Worker->RequestPickedInstaller(WrongPath);
		TestTrue(TEXT("wrong picked file has its own reason"),
			WaitForReason(LastReason,
				EWoTOriginalGameNeededReason::PickedWrongFile));
		const int32 HashesAfterWrong = Hashes.Load();
		if (!TestTrue(TEXT("the wrong pick is re-polled while waiting"),
			WoTGameBootstrapWorkerSpecWaitForAtLeast(
				Sweeps, Sweeps.Load() + NegativeCheckSweeps)))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		TestEqual(TEXT("known wrong picked identity is not rehashed"),
			Hashes.Load(), HashesAfterWrong);
		TestTrue(TEXT("valid picked fixture is written"),
			FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecInstallerBytes(), *ValidPath));
		Worker->RequestPickedInstaller(ValidPath);
		TestTrue(TEXT("a later valid pick is acquired"),
			Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)));
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		TestEqual(TEXT("wrong and valid picked identities hash once each"),
			Hashes.Load(), 2);
		TestEqual(TEXT("valid pick routes to extraction"),
			Pending->Completion.NextPhase,
			EWoTBootstrapPhase::ExtractingOriginalGame);
	}

	// One unavailable location is a heartbeat/retry condition, not a bootstrap
	// failure; Browse remains able to supply a valid installer.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		auto DownloadsFault = [](EWoTInstallerSource Source, const FString&)
			-> TOptional<FWoTBootstrapError>
		{
			return Source == EWoTInstallerSource::Downloads
				? TOptional<FWoTBootstrapError>(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("downloads denied")))
				: TOptional<FWoTBootstrapError>();
		};
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, DownloadsFault);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 92, Checked)) return false;
		TestEqual(TEXT("unavailable Downloads remains recoverable"),
			Checked.NextPhase, EWoTBootstrapPhase::OriginalGameNeeded);
		FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(false);
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			StartWaitingPhase(*Worker, 93, Entered);
		if (!Entered->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)))
		{
			Worker->CancelAndJoin();
			FPlatformProcess::ReturnSynchEventToPool(Entered);
			return false;
		}
		const FString PickedPath = FPaths::Combine(
			Fixture.InstallRoot, TEXT("picked-after-denial.exe"));
		TestTrue(TEXT("picked-after-denial fixture is written"),
			FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecInstallerBytes(), *PickedPath));
		Worker->RequestPickedInstaller(PickedPath);
		TestTrue(TEXT("picked file succeeds despite unavailable Downloads"),
			Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)));
		FPlatformProcess::ReturnSynchEventToPool(Entered);
		TestEqual(TEXT("picked-after-denial routes to extraction"),
			Pending->Completion.NextPhase,
			EWoTBootstrapPhase::ExtractingOriginalGame);
	}

	// Adjacency retains priority over Downloads and short-circuits its hash.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)
			|| !Fixture.WriteInstaller(
				WoTGameBootstrapWorkerSpecInstallerBytes())) return false;
		const FString DownloadsPath = FPaths::Combine(
			Fixture.InstallRoot + TEXT("-downloads"),
			WoT::Bootstrap::InstallerName);
		if (!FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecInstallerBytes(), *DownloadsPath)) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TAtomic<int32> Hashes{0};
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 94, Checked)) return false;
		TestEqual(TEXT("adjacent wins over Downloads"),
			Checked.NextPhase, EWoTBootstrapPhase::ExtractingOriginalGame);
		TestEqual(TEXT("Downloads is not hashed after adjacent wins"),
			Hashes.Load(), 1);
		Worker->RequestPickedInstaller(TEXT("/ignored-after-phase.exe"));
	}

	// Direct leaf lookup follows the platform volume's case rules instead of a
	// case-sensitive directory-enumeration comparison.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		const FString UppercasePath = FPaths::Combine(
			Fixture.InstallRoot, TEXT("FIXTURE-INSTALLER.EXE"));
		if (!FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecInstallerBytes(), *UppercasePath)) return false;
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> CaseParent =
			OpenNativeDirectoryNoFollow(Fixture.InstallRoot);
		if (CaseParent.IsError()) return false;
		TWoTBootstrapResult<TOptional<EWoTTreeEntryType>> LowercaseLookup =
			QueryNativeEntryTypeAtNoFollow(CaseParent.GetValue(),
				WoTGameBootstrapWorkerSpecInstallerPins().Name);
		if (LowercaseLookup.IsError()) return false;
		if (!LowercaseLookup.GetValue().IsSet())
		{
			AddInfo(TEXT("fixture volume is case-sensitive; case-insensitive leaf lookup skipped"));
		}
		else
		{
			FWoTInstallerLocation DirectLocation;
			DirectLocation.CanonicalPath =
				WoTGameBootstrapWorkerSpecInstallerPath(Fixture.InstallRoot);
			TWoTBootstrapResult<FWoTPinnedAdjacentInstallerResult> DirectAcquired =
				WoTTryAcquireInstallerLocationForTesting(DirectLocation,
					WoTGameBootstrapWorkerSpecInstallerPins());
			if (DirectAcquired.IsError())
			{
				AddError(TEXT("case-insensitive direct acquisition: ")
					+ DirectAcquired.GetError().ToString());
				return false;
			}
			if (!TestEqual(TEXT("case-insensitive direct acquisition is valid"),
				static_cast<int32>(DirectAcquired.GetValue().State),
				static_cast<int32>(EWoTPinnedAdjacentInstallerProbe::Valid)))
			{
				return false;
			}
			TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
				MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
			TUniquePtr<FWoTGameBootstrapWorker> Worker =
				WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
			if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
			FWoTBootstrapWorkerCompletion Checked;
			if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 95, Checked)) return false;
			TestEqual(TEXT("case-insensitive adjacent name is accepted"),
				static_cast<int32>(Checked.NextPhase),
				static_cast<int32>(EWoTBootstrapPhase::ExtractingOriginalGame));
		}
	}

	// Update eligibility does not weaken the independently captured adjacent
	// parent. A notify-only copy whose parent was substituted fails closed when
	// original-game inspection begins.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		const FString ForeignRoot = Fixture.InstallRoot + TEXT("-foreign-parent");
		if (!IFileManager::Get().MakeDirectory(*ForeignRoot, true)) return false;
		ON_SCOPE_EXIT
		{
			IFileManager::Get().DeleteDirectory(*ForeignRoot, false, true);
		};
		auto Foreign = OpenNativeDirectoryNoFollow(ForeignRoot);
		if (!TestFalse(TEXT("foreign adjacent parent identity opens"),
			Foreign.IsError())) return false;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				{}, {}, {}, TEXT("install-root-unsupported"), {},
				Foreign.GetValue().GetIdentity());
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Checked;
		if (!TestTrue(TEXT("notify-only substituted-parent check completes"),
			WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 86, Checked)))
		{
			return false;
		}
		TestEqual(TEXT("notify-only parent substitution is fatal"),
			Checked.NextPhase, EWoTBootstrapPhase::Fatal);
		TestTrue(TEXT("fatal detail names the authenticated parent change"),
			Checked.FailureDetail.Contains(
				TEXT("canonical installer parent identity changed"),
				ESearchCase::CaseSensitive));
	}

	// A notify-only start still deletes the retired resume directory.
	{
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this)) return false;
		const FString Legacy = FPaths::Combine(
			Fixture.Root.StateDir, TEXT("installer"));
		if (!IFileManager::Get().MakeDirectory(*Legacy, true)
			|| !FFileHelper::SaveStringToFile(
				TEXT("retired"), *FPaths::Combine(Legacy, TEXT("old.part"))))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				{}, {}, {}, TEXT("install-root-unsupported"), {}, {}, {}, {},
				[]() { return true; }, []()
				{
					return TWoTBootstrapResult<bool>::Success(false);
				});
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		TestFalse(TEXT("notify-only startup removes legacy installer state"),
			IFileManager::Get().DirectoryExists(*Legacy));
		FWoTBootstrapWorkerCompletion Checked;
		if (!TestTrue(TEXT("unwritable parent check completes"),
			WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 87, Checked)))
		{
			return false;
		}
		TestEqual(TEXT("unwritable adjacent parent still reaches waiting"),
			Checked.NextPhase, EWoTBootstrapPhase::OriginalGameNeeded);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecDownloadsCandidatesShareParentIdentity,
	"WoT.Update.Worker.DownloadsCandidatesShareParentIdentity",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecDownloadsCandidatesShareParentIdentity::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	const FString DownloadsRoot = Fixture.InstallRoot + TEXT("-downloads");
	const FString OriginalDownloadsRoot = DownloadsRoot + TEXT("-original");
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(
			*OriginalDownloadsRoot, false, true);
	};
	TAtomic<bool> bSubstituted{false};
	auto SubstituteBeforeDuplicate =
		[&](EWoTInstallerSource Source, const FString& CandidatePath)
			-> TOptional<FWoTBootstrapError>
	{
		if (Source != EWoTInstallerSource::Downloads
			|| !CandidatePath.EndsWith(TEXT(" (1).exe"),
				ESearchCase::CaseSensitive)
			|| bSubstituted.Load())
		{
			return {};
		}
		bSubstituted.Store(true);
		if (!IFileManager::Get().Move(*OriginalDownloadsRoot, *DownloadsRoot,
				true, false, false, true)
			|| !IFileManager::Get().MakeDirectory(*DownloadsRoot, true)
			|| !FFileHelper::SaveArrayToFile(
				WoTGameBootstrapWorkerSpecInstallerBytes(), *CandidatePath))
		{
			return MakeWoTBootstrapError(EWoTBootstrapError::Io,
				TEXT("could not substitute the Downloads parent fixture"));
		}
		return {};
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TAtomic<int32> Hashes{0};
	FWoTInstallerAcquisitionTestHooks Hooks;
	Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks,
			{}, {}, {}, {}, {}, {}, {}, {}, {}, {},
			SubstituteBeforeDuplicate);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTBootstrapWorkerCompletion Checked;
	if (!TestTrue(TEXT("substituted Downloads check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 96, Checked)))
	{
		return false;
	}
	TestTrue(TEXT("candidate zero captured before Downloads substitution"),
		bSubstituted.Load());
	TestEqual(TEXT("replacement duplicate is never acquired during its sweep"),
		Checked.NextPhase, EWoTBootstrapPhase::OriginalGameNeeded);
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		WoTGameBootstrapWorkerSpecStartPhase(*Worker,
			EWoTBootstrapPhase::OriginalGameNeeded, 97);
	if (!TestTrue(TEXT("the next sweep rejects the substituted parent"),
		Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds))))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestEqual(TEXT("Downloads parent substitution is fatal"),
		Pending->Completion.NextPhase, EWoTBootstrapPhase::Fatal);
	TestTrue(TEXT("the shared parent mismatch is reported"),
		Pending->Completion.FailureDetail.Contains(
			TEXT("canonical installer parent identity changed"),
			ESearchCase::CaseSensitive));
	TestEqual(TEXT("the replacement duplicate is never hashed"), Hashes.Load(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecLeafErrorDoesNotSuppressSiblings,
	"WoT.Update.Worker.LeafErrorDoesNotSuppressSiblings",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecLeafErrorDoesNotSuppressSiblings::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	const FString DownloadsRoot = Fixture.InstallRoot + TEXT("-downloads");
	const FString DuplicatePath = FPaths::Combine(DownloadsRoot,
		TEXT("setup_the_wheel_of_time_333b_(54846) (1).exe"));
	if (!TestTrue(TEXT("valid Downloads sibling is written"),
		FFileHelper::SaveArrayToFile(
			WoTGameBootstrapWorkerSpecInstallerBytes(), *DuplicatePath)))
	{
		return false;
	}
	auto FaultPinnedLeaf = [](EWoTInstallerSource Source,
		const FString& CandidatePath) -> TOptional<FWoTBootstrapError>
	{
		return Source == EWoTInstallerSource::Downloads
			&& FPaths::GetCleanFilename(CandidatePath).Equals(
				WoT::Bootstrap::InstallerName, ESearchCase::CaseSensitive)
			? TOptional<FWoTBootstrapError>(MakeWoTBootstrapError(
				EWoTBootstrapError::Io, TEXT("injected leaf sharing violation")))
			: TOptional<FWoTBootstrapError>();
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TAtomic<int32> Hashes{0};
	FWoTInstallerAcquisitionTestHooks Hooks;
	Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks,
			{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, FaultPinnedLeaf);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTBootstrapWorkerCompletion Checked;
	if (!TestTrue(TEXT("leaf-fault check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 98, Checked)))
	{
		return false;
	}
	TestEqual(TEXT("a leaf error does not suppress its valid sibling"),
		Checked.NextPhase, EWoTBootstrapPhase::ExtractingOriginalGame);
	TestEqual(TEXT("only the valid sibling is hashed"), Hashes.Load(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecDownloadsParentHeartbeatDedup,
	"WoT.Update.Worker.DownloadsParentHeartbeatDedup",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecDownloadsParentHeartbeatDedup::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	const FString DownloadsRoot = Fixture.InstallRoot + TEXT("-downloads");
	const FString DownloadsLeaf = FPaths::GetCleanFilename(DownloadsRoot);
	TAtomic<bool> bFaultParent{true};
	TAtomic<int32> ParentAttempts{0};
	TAtomic<int32> LeafAttempts{0};
	auto ParentFault = [&](EWoTInstallerSource Source,
		const FString& CandidatePath) -> TOptional<FWoTBootstrapError>
	{
		if (Source != EWoTInstallerSource::Downloads)
		{
			return {};
		}
		if (FPaths::IsSamePath(CandidatePath, DownloadsRoot))
		{
			++ParentAttempts;
			return bFaultParent.Load()
				? TOptional<FWoTBootstrapError>(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("injected Downloads parent refusal")))
				: TOptional<FWoTBootstrapError>();
		}
		++LeafAttempts;
		return {};
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
			{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, ParentFault);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTBootstrapWorkerCompletion Checked;
	if (!TestTrue(TEXT("parent-fault check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 100, Checked)))
	{
		return false;
	}
	TestEqual(TEXT("parent fault remains recoverable"), Checked.NextPhase,
		EWoTBootstrapPhase::OriginalGameNeeded);
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		WoTGameBootstrapWorkerSpecStartPhase(*Worker,
			EWoTBootstrapPhase::OriginalGameNeeded, 101);
	if (!TestTrue(TEXT("the same parent fault spans at least three polls"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(ParentAttempts, 4)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestEqual(TEXT("a parent-open failure suppresses every Downloads leaf"),
		LeafAttempts.Load(), 0);
	TestTrue(TEXT("the first parent heartbeat is durably recorded"),
		WoTGameBootstrapWorkerSpecWaitForUnavailableHeartbeats(
			Fixture.UserStateRoot, DownloadsLeaf, 1));
	TestEqual(TEXT("repeated parent faults emit exactly one heartbeat"),
		WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
			Fixture.UserStateRoot, DownloadsLeaf), 1);

	bFaultParent.Store(false);
	const int32 BeforeClear = ParentAttempts.Load();
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("a fault-free sweep clears the parent error"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(
			ParentAttempts, BeforeClear + 2)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestTrue(TEXT("the recovered parent permits leaf probes"),
		LeafAttempts.Load() > 0);

	bFaultParent.Store(true);
	const int32 BeforeRefault = ParentAttempts.Load();
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("the parent can fault again after recovery"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(
			ParentAttempts, BeforeRefault + 1))
		|| !TestTrue(TEXT("the recurring parent fault is durably recorded"),
			WoTGameBootstrapWorkerSpecWaitForUnavailableHeartbeats(
				Fixture.UserStateRoot, DownloadsLeaf, 2)))
	{
		Worker->CancelAndJoin();
		return false;
	}

	const FString PickedPath = FPaths::Combine(
		Fixture.InstallRoot, TEXT("picked-after-parent-refault.exe"));
	if (!TestTrue(TEXT("parent-fault completion fixture is written"),
		FFileHelper::SaveArrayToFile(
			WoTGameBootstrapWorkerSpecInstallerBytes(), *PickedPath)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	Worker->RequestPickedInstaller(PickedPath);
	if (!TestTrue(TEXT("a picked installer ends the parent-fault flow"),
		Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds))))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestEqual(TEXT("one parent heartbeat plus one recurrence are recorded"),
		WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
			Fixture.UserStateRoot, DownloadsLeaf), 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecDownloadsLeafHeartbeatDedup,
	"WoT.Update.Worker.DownloadsLeafHeartbeatDedup",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecDownloadsLeafHeartbeatDedup::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	const FString DownloadsRoot = Fixture.InstallRoot + TEXT("-downloads");
	const FString PinnedLeaf(WoT::Bootstrap::InstallerName);
	const FString PinnedPath = FPaths::Combine(DownloadsRoot, PinnedLeaf);
	TAtomic<bool> bFaultParent{false};
	TAtomic<bool> bFaultLeaf{true};
	TAtomic<int32> ParentAttempts{0};
	TAtomic<int32> PinnedLeafAttempts{0};
	TAtomic<int32> SiblingLeafAttempts{0};
	auto LeafFault = [&](EWoTInstallerSource Source,
		const FString& CandidatePath) -> TOptional<FWoTBootstrapError>
	{
		if (Source != EWoTInstallerSource::Downloads)
		{
			return {};
		}
		if (FPaths::IsSamePath(CandidatePath, DownloadsRoot))
		{
			++ParentAttempts;
			return bFaultParent.Load()
				? TOptional<FWoTBootstrapError>(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("injected parent suppression of leaf probes")))
				: TOptional<FWoTBootstrapError>();
		}
		if (FPaths::IsSamePath(CandidatePath, PinnedPath))
		{
			++PinnedLeafAttempts;
			return bFaultLeaf.Load()
				? TOptional<FWoTBootstrapError>(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("injected Downloads leaf refusal")))
				: TOptional<FWoTBootstrapError>();
		}
		++SiblingLeafAttempts;
		return {};
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
			{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, LeafFault);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTBootstrapWorkerCompletion Checked;
	if (!TestTrue(TEXT("leaf-fault check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 102, Checked)))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		WoTGameBootstrapWorkerSpecStartPhase(*Worker,
			EWoTBootstrapPhase::OriginalGameNeeded, 103);
	if (!TestTrue(TEXT("the same leaf fault spans at least three polls"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(PinnedLeafAttempts, 4)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestTrue(TEXT("a leaf fault does not suppress Downloads siblings"),
		SiblingLeafAttempts.Load() >= 27);
	TestTrue(TEXT("the first leaf heartbeat is durably recorded"),
		WoTGameBootstrapWorkerSpecWaitForUnavailableHeartbeats(
			Fixture.UserStateRoot, PinnedLeaf, 1));
	TestEqual(TEXT("repeated leaf faults emit exactly one heartbeat"),
		WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
			Fixture.UserStateRoot, PinnedLeaf), 1);

	bFaultParent.Store(true);
	const int32 BeforeParentFault = ParentAttempts.Load();
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("parent suppression begins while the leaf stays faulty"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(
			ParentAttempts, BeforeParentFault + 2)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	const int32 PinnedAttemptsWhileSuppressed = PinnedLeafAttempts.Load();
	if (!TestTrue(TEXT("parent suppression spans multiple complete sweeps"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(
			ParentAttempts, BeforeParentFault + 4)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestEqual(TEXT("parent failure suppresses the still-faulty leaf"),
		PinnedLeafAttempts.Load(), PinnedAttemptsWhileSuppressed);
	bFaultParent.Store(false);
	const int32 BeforeParentRecovery = PinnedLeafAttempts.Load();
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("the unchanged leaf fault returns after parent recovery"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(
			PinnedLeafAttempts, BeforeParentRecovery + 1)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestEqual(TEXT("parent suppression does not clear the leaf heartbeat key"),
		WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
			Fixture.UserStateRoot, PinnedLeaf), 1);

	bFaultLeaf.Store(false);
	const int32 BeforeClear = ParentAttempts.Load();
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("a fault-free sweep clears the leaf error"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(
			ParentAttempts, BeforeClear + 2)))
	{
		Worker->CancelAndJoin();
		return false;
	}

	bFaultLeaf.Store(true);
	const int32 BeforeRefault = PinnedLeafAttempts.Load();
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("the leaf can fault again after recovery"),
		WoTGameBootstrapWorkerSpecWaitForAtLeast(
			PinnedLeafAttempts, BeforeRefault + 1))
		|| !TestTrue(TEXT("the recurring leaf fault is durably recorded"),
			WoTGameBootstrapWorkerSpecWaitForUnavailableHeartbeats(
				Fixture.UserStateRoot, PinnedLeaf, 2)))
	{
		Worker->CancelAndJoin();
		return false;
	}

	const FString PickedPath = FPaths::Combine(
		Fixture.InstallRoot, TEXT("picked-after-leaf-refault.exe"));
	if (!TestTrue(TEXT("leaf-fault completion fixture is written"),
		FFileHelper::SaveArrayToFile(
			WoTGameBootstrapWorkerSpecInstallerBytes(), *PickedPath)))
	{
		Worker->CancelAndJoin();
		return false;
	}
	Worker->RequestPickedInstaller(PickedPath);
	if (!TestTrue(TEXT("a picked installer ends the leaf-fault flow"),
		Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds))))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestEqual(TEXT("one leaf heartbeat plus one recurrence are recorded"),
		WoTGameBootstrapWorkerSpecCountUnavailableHeartbeats(
			Fixture.UserStateRoot, PinnedLeaf), 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecTranslocatedLayoutSkipsAdjacentParent,
	"WoT.Update.Worker.TranslocatedLayoutSkipsAdjacentParent",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecTranslocatedLayoutSkipsAdjacentParent::RunTest(
	const FString&)
{
#if PLATFORM_MAC
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::AutomationTransientDir(), TEXT("wot-translocated-layout-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString UserStateRoot = Root + TEXT("-user");
	const FString TreeRoot = FPaths::Combine(Root, TEXT("WoT.app"));
	const FString Executable = FPaths::Combine(
		TreeRoot, TEXT("Contents"), TEXT("MacOS"), TEXT("WoT"));
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().DeleteDirectory(*UserStateRoot, false, true);
	};
	if (!TestTrue(TEXT("translocated layout fixture tree is created"),
		IFileManager::Get().MakeDirectory(*TreeRoot, true)))
	{
		return false;
	}
	WoTGameBootstrapWorkerSpecResetAdjacentParentOpenCount();
	const FWoTResolvedExecutableLayout Layout =
		WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithNativeTreePath(
			Executable, UserStateRoot, TreeRoot, false, true);
	TestTrue(TEXT("translocated guarded layout stays valid"), Layout.IsValid());
	TestEqual(TEXT("translocated layout is notify-only at resolution"),
		Layout.UpdateDisableReason, FString(TEXT("translocated")));
	TestFalse(TEXT("translocated layout leaves adjacent identity unset"),
		Layout.bHasAdjacentInstallerParentIdentity);
	TestTrue(TEXT("translocated layout leaves adjacent path absent"),
		Layout.CanonicalAdjacentInstallerPath.IsEmpty());
	TestEqual(TEXT("translocated layout performs zero adjacent-parent opens"),
		WoTGameBootstrapWorkerSpecAdjacentParentOpenCount(), 0);
#else
	AddInfo(TEXT("App Translocation layout coverage is Mac-only"));
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecMustStageAndStage,
	"WoT.Update.Worker.MustStageAndStage",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecMustStageAndStage::RunTest(const FString&)
{
	FWoTReleaseStagingCodeSealVerifierOverrideForTesting CodeSealOverride(
		[](const FString&) { return FWoTBootstrapStatus::Success(); });
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	auto Hops = MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
	WoTGameBootstrapWorkerSpecAppendMetadataScript(*Hops, Fixture.V2);
	Hops->Steps.Add(WoTGameBootstrapWorkerSpecHop(Fixture.V2.Archive, 200, WoTGameBootstrapWorkerSpecArchiveUrl));
	int32 QuitCalls = 0;
	auto Worker = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops,
		[&QuitCalls]() { ++QuitCalls; });
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTUpdateState Ready;
	TestTrue(TEXT("background staging completes"), WoTGameBootstrapWorkerSpecRunBackground(*Worker, Ready));
	TestEqual(TEXT("verified release is ready"), static_cast<int32>(Ready.Status), static_cast<int32>(EWoTUpdateStatus::Ready));
	TestTrue(TEXT("ready marker exists"), IFileManager::Get().FileExists(*Fixture.Root.ReadyPath));
	TestFalse(TEXT("download directory is retired"), IFileManager::Get().DirectoryExists(*Fixture.Root.WorkDir));
	TestTrue(TEXT("ready action requests ordinary exit"), Worker->RequestRestartToUpdate());
	TestEqual(TEXT("exactly one quit"), QuitCalls, 1);
	TestFalse(TEXT("duplicate quit is refused"), Worker->RequestRestartToUpdate());
	return !HasAnyErrors();
}

bool WoTGameBootstrapWorkerSpecStageUpdate(
	FAutomationTestBase& Test, FWoTGameBootstrapWorkerSpecFixture& Fixture)
{
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
	WoTGameBootstrapWorkerSpecAppendMetadataScript(*Hops, Fixture.V2);
	Hops->Steps.Add(WoTGameBootstrapWorkerSpecHop(
		Fixture.V2.Archive, 200, WoTGameBootstrapWorkerSpecArchiveUrl));
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(Test, *Worker)) return false;
	FWoTBootstrapWorkerCompletion Bootstrap;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
		EWoTBootstrapPhase::CheckingGameUpdates, 2, Bootstrap)
		|| !Bootstrap.bSucceeded) return false;
	FWoTUpdateState Ready;
	return WoTGameBootstrapWorkerSpecRunBackground(*Worker, Ready)
		&& Ready.Status == EWoTUpdateStatus::Ready;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecBackgroundDownloadFailureReported,
	"WoT.Update.Worker.BackgroundDownloadFailureReported",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecBackgroundDownloadFailureReported::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
	WoTGameBootstrapWorkerSpecAppendMetadataScript(*Hops, Fixture.V2);
	TArray<uint8> Corrupt = Fixture.V2.Archive;
	if (!TestTrue(TEXT("the fixture archive is not empty"), Corrupt.Num() > 0))
	{
		return false;
	}
	Corrupt[Corrupt.Num() / 2] ^= 0x5a;
	Hops->Steps.Add(WoTGameBootstrapWorkerSpecHop(
		Corrupt, 200, WoTGameBootstrapWorkerSpecArchiveUrl));
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
	FWoTUpdateState Result;
	TestTrue(TEXT("a failed download still completes the background check"),
		WoTGameBootstrapWorkerSpecRunBackground(*Worker, Result));
	TestFalse(TEXT("a failed download never reaches Ready"),
		Result.Status == EWoTUpdateStatus::Ready);
	// The player is told: the banner gets Available with the failure reason
	// instead of the Downloading banner silently disappearing.
	TestEqual(TEXT("a failed download publishes Available"),
		Result.Status, EWoTUpdateStatus::Available);
	TestEqual(TEXT("a failed download publishes its reason"),
		Result.Reason, FString(TEXT("download-failed")));
	FString Heartbeat;
	TestTrue(TEXT("download-failure heartbeat is readable"),
		FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
			Fixture.UserStateRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName())));
	TestTrue(TEXT("a failed download says why"),
		Heartbeat.Contains(TEXT("\"event\":\"update-check-failed\""),
			ESearchCase::CaseSensitive)
		&& Heartbeat.Contains(TEXT("stage=download diagnostic="),
			ESearchCase::CaseSensitive)
		&& Heartbeat.Contains(TEXT("INSTALL-FAILED detail="),
			ESearchCase::CaseSensitive)
		&& Heartbeat.Contains(
			TEXT("\"detail\":\"download-failed status=Available sequence="),
			ESearchCase::CaseSensitive)
		&& Heartbeat.Contains(TEXT("reason=download-failed\""),
			ESearchCase::CaseSensitive));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecTamperedMetadata,
	"WoT.Update.Worker.TamperedMetadata",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecTamperedMetadata::RunTest(
	const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
	WoTGameBootstrapWorkerSpecAppendMetadataScript(
		*Hops, Fixture.V2, true);
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTUpdateState Refused;
	TestTrue(TEXT("tampered metadata background check completes"),
		WoTGameBootstrapWorkerSpecRunBackground(*Worker, Refused));
	TestEqual(TEXT("tampered metadata remains invisible"),
		static_cast<int32>(Refused.Status),
		static_cast<int32>(EWoTUpdateStatus::None));
	TestEqual(TEXT("tamper is refused before any archive request"),
		Hops->Requests.Num(), 6);
	FString Heartbeat;
	TestTrue(TEXT("tampered metadata heartbeat is readable"),
		FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
			Fixture.UserStateRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName())));
	TestTrue(TEXT("a failed check is reported as failed, with its diagnostic"),
		Heartbeat.Contains(TEXT("\"event\":\"update-check-failed\""),
			ESearchCase::CaseSensitive)
		&& Heartbeat.Contains(TEXT("\"detail\":\"failed\""),
			ESearchCase::CaseSensitive));
	TestFalse(TEXT("a failed check never reads as no update"),
		Heartbeat.Contains(TEXT("\"detail\":\"none\""),
			ESearchCase::CaseSensitive));
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecCancellation,
	"WoT.Update.Worker.Cancellation",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecCancellation::RunTest(
	const FString&)
{
	FWoTReleaseStagingCodeSealVerifierOverrideForTesting CodeSealOverride(
		[](const FString&) { return FWoTBootstrapStatus::Success(); });
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	constexpr int32 FirstChunkSize = 16 * 1024 * 1024;
	const FWoTGameBootstrapWorkerSpecRelease Release =
		Fixture.BuildRelease(2, 1, 18 * 1024 * 1024);
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Release);
	WoTGameBootstrapWorkerSpecAppendMetadataScript(*Hops, Release);
	TArray<uint8> FirstChunk;
	FirstChunk.Append(Release.Archive.GetData(), FirstChunkSize);
	TArray<uint8> RemainingChunk;
	RemainingChunk.Append(Release.Archive.GetData() + FirstChunkSize,
		Release.Archive.Num() - FirstChunkSize);
	FWoTGameBootstrapWorkerSpecHopStep InterruptedArchive;
	InterruptedArchive.Response.Status = 200;
	InterruptedArchive.Response.BodyLength =
		static_cast<uint64>(Release.Archive.Num());

	InterruptedArchive.ExpectedUrl = WoTGameBootstrapWorkerSpecArchiveUrl;
	InterruptedArchive.StreamChunks = {FirstChunk, RemainingChunk};
	Hops->Steps.Add(MoveTemp(InterruptedArchive));
	Hops->BlockAfterStreamWriteAtRequest = 6;
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	TAtomic<int32> CompletionCount{0};
	Worker->StartBackgroundUpdate(
		[&CompletionCount](FWoTUpdateState&& State)
		{
			if (State.Status != EWoTUpdateStatus::Checking
				&& State.Status != EWoTUpdateStatus::Downloading)
			{
				++CompletionCount;
			}
		});
	TestTrue(TEXT("download reaches the blocked archive hop"),
		Hops->BlockEntered->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds)));

	// Only CancelCurrentRequest (or this spec) releases the blocked body, so
	// a join that finishes before the spec releases it proves the abort; a
	// one-second budget on top of that measured machine load.
	FEvent* CancelStarted =
		FPlatformProcess::GetSynchEventFromPool(false);
	TFuture<void> Cancelling = Async(
		EAsyncExecution::Thread,
		[&Worker, CancelStarted]()
		{
			CancelStarted->Trigger();
			Worker->CancelAndJoin();
		});
	CancelStarted->Wait();
	FPlatformProcess::ReturnSynchEventToPool(CancelStarted);
	const bool bCancelledPromptly = Cancelling.WaitFor(
		FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds));
	if (!bCancelledPromptly)
	{
		Hops->BlockRelease->Trigger();
	}
	Cancelling.Wait();
	TestTrue(TEXT("CancelAndJoin aborts the blocked body without test release"),
		bCancelledPromptly && Hops->CancelCalls.Load() > 0);
	FPlatformProcess::Sleep(0.05f);
	TestEqual(TEXT("cancel suppresses the in-flight completion"),
		CompletionCount.Load(), 0);
	Worker.Reset();
	TestFalse(TEXT("cancel removes the process staging directory"), IFileManager::Get().DirectoryExists(*Fixture.Root.WorkDir));
	TestFalse(TEXT("cancel never publishes ready"), IFileManager::Get().FileExists(*Fixture.Root.ReadyPath));
	auto RetryHops = MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*RetryHops, Release);
	WoTGameBootstrapWorkerSpecAppendMetadataScript(*RetryHops, Release);
	RetryHops->Steps.Add(WoTGameBootstrapWorkerSpecHop(Release.Archive, 200, WoTGameBootstrapWorkerSpecArchiveUrl));
	auto Retry = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, RetryHops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Retry, 70)) return false;
	FWoTUpdateState Retried;
	TestTrue(TEXT("next session downloads from zero"), WoTGameBootstrapWorkerSpecRunBackground(*Retry, Retried));
	TestEqual(TEXT("retry reaches ready"), static_cast<int32>(Retried.Status), static_cast<int32>(EWoTUpdateStatus::Ready));
	TestEqual(TEXT("retry fetches metadata and the whole archive"), RetryHops->Requests.Num(), 7);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecInstallRoot,
	"WoT.Update.Worker.InstallRootResolution",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecInstallRoot::RunTest(
	const FString&)
{
	FString TransientRoot = FPaths::ConvertRelativePathToFull(
		FPaths::AutomationTransientDir());
	FPaths::NormalizeDirectoryName(TransientRoot);
	FPaths::CollapseRelativeDirectories(TransientRoot);
	FString InstallRoot = FPaths::Combine(
		TransientRoot, TEXT("wot-worker-root-resolution-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString UserStateRoot = InstallRoot + TEXT("-user");
	FPaths::NormalizeDirectoryName(InstallRoot);
	FPaths::NormalizeDirectoryName(UserStateRoot);
#if PLATFORM_MAC
	const FString Executable = FPaths::Combine(InstallRoot, TEXT("WoT.app"),
		TEXT("Contents"), TEXT("MacOS"), TEXT("WoT"));
	const FString ExpectedTree = FPaths::Combine(InstallRoot, TEXT("WoT.app"));
	const FString ExpectedRelative = TEXT("Contents/MacOS/WoT");
	const FString ExpectedVolumeRoot = TEXT("/");
	const FString ExpectedVolumeLeaf = TEXT("WoT 2.app");
	const TArray<FString> LiveLeaves = {TEXT("WoT.app"), TEXT("WoT 2.app")};
#else
	const FString Executable = FPaths::Combine(InstallRoot, TEXT("Game"),
		TEXT("WoT"), TEXT("Binaries"), TEXT("Win64"), TEXT("WoT.exe"));
	const FString ExpectedTree = FPaths::Combine(
		InstallRoot, TEXT("Game"));
	const FString ExpectedRelative = TEXT("WoT/Binaries/Win64/WoT.exe");
	const FString ExpectedVolumeRoot = InstallRoot.Left(3);
	const FString ExpectedVolumeLeaf = TEXT("wot-worker-volume-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString VolumeExecutable = FPaths::Combine(ExpectedVolumeRoot,
		ExpectedVolumeLeaf, TEXT("WoT"), TEXT("Binaries"), TEXT("Win64"),
		TEXT("WoT.exe"));
	const TArray<FString> LiveLeaves = {
		TEXT("Game"), TEXT("game-copy"), TEXT("WoT-Win64 (1)"),
		TEXT("Wheel Of Time")};
#endif
	for (const FString& LiveLeaf : LiveLeaves)
	{
		if (!IFileManager::Get().MakeDirectory(
				*FPaths::Combine(InstallRoot, LiveLeaf), true))
		{
			AddError(TEXT("could not create an executable-layout live tree"));
			return false;
		}
	}
#if PLATFORM_WINDOWS
	if (!IFileManager::Get().MakeDirectory(
			*FPaths::Combine(ExpectedVolumeRoot, ExpectedVolumeLeaf), true))
	{
		AddError(TEXT("could not create the volume-root executable-layout tree"));
		IFileManager::Get().DeleteDirectory(*InstallRoot, false, true);
		return false;
	}
#endif
	const FWoTResolvedExecutableLayout Layout =
		WoTResolveExecutableLayoutFromPath(Executable, UserStateRoot);
	const FString CanonicalUserState =
		WoTGameBootstrapWorkerSpecCanonicalOwnedRoot(UserStateRoot);
	const FString CanonicalTree =
		WoTGameBootstrapWorkerSpecCanonicalOwnedRoot(ExpectedTree);
	const FString CanonicalInstallRoot = FPaths::GetPath(CanonicalTree);
	TestTrue(TEXT("canonical running layout resolves both roots"),
		Layout.IsValid());
	TestEqual(TEXT("user state resolves independently"),
		Layout.UserStateRoot, CanonicalUserState);
	TestEqual(TEXT("install root is the running tree parent"),
		Layout.InstallRoot, CanonicalInstallRoot);
	TestEqual(TEXT("the actual live leaf is preserved"),
		Layout.LiveLeaf, FPaths::GetCleanFilename(ExpectedTree));
	TestEqual(TEXT("the canonical running tree is exact"),
		Layout.InstalledTreeRoot, CanonicalTree);
	TestEqual(TEXT("the executable member is tree-relative"),
		Layout.ExecutableTreeRelativePath, ExpectedRelative);
	TestEqual(TEXT("the adjacent installer belongs to the copy"),
		Layout.CanonicalAdjacentInstallerPath,
		FPaths::Combine(CanonicalInstallRoot, WoT::Bootstrap::InstallerName));
	TestEqual(TEXT("the cache remains explicitly under user state"),
		WoTGameBootstrapWorkerSpecResolveProductionContentCacheRootFromExecutablePath(
			Executable, UserStateRoot),
		FPaths::Combine(CanonicalUserState, TEXT("ContentCache")));
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> HeldFinalInstallRoot =
		OpenNativeDirectoryNoFollow(Layout.InstallRoot);
	TestTrue(TEXT("the final projected InstallRoot has a captured identity"),
		Layout.bHasInstallRootIdentity && !HeldFinalInstallRoot.IsError());
	if (!HeldFinalInstallRoot.IsError())
	{
		TestTrue(TEXT("the captured identity belongs to the final projected InstallRoot"),
			Layout.InstallRootIdentity.IsSameObject(
				HeldFinalInstallRoot.GetValue().GetIdentity()));
	}

	const FString ReboundInstallRoot = FPaths::Combine(
		TransientRoot, TEXT("wot-worker-rebound-root-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ReboundTree = FPaths::Combine(
		ReboundInstallRoot, FPaths::GetCleanFilename(ExpectedTree));
	if (!IFileManager::Get().MakeDirectory(*ReboundTree, true))
	{
		AddError(TEXT("could not create the rebound executable-layout tree"));
		return false;
	}
	const FWoTResolvedExecutableLayout Rebound =
		WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithReboundNativeTreePath(
			Executable, UserStateRoot, ReboundTree);
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> HeldReboundRoot =
		OpenNativeDirectoryNoFollow(ReboundInstallRoot);
	TestTrue(TEXT("a rebound native tree path remains update-capable"),
		Rebound.CanUpdate());
	TestEqual(TEXT("native tree projection replaces the launch parent"),
		Rebound.InstallRoot, ReboundInstallRoot);
	TestTrue(TEXT("identity capture is rebound after native tree projection"),
		!HeldReboundRoot.IsError() && Rebound.bHasInstallRootIdentity
			&& Rebound.InstallRootIdentity.IsSameObject(
				HeldReboundRoot.GetValue().GetIdentity()));
	TestFalse(TEXT("the rebound identity is not the launch parent identity"),
		HeldFinalInstallRoot.IsError()
			|| Rebound.InstallRootIdentity.IsSameObject(
				HeldFinalInstallRoot.GetValue().GetIdentity()));
	IFileManager::Get().DeleteDirectory(*ReboundInstallRoot, false, true);
	const FWoTResolvedExecutableLayout IdentityMismatch =
		WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithIdentityMismatch(
			Executable, UserStateRoot);
	TestTrue(TEXT("a canonical identity race preserves guarded startup"),
		IdentityMismatch.IsValid());
	TestFalse(TEXT("a canonical identity race disables in-place updates"),
		IdentityMismatch.CanUpdate());
	TestEqual(TEXT("a canonical identity race has the precise reason"),
		IdentityMismatch.UpdateDisableReason,
		FString(TEXT("live-tree-identity-mismatch")));
	for (const FString& LiveLeaf : LiveLeaves)
	{
#if PLATFORM_MAC
		const FString RenamedExecutable = FPaths::Combine(InstallRoot, LiveLeaf,
			TEXT("Contents"), TEXT("MacOS"), TEXT("WoT"));
#else
		const FString RenamedExecutable = FPaths::Combine(InstallRoot, LiveLeaf,
			TEXT("WoT"), TEXT("Binaries"), TEXT("Win64"), TEXT("WoT.exe"));
#endif
		const FWoTResolvedExecutableLayout Renamed =
			WoTResolveExecutableLayoutFromPath(RenamedExecutable, UserStateRoot);
		TestTrue(LiveLeaf + TEXT(" resolves by layout"), Renamed.IsValid());
		TestEqual(LiveLeaf + TEXT(" keeps its actual live name"),
			Renamed.LiveLeaf, LiveLeaf);
		TestEqual(LiveLeaf + TEXT(" keeps the same per-copy parent"),
			Renamed.InstallRoot, CanonicalInstallRoot);
		TestEqual(LiveLeaf + TEXT(" keeps the per-user content cache"),
			WoTGameBootstrapWorkerSpecResolveProductionContentCacheRootFromExecutablePath(
				RenamedExecutable, UserStateRoot),
			FPaths::Combine(CanonicalUserState, TEXT("ContentCache")));
	}
#if PLATFORM_WINDOWS
	const FString AlternateCaseExecutable = FPaths::Combine(
		InstallRoot, TEXT("wheel of time"), TEXT("WoT"), TEXT("Binaries"),
		TEXT("Win64"), TEXT("WoT.exe"));
	const FWoTResolvedExecutableLayout AlternateCase =
		WoTResolveExecutableLayoutFromPath(
			AlternateCaseExecutable, UserStateRoot);
	TestTrue(TEXT("a case-alternate launch path resolves"),
		AlternateCase.IsValid());
	TestEqual(TEXT("the live leaf uses its held-directory native spelling"),
		AlternateCase.LiveLeaf, FString(TEXT("Wheel Of Time")));
	TestEqual(TEXT("the installed tree root uses the on-disk live spelling"),
		AlternateCase.InstalledTreeRoot,
		FPaths::Combine(InstallRoot, TEXT("Wheel Of Time")));
	const FString LowerFixedExecutable = FPaths::Combine(
		InstallRoot, TEXT("Wheel Of Time"), TEXT("wot"), TEXT("binaries"),
		TEXT("win64"), TEXT("wot.exe"));
	const FWoTResolvedExecutableLayout LowerFixed =
		WoTResolveExecutableLayoutFromPath(LowerFixedExecutable, UserStateRoot);
	TestTrue(TEXT("lowercase Win64 fixed components and executable resolve"),
		LowerFixed.IsValid());

	const FString LongExecutable = FPaths::Combine(
		InstallRoot, TEXT("Wheel Of Time"), TEXT("WoT"), TEXT("Binaries"),
		TEXT("Win64"), TEXT("WoT.exe"));
	if (IFileManager::Get().MakeDirectory(*FPaths::GetPath(LongExecutable), true)
		&& FFileHelper::SaveStringToFile(TEXT("fixture"), *LongExecutable))
	{
		const DWORD ShortCapacity = ::GetShortPathNameW(*LongExecutable, nullptr, 0);
		TArray<WCHAR> ShortBuffer;
		if (ShortCapacity > 0)
		{
			ShortBuffer.SetNumUninitialized(static_cast<int32>(ShortCapacity) + 1);
			const DWORD ShortLength = ::GetShortPathNameW(*LongExecutable,
				ShortBuffer.GetData(), static_cast<DWORD>(ShortBuffer.Num()));
			if (ShortLength > 0
				&& ShortLength < static_cast<DWORD>(ShortBuffer.Num()))
			{
				const FString ShortPath(
					static_cast<int32>(ShortLength), ShortBuffer.GetData());
				if (!ShortPath.Contains(TEXT("~")))
				{
					AddInfo(TEXT("8.3 aliases are disabled on this volume; short-name coverage skipped"));
				}
				else
				{
					TWoTBootstrapResult<FWoTNativeFileHandle> ShortOpened =
						OpenNativeFileReadNoFollow(ShortPath);
					TWoTBootstrapResult<FString> FinalPath = ShortOpened.IsError()
						? TWoTBootstrapResult<FString>::Error(
							MoveTemp(ShortOpened).GetError())
						: QueryNativeFilePath(ShortOpened.GetValue());
					TestTrue(TEXT("a held 8.3 spelling resolves to a final normalized path"),
						!FinalPath.IsError()
							&& !FinalPath.GetValue().Contains(TEXT("~")));
					if (!FinalPath.IsError())
					{
						TestTrue(TEXT("the final handle path resolves the executable layout"),
							WoTResolveExecutableLayoutFromPath(
								FinalPath.GetValue(), UserStateRoot).IsValid());
					}
				}
			}
		}
		else
		{
			AddInfo(TEXT("8.3 names are unavailable on this volume; short-name coverage skipped"));
		}
	}

	const FWoTResolvedExecutableLayout UnresolvedName =
		WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithNativeTreePath(
			Executable, UserStateRoot, FStringView(), true);
	TestTrue(TEXT("a native name-query failure preserves the guarded layout"),
		UnresolvedName.IsValid());
	TestFalse(TEXT("a native name-query failure disables only updates"),
		UnresolvedName.CanUpdate());
	TestEqual(TEXT("a native name-query failure has the precise reason"),
		UnresolvedName.UpdateDisableReason,
		FString(TEXT("live-tree-name-unresolved")));
	TestTrue(TEXT("an unresolved live name projects no updater InstallRoot"),
		UnresolvedName.InstallRoot.IsEmpty());
	TestTrue(TEXT("an unresolved live name projects no updater live leaf"),
		UnresolvedName.LiveLeaf.IsEmpty());
	TestEqual(TEXT("name-query failure leaves the per-user cache unchanged"),
		UnresolvedName.ContentCacheRoot,
		FPaths::Combine(CanonicalUserState, TEXT("ContentCache")));

	const FString UncTree = TEXT("//server/share/Wheel Of Time");
	const FWoTResolvedExecutableLayout UncLayout =
		WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithNativeTreePath(
			Executable, UserStateRoot, UncTree);
	TestTrue(TEXT("a UNC share-root update layout is supported"),
		UncLayout.CanUpdate());
	TestEqual(TEXT("the UNC share itself is InstallRoot"),
		UncLayout.InstallRoot, FString(TEXT("//server/share")));
	TestEqual(TEXT("the UNC live leaf keeps native spelling"),
		UncLayout.LiveLeaf, FString(TEXT("Wheel Of Time")));
	TWoTBootstrapResult<FWoTUpdaterStateRoot> UncState =
		ResolveUpdaterStateRoot(
			UserStateRoot, UncLayout.InstallRoot, UncLayout.LiveLeaf);
	TestFalse(TEXT("the updater accepts a UNC share root"), UncState.IsError());

	const FWoTResolvedExecutableLayout VolumeLayout =
		WoTResolveExecutableLayoutFromPath(VolumeExecutable, UserStateRoot);
	TestTrue(TEXT("a volume-root install is accepted"), VolumeLayout.IsValid());
	TestEqual(TEXT("the volume root is preserved as InstallRoot"),
		VolumeLayout.InstallRoot, ExpectedVolumeRoot);
	TestEqual(TEXT("the renamed volume-root live leaf is preserved"),
		VolumeLayout.LiveLeaf, ExpectedVolumeLeaf);
#endif
	TWoTBootstrapResult<FWoTUpdaterStateRoot> VolumeState =
		ResolveUpdaterStateRoot(
			UserStateRoot, ExpectedVolumeRoot, ExpectedVolumeLeaf);
	TestFalse(TEXT("the updater accepts a filesystem-root InstallRoot"),
		VolumeState.IsError());
	if (!VolumeState.IsError())
	{
		TestEqual(TEXT("the volume-root update directory remains owned"),
			VolumeState.GetValue().UpdateDir,
			FPaths::Combine(ExpectedVolumeRoot, TEXT(".wot-update")));
	}
	const FString Outside = FPaths::Combine(
		TransientRoot, TEXT("outside"), TEXT("WoT.exe"));
	TestFalse(TEXT("an executable outside the canonical tree is refused"),
		WoTResolveExecutableLayoutFromPath(Outside, UserStateRoot).IsValid());
	TestTrue(TEXT("an invalid layout has no fallback install root"),
		WoTGameBootstrapWorkerSpecResolveInstallRootFromExecutablePath(
			Outside, UserStateRoot).IsEmpty());
	TestFalse(TEXT("an empty user-data root fails closed"),
		WoTResolveExecutableLayoutFromPath(Executable, FStringView()).IsValid());
	TestTrue(TEXT("an empty user-data root never falls back a content cache"),
		WoTGameBootstrapWorkerSpecResolveProductionContentCacheRootFromExecutablePath(
			Executable, FStringView()).IsEmpty());
	IFileManager::Get().DeleteDirectory(*InstallRoot, false, true);
	IFileManager::Get().DeleteDirectory(*UserStateRoot, false, true);
#if PLATFORM_WINDOWS
	IFileManager::Get().DeleteDirectory(
		*FPaths::Combine(ExpectedVolumeRoot, ExpectedVolumeLeaf), false, true);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecSingleStartupResolution,
	"WoT.Update.Worker.SingleStartupLayoutResolution",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecSingleStartupResolution::RunTest(
	const FString&)
{
	const FString InstallRoot = FPaths::Combine(
		FPlatformProcess::UserTempDir(), TEXT("wot-single-layout-resolution-"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString UserStateRoot = InstallRoot + TEXT("-user");
#if PLATFORM_MAC
	const FString LiveRoot = FPaths::Combine(InstallRoot, TEXT("WoT.app"));
	const FString Executable = FPaths::Combine(
		LiveRoot, TEXT("Contents"), TEXT("MacOS"), TEXT("WoT"));
#else
	const FString LiveRoot = FPaths::Combine(InstallRoot, TEXT("game"));
	const FString Executable = FPaths::Combine(
		LiveRoot, TEXT("WoT"), TEXT("Binaries"), TEXT("Win64"),
		TEXT("WoT.exe"));
#endif
	if (!IFileManager::Get().MakeDirectory(*LiveRoot, true)
		|| !IFileManager::Get().MakeDirectory(*UserStateRoot, true))
	{
		AddError(TEXT("could not create the single-resolution fixture"));
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*InstallRoot, false, true);
		IFileManager::Get().DeleteDirectory(*UserStateRoot, false, true);
	};

	WoTGameBootstrapWorkerSpecResetLayoutResolutionCount();
	const FWoTResolvedExecutableLayout Layout =
		WoTResolveExecutableLayoutFromPath(Executable, UserStateRoot);
	TestTrue(TEXT("the startup layout resolves"), Layout.IsValid());
	TestEqual(TEXT("startup resolves the executable layout once"),
		WoTGameBootstrapWorkerSpecLayoutResolutionCount(), 1);
	TestEqual(TEXT("cache resolution consumes the immutable startup layout"),
		WoTResolveProductionContentCacheRoot(Layout), Layout.ContentCacheRoot);
	TUniquePtr<IWoTFreeSpaceProbe> FreeSpace =
		WoTCreateProductionFreeSpaceProbe(Layout);
	TestTrue(TEXT("free-space assembly consumes the startup layout"),
		FreeSpace.IsValid());
	TUniquePtr<IWoTBootstrapWorker> Worker =
		WoTCreateProductionBootstrapWorker(Layout);
	TestTrue(TEXT("worker assembly consumes the startup layout"),
		Worker.IsValid());
	TestEqual(TEXT("all startup consumers keep one layout resolution"),
		WoTGameBootstrapWorkerSpecLayoutResolutionCount(), 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecNoUpdateInstallWrites,
	"WoT.Update.Worker.NoUpdateInstallWrites",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecNoUpdateInstallWrites::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	if (!Fixture.PublishActiveCache(*this)
		|| !Fixture.WriteInstaller(WoTGameBootstrapWorkerSpecInstallerBytes()))
	{
		return false;
	}
	TestFalse(TEXT("the Ready no-write fixture starts without updater state"),
		IFileManager::Get().DirectoryExists(*Fixture.Root.UpdateDir));
	TArray<FString> ReadyEntriesBefore;
	IFileManager::Get().FindFiles(ReadyEntriesBefore,
		*FPaths::Combine(Fixture.InstallRoot, TEXT("*")), true, true);
	ReadyEntriesBefore.Sort();
	auto DriveThroughReady = [this](FWoTGameBootstrapWorker& Worker,
		uint64 FirstEpoch)
	{
		struct FStep
		{
			EWoTBootstrapPhase Phase;
			EWoTBootstrapPhase Next;
		};
		const FStep Steps[] = {
			{EWoTBootstrapPhase::CheckingGameUpdates,
				EWoTBootstrapPhase::CheckingOriginalGameData},
			{EWoTBootstrapPhase::CheckingOriginalGameData,
				EWoTBootstrapPhase::VerifyingContent},
			{EWoTBootstrapPhase::VerifyingContent,
				EWoTBootstrapPhase::StartingGame},
			{EWoTBootstrapPhase::StartingGame, EWoTBootstrapPhase::Ready},
		};
		uint64 Epoch = FirstEpoch;
		for (const FStep& Step : Steps)
		{
			FWoTBootstrapWorkerCompletion Completion;
			if (!TestTrue(TEXT("plain launch phase completes through Ready"),
					WoTGameBootstrapWorkerSpecRunPhase(
						Worker, Step.Phase, Epoch++, Completion))
				|| !TestTrue(TEXT("plain launch phase succeeds"),
					Completion.bSucceeded)
				|| !TestEqual(TEXT("plain launch follows the Ready path"),
					static_cast<int32>(Completion.NextPhase),
					static_cast<int32>(Step.Next)))
			{
				return false;
			}
		}
		return true;
	};
	for (bool bOffline : {false, true})
	{
		int32 ProbeCalls = 0;
		int32 RecoveryCalls = 0;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		if (bOffline)
		{
			Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		}
		else
		{
			WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V1);
		}
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				[&ProbeCalls]()
				{
					++ProbeCalls;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::Io,
						TEXT("a plain launch must not probe the install root")));
				},
				[&RecoveryCalls]()
				{
					++RecoveryCalls;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::Io,
						TEXT("missing updater state must not run recovery")));
				});
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)
			|| !DriveThroughReady(*Worker, bOffline ? 20 : 10))
		{
			return false;
		}
		FWoTUpdateState Background;
		TestTrue(bOffline
				? TEXT("offline background check completes after Ready")
				: TEXT("current background check completes after Ready"),
			WoTGameBootstrapWorkerSpecRunBackground(*Worker, Background));
		TestEqual(bOffline
				? TEXT("offline background check remains None")
				: TEXT("current background check remains None"),
			static_cast<int32>(Background.Status),
			static_cast<int32>(EWoTUpdateStatus::None));
		TestEqual(TEXT("plain launch performs no install writability probe"),
			ProbeCalls, 0);
		TestEqual(TEXT("missing updater state performs no activation recovery"),
			RecoveryCalls, 0);
		TestFalse(TEXT("Ready and background create no updater directory"),
			IFileManager::Get().DirectoryExists(*Fixture.Root.UpdateDir));
	}
	TArray<FString> ReadyEntriesAfter;
	IFileManager::Get().FindFiles(ReadyEntriesAfter,
		*FPaths::Combine(Fixture.InstallRoot, TEXT("*")), true, true);
	ReadyEntriesAfter.Sort();
	TestTrue(TEXT("Ready plus offline/current background preserve install entries"),
		ReadyEntriesAfter == ReadyEntriesBefore);

	TestTrue(TEXT("the existing-empty updater-state fixture is created"),
		IFileManager::Get().MakeDirectory(*Fixture.Root.UpdateDir, true));
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> EmptyUpdateBefore =
		OpenNativeDirectoryNoFollow(Fixture.Root.UpdateDir);
	if (!TestFalse(TEXT("the existing-empty updater directory opens"),
		EmptyUpdateBefore.IsError()))
	{
		return false;
	}
	const FWoTNativeFileIdentity EmptyUpdateIdentity =
		EmptyUpdateBefore.GetValue().GetIdentity();
	EmptyUpdateBefore = TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Success(
		FWoTNativeDirectoryHandle());
	TArray<FString> EmptyInstallEntriesBefore;
	IFileManager::Get().FindFiles(EmptyInstallEntriesBefore,
		*FPaths::Combine(Fixture.InstallRoot, TEXT("*")), true, true);
	EmptyInstallEntriesBefore.Sort();
	TArray<FString> EmptyUpdateEntriesBefore;
	IFileManager::Get().FindFiles(EmptyUpdateEntriesBefore,
		*FPaths::Combine(Fixture.Root.UpdateDir, TEXT("*")), true, true);
	EmptyUpdateEntriesBefore.Sort();
	for (bool bOffline : {false, true})
	{
		int32 ProbeCalls = 0;
		int32 RecoveryCalls = 0;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		if (bOffline)
		{
			Hops->Steps.Add(WoTGameBootstrapWorkerSpecFailureHop());
		}
		else
		{
			WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V1);
		}
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(
				Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
				[&ProbeCalls]()
				{
					++ProbeCalls;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::Io,
						TEXT("a plain launch must not probe the install root")));
				},
				[&RecoveryCalls]()
				{
					++RecoveryCalls;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::Io,
						TEXT("an empty update directory must not run recovery")));
				});
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker,
				bOffline ? 40 : 30)
			|| !DriveThroughReady(*Worker, bOffline ? 50 : 60))
		{
			return false;
		}
		FWoTUpdateState Background;
		TestTrue(TEXT("existing-empty background check completes after Ready"),
			WoTGameBootstrapWorkerSpecRunBackground(*Worker, Background));
		TestEqual(TEXT("existing-empty background check remains None"),
			static_cast<int32>(Background.Status),
			static_cast<int32>(EWoTUpdateStatus::None));
		TestEqual(TEXT("existing-empty launch performs no install probe"),
			ProbeCalls, 0);
		TestEqual(TEXT("existing-empty launch performs no activation recovery"),
			RecoveryCalls, 0);
	}
	TArray<FString> EmptyInstallEntriesAfter;
	IFileManager::Get().FindFiles(EmptyInstallEntriesAfter,
		*FPaths::Combine(Fixture.InstallRoot, TEXT("*")), true, true);
	EmptyInstallEntriesAfter.Sort();
	TArray<FString> EmptyUpdateEntriesAfter;
	IFileManager::Get().FindFiles(EmptyUpdateEntriesAfter,
		*FPaths::Combine(Fixture.Root.UpdateDir, TEXT("*")), true, true);
	EmptyUpdateEntriesAfter.Sort();
	TestTrue(TEXT("existing-empty launch preserves every install-root entry"),
		EmptyInstallEntriesAfter == EmptyInstallEntriesBefore);
	TestTrue(TEXT("existing-empty launch leaves the updater directory empty"),
		EmptyUpdateEntriesAfter == EmptyUpdateEntriesBefore);
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> EmptyUpdateAfter =
		OpenNativeDirectoryNoFollow(Fixture.Root.UpdateDir);
	TestTrue(TEXT("existing-empty launch preserves the updater directory object"),
		!EmptyUpdateAfter.IsError()
			&& EmptyUpdateAfter.GetValue().GetIdentity() == EmptyUpdateIdentity);
	FString LaunchTempRoot = FPlatformProcess::UserTempDir();
#if PLATFORM_MAC
	if (LaunchTempRoot.StartsWith(
			TEXT("/private/var/"), ESearchCase::CaseSensitive))
	{
		LaunchTempRoot = TEXT("/var/") + LaunchTempRoot.RightChop(13);
	}
#endif
	const FString Base = FPaths::Combine(LaunchTempRoot,
		TEXT("wot-no-update-install-writes-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
#if PLATFORM_MAC
	const TArray<FString> Leaves = {TEXT("WoT.app"), TEXT("WoT 2.app")};
#else
	const TArray<FString> Leaves = {TEXT("WoT-Win64"), TEXT("game"),
		TEXT("WoT-Win64 (1)"), TEXT("Wheel of Time")};
#endif
	for (int32 Index = 0; Index < Leaves.Num(); ++Index)
	{
		const FString InstallRoot = FPaths::Combine(
			Base, FString::Printf(TEXT("copy-%d"), Index));
		const FString UserStateRoot = InstallRoot + TEXT("-user");
		const FString LiveRoot = FPaths::Combine(InstallRoot, Leaves[Index]);
		if (!IFileManager::Get().MakeDirectory(*LiveRoot, true)
			|| !IFileManager::Get().MakeDirectory(*UserStateRoot, true))
		{
			AddError(TEXT("could not create the no-update launch fixture"));
			return false;
		}
		TArray<FString> InstallEntriesBefore;
		IFileManager::Get().FindFiles(
			InstallEntriesBefore, *FPaths::Combine(InstallRoot, TEXT("*")), true, true);
		InstallEntriesBefore.Sort();
#if PLATFORM_MAC
		if (Index == 0)
		{
			const FString GuardedExecutable = FPaths::Combine(
				LiveRoot, TEXT("Contents"), TEXT("MacOS"), TEXT("WoT"));
			const FWoTResolvedExecutableLayout SymlinkAncestorLayout =
				WoTResolveExecutableLayoutFromPath(
					GuardedExecutable, UserStateRoot);
			TestTrue(TEXT("a /var launch preserves guarded startup"),
				SymlinkAncestorLayout.IsValid());
			TestTrue(TEXT("a /var launch keeps updates enabled"),
				SymlinkAncestorLayout.CanUpdate());
			TestTrue(TEXT("a /var launch has no notify-only reason"),
				SymlinkAncestorLayout.UpdateDisableReason.IsEmpty());
			TestTrue(TEXT("the launch path uses a macOS ancestor alias"),
				LiveRoot.StartsWith(TEXT("/var/"), ESearchCase::CaseSensitive)
				|| LiveRoot.StartsWith(
					TEXT("/tmp/"), ESearchCase::CaseSensitive));
			TestNotEqual(TEXT("the live tree is canonicalized from its handle"),
				SymlinkAncestorLayout.InstalledTreeRoot, LiveRoot);
			SetWoTBootstrapHeartbeatRootForTesting(UserStateRoot);
			TestTrue(TEXT("a /var user root accepts a heartbeat"),
				WoTRecordBootstrapHeartbeat(TEXT("phase-begin"),
					EWoTBootstrapPhase::EstablishingIdentity, 0));
			FString VarHeartbeat;
			TestTrue(TEXT("the /var heartbeat is readable"),
				FFileHelper::LoadFileToString(VarHeartbeat,
					*FPaths::Combine(UserStateRoot, TEXT("UpdaterState"),
						WoTBootstrapHeartbeatFileName())));
			SetWoTBootstrapHeartbeatRootForTesting(FString());
		}
#endif
		int32 ProbeCalls = 0;
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V1);
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, LiveRoot,
				{}, {}, [&ProbeCalls]()
				{
					++ProbeCalls;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::Io,
						TEXT("a read-only no-update copy must not be probed")));
				}, {}, UserStateRoot);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker)) return false;
		FWoTBootstrapWorkerCompletion Current;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Current)) return false;
		TestTrue(Leaves[Index] + TEXT(" no-update launch succeeds"),
			Current.bSucceeded);
		TestEqual(Leaves[Index] + TEXT(" performs no writability probe"),
			ProbeCalls, 0);
		TArray<FString> InstallEntriesAfter;
		IFileManager::Get().FindFiles(
			InstallEntriesAfter, *FPaths::Combine(InstallRoot, TEXT("*")), true, true);
		InstallEntriesAfter.Sort();
		TestTrue(Leaves[Index] + TEXT(" leaves the install-root entries byte-identical"),
			InstallEntriesAfter == InstallEntriesBefore);
		TestFalse(Leaves[Index] + TEXT(" creates no updater directory on plain start"),
			IFileManager::Get().DirectoryExists(
				*FPaths::Combine(InstallRoot, TEXT(".wot-update"))));
	}
	const TArray<FString> ReservedLeaves = {
		TEXT(".wot-update"), TEXT(".WOT-UPDATE")};
	for (int32 Index = 0; Index < ReservedLeaves.Num(); ++Index)
	{
		const FString InstallRoot = FPaths::Combine(
			Base, FString::Printf(TEXT("reserved-copy-%d"), Index));
		const FString UserStateRoot = InstallRoot + TEXT("-user");
		const FString LiveRoot = FPaths::Combine(
			InstallRoot, ReservedLeaves[Index]);
		if (!IFileManager::Get().MakeDirectory(*LiveRoot, true)
			|| !IFileManager::Get().MakeDirectory(*UserStateRoot, true))
		{
			AddError(TEXT("could not create the reserved-leaf launch fixture"));
			return false;
		}
#if PLATFORM_MAC
		const FString GuardedExecutable = FPaths::Combine(
			Base, TEXT("copy-0"), TEXT("WoT.app"), TEXT("Contents"),
			TEXT("MacOS"), TEXT("WoT"));
		const FWoTResolvedExecutableLayout ReservedLayout =
			WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithNativeTreePath(
				GuardedExecutable, UserStateRoot, LiveRoot);
#else
		const FString GuardedExecutable = FPaths::Combine(
			LiveRoot, TEXT("WoT"), TEXT("Binaries"), TEXT("Win64"),
			TEXT("WoT.exe"));
		const FWoTResolvedExecutableLayout ReservedLayout =
			WoTResolveExecutableLayoutFromPath(
				GuardedExecutable, UserStateRoot);
#endif
		TestTrue(ReservedLeaves[Index] + TEXT(" preserves the guarded startup layout"),
			ReservedLayout.IsValid());
		TestFalse(ReservedLeaves[Index] + TEXT(" disables in-place updates"),
			ReservedLayout.CanUpdate());
		TestEqual(ReservedLeaves[Index] + TEXT(" has the reserved-leaf reason"),
			ReservedLayout.UpdateDisableReason,
			FString(TEXT("reserved-live-leaf")));
		TestEqual(ReservedLeaves[Index] + TEXT(" leaves the cache under user state"),
			ReservedLayout.ContentCacheRoot,
			FPaths::Combine(
				WoTGameBootstrapWorkerSpecCanonicalOwnedRoot(UserStateRoot),
				TEXT("ContentCache")));
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*Hops, Fixture.V2);
		int32 ProbeCalls = 0;
		int32 RecoveryCalls = 0;
		int32 TranslocationCalls = 0;
		SetWoTBootstrapHeartbeatRootForTesting(UserStateRoot);
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, LiveRoot,
				{}, {}, [&ProbeCalls]()
				{
					++ProbeCalls;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::Io,
						TEXT("reserved live leaf must not be probed")));
				}, [&RecoveryCalls]()
				{
					++RecoveryCalls;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::Io,
						TEXT("reserved live leaf must not be recovered")));
			}, UserStateRoot, TEXT("reserved-live-leaf"),
			[&TranslocationCalls](const FString&)
			{
				++TranslocationCalls;
				return true;
			});
		FWoTBootstrapWorkerCompletion Established;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::EstablishingIdentity, 1, Established))
		{
			AddError(TEXT("reserved-leaf identity establishment timed out"));
			return false;
		}
		TestTrue(ReservedLeaves[Index] + TEXT(" startup succeeds"),
			Established.bSucceeded);
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Checked))
		{
			AddError(TEXT("reserved-leaf update check timed out"));
			return false;
		}
		TestTrue(ReservedLeaves[Index] + TEXT(" update check succeeds"),
			Checked.bSucceeded);
		TestTrue(ReservedLeaves[Index] + TEXT(" updates are notify-only"),
			Checked.bUpdateNotifyOnly);
		TestEqual(ReservedLeaves[Index] + TEXT(" performs no install probe"),
			ProbeCalls, 0);
		TestEqual(ReservedLeaves[Index] + TEXT(" performs no activation recovery"),
			RecoveryCalls, 0);
		TestEqual(ReservedLeaves[Index]
			+ TEXT(" does not query translocation for an invalid layout"),
			TranslocationCalls, 0);
		FString Heartbeat;
		TestTrue(ReservedLeaves[Index] + TEXT(" notify-only heartbeat is readable"),
			FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
				UserStateRoot, TEXT("UpdaterState"),
				WoTBootstrapHeartbeatFileName())));
		TestTrue(ReservedLeaves[Index] + TEXT(" reports the precise reason"),
			Heartbeat.Contains(TEXT("notify-only reason=reserved-live-leaf"),
				ESearchCase::CaseSensitive));
		TArray<FString> LiveChildren;
		IFileManager::Get().FindFiles(LiveChildren,
			*FPaths::Combine(LiveRoot, TEXT("*")), true, true);
		TestEqual(ReservedLeaves[Index] + TEXT(" startup writes nothing to the live tree"),
			LiveChildren.Num(), 0);
		Worker.Reset();
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	}

	const FString UnresolvedInstallRoot = FPaths::Combine(
		Base, TEXT("name-query-failure-copy"));
	const FString UnresolvedUserStateRoot =
		UnresolvedInstallRoot + TEXT("-user");
#if PLATFORM_MAC
	const FString UnresolvedLiveRoot = FPaths::Combine(
		UnresolvedInstallRoot, TEXT("WoT.app"));
	const FString UnresolvedExecutable = FPaths::Combine(
		UnresolvedLiveRoot, TEXT("Contents"), TEXT("MacOS"), TEXT("WoT"));
#else
	const FString UnresolvedLiveRoot = FPaths::Combine(
		UnresolvedInstallRoot, TEXT("Wheel Of Time"));
	const FString UnresolvedExecutable = FPaths::Combine(
		UnresolvedLiveRoot, TEXT("WoT"), TEXT("Binaries"), TEXT("Win64"),
		TEXT("WoT.exe"));
#endif
	if (!IFileManager::Get().MakeDirectory(*UnresolvedLiveRoot, true)
		|| !IFileManager::Get().MakeDirectory(*UnresolvedUserStateRoot, true))
	{
		AddError(TEXT("could not create the name-query failure launch fixture"));
		return false;
	}
	const FWoTResolvedExecutableLayout UnresolvedLayout =
		WoTGameBootstrapWorkerSpecResolveExecutableLayoutWithNativeTreePath(
			UnresolvedExecutable, UnresolvedUserStateRoot, FStringView(), true);
	TestTrue(TEXT("name-query failure preserves startup"),
		UnresolvedLayout.IsValid());
	TestFalse(TEXT("name-query failure disables updates"),
		UnresolvedLayout.CanUpdate());
	TestEqual(TEXT("name-query failure reports its precise reason"),
		UnresolvedLayout.UpdateDisableReason,
		FString(TEXT("live-tree-name-unresolved")));
	auto UnresolvedAdjacentParent =
		OpenNativeDirectoryNoFollow(UnresolvedInstallRoot);
	TestTrue(TEXT("notify-only layout retains its adjacent parent identity"),
		UnresolvedLayout.bHasAdjacentInstallerParentIdentity
			&& !UnresolvedAdjacentParent.IsError()
			&& UnresolvedLayout.AdjacentInstallerParentIdentity.IsSameObject(
				UnresolvedAdjacentParent.GetValue().GetIdentity()));
	TestEqual(TEXT("name-query failure preserves the cache root"),
		UnresolvedLayout.ContentCacheRoot,
		FPaths::Combine(
			WoTGameBootstrapWorkerSpecCanonicalOwnedRoot(
				UnresolvedUserStateRoot),
			TEXT("ContentCache")));
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> UnresolvedHops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(
		*UnresolvedHops, Fixture.V2);
	int32 UnresolvedProbeCalls = 0;
	int32 UnresolvedRecoveryCalls = 0;
	SetWoTBootstrapHeartbeatRootForTesting(UnresolvedUserStateRoot);
	TUniquePtr<FWoTGameBootstrapWorker> UnresolvedWorker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			UnresolvedInstallRoot, UnresolvedHops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {},
			UnresolvedLayout.InstalledTreeRoot, {}, {},
			[&UnresolvedProbeCalls]()
			{
				++UnresolvedProbeCalls;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("unresolved update layout must not be probed")));
			}, [&UnresolvedRecoveryCalls]()
			{
				++UnresolvedRecoveryCalls;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("unresolved update layout must not be recovered")));
			}, UnresolvedUserStateRoot, UnresolvedLayout.UpdateDisableReason);
	TestTrue(TEXT("name-query failure establishes the game session"),
		WoTGameBootstrapWorkerSpecEstablish(*this, *UnresolvedWorker));
	FWoTBootstrapWorkerCompletion UnresolvedChecked;
	TestTrue(TEXT("name-query failure completes the update check"),
		WoTGameBootstrapWorkerSpecRunPhase(*UnresolvedWorker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, UnresolvedChecked));
	TestTrue(TEXT("name-query failure remains playable"),
		UnresolvedChecked.bSucceeded);
	TestTrue(TEXT("name-query failure is notify-only"),
		UnresolvedChecked.bUpdateNotifyOnly);
	TestEqual(TEXT("name-query failure performs no install probe"),
		UnresolvedProbeCalls, 0);
	TestEqual(TEXT("name-query failure performs no activation recovery"),
		UnresolvedRecoveryCalls, 0);
	FString UnresolvedHeartbeat;
	TestTrue(TEXT("name-query failure heartbeat is readable"),
		FFileHelper::LoadFileToString(UnresolvedHeartbeat, *FPaths::Combine(
			UnresolvedUserStateRoot, TEXT("UpdaterState"),
			WoTBootstrapHeartbeatFileName())));
	TestTrue(TEXT("name-query failure heartbeat has the precise reason"),
		UnresolvedHeartbeat.Contains(
			TEXT("notify-only reason=live-tree-name-unresolved"),
			ESearchCase::CaseSensitive));
	TArray<FString> UnresolvedLiveChildren;
	IFileManager::Get().FindFiles(UnresolvedLiveChildren,
		*FPaths::Combine(UnresolvedLiveRoot, TEXT("*")), true, true);
	TestEqual(TEXT("name-query failure writes nothing in the live tree"),
		UnresolvedLiveChildren.Num(), 0);
	UnresolvedWorker.Reset();
	SetWoTBootstrapHeartbeatRootForTesting(FString());

#if PLATFORM_WINDOWS
	FString ExecutablePathFailureReason;
	WoTSetCurrentExecutableFinalPathQueryFailureForTesting(true);
	TWoTBootstrapResult<FWoTNativeExecutableIdentity> HeldRunningImage =
		OpenCurrentExecutableIdentityNoFollow(&ExecutablePathFailureReason);
	WoTSetCurrentExecutableFinalPathQueryFailureForTesting(false);
	TestFalse(TEXT("injected held-image final-name query keeps the image identity"),
		HeldRunningImage.IsError());
	TestEqual(TEXT("held-image query failure reports the update-disable reason"),
		ExecutablePathFailureReason,
		FString(TEXT("executable-path-unresolved")));
	FWoTResolvedExecutableLayout ExecutableQueryLayout =
		WoTResolveExecutableLayoutFromPath(
			UnresolvedExecutable, UnresolvedUserStateRoot);
	ExecutableQueryLayout.UpdateDisableReason = ExecutablePathFailureReason;
	TestTrue(TEXT("held-image query failure preserves the guarded startup layout"),
		ExecutableQueryLayout.IsValid());
	TestFalse(TEXT("held-image query failure disables updates"),
		ExecutableQueryLayout.CanUpdate());
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> ExecutableQueryHops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(
		*ExecutableQueryHops, Fixture.V2);
	SetWoTBootstrapHeartbeatRootForTesting(UnresolvedUserStateRoot);
	TUniquePtr<FWoTGameBootstrapWorker> ExecutableQueryWorker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			UnresolvedInstallRoot, ExecutableQueryHops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {},
			ExecutableQueryLayout.InstalledTreeRoot, {}, {}, {}, {},
			UnresolvedUserStateRoot,
			ExecutableQueryLayout.UpdateDisableReason);
	TestTrue(TEXT("held-image query failure still establishes the game session"),
		WoTGameBootstrapWorkerSpecEstablish(*this, *ExecutableQueryWorker));
	FWoTBootstrapWorkerCompletion ExecutableQueryChecked;
	TestTrue(TEXT("held-image query failure completes notify-only startup"),
		WoTGameBootstrapWorkerSpecRunPhase(*ExecutableQueryWorker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2,
			ExecutableQueryChecked));
	TestTrue(TEXT("held-image query failure remains playable"),
		ExecutableQueryChecked.bSucceeded
			&& ExecutableQueryChecked.bUpdateNotifyOnly);
	TestEqual(TEXT("held-image query failure preserves its exact reason"),
		ExecutableQueryChecked.NotifyOnlyReason,
		FString(TEXT("executable-path-unresolved")));
	ExecutableQueryWorker.Reset();
	SetWoTBootstrapHeartbeatRootForTesting(FString());
#endif

#if PLATFORM_MAC
	const FString VolumeRoot = TEXT("/");
	const FString VolumeLive = TEXT("/WoT 2.app");
#else
	const FString VolumeRoot = TEXT("C:/");
	const FString VolumeLive = TEXT("C:/Wheel of Time");
#endif
	const FString VolumeUpdate = FPaths::Combine(VolumeRoot, TEXT(".wot-update"));
	const bool bVolumeUpdateExisted =
		IFileManager::Get().DirectoryExists(*VolumeUpdate);
	const FString VolumeUserState = FPaths::Combine(Base, TEXT("volume-user"));
	IFileManager::Get().MakeDirectory(*VolumeUserState, true);
	int32 VolumeProbeCalls = 0;
	int32 VolumeRecoveryCalls = 0;
	int32 VolumeTranslocationCalls = 0;
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> VolumeHops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	WoTGameBootstrapWorkerSpecAppendAnnouncementScript(*VolumeHops, Fixture.V1);
	TUniquePtr<FWoTGameBootstrapWorker> VolumeWorker =
		WoTGameBootstrapWorkerSpecMakeWorker(VolumeRoot, VolumeHops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, VolumeLive,
			{}, {}, [&VolumeProbeCalls]()
			{
				++VolumeProbeCalls;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("a volume-root no-update copy must not be probed")));
			}, [&VolumeRecoveryCalls]()
			{
				++VolumeRecoveryCalls;
				return FWoTBootstrapStatus::Success();
			}, VolumeUserState, FString(),
			[&VolumeTranslocationCalls](const FString&)
			{
				++VolumeTranslocationCalls;
				// Models SecTranslocate's conservative answer when the volume-root
				// launch path cannot be queried in this unprivileged fixture.
				return true;
			});
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *VolumeWorker)) return false;
	FWoTBootstrapWorkerCompletion VolumeCurrent;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*VolumeWorker,
			EWoTBootstrapPhase::CheckingGameUpdates, 2, VolumeCurrent)) return false;
	if (!TestTrue(TEXT("the volume-root no-update launch succeeds"),
		VolumeCurrent.bSucceeded))
	{
		AddError(TEXT("volume-root failure detail: ") + VolumeCurrent.FailureDetail);
	}
	TestEqual(TEXT("the volume-root no-update launch performs no probe"),
		VolumeProbeCalls, 0);
	TestEqual(TEXT("the volume-root launch performs one translocation query"),
		VolumeTranslocationCalls, 1);
	TestEqual(TEXT("the volume-root notify-only launch performs no recovery"),
		VolumeRecoveryCalls, 0);
	TestTrue(TEXT("the volume-root launch is notify-only after query failure"),
		VolumeCurrent.bUpdateNotifyOnly);
	TestEqual(TEXT("the volume-root launch reports the translocation reason"),
		VolumeCurrent.NotifyOnlyReason, FString(TEXT("translocated")));
	TestEqual(TEXT("the volume-root no-update launch preserves .wot-update absence"),
		IFileManager::Get().DirectoryExists(*VolumeUpdate), bVolumeUpdateExisted);
	VolumeWorker.Reset();
	IFileManager::Get().DeleteDirectory(*Base, false, true);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecCanonicalFreshState,
	"WoT.Update.Worker.CanonicalFreshState",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecCanonicalFreshState::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this)) return false;
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	TestTrue(TEXT("a fresh canonical copy establishes its two roots"),
		WoTGameBootstrapWorkerSpecEstablish(*this, *Worker));
	Worker.Reset();
	if (!Fixture.SeedUpdateState(*this)) return false;
	Worker = WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	TestTrue(TEXT("signed knowledge does not change the running copy root"),
		WoTGameBootstrapWorkerSpecEstablish(*this, *Worker, 2));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecSubsystem,
	"WoT.Update.Worker.Subsystem",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecSubsystem::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecGlobalCleanup Cleanup;
	ResetWoTPayloadActivationForTesting();
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	TArray<uint8> InstallerBytes;
	if (!TestTrue(TEXT("subsystem synthetic Inno fixture loads"),
		WoTGameBootstrapWorkerSpecLoadInnoInstaller(InstallerBytes)))
	{
		return false;
	}
	if (!TestTrue(TEXT("subsystem worker has a valid adjacent installer"),
		Fixture.WriteInstaller(InstallerBytes)))
	{
		return false;
	}
	const FWoTRetailContract Contract =
		WoTWorldMaterializerTest_WorldContract();
	FWoTGameBootstrapWorkerSpecContractScope ContractScope(Contract);
	const FWoTInstallerAcquisitionPins InstallerPins =
		WoTGameBootstrapWorkerSpecInnoPins(InstallerBytes);
	TWoTBootstrapResult<FWoTCacheStore> CacheOpened =
		FWoTCacheStore::ForTesting(
			WoTGameBootstrapWorkerSpecCacheRoot(Fixture.InstallRoot));
	TWoTBootstrapResult<FWoTVerifiedInstaller> VerifiedInstaller =
		WoTGameBootstrapWorkerSpecVerifiedInstaller(
			WoTGameBootstrapWorkerSpecInstallerPath(Fixture.InstallRoot));
	if (!TestFalse(TEXT("subsystem cache store opens"), CacheOpened.IsError())
		|| !TestFalse(TEXT("subsystem installer snapshot verifies"),
			VerifiedInstaller.IsError()))
	{
		return false;
	}
	FWoTGameBootstrapWorkerSpecCacheProgress CacheProgress;
	FWoTCacheBootstrapOperation CacheOperation(
		[](FWoTRetailPackage& Package,
			const FWoTRetailContract& InContract, IWoTObjectWriter& Writer,
			IWoTProgressSink& Progress)
		{
			return WoTWorldMaterializerTest_BuildWorldSet(
				Package, InContract, Writer, Progress);
		});
	TWoTBootstrapResult<FWoTPublishedCacheReceipt> Published =
		CacheOperation.BuildAndPublish(
			MoveTemp(VerifiedInstaller).GetValue(), CacheOpened.GetValue(),
			CacheProgress);
	if (!TestFalse(TEXT("subsystem cached world set publishes"),
		Published.IsError()))
	{
		return false;
	}
	FWoTCacheGenerationBuildIdentity Expected;
	Expected.bHasBuildIdentity = true;
	Expected.InstallerSha256 = Published.GetValue().InstallerSha256;
	Expected.ContractSha256 = Published.GetValue().ContractSha256;
	Expected.ProductionDescriptorSha256 =
		Published.GetValue().ProductionDescriptorSha256;
	int32 SnapshotPhysicalReads = 0;
	auto SnapshotPhysicalReadObserver =
		WoTObserveCacheSnapshotPhysicalReadsForTesting(
			[&](FStringView) { ++SnapshotPhysicalReads; });
	TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>> Current =
		CacheOpened.GetValue().TryOpenCurrent(Expected, &CacheProgress);
	if (!TestFalse(TEXT("subsystem current snapshot opens"), Current.IsError())
		|| !TestTrue(TEXT("subsystem current snapshot exists"),
			Current.GetValue().IsSet()))
	{
		return false;
	}
	const FWoTSha256 SnapshotManifest =
		Current.GetValue().GetValue().GetManifestSha256();
	TSharedRef<TOptional<FWoTCacheGenerationLease>> PreparedSnapshot =
		MakeShared<TOptional<FWoTCacheGenerationLease>>();
	PreparedSnapshot->Emplace(MoveTemp(Current.GetValue().GetValue()));
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();

	UWoTBootstrapSubsystem::ResetTestingAuthorityAndAdapters();
	FWoTWorldMaterializer::ResetAuthorityForAutomationTests();
	UPackage* BootstrapPackage =
		CreatePackage(TEXT("/Game/WoT/Maps/WoTBootstrap"));
	BootstrapPackage->SetFlags(RF_Transient);
	UWorld* FixedBootstrapWorld = UWorld::CreateWorld(
		EWorldType::Game, false,
		MakeUniqueObjectName(BootstrapPackage, UWorld::StaticClass(),
			FName(TEXT("World"))),
		BootstrapPackage, /*bAddToRoot*/ true, ERHIFeatureLevel::Num,
		/*InIVS*/ nullptr, /*bInSkipInitWorld*/ true);
	if (!TestNotNull(TEXT("fixed bootstrap world exists"), FixedBootstrapWorld))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		if (!FixedBootstrapWorld->HasAnyFlags(
			RF_BeginDestroyed | RF_FinishDestroyed))
		{
			FixedBootstrapWorld->DestroyWorld(false);
		}
	};
	FString BootstrapExposureError;
	if (!TestTrue(TEXT("fixed bootstrap world is exposed before adoption"),
		WoTGuardedStartup_MaterializeWorld(FixedBootstrapWorld,
			EWoTWorldStartPath::NormalInitialLoad, &BootstrapExposureError)
		&& WoTCommitAuthenticatedWorldExposure(
			FixedBootstrapWorld, &BootstrapExposureError)))
	{
		AddError(BootstrapExposureError);
		return false;
	}
	UWoTBootstrapSubsystem::SetCreationEnabledForTesting(true);
	UWoTPreferencesSubsystem::SetCreationEnabledForTesting(true);
	TestTrue(TEXT("subsystem worker spec publishes EarlyUI"),
		WoTGameBootstrapWorkerSpecPublishEarlyUI());
	const FWoTResolvedExecutableLayout ProcessLayout =
		WoTResolveExecutableLayoutFromPath(
			FPaths::Combine(Fixture.InstallRoot,
				WoTGameBootstrapWorkerSpecRootLeaf,
#if PLATFORM_MAC
				TEXT("Contents/MacOS/WoT")),
#else
				TEXT("WoT/Binaries/Win64/WoT.exe")),
#endif
			Fixture.UserStateRoot);
	TestTrue(TEXT("subsystem fixture has one canonical process layout"),
		ProcessLayout.IsValid());
	UWoTBootstrapSubsystem::SetCanonicalAdjacentInstallerPathForTesting(
		ProcessLayout.CanonicalAdjacentInstallerPath);
	const FString InstallRoot = Fixture.InstallRoot;
	UWoTBootstrapSubsystem::SetAdaptersForTesting(
		[InstallRoot, Hops, InstallerPins, PreparedSnapshot]()
			-> TUniquePtr<IWoTBootstrapWorker>
		{
			TUniquePtr<FWoTGameBootstrapWorker> Worker =
				WoTGameBootstrapWorkerSpecMakeWorker(
				InstallRoot, Hops, TFunction<void()>(),
				WoT::Bootstrap::Trust::AnnouncementUrl, InstallerPins,
				WoTGameBootstrapWorkerSpecContentBuild);
			if (PreparedSnapshot->IsSet())
			{
				Worker->SetInitialCacheSnapshotForTesting(
					MoveTemp(PreparedSnapshot->GetValue()));
				PreparedSnapshot->Reset();
			}
			return Worker;
		},
		[]() -> TUniquePtr<IWoTFreeSpaceProbe>
		{
			return MakeUnique<
				FWoTGameBootstrapWorkerSpecFreeSpace>();
		});
	int32 Activations = 0;
	bool bReadySawPayload = false;
	bool bReadySawWorld = false;
	bool bManifestMutatedBeforeActivation = false;
	const FString ManifestPath = FPaths::Combine(
		WoTGameBootstrapWorkerSpecCacheRoot(Fixture.InstallRoot),
		TEXT("current"), TEXT("manifest.json"));
	UWoTBootstrapSubsystem::SetReadyAdoptionObserverForTesting(
		[&](bool bPayload, bool bWorld)
		{
			bReadySawPayload = bPayload;
			bReadySawWorld = bWorld;
		});
	UWoTBootstrapSubsystem::SetBeforeCacheActivationObserverForTesting(
		[&](const FWoTCacheGenerationLease& Snapshot)
		{
			++Activations;
			TestTrue(TEXT("worker hands off the preopened snapshot"),
				Snapshot.GetManifestSha256() == SnapshotManifest);
			TArray<uint8> Bytes;
			if (FFileHelper::LoadFileToArray(Bytes, *ManifestPath)
				&& !Bytes.IsEmpty())
			{
				Bytes[Bytes.Num() / 2] ^= 1u;
				bManifestMutatedBeforeActivation =
					FFileHelper::SaveArrayToFile(Bytes, *ManifestPath);
			}
		});

	WoTTest::FSpecWorldOptions Options;
	Options.GameInstanceClass = UWoTGameInstance::StaticClass();
	Options.bStartPlay = false;
	Options.bActivatePayload = false;
	UWorld* World = WoTTest::CreateSpecWorld(Options);
	if (!TestNotNull(TEXT("worker subsystem world exists"), World))
		return false;
	UWoTBootstrapSubsystem* Bootstrap =
		World->GetGameInstance()->GetSubsystem<UWoTBootstrapSubsystem>();
	if (!TestNotNull(TEXT("worker subsystem exists"), Bootstrap))
	{
		WoTTest::DestroySpecWorld(World);
		return false;
	}
	const double Deadline =
		FPlatformTime::Seconds() + WoTGameBootstrapWorkerSpecHangSeconds;
	while (!Bootstrap->Snapshot().IsTerminal()
		&& FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.01f);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(
			ENamedThreads::GameThread);
		WoTTest::TickWorld(World, 1);
	}
	TestFalse(TEXT("worker subsystem initializes"),
		Bootstrap->HasInitializationFailed());
	TestEqual(TEXT("worker subsystem reaches Ready"),
		static_cast<int32>(Bootstrap->Snapshot().Phase),
		static_cast<int32>(EWoTBootstrapPhase::Ready));
	TestEqual(TEXT("cached worker drives the five startup phases"),
		Bootstrap->WorkerStartCountForTesting(), 5);
	TestTrue(TEXT("warm cache stays within five physical reads"),
		SnapshotPhysicalReads > 0 && SnapshotPhysicalReads <= 5);
	TestEqual(TEXT("one current snapshot is activated"), Activations, 1);
	TestTrue(TEXT("manifest mutates only after validation"),
		bManifestMutatedBeforeActivation);
	TestTrue(TEXT("Ready observes payload adoption"), bReadySawPayload);
	TestTrue(TEXT("Ready observes world adoption"), bReadySawWorld);
	TWoTBootstrapResult<TOptional<FWoTCacheGenerationLease>> Reopened =
		CacheOpened.GetValue().TryOpenCurrent(Expected, &CacheProgress);
	TestFalse(TEXT("new cache open treats mutated manifest as invalid"),
		Reopened.IsError());
	if (!Reopened.IsError())
	{
		TestFalse(TEXT("new cache open has no current view"),
			Reopened.GetValue().IsSet());
	}
	const double BackgroundDeadline =
		FPlatformTime::Seconds() + WoTGameBootstrapWorkerSpecHangSeconds;
	while (Hops->RequestCount.Load() < 2
		&& FPlatformTime::Seconds() < BackgroundDeadline)
	{
		FPlatformProcess::SleepNoStats(0.01f);
	}
	TestEqual(TEXT("Ready starts release notes and update work"),
		Hops->RequestCount.Load(), 2);
	if (Hops->Requests.Num() == 2)
	{
		TestEqual(TEXT("Ready queues release notes before update"),
			Hops->Requests[0].Url, FString(WoTReleaseNotes::LatestUrl));
		TestEqual(TEXT("Ready starts exactly one background update attempt"),
			Hops->Requests[1].Url,
			FString(WoT::Bootstrap::Trust::AnnouncementUrl));
	}
	WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

// The remembered installer: a Browse pick that built the cache is tried again
// on a later rebuild, right after the installer beside the game.
namespace
{
constexpr TCHAR WoTGameBootstrapWorkerSpecRememberedLeaf[] =
	TEXT("archived wot installer.exe");

FString WoTGameBootstrapWorkerSpecRememberedRecordFile(
	const FString& UserStateRoot)
{
	return FPaths::Combine(UserStateRoot, TEXT("UpdaterState"),
		WoTRememberedInstallerLeaf);
}

/** Reads the record exactly as the worker does. */
TOptional<FString> WoTGameBootstrapWorkerSpecRememberedPath(
	const FString& UserStateRoot)
{
	FWoTRememberedInstallerRead Read =
		WoTReadRememberedInstaller(UserStateRoot, nullptr);
	return Read.CanonicalInstallerPath;
}

/** True when this simulated process run's heartbeat has the event on a line
 * that also contains Fragment (an empty Fragment matches any such line). */
bool WoTGameBootstrapWorkerSpecHeartbeatHas(const FString& UserStateRoot,
	const TCHAR* Event, const FString& Fragment = FString())
{
	FString Heartbeat;
	if (!FFileHelper::LoadFileToString(Heartbeat, *FPaths::Combine(
		UserStateRoot, TEXT("UpdaterState"),
		WoTBootstrapHeartbeatFileName())))
	{
		return false;
	}
	TArray<FString> Lines;
	Heartbeat.ParseIntoArrayLines(Lines, false);
	const FString EventField =
		FString::Printf(TEXT("\"event\":\"%s\""), Event);
	for (const FString& Line : Lines)
	{
		if (Line.Contains(EventField, ESearchCase::CaseSensitive)
			&& (Fragment.IsEmpty()
				|| Line.Contains(Fragment, ESearchCase::CaseSensitive)))
		{
			return true;
		}
	}
	return false;
}

FString WoTGameBootstrapWorkerSpecDetectedFrom(const TCHAR* Source)
{
	return FString::Printf(TEXT("\"detail\":\"source=%s\","), Source);
}

/** Records InstallerPath exactly as a successful Browse pick would. */
bool WoTGameBootstrapWorkerSpecSeedRemembered(FAutomationTestBase& Test,
	const FString& UserStateRoot, const FString& InstallerPath,
	FString& OutRecordedPath)
{
	TWoTBootstrapResult<FString> Canonical =
		WoTCanonicalizeInstallerPickPath(InstallerPath);
	if (Canonical.IsError())
	{
		Test.AddError(TEXT("remembered fixture path: ")
			+ Canonical.GetError().ToString());
		return false;
	}
	FWoTBootstrapStatus Written = WoTWriteRememberedInstaller(
		UserStateRoot, nullptr, Canonical.GetValue());
	if (Written.IsError())
	{
		Test.AddError(TEXT("remembered fixture record: ")
			+ Written.GetError().ToString());
		return false;
	}
	OutRecordedPath = MoveTemp(Canonical).GetValue();
	return true;
}

bool WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(
	const FString& InstallRoot, TConstArrayView<uint8> Bytes)
{
	const FString Directory = InstallRoot + TEXT("-downloads");
	return IFileManager::Get().MakeDirectory(*Directory, true)
		&& WoTGameBootstrapWorkerSpecSave(
			FPaths::Combine(Directory, WoT::Bootstrap::InstallerName), Bytes);
}

struct FWoTGameBootstrapWorkerSpecRememberedCheck
{
	FWoTBootstrapWorkerCompletion Completion;
	int32 Hashes = 0;
	int32 Requests = 0;
};

/** One fresh worker (one simulated launch) through the content check, with
 * the small default installer pins. */
bool WoTGameBootstrapWorkerSpecRunRememberedCheck(FAutomationTestBase& Test,
	const FString& InstallRoot, FWoTGameBootstrapWorkerSpecRememberedCheck& Out)
{
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TAtomic<int32> HashCount{0};
	FWoTInstallerAcquisitionTestHooks Hooks;
	Hooks.FullHash = [&HashCount](uint64) { ++HashCount; };
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, Hooks);
	if (!WoTGameBootstrapWorkerSpecEstablish(Test, *Worker))
	{
		return false;
	}
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 2, Out.Completion))
	{
		Test.AddError(TEXT("the remembered-installer content check timed out"));
		return false;
	}
	Worker.Reset();
	Out.Hashes = HashCount.Load();
	Out.Requests = Hops->Requests.Num();
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecRememberedRecordFormat,
	"WoT.Update.Worker.RememberedInstaller.RecordFormat",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecRememberedRecordFormat::RunTest(const FString&)
{
#if PLATFORM_WINDOWS
	const FString Valid =
		TEXT("D:/Old Games/setup_the_wheel_of_time_333b_(54846).exe");
	const FString NonAscii = TEXT("D:/Caf\u00e9 Games/setup.exe");
	const FString TooLong = TEXT("C:/") + FString::ChrN(5000, TEXT('a'));
	const TCHAR* const Accepted[] = {
		TEXT("C:/setup.exe"),
		TEXT("z:/a/b/c.exe"),
		TEXT("//nas/games/setup.exe"),
	};
	const TCHAR* const Refused[] = {
		TEXT(""),
		TEXT("setup.exe"),
		TEXT("Old Games/setup.exe"),
		TEXT("/Old Games/setup.exe"),
		TEXT("C:setup.exe"),
		TEXT("C:/"),
		TEXT("1:/setup.exe"),
		TEXT("C:\\Old Games\\setup.exe"),
		TEXT("C:/Old Games//setup.exe"),
		TEXT("C:/Old Games/./setup.exe"),
		TEXT("C:/Old Games/../setup.exe"),
		TEXT("C:/Old Games/setup.exe/"),
		TEXT("C:/Old Games/setup.exe:hidden"),
		TEXT("C:/Old Games/setup.exe."),
		TEXT("C:/Old Games/setup.exe "),
		TEXT("C:/Old*Games/setup.exe"),
		TEXT("C:/Old Games/set\tup.exe"),
		TEXT("//?/C:/Old Games/setup.exe"),
		TEXT("//./PhysicalDrive0/setup.exe"),
		TEXT("//nas/games"),
		TEXT("//nas//setup.exe"),
	};
#else
	const FString Valid =
		TEXT("/Volumes/Old Games/setup_the_wheel_of_time_333b_(54846).exe");
	const FString NonAscii = TEXT("/Users/player/Caf\u00e9 Games/setup.exe");
	const FString TooLong = TEXT("/") + FString::ChrN(5000, TEXT('a'));
	const TCHAR* const Accepted[] = {
		TEXT("/setup.exe"),
		TEXT("/Users/player/Downloads/setup.exe"),
	};
	const TCHAR* const Refused[] = {
		TEXT(""),
		TEXT("setup.exe"),
		TEXT("Users/player/setup.exe"),
		TEXT("C:/Old Games/setup.exe"),
		TEXT("/"),
		TEXT("//Users/setup.exe"),
		TEXT("/Users/player/./setup.exe"),
		TEXT("/Users/player/../setup.exe"),
		TEXT("/Users/player/setup.exe/"),
		TEXT("/Users/player\\setup.exe"),
		TEXT("/Users/pl\tayer/setup.exe"),
	};
#endif
	for (const TCHAR* Candidate : Accepted)
	{
		TestTrue(FString::Printf(TEXT("a canonical path is rememberable: %s"),
			Candidate), WoTIsRememberableInstallerPath(Candidate));
	}
	for (const TCHAR* Candidate : Refused)
	{
		TestFalse(FString::Printf(TEXT("a non-canonical path is refused: '%s'"),
			Candidate), WoTIsRememberableInstallerPath(Candidate));
		TestTrue(FString::Printf(TEXT("a refused path is never written: '%s'"),
			Candidate), WoTEncodeRememberedInstallerRecord(Candidate).IsError());
	}
	TestFalse(TEXT("an over-long path is refused"),
		WoTIsRememberableInstallerPath(TooLong));

	const FString Format = TEXT("wot-remembered-installer-v1");
	const FString Expected = FString::Printf(
		TEXT("{\"format\":\"%s\",\"path\":\"%s\"}"), *Format, *Valid);
	TWoTBootstrapResult<TArray<uint8>> Encoded =
		WoTEncodeRememberedInstallerRecord(Valid);
	if (!TestFalse(TEXT("a canonical path encodes"), Encoded.IsError()))
	{
		return false;
	}
	TestTrue(TEXT("the record is exactly its canonical JSON bytes"),
		Encoded.GetValue() == WoTGameBootstrapWorkerSpecAscii(Expected));
	TWoTBootstrapResult<FString> Decoded =
		WoTParseRememberedInstallerRecord(Encoded.GetValue());
	TestTrue(TEXT("the record round-trips"), !Decoded.IsError()
		&& Decoded.GetValue().Equals(Valid, ESearchCase::CaseSensitive));

	TWoTBootstrapResult<TArray<uint8>> Escaped =
		WoTEncodeRememberedInstallerRecord(NonAscii);
	if (!TestFalse(TEXT("a non-ASCII path encodes"), Escaped.IsError()))
	{
		return false;
	}
	bool bAscii = true;
	for (const uint8 Byte : Escaped.GetValue())
	{
		bAscii &= Byte < 0x80;
	}
	TestTrue(TEXT("the record is ASCII, other characters escaped"), bAscii);
	TWoTBootstrapResult<FString> NonAsciiDecoded =
		WoTParseRememberedInstallerRecord(Escaped.GetValue());
	TestTrue(TEXT("a non-ASCII path round-trips"), !NonAsciiDecoded.IsError()
		&& NonAsciiDecoded.GetValue().Equals(NonAscii, ESearchCase::CaseSensitive));

	// The record is untrusted input: only the exact canonical bytes of one v1
	// record with a canonical path are accepted.
	const FString Malformed[] = {
		FString(),
		TEXT("not a record"),
		Expected + TEXT("\n"),
		FString::Printf(TEXT("{ \"format\": \"%s\", \"path\": \"%s\" }"),
			*Format, *Valid),
		FString::Printf(TEXT("{\"path\":\"%s\",\"format\":\"%s\"}"),
			*Valid, *Format),
		FString::Printf(TEXT("{\"format\":\"%s\"}"), *Format),
		FString::Printf(TEXT("{\"path\":\"%s\"}"), *Valid),
		FString::Printf(TEXT("{\"extra\":0,\"format\":\"%s\",\"path\":\"%s\"}"),
			*Format, *Valid),
		FString::Printf(
			TEXT("{\"format\":\"wot-remembered-installer-v2\",\"path\":\"%s\"}"),
			*Valid),
		FString::Printf(TEXT("{\"format\":\"%s\",\"path\":7}"), *Format),
		FString::Printf(TEXT("{\"format\":\"%s\",\"path\":{\"inner\":\"%s\"}}"),
			*Format, *Valid),
		FString::Printf(TEXT("[\"%s\",\"%s\"]"), *Format, *Valid),
		FString::Printf(TEXT("{\"format\":\"%s\",\"path\":\"%s\\u0001.exe\"}"),
			*Format, *Valid),
		FString::Printf(
			TEXT("{\"format\":\"%s\",\"path\":\"relative/setup.exe\"}"), *Format),
		FString::Printf(TEXT("{\"format\":\"%s\",\"path\":\"%s\"}"), *Format,
			*FString::ChrN(40000, TEXT('a'))),
	};
	int32 Index = 0;
	for (const FString& Record : Malformed)
	{
		TestTrue(FString::Printf(TEXT("malformed record %d is refused"), Index),
			WoTParseRememberedInstallerRecord(
				WoTGameBootstrapWorkerSpecAscii(Record)).IsError());
		++Index;
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecRememberedPickReused,
	"WoT.Update.Worker.RememberedInstaller.PickBuildIsRememberedAndReused",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecRememberedPickReused::RunTest(const FString&)
{
	TArray<uint8> InstallerBytes;
	if (!TestTrue(TEXT("remembered-pick Inno fixture loads"),
		WoTGameBootstrapWorkerSpecLoadInnoInstaller(InstallerBytes)))
	{
		return false;
	}
	const FWoTRetailContract Contract =
		WoTGameBootstrapWorkerSpecContentContract();
	FWoTGameBootstrapWorkerSpecContractScope ContractScope(Contract);
	const FWoTInstallerAcquisitionPins Pins =
		WoTGameBootstrapWorkerSpecInnoPins(InstallerBytes);
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	const FString Elsewhere = Fixture.InstallRoot + TEXT("-elsewhere");
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Elsewhere, false, true);
	};
	const FString PickedPath = FPaths::Combine(
		Elsewhere, WoTGameBootstrapWorkerSpecRememberedLeaf);
	if (!TestTrue(TEXT("the installer is kept outside the game and Downloads"),
		IFileManager::Get().MakeDirectory(*Elsewhere, true)
			&& WoTGameBootstrapWorkerSpecSave(PickedPath, InstallerBytes)))
	{
		return false;
	}
	const FString RecordFile =
		WoTGameBootstrapWorkerSpecRememberedRecordFile(Fixture.UserStateRoot);

	// First install: nothing beside the game or in Downloads, so the player
	// browses to the installer and it builds the cache.
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, Pins,
				WoTGameBootstrapWorkerSpecContentBuild);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked)
			|| !TestEqual(TEXT("with no installer found the player is asked to Browse"),
				Checked.NextPhase, EWoTBootstrapPhase::OriginalGameNeeded))
		{
			return false;
		}
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			WoTGameBootstrapWorkerSpecStartPhase(*Worker,
				EWoTBootstrapPhase::OriginalGameNeeded, 3);
		Worker->RequestPickedInstaller(PickedPath);
		if (!TestTrue(TEXT("the Browse pick is acquired"),
			Pending->Done->Wait(FTimespan::FromSeconds(WoTGameBootstrapWorkerSpecHangSeconds))))
		{
			Worker->CancelAndJoin();
			return false;
		}
		TestEqual(TEXT("the Browse pick routes to extraction"),
			Pending->Completion.NextPhase,
			EWoTBootstrapPhase::ExtractingOriginalGame);
		TestFalse(TEXT("acquiring a pick records nothing before the cache builds"),
			IFileManager::Get().FileExists(*RecordFile));
		FWoTBootstrapWorkerCompletion Extracted, Built, Verified;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::ExtractingOriginalGame, 4, Extracted)
			|| !TestEqual(TEXT("the picked installer extracts"),
				Extracted.NextPhase, EWoTBootstrapPhase::BuildingContent)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::BuildingContent, 5, Built))
		{
			return false;
		}
		TestTrue(TEXT("the picked installer builds the cache"),
			Built.bSucceeded
				&& Built.NextPhase == EWoTBootstrapPhase::VerifyingContent);
		const TOptional<FString> Remembered =
			WoTGameBootstrapWorkerSpecRememberedPath(Fixture.UserStateRoot);
		TestTrue(TEXT("a build from a Browse pick records the pick's path"),
			Remembered.IsSet()
				&& FPaths::IsSamePath(Remembered.GetValue(), PickedPath));
		TestTrue(TEXT("the saved record is reported"),
			WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
				TEXT("installer-remembered"),
				TEXT("\"detail\":\"result=saved\",")));
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::VerifyingContent, 6, Verified))
		{
			return false;
		}
		TestTrue(TEXT("the cache built from the pick verifies"),
			Verified.bSucceeded);
		TestEqual(TEXT("the pick flow is zero-network"), Hops->Requests.Num(), 0);
	}

	// A later rebuild of this install (here the cache is marked damaged; an
	// update that changes the content revision takes the same path) uses the
	// remembered installer before Downloads and without asking again.
	const TOptional<FString> RecordedBefore =
		WoTGameBootstrapWorkerSpecRememberedPath(Fixture.UserStateRoot);
	const TArray<uint8> EmptyMarker;
	if (!TestTrue(TEXT("the published cache is marked damaged"),
			FFileHelper::SaveArrayToFile(EmptyMarker, *FPaths::Combine(
				WoTGameBootstrapWorkerSpecCacheRoot(Fixture.InstallRoot),
				TEXT("current"), TEXT("damaged"))))
		|| !TestTrue(TEXT("a valid installer also waits in Downloads"),
			WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(
				Fixture.InstallRoot, InstallerBytes)))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	{
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TAtomic<int32> Hashes{0};
		FWoTInstallerAcquisitionTestHooks Hooks;
		Hooks.FullHash = [&Hashes](uint64) { ++Hashes; };
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, Pins,
				WoTGameBootstrapWorkerSpecContentBuild, {}, {}, Hooks);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Checked, Extracted, Built;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked))
		{
			return false;
		}
		TestEqual(TEXT("the remembered installer goes straight to extraction, no Browse"),
			Checked.NextPhase, EWoTBootstrapPhase::ExtractingOriginalGame);
		TestTrue(TEXT("the remembered installer is the detected source"),
			WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
				TEXT("original-game-detected"),
				WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("remembered"))));
		TestEqual(TEXT("Downloads is never hashed once the remembered installer wins"),
			Hashes.Load(), 1);
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::ExtractingOriginalGame, 3, Extracted)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::BuildingContent, 4, Built))
		{
			return false;
		}
		TestTrue(TEXT("the remembered installer rebuilds the cache"),
			Built.bSucceeded
				&& Built.NextPhase == EWoTBootstrapPhase::VerifyingContent);
		TestFalse(TEXT("a remembered build is not recorded again"),
			WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
				TEXT("installer-remembered")));
		const TOptional<FString> RecordedAfter =
			WoTGameBootstrapWorkerSpecRememberedPath(Fixture.UserStateRoot);
		TestTrue(TEXT("the record is unchanged"),
			RecordedBefore.IsSet() && RecordedAfter.IsSet()
				&& RecordedBefore.GetValue().Equals(
					RecordedAfter.GetValue(), ESearchCase::CaseSensitive));
		TestEqual(TEXT("the remembered rebuild is zero-network"),
			Hops->Requests.Num(), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecRememberedAutomaticSources,
	"WoT.Update.Worker.RememberedInstaller.AutomaticSourcesAreNotRecorded",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecRememberedAutomaticSources::RunTest(
	const FString&)
{
	TArray<uint8> InstallerBytes;
	if (!TestTrue(TEXT("automatic-source Inno fixture loads"),
		WoTGameBootstrapWorkerSpecLoadInnoInstaller(InstallerBytes)))
	{
		return false;
	}
	const FWoTRetailContract Contract =
		WoTGameBootstrapWorkerSpecContentContract();
	FWoTGameBootstrapWorkerSpecContractScope ContractScope(Contract);
	const FWoTInstallerAcquisitionPins Pins =
		WoTGameBootstrapWorkerSpecInnoPins(InstallerBytes);
	for (const bool bAdjacent : {true, false})
	{
		const FString Case = bAdjacent ? TEXT("adjacent") : TEXT("downloads");
		FWoTGameBootstrapWorkerSpecFixture Fixture;
		if (!Fixture.Initialize(*this))
		{
			return false;
		}
		const bool bWritten = bAdjacent
			? Fixture.WriteInstaller(InstallerBytes)
			: WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(
				Fixture.InstallRoot, InstallerBytes);
		if (!TestTrue(Case + TEXT(": the installer fixture is written"), bWritten))
		{
			return false;
		}
		SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
		ON_SCOPE_EXIT
		{
			SetWoTBootstrapHeartbeatRootForTesting(FString());
		};
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops, {},
				WoTGameBootstrapWorkerSpecAnnouncementUrl, Pins,
				WoTGameBootstrapWorkerSpecContentBuild);
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Checked, Extracted, Built;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked)
			|| !TestEqual(Case + TEXT(": the installer is found without Browse"),
				Checked.NextPhase, EWoTBootstrapPhase::ExtractingOriginalGame)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::ExtractingOriginalGame, 3, Extracted)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::BuildingContent, 4, Built))
		{
			return false;
		}
		TestTrue(Case + TEXT(": the cache builds"), Built.bSucceeded
			&& Built.NextPhase == EWoTBootstrapPhase::VerifyingContent);
		TestTrue(Case + TEXT(": the detected source is reported"),
			WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
				TEXT("original-game-detected"),
				WoTGameBootstrapWorkerSpecDetectedFrom(*Case)));
		TestFalse(Case + TEXT(": no record is written"),
			IFileManager::Get().FileExists(*WoTGameBootstrapWorkerSpecRememberedRecordFile(
				Fixture.UserStateRoot)));
		TestFalse(Case + TEXT(": nothing is reported as remembered"),
			WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
				TEXT("installer-remembered")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecRememberedFallsThrough,
	"WoT.Update.Worker.RememberedInstaller.MissingOrTamperedFallsThrough",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecRememberedFallsThrough::RunTest(const FString&)
{
	enum class ERememberedFault : uint8
	{
		Missing,
		Tampered,
		FolderGone,
	};
	for (const ERememberedFault Fault : {ERememberedFault::Missing,
		ERememberedFault::Tampered, ERememberedFault::FolderGone})
	{
		for (const bool bDownloads : {true, false})
		{
			const FString Case = FString::Printf(TEXT("%s, %s"),
				Fault == ERememberedFault::Missing ? TEXT("missing file")
					: Fault == ERememberedFault::Tampered ? TEXT("tampered file")
					: TEXT("folder gone"),
				bDownloads ? TEXT("Downloads has the installer")
					: TEXT("nothing else"));
			FWoTGameBootstrapWorkerSpecFixture Fixture;
			if (!Fixture.Initialize(*this))
			{
				return false;
			}
			const FString Elsewhere = Fixture.InstallRoot + TEXT("-elsewhere");
			SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
			ON_SCOPE_EXIT
			{
				SetWoTBootstrapHeartbeatRootForTesting(FString());
				IFileManager::Get().DeleteDirectory(*Elsewhere, false, true);
			};
			const FString RememberedFile = FPaths::Combine(
				Elsewhere, WoTGameBootstrapWorkerSpecRememberedLeaf);
			FString RecordedPath;
			if (!TestTrue(Case + TEXT(": the remembered folder exists"),
					IFileManager::Get().MakeDirectory(*Elsewhere, true))
				|| !WoTGameBootstrapWorkerSpecSeedRemembered(*this,
					Fixture.UserStateRoot, RememberedFile, RecordedPath))
			{
				return false;
			}
			if (Fault == ERememberedFault::Tampered
				&& !TestTrue(Case + TEXT(": the tampered file is written"),
					WoTGameBootstrapWorkerSpecSave(RememberedFile,
						WoTGameBootstrapWorkerSpecBadInstallerBytes())))
			{
				return false;
			}
			if (Fault == ERememberedFault::FolderGone
				&& !TestTrue(Case + TEXT(": the remembered folder is removed"),
					IFileManager::Get().DeleteDirectory(*Elsewhere, false, true)))
			{
				return false;
			}
			if (bDownloads && !TestTrue(Case + TEXT(": Downloads is seeded"),
				WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(
					Fixture.InstallRoot,
					WoTGameBootstrapWorkerSpecInstallerBytes())))
			{
				return false;
			}
			FWoTGameBootstrapWorkerSpecRememberedCheck Checked;
			if (!WoTGameBootstrapWorkerSpecRunRememberedCheck(
					*this, Fixture.InstallRoot, Checked))
			{
				return false;
			}
			if (bDownloads)
			{
				TestEqual(Case + TEXT(": Downloads supplies the installer"),
					Checked.Completion.NextPhase,
					EWoTBootstrapPhase::ExtractingOriginalGame);
				TestTrue(Case + TEXT(": Downloads is the detected source"),
					WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
						TEXT("original-game-detected"),
						WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("downloads"))));
			}
			else
			{
				TestEqual(Case + TEXT(": the player is asked to Browse"),
					Checked.Completion.NextPhase,
					EWoTBootstrapPhase::OriginalGameNeeded);
				TestEqual(Case + TEXT(": the remembered file shows no error of its own"),
					Checked.Completion.OriginalGameNeededReason,
					EWoTOriginalGameNeededReason::Missing);
				TestTrue(Case + TEXT(": the content check itself succeeds"),
					Checked.Completion.bSucceeded);
			}
			TestFalse(Case + TEXT(": the remembered file is never used"),
				WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
					TEXT("original-game-detected"),
					WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("remembered"))));
			TestEqual(Case + TEXT(": only a tampered file and a Downloads copy are hashed"),
				Checked.Hashes,
				(Fault == ERememberedFault::Tampered ? 1 : 0) + (bDownloads ? 1 : 0));
			TestEqual(Case + TEXT(": zero network"), Checked.Requests, 0);
			if (Fault == ERememberedFault::FolderGone)
			{
				TestTrue(Case + TEXT(": the unreachable folder is a diagnostic only"),
					WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
						TEXT("installer-location-unavailable"),
						FString::Printf(TEXT("source=remembered leaf=%s "),
							WoTGameBootstrapWorkerSpecRememberedLeaf)));
			}
			// A stale record is kept: the drive may come back, and the next
			// successful Browse pick replaces it.
			const TOptional<FString> Kept =
				WoTGameBootstrapWorkerSpecRememberedPath(Fixture.UserStateRoot);
			TestTrue(Case + TEXT(": the record is kept"), Kept.IsSet()
				&& Kept.GetValue().Equals(RecordedPath, ESearchCase::CaseSensitive));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecRememberedMalformed,
	"WoT.Update.Worker.RememberedInstaller.MalformedRecordIsIgnored",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecRememberedMalformed::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	const FString Elsewhere = Fixture.InstallRoot + TEXT("-elsewhere");
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
		IFileManager::Get().DeleteDirectory(*Elsewhere, false, true);
	};
	// A valid installer sits where each damaged record points, so only the
	// record's own defects can keep it from being used.
	const FString RememberedFile = FPaths::Combine(
		Elsewhere, WoTGameBootstrapWorkerSpecRememberedLeaf);
	FString CanonicalPath;
	if (!TestTrue(TEXT("the remembered installer is written"),
			IFileManager::Get().MakeDirectory(*Elsewhere, true)
				&& WoTGameBootstrapWorkerSpecSave(RememberedFile,
					WoTGameBootstrapWorkerSpecInstallerBytes()))
		|| !WoTGameBootstrapWorkerSpecSeedRemembered(*this,
			Fixture.UserStateRoot, RememberedFile, CanonicalPath)
		|| !TestTrue(TEXT("Downloads is seeded"),
			WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(Fixture.InstallRoot,
				WoTGameBootstrapWorkerSpecInstallerBytes())))
	{
		return false;
	}
	const FString RecordFile =
		WoTGameBootstrapWorkerSpecRememberedRecordFile(Fixture.UserStateRoot);
	const FString Format = TEXT("wot-remembered-installer-v1");
	const FString Parent = FPaths::GetPath(CanonicalPath);
	const FString Leaf = FPaths::GetCleanFilename(CanonicalPath);
	// The same file through a differently spelled parent: syntactically fine,
	// but not what canonicalization produces, so it is refused before use.
	const FString Respelled = FPaths::Combine(FPaths::GetPath(Parent),
		FPaths::GetCleanFilename(Parent).ToUpper(), Leaf);
	struct FMalformedCase
	{
		FString Name;
		FString Text;
		FString Diagnostic;
	};
	const FString Malformed = TEXT("\"detail\":\"reason=malformed\",");
	const TArray<FMalformedCase> Cases = {
		{TEXT("garbage"), TEXT("not a record"), Malformed},
		{TEXT("trailing newline"), FString::Printf(
			TEXT("{\"format\":\"%s\",\"path\":\"%s\"}\n"), *Format, *CanonicalPath),
			Malformed},
		{TEXT("whitespace"), FString::Printf(
			TEXT("{ \"format\": \"%s\", \"path\": \"%s\" }"), *Format, *CanonicalPath),
			Malformed},
		{TEXT("unknown field"), FString::Printf(
			TEXT("{\"extra\":1,\"format\":\"%s\",\"path\":\"%s\"}"), *Format,
			*CanonicalPath), Malformed},
		{TEXT("future format"), FString::Printf(
			TEXT("{\"format\":\"wot-remembered-installer-v2\",\"path\":\"%s\"}"),
			*CanonicalPath), Malformed},
		{TEXT("dot segment"), FString::Printf(
			TEXT("{\"format\":\"%s\",\"path\":\"%s/./%s\"}"), *Format, *Parent,
			*Leaf), Malformed},
		{TEXT("relative path"), FString::Printf(
			TEXT("{\"format\":\"%s\",\"path\":\"%s\"}"), *Format, *Leaf),
			Malformed},
		{TEXT("oversized"), FString::ChrN(40000, TEXT(' ')),
			TEXT("\"detail\":\"reason=too-large\",")},
		{TEXT("respelled parent"), FString::Printf(
			TEXT("{\"format\":\"%s\",\"path\":\"%s\"}"), *Format, *Respelled),
			FString()},
	};
	for (const FMalformedCase& Case : Cases)
	{
		if (!TestTrue(Case.Name + TEXT(": the damaged record is written"),
			FFileHelper::SaveStringToFile(Case.Text, *RecordFile,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)))
		{
			return false;
		}
		SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
		FWoTGameBootstrapWorkerSpecRememberedCheck Checked;
		if (!WoTGameBootstrapWorkerSpecRunRememberedCheck(
				*this, Fixture.InstallRoot, Checked))
		{
			return false;
		}
		TestEqual(Case.Name + TEXT(": Downloads still supplies the installer"),
			Checked.Completion.NextPhase, EWoTBootstrapPhase::ExtractingOriginalGame);
		TestTrue(Case.Name + TEXT(": Downloads is the detected source"),
			WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
				TEXT("original-game-detected"),
				WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("downloads"))));
		TestEqual(Case.Name + TEXT(": only the Downloads copy is hashed"),
			Checked.Hashes, 1);
		if (!Case.Diagnostic.IsEmpty())
		{
			TestTrue(Case.Name + TEXT(": the ignored record is a diagnostic only"),
				WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
					TEXT("remembered-installer-ignored"), Case.Diagnostic));
		}
	}

	// A record that is not a regular file is ignored the same way.
	IFileManager::Get().Delete(*RecordFile, false, true, true);
	if (!TestTrue(TEXT("the record name holds a directory"),
		IFileManager::Get().MakeDirectory(*RecordFile, true)))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	FWoTGameBootstrapWorkerSpecRememberedCheck DirectoryChecked;
	if (!WoTGameBootstrapWorkerSpecRunRememberedCheck(
			*this, Fixture.InstallRoot, DirectoryChecked))
	{
		return false;
	}
	TestEqual(TEXT("a directory record falls through to Downloads"),
		DirectoryChecked.Completion.NextPhase,
		EWoTBootstrapPhase::ExtractingOriginalGame);
	TestTrue(TEXT("a directory record is a diagnostic only"),
		WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
			TEXT("remembered-installer-ignored"),
			TEXT("\"detail\":\"reason=not-a-file\",")));

	// Control: the same installer behind a well-formed record is used.
	IFileManager::Get().DeleteDirectory(*RecordFile, false, true);
	FString ControlPath;
	if (!WoTGameBootstrapWorkerSpecSeedRemembered(*this,
			Fixture.UserStateRoot, RememberedFile, ControlPath))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	FWoTGameBootstrapWorkerSpecRememberedCheck ControlChecked;
	if (!WoTGameBootstrapWorkerSpecRunRememberedCheck(
			*this, Fixture.InstallRoot, ControlChecked))
	{
		return false;
	}
	TestTrue(TEXT("a well-formed record to the same file is used"),
		WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
			TEXT("original-game-detected"),
			WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("remembered"))));
	TestEqual(TEXT("the remembered file is the only one hashed"),
		ControlChecked.Hashes, 1);
	return !HasAnyErrors();
}

// ---------------------------------------------------------------------------
// LAN installer receiving (spec B5, B6): the received location in the sweep,
// and the receiver's life inside the original-game wait. Every transfer here
// is loopback only.

namespace
{
/** Every progress event of one phase, collected from the worker's thread. */
struct FWoTGameBootstrapWorkerSpecEventLog
{
	void Add(const FWoTBootstrapProgressEvent& Event)
	{
		FScopeLock Lock(&Guard);
		Events.Add(Event);
	}

	TArray<FWoTBootstrapProgressEvent> Snapshot() const
	{
		FScopeLock Lock(&Guard);
		return Events;
	}

	int32 Num() const
	{
		FScopeLock Lock(&Guard);
		return Events.Num();
	}

	/** The first event matching Predicate, if any. */
	TOptional<FWoTBootstrapProgressEvent> Find(
		TFunctionRef<bool(const FWoTBootstrapProgressEvent&)> Predicate) const
	{
		FScopeLock Lock(&Guard);
		for (const FWoTBootstrapProgressEvent& Event : Events)
		{
			if (Predicate(Event))
			{
				return Event;
			}
		}
		return {};
	}

	mutable FCriticalSection Guard;
	TArray<FWoTBootstrapProgressEvent> Events;
};

TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase>
WoTGameBootstrapWorkerSpecStartLoggedPhase(IWoTBootstrapWorker& Worker,
	EWoTBootstrapPhase Phase, uint64 Epoch,
	TSharedRef<FWoTGameBootstrapWorkerSpecEventLog> Log)
{
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		MakeShared<FWoTGameBootstrapWorkerSpecPendingPhase>();
	Worker.StartPhase(WoTGameBootstrapWorkerSpecRequest(Phase, Epoch),
		[Log](FWoTBootstrapProgressEvent&& Event)
		{
			Log->Add(Event);
		},
		[Pending](FWoTBootstrapWorkerCompletion&& Completion)
		{
			Pending->Completion = MoveTemp(Completion);
			Pending->bCompleted.Store(true);
			Pending->Done->Trigger();
		});
	return Pending;
}

bool WoTGameBootstrapWorkerSpecWaitUntil(TFunctionRef<bool()> Done,
	double Seconds = WoTGameBootstrapWorkerSpecHangSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + Seconds;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (Done())
		{
			return true;
		}
		FPlatformProcess::Sleep(0.005f);
	}
	return Done();
}

FWoTInstallerReceiverOverridesForTesting WoTGameBootstrapWorkerSpecReceiver(
	TSharedRef<WoTReceiverSpec::FLog> Log, bool bStartsAutomatically)
{
	FWoTInstallerReceiverOverridesForTesting Overrides;
	Overrides.Network = MakeShared<WoTReceiverSpec::FRecordingNetwork>(Log);
	Overrides.bStartsAutomatically = bStartsAutomatically;
	Overrides.FreeBytes = []() { return TOptional<uint64>(MAX_uint64); };
	Overrides.PublishIntervalSeconds = 0.01;
	return Overrides;
}

FString WoTGameBootstrapWorkerSpecReceivedFolder(const FString& UserStateRoot)
{
	return FPaths::Combine(UserStateRoot, TEXT("OriginalGame"));
}

FString WoTGameBootstrapWorkerSpecReceivedPath(const FString& UserStateRoot)
{
	return FPaths::Combine(WoTGameBootstrapWorkerSpecReceivedFolder(UserStateRoot),
		WoTGameBootstrapWorkerSpecInstallerPins().Name);
}

int32 WoTGameBootstrapWorkerSpecPartFiles(const FString& UserStateRoot)
{
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *FPaths::Combine(
		WoTGameBootstrapWorkerSpecReceivedFolder(UserStateRoot), TEXT("*.part")),
		true, false);
	return Found.Num();
}

bool WoTGameBootstrapWorkerSpecIsCopyEvent(const FWoTBootstrapProgressEvent& Event)
{
	return Event.StageId.StartsWith(WoTInstallerCopyStagePrefix)
		&& Event.CompletedBytes.IsSet();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReceivedAccepted,
	"WoT.Update.Worker.ReceivedInstaller.AcceptedSilently",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReceivedAccepted::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	// A copy an earlier run received, a leftover partial copy beside it, and
	// the installer in Downloads too: the received copy comes first.
	if (!WoTGameBootstrapWorkerSpecSave(
			WoTGameBootstrapWorkerSpecReceivedPath(Fixture.UserStateRoot),
			WoTGameBootstrapWorkerSpecInstallerBytes())
		|| !WoTGameBootstrapWorkerSpecSave(FPaths::Combine(
			WoTGameBootstrapWorkerSpecReceivedFolder(Fixture.UserStateRoot),
			TEXT("leftover.part")), WoTGameBootstrapWorkerSpecAscii(TEXT("abc")))
		|| !WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(Fixture.InstallRoot,
			WoTGameBootstrapWorkerSpecInstallerBytes()))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Checked;
	if (!TestTrue(TEXT("the content check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked)))
	{
		return false;
	}
	TestEqual(TEXT("a received copy routes straight to extraction"),
		Checked.NextPhase, EWoTBootstrapPhase::ExtractingOriginalGame);
	TestTrue(TEXT("it came from the received folder, before Downloads"),
		WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
			TEXT("original-game-detected"),
			WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("received"))));
	TestEqual(TEXT("a leftover partial copy is deleted once one is accepted"),
		WoTGameBootstrapWorkerSpecPartFiles(Fixture.UserStateRoot), 0);
	TestTrue(TEXT("the received copy itself stays"), IFileManager::Get().FileExists(
		*WoTGameBootstrapWorkerSpecReceivedPath(Fixture.UserStateRoot)));
	TestEqual(TEXT("zero network"), Hops->Requests.Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReceivedUnavailable,
	"WoT.Update.Worker.ReceivedInstaller.UnavailableFallsThroughToDownloads",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReceivedUnavailable::RunTest(const FString&)
{
	// The location fault hook and the unavailable-slot heartbeat both carry
	// the received source; an unreadable received copy falls through to
	// Downloads like any other location.
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	if (!WoTGameBootstrapWorkerSpecSave(
			WoTGameBootstrapWorkerSpecReceivedPath(Fixture.UserStateRoot),
			WoTGameBootstrapWorkerSpecInstallerBytes())
		|| !WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(Fixture.InstallRoot,
			WoTGameBootstrapWorkerSpecInstallerBytes()))
	{
		return false;
	}
	TAtomic<int32> ReceivedProbes{0};
	auto ReceivedFault = [&ReceivedProbes](EWoTInstallerSource Source,
		const FString&) -> TOptional<FWoTBootstrapError>
	{
		if (Source != EWoTInstallerSource::Received)
		{
			return {};
		}
		++ReceivedProbes;
		return MakeWoTBootstrapError(EWoTBootstrapError::Io,
			TEXT("received denied"));
	};
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(
			Fixture.InstallRoot, Hops, {},
			WoTGameBootstrapWorkerSpecAnnouncementUrl, {}, {}, {}, {}, {},
			{}, {}, {}, {}, {}, {}, {}, {}, {}, {}, ReceivedFault);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Checked;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked))
	{
		return false;
	}
	TestTrue(TEXT("the fault hook saw the received source"), ReceivedProbes.Load() > 0);
	TestEqual(TEXT("still routes to extraction"), Checked.NextPhase,
		EWoTBootstrapPhase::ExtractingOriginalGame);
	TestEqual(TEXT("an unreadable received copy changes no reason"),
		Checked.OriginalGameNeededReason, EWoTOriginalGameNeededReason::Missing);
	TestTrue(TEXT("its slot is recorded unavailable"),
		WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
			TEXT("installer-location-unavailable"),
			FString::Printf(TEXT("source=received leaf=%s detail=Io"),
				*WoTGameBootstrapWorkerSpecInstallerPins().Name)));
	TestTrue(TEXT("Downloads supplies the installer"),
		WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
			TEXT("original-game-detected"),
			WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("downloads"))));
	TestTrue(TEXT("the received copy is untouched"), IFileManager::Get().FileExists(
		*WoTGameBootstrapWorkerSpecReceivedPath(Fixture.UserStateRoot)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReceivedBroken,
	"WoT.Update.Worker.ReceivedInstaller.BrokenIsDeletedSilently",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReceivedBroken::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	const FString ReceivedPath =
		WoTGameBootstrapWorkerSpecReceivedPath(Fixture.UserStateRoot);
	if (!WoTGameBootstrapWorkerSpecSave(ReceivedPath,
			WoTGameBootstrapWorkerSpecBadInstallerBytes()))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Checked;
	if (!TestTrue(TEXT("the content check completes"),
		WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked)))
	{
		return false;
	}
	TestEqual(TEXT("a broken received copy leaves the original game needed"),
		Checked.NextPhase, EWoTBootstrapPhase::OriginalGameNeeded);
	TestEqual(TEXT("and never changes the reason"),
		Checked.OriginalGameNeededReason, EWoTOriginalGameNeededReason::Missing);
	TestFalse(TEXT("it is deleted"), IFileManager::Get().FileExists(*ReceivedPath));
	TestTrue(TEXT("received-installer-deleted reason=hash"),
		WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
			TEXT("received-installer-deleted"), TEXT("reason=hash result=deleted")));

	// While waiting: a short file there is broken too (the receiver only ever
	// renames a complete, verified copy in), then a good one is accepted.
	TSharedRef<FWoTGameBootstrapWorkerSpecEventLog> Log =
		MakeShared<FWoTGameBootstrapWorkerSpecEventLog>();
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		WoTGameBootstrapWorkerSpecStartLoggedPhase(*Worker,
			EWoTBootstrapPhase::OriginalGameNeeded, 3, Log);
	// Files appear there the way the receiver places them: complete, by a
	// rename, so a sweep never sees one half written.
	const FString Incoming = FPaths::Combine(
		WoTGameBootstrapWorkerSpecReceivedFolder(Fixture.UserStateRoot),
		TEXT("incoming.tmp"));
	auto Place = [&Incoming, &ReceivedPath](TConstArrayView<uint8> Bytes)
	{
		return WoTGameBootstrapWorkerSpecSave(Incoming, Bytes)
			&& IFileManager::Get().Move(*ReceivedPath, *Incoming, false);
	};
	TArray<uint8> Short = WoTGameBootstrapWorkerSpecInstallerBytes();
	Short.SetNum(4);
	TestTrue(TEXT("a short file is placed"), Place(Short));
	Worker->RequestOriginalGameRecheck();
	TestTrue(TEXT("the short file is deleted"), WoTGameBootstrapWorkerSpecWaitUntil(
		[&ReceivedPath]() { return !IFileManager::Get().FileExists(*ReceivedPath); }));
	TestTrue(TEXT("received-installer-deleted reason=short"),
		WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
			TEXT("received-installer-deleted"), TEXT("reason=short")));
	TestTrue(TEXT("a good copy is placed"),
		Place(WoTGameBootstrapWorkerSpecInstallerBytes()));
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("the good copy is accepted"),
		Pending->Done->Wait(FTimespan::FromSeconds(
			WoTGameBootstrapWorkerSpecHangSeconds))))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestEqual(TEXT("to extraction"), Pending->Completion.NextPhase,
		EWoTBootstrapPhase::ExtractingOriginalGame);
	TestFalse(TEXT("the waiting reason never moved"), Log->Find(
		[](const FWoTBootstrapProgressEvent& Event)
		{
			return Event.OriginalGameNeededReason.IsSet()
				&& Event.OriginalGameNeededReason.GetValue()
					!= EWoTOriginalGameNeededReason::Missing;
		}).IsSet());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReceivedFolderChanged,
	"WoT.Update.Worker.ReceivedInstaller.ChangedFolderIsDroppedNotFatal",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReceivedFolderChanged::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	const FString Folder =
		WoTGameBootstrapWorkerSpecReceivedFolder(Fixture.UserStateRoot);
	if (!IFileManager::Get().MakeDirectory(*Folder, true))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Checked;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked)
		|| !TestEqual(TEXT("waiting"), Checked.NextPhase,
			EWoTBootstrapPhase::OriginalGameNeeded))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecEventLog> Log =
		MakeShared<FWoTGameBootstrapWorkerSpecEventLog>();
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		WoTGameBootstrapWorkerSpecStartLoggedPhase(*Worker,
			EWoTBootstrapPhase::OriginalGameNeeded, 3, Log);
	// Another folder takes its place, holding a valid installer: it is a
	// different object, so it is never searched, and nothing fails.
	TestTrue(TEXT("folder removed"), IFileManager::Get().DeleteDirectory(*Folder, false, true));
	TestTrue(TEXT("a replacement with an installer"), WoTGameBootstrapWorkerSpecSave(
		WoTGameBootstrapWorkerSpecReceivedPath(Fixture.UserStateRoot),
		WoTGameBootstrapWorkerSpecInstallerBytes()));
	Worker->RequestOriginalGameRecheck();
	TestTrue(TEXT("the change is recorded as unavailable"),
		WoTGameBootstrapWorkerSpecWaitUntil([&Fixture]()
		{
			return WoTGameBootstrapWorkerSpecHeartbeatHas(Fixture.UserStateRoot,
				TEXT("installer-location-unavailable"), TEXT("source=received"));
		}));
	const int32 Seen = Log->Num();
	TestTrue(TEXT("the wait keeps polling"), WoTGameBootstrapWorkerSpecWaitUntil(
		[&Log, Seen]() { return Log->Num() >= Seen + 5; }));
	TestFalse(TEXT("not fatal, and the replacement is not used"),
		Pending->bCompleted.Load());
	TestTrue(TEXT("Downloads still works"), WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(
		Fixture.InstallRoot, WoTGameBootstrapWorkerSpecInstallerBytes()));
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("the Downloads installer is accepted"),
		Pending->Done->Wait(FTimespan::FromSeconds(
			WoTGameBootstrapWorkerSpecHangSeconds))))
	{
		Worker->CancelAndJoin();
		return false;
	}
	TestTrue(TEXT("succeeded"), Pending->Completion.bSucceeded);
	TestEqual(TEXT("to extraction"), Pending->Completion.NextPhase,
		EWoTBootstrapPhase::ExtractingOriginalGame);
	TestTrue(TEXT("from Downloads"), WoTGameBootstrapWorkerSpecHeartbeatHas(
		Fixture.UserStateRoot, TEXT("original-game-detected"),
		WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("downloads"))));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReceiverDormancy,
	"WoT.Update.Worker.InstallerReceiver.Dormancy",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReceiverDormancy::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	const FString Folder =
		WoTGameBootstrapWorkerSpecReceivedFolder(Fixture.UserStateRoot);
	// Windows and Mac: no socket before the original game is needed, and none
	// at that screen until the player's click.
	{
		TSharedRef<WoTReceiverSpec::FLog> Log = MakeShared<WoTReceiverSpec::FLog>();
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
		Worker->SetInstallerReceiverForTesting(
			WoTGameBootstrapWorkerSpecReceiver(Log, /*bStartsAutomatically*/ false));
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Updates;
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingGameUpdates, 2, Updates)
			|| !WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 3, Checked)
			|| !TestEqual(TEXT("waiting"), Checked.NextPhase,
				EWoTBootstrapPhase::OriginalGameNeeded))
		{
			return false;
		}
		TestEqual(TEXT("no socket before the original game is needed"),
			Log->DatagramsCreated.Load() + Log->StreamsCreated.Load(), 0);
		TestFalse(TEXT("no received folder before a receiver"),
			IFileManager::Get().DirectoryExists(*Folder));
		TSharedRef<FWoTGameBootstrapWorkerSpecEventLog> Events =
			MakeShared<FWoTGameBootstrapWorkerSpecEventLog>();
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			WoTGameBootstrapWorkerSpecStartLoggedPhase(*Worker,
				EWoTBootstrapPhase::OriginalGameNeeded, 4, Events);
		TestTrue(TEXT("the wait polls"), WoTGameBootstrapWorkerSpecWaitUntil(
			[&Events]() { return Events->Num() >= 10; }));
		TestEqual(TEXT("no socket at the screen until the button"),
			Log->DatagramsCreated.Load() + Log->StreamsCreated.Load(), 0);
		TestFalse(TEXT("still no folder"), IFileManager::Get().DirectoryExists(*Folder));
		const TArray<FWoTBootstrapProgressEvent> Before = Events->Snapshot();
		TestTrue(TEXT("the screen is told: off, with a button"),
			Before.Num() > 0 && Before.Last().InstallerCopy.IsSet()
			&& Before.Last().InstallerCopy->Status == EWoTInstallerCopyStatus::Off
			&& !Before.Last().InstallerCopy->bAutomatic);
		Worker->RequestInstallerReceive();
		TestTrue(TEXT("the button starts the receiver"), WoTGameBootstrapWorkerSpecWaitUntil(
			[&Log]() { return Log->DiscoveryPort.Load() != 0; }));
		TestEqual(TEXT("one discovery socket"), Log->DatagramsCreated.Load(), 1);
		TestTrue(TEXT("the folder exists before it listens"),
			IFileManager::Get().DirectoryExists(*Folder));
		TestTrue(TEXT("the screen is told it listens"), WoTGameBootstrapWorkerSpecWaitUntil(
			[&Events]()
			{
				return Events->Find([](const FWoTBootstrapProgressEvent& Event)
				{
					return Event.InstallerCopy.IsSet()
						&& Event.InstallerCopy->Status == EWoTInstallerCopyStatus::Waiting;
				}).IsSet();
			}));
		Worker->RequestInstallerReceive();
		FPlatformProcess::Sleep(0.05f);
		TestEqual(TEXT("a second click starts nothing more"),
			Log->DatagramsCreated.Load(), 1);
		Worker->CancelAndJoin();
		TestEqual(TEXT("CancelAndJoin closes every socket"), Log->Open.Load(), 0);
		TestFalse(TEXT("the cancelled wait never completes"),
			Pending->bCompleted.Load());
	}
	// Wine on Linux (Proton): it listens by itself at that screen, still
	// never before it.
	{
		TSharedRef<WoTReceiverSpec::FLog> Log = MakeShared<WoTReceiverSpec::FLog>();
		TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
			MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
		TUniquePtr<FWoTGameBootstrapWorker> Worker =
			WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
		Worker->SetInstallerReceiverForTesting(
			WoTGameBootstrapWorkerSpecReceiver(Log, /*bStartsAutomatically*/ true));
		if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker, 10))
		{
			return false;
		}
		FWoTBootstrapWorkerCompletion Checked;
		if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
				EWoTBootstrapPhase::CheckingOriginalGameData, 11, Checked))
		{
			return false;
		}
		TestEqual(TEXT("automatic: still no socket before the screen"),
			Log->DatagramsCreated.Load() + Log->StreamsCreated.Load(), 0);
		TSharedRef<FWoTGameBootstrapWorkerSpecEventLog> Events =
			MakeShared<FWoTGameBootstrapWorkerSpecEventLog>();
		TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
			WoTGameBootstrapWorkerSpecStartLoggedPhase(*Worker,
				EWoTBootstrapPhase::OriginalGameNeeded, 12, Events);
		TestTrue(TEXT("it listens without a click"), WoTGameBootstrapWorkerSpecWaitUntil(
			[&Log]() { return Log->DiscoveryPort.Load() != 0; }));
		TestTrue(TEXT("the screen is told: automatic"), WoTGameBootstrapWorkerSpecWaitUntil(
			[&Events]()
			{
				return Events->Find([](const FWoTBootstrapProgressEvent& Event)
				{
					return Event.InstallerCopy.IsSet() && Event.InstallerCopy->bAutomatic
						&& Event.InstallerCopy->Status == EWoTInstallerCopyStatus::Waiting;
				}).IsSet();
			}));
		Worker->RequestInstallerReceive();
		FPlatformProcess::Sleep(0.05f);
		TestEqual(TEXT("one receiver"), Log->DatagramsCreated.Load(), 1);
		Worker->CancelAndJoin();
		TestEqual(TEXT("closed on CancelAndJoin"), Log->Open.Load(), 0);
		TestFalse(TEXT("cancelled"), Pending->bCompleted.Load());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReceiverProgress,
	"WoT.Update.Worker.InstallerReceiver.ProgressAcrossADamagedCopy",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReceiverProgress::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	// The screen's own model, at the same epoch as the worker's wait.
	FWoTBootstrapViewModel Model;
	Model.TransitionTo(EWoTBootstrapPhase::CheckingGameUpdates);
	Model.TransitionTo(EWoTBootstrapPhase::CheckingOriginalGameData);
	Model.TransitionTo(EWoTBootstrapPhase::OriginalGameNeeded);
	const uint64 WaitEpoch = Model.Snapshot().OperationEpoch;

	TSharedRef<WoTReceiverSpec::FLog> Log = MakeShared<WoTReceiverSpec::FLog>();
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	Worker->SetInstallerReceiverForTesting(
		WoTGameBootstrapWorkerSpecReceiver(Log, /*bStartsAutomatically*/ true));
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Checked;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecEventLog> Events =
		MakeShared<FWoTGameBootstrapWorkerSpecEventLog>();
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		WoTGameBootstrapWorkerSpecStartLoggedPhase(*Worker,
			EWoTBootstrapPhase::OriginalGameNeeded, WaitEpoch, Events);
	ON_SCOPE_EXIT
	{
		Worker->CancelAndJoin();
	};
	if (!TestTrue(TEXT("listening"), WoTGameBootstrapWorkerSpecWaitUntil(
			[&Log]() { return Log->DiscoveryPort.Load() != 0; })))
	{
		return false;
	}
	const uint16 Port = static_cast<uint16>(Log->DiscoveryPort.Load());
	const FWoTInstallerAcquisitionPins Pins = WoTGameBootstrapWorkerSpecInstallerPins();
	const TArray<uint8> Good = WoTGameBootstrapWorkerSpecInstallerBytes();
	const TArray<uint8> Bad = WoTGameBootstrapWorkerSpecBadInstallerBytes();
	auto Idle = []() { FPlatformProcess::Sleep(0.001f); };
	auto Slice = [](const TArray<uint8>& Bytes, int32 From, int32 To)
	{
		return TArray<uint8>(Bytes.GetData() + From, To - From);
	};

	// Copy 1 arrives damaged, after its progress was on screen.
	FString FirstStage;
	{
		WoTReceiverSpec::FPeer Peer(21);
		TOptional<FWoTShareRequest> Request;
		if (!TestTrue(TEXT("first peer"), Peer.Open()))
		{
			return false;
		}
		Request = Peer.OfferAndAwaitRequest(Port, Pins.Size, Pins.Sha256, Idle);
		if (!TestTrue(TEXT("first REQUEST"), Request.IsSet())
			|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
			|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
				Pins.Size, Idle))
			|| !TestTrue(TEXT("half"), Peer.Stream(Slice(Bad, 0, 4), Idle)))
		{
			return false;
		}
		TestTrue(TEXT("the first copy's half is published"),
			WoTGameBootstrapWorkerSpecWaitUntil([&Events]()
			{
				return Events->Find([](const FWoTBootstrapProgressEvent& Event)
				{
					return WoTGameBootstrapWorkerSpecIsCopyEvent(Event)
						&& Event.CompletedBytes.GetValue() == 4;
				}).IsSet();
			}));
		const TOptional<FWoTBootstrapProgressEvent> Half = Events->Find(
			[](const FWoTBootstrapProgressEvent& Event)
			{
				return WoTGameBootstrapWorkerSpecIsCopyEvent(Event);
			});
		if (Half.IsSet())
		{
			FirstStage = Half->StageId;
			TestEqual(TEXT("its total is the pinned size"), Half->TotalBytes.Get(0),
				Pins.Size);
			TestTrue(TEXT("its state says copying, from whom"),
				Half->InstallerCopy.IsSet()
				&& Half->InstallerCopy->Status == EWoTInstallerCopyStatus::Copying
				&& Half->InstallerCopy->PeerLabel == WoTInstallerShare::LabelWindowsPC);
		}
		if (!TestTrue(TEXT("the rest"), Peer.Stream(Slice(Bad, 4, 8), Idle)))
		{
			return false;
		}
		TestTrue(TEXT("the damaged copy is published"),
			WoTGameBootstrapWorkerSpecWaitUntil([&Events]()
			{
				return Events->Find([](const FWoTBootstrapProgressEvent& Event)
				{
					return Event.InstallerCopy.IsSet()
						&& Event.InstallerCopy->Status == EWoTInstallerCopyStatus::Damaged;
				}).IsSet();
			}));
	}

	// Copy 2 starts again from zero under a new stage, and completes.
	{
		WoTReceiverSpec::FPeer Peer(22);
		if (!TestTrue(TEXT("second peer"), Peer.Open()))
		{
			return false;
		}
		const TOptional<FWoTShareRequest> Request =
			Peer.OfferAndAwaitRequest(Port, Pins.Size, Pins.Sha256, Idle);
		if (!TestTrue(TEXT("second REQUEST"), Request.IsSet()))
		{
			return false;
		}
		TestEqual(TEXT("the damaged bytes are not resumed"), Request->ResumeOffset,
			static_cast<uint64>(0));
		if (!TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
			|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0,
				Pins.Size, Idle))
			|| !TestTrue(TEXT("a quarter"), Peer.Stream(Slice(Good, 0, 2), Idle)))
		{
			return false;
		}
		TestTrue(TEXT("the second copy restarts at a quarter"),
			WoTGameBootstrapWorkerSpecWaitUntil([&Events, &FirstStage]()
			{
				return Events->Find([&FirstStage](const FWoTBootstrapProgressEvent& Event)
				{
					return WoTGameBootstrapWorkerSpecIsCopyEvent(Event)
						&& Event.StageId != FirstStage
						&& Event.CompletedBytes.GetValue() == 2;
				}).IsSet();
			}));
		if (!TestTrue(TEXT("the rest"), Peer.Stream(Slice(Good, 2, 8), Idle)))
		{
			return false;
		}
		if (!TestTrue(TEXT("the received copy completes the wait"),
			Pending->Done->Wait(FTimespan::FromSeconds(
				WoTGameBootstrapWorkerSpecHangSeconds))))
		{
			return false;
		}
	}
	TestTrue(TEXT("succeeded"), Pending->Completion.bSucceeded);
	TestEqual(TEXT("to extraction"), Pending->Completion.NextPhase,
		EWoTBootstrapPhase::ExtractingOriginalGame);
	TestTrue(TEXT("from the received folder"), WoTGameBootstrapWorkerSpecHeartbeatHas(
		Fixture.UserStateRoot, TEXT("original-game-detected"),
		WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("received"))));
	TestTrue(TEXT("the damaged copy was recorded"), WoTGameBootstrapWorkerSpecHeartbeatHas(
		Fixture.UserStateRoot, TEXT("lan-receive-failed"), TEXT("reason=damaged")));
	TestTrue(TEXT("the good one too"), WoTGameBootstrapWorkerSpecHeartbeatHas(
		Fixture.UserStateRoot, TEXT("lan-receive-verified")));
	TestEqual(TEXT("no .part is left"),
		WoTGameBootstrapWorkerSpecPartFiles(Fixture.UserStateRoot), 0);
	TestEqual(TEXT("the receiver is gone with the wait"), Log->Open.Load(), 0);
	TestEqual(TEXT("it never wrote to a data connection"),
		Log->ConnectionSends.Load(), 0);

	// One publisher: the screen's model takes every event in order, the
	// second copy's lower byte count included.
	const TArray<FWoTBootstrapProgressEvent> Published = Events->Snapshot();
	int32 Accepted = 0;
	bool bSawFirstHalf = false;
	bool bSawRestart = false;
	for (const FWoTBootstrapProgressEvent& Event : Published)
	{
		FWoTBootstrapProgressEvent Copy = Event;
		if (!Model.UpdateProgress(MoveTemp(Copy)))
		{
			AddError(FString::Printf(TEXT("the screen refused stage '%s' bytes %llu"),
				*Event.StageId, static_cast<unsigned long long>(
					Event.CompletedBytes.Get(0))));
			continue;
		}
		++Accepted;
		const FWoTBootstrapSnapshot Snapshot = Model.Snapshot();
		if (Snapshot.StageId == FirstStage && Snapshot.CompletedBytes.Get(0) == 4)
		{
			bSawFirstHalf = true;
		}
		if (bSawFirstHalf && !FirstStage.IsEmpty() && Snapshot.StageId != FirstStage
			&& Snapshot.StageId.StartsWith(WoTInstallerCopyStagePrefix)
			&& Snapshot.CompletedBytes.Get(0) == 2)
		{
			bSawRestart = true;
			TestEqual(TEXT("the bar restarts with the copy"),
				Snapshot.PhaseFraction.Get(-1.0f), 0.25f);
		}
		TestEqual(TEXT("the reason stays silent"), Snapshot.OriginalGameNeededReason,
			EWoTOriginalGameNeededReason::Missing);
	}
	TestEqual(TEXT("every event is accepted"), Accepted, Published.Num());
	TestTrue(TEXT("the first copy showed half"), bSawFirstHalf);
	TestTrue(TEXT("the second copy restarted from below it"), bSawRestart);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWoTGameBootstrapWorkerSpecReceiverPhaseExit,
	"WoT.Update.Worker.InstallerReceiver.PhaseExitStopsTheListener",
	WoTGameBootstrapWorkerSpecFlags)

bool FWoTGameBootstrapWorkerSpecReceiverPhaseExit::RunTest(const FString&)
{
	FWoTGameBootstrapWorkerSpecFixture Fixture;
	if (!Fixture.Initialize(*this))
	{
		return false;
	}
	SetWoTBootstrapHeartbeatRootForTesting(Fixture.UserStateRoot);
	ON_SCOPE_EXIT
	{
		SetWoTBootstrapHeartbeatRootForTesting(FString());
	};
	TSharedRef<WoTReceiverSpec::FLog> Log = MakeShared<WoTReceiverSpec::FLog>();
	TSharedRef<FWoTGameBootstrapWorkerSpecHopState> Hops =
		MakeShared<FWoTGameBootstrapWorkerSpecHopState>();
	TUniquePtr<FWoTGameBootstrapWorker> Worker =
		WoTGameBootstrapWorkerSpecMakeWorker(Fixture.InstallRoot, Hops);
	Worker->SetInstallerReceiverForTesting(
		WoTGameBootstrapWorkerSpecReceiver(Log, /*bStartsAutomatically*/ false));
	if (!WoTGameBootstrapWorkerSpecEstablish(*this, *Worker))
	{
		return false;
	}
	FWoTBootstrapWorkerCompletion Checked;
	if (!WoTGameBootstrapWorkerSpecRunPhase(*Worker,
			EWoTBootstrapPhase::CheckingOriginalGameData, 2, Checked))
	{
		return false;
	}
	TSharedRef<FWoTGameBootstrapWorkerSpecEventLog> Events =
		MakeShared<FWoTGameBootstrapWorkerSpecEventLog>();
	TSharedRef<FWoTGameBootstrapWorkerSpecPendingPhase> Pending =
		WoTGameBootstrapWorkerSpecStartLoggedPhase(*Worker,
			EWoTBootstrapPhase::OriginalGameNeeded, 3, Events);
	ON_SCOPE_EXIT
	{
		Worker->CancelAndJoin();
	};
	Worker->RequestInstallerReceive();
	if (!TestTrue(TEXT("listening after the button"), WoTGameBootstrapWorkerSpecWaitUntil(
			[&Log]() { return Log->DiscoveryPort.Load() != 0; })))
	{
		return false;
	}
	const uint16 Port = static_cast<uint16>(Log->DiscoveryPort.Load());
	// A copy is mid-way, with a stale partial copy beside it, when the
	// installer shows up in Downloads.
	const FWoTInstallerAcquisitionPins Pins = WoTGameBootstrapWorkerSpecInstallerPins();
	const TArray<uint8> Good = WoTGameBootstrapWorkerSpecInstallerBytes();
	auto Idle = []() { FPlatformProcess::Sleep(0.001f); };
	WoTReceiverSpec::FPeer Peer(31);
	if (!TestTrue(TEXT("peer"), Peer.Open()))
	{
		return false;
	}
	const TOptional<FWoTShareRequest> Request =
		Peer.OfferAndAwaitRequest(Port, Pins.Size, Pins.Sha256, Idle);
	if (!TestTrue(TEXT("REQUEST"), Request.IsSet())
		|| !TestTrue(TEXT("connect"), Peer.Connect(Request->TcpPort))
		|| !TestTrue(TEXT("header"), Peer.SendHeader(Request->Token, 0, Pins.Size, Idle))
		|| !TestTrue(TEXT("half"), Peer.Stream(
			TArray<uint8>(Good.GetData(), 4), Idle)))
	{
		return false;
	}
	TestTrue(TEXT("copying"), WoTGameBootstrapWorkerSpecWaitUntil([&Events]()
	{
		return Events->Find([](const FWoTBootstrapProgressEvent& Event)
		{
			return WoTGameBootstrapWorkerSpecIsCopyEvent(Event)
				&& Event.CompletedBytes.GetValue() == 4;
		}).IsSet();
	}));
	TestTrue(TEXT("a stale partial copy"), WoTGameBootstrapWorkerSpecSave(FPaths::Combine(
		WoTGameBootstrapWorkerSpecReceivedFolder(Fixture.UserStateRoot),
		TEXT("stale.part")), WoTGameBootstrapWorkerSpecAscii(TEXT("ab"))));
	TestTrue(TEXT("the installer lands in Downloads"),
		WoTGameBootstrapWorkerSpecSaveDownloadsInstaller(Fixture.InstallRoot, Good));
	Worker->RequestOriginalGameRecheck();
	if (!TestTrue(TEXT("the Downloads installer ends the wait"),
		Pending->Done->Wait(FTimespan::FromSeconds(
			WoTGameBootstrapWorkerSpecHangSeconds))))
	{
		return false;
	}
	TestEqual(TEXT("to extraction"), Pending->Completion.NextPhase,
		EWoTBootstrapPhase::ExtractingOriginalGame);
	TestTrue(TEXT("from Downloads"), WoTGameBootstrapWorkerSpecHeartbeatHas(
		Fixture.UserStateRoot, TEXT("original-game-detected"),
		WoTGameBootstrapWorkerSpecDetectedFrom(TEXT("downloads"))));
	TestEqual(TEXT("leaving the phase closes every socket"), Log->Open.Load(), 0);
	TestTrue(TEXT("the discovery port is free"), WoTReceiverSpec::PortIsFree(Port));
	bool bClosed = false;
	TestEqual(TEXT("nothing came back on the copy's connection"),
		Peer.BytesFromReceiver(bClosed, 2.0), 0);
	TestTrue(TEXT("and it was closed"), bClosed);
	TestEqual(TEXT("every partial copy is deleted once one is accepted"),
		WoTGameBootstrapWorkerSpecPartFiles(Fixture.UserStateRoot), 0);
	return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
