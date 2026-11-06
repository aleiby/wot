// Hostile archive and closed-tree specs for per-user release staging.
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "WoTBootstrapCore/Ed25519.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/InstalledTree.h"
#include "WoTUpdaterCore/ReleaseStaging.h"
#include "WoTUpdaterCore/UpdateFailureNotice.h"
#include "WoTUpdaterCore/UpdateState.h"

#if PLATFORM_MAC
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utime.h>
#elif PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#if WITH_AUTOMATION_TESTS

// Test-only declarations for the OpenSSL already statically linked by
// WoTBootstrapCore. The deterministic seed below authorizes no shipped ring.
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
	size_t* SignatureLength, const unsigned char* Message, size_t MessageLength);
}

namespace
{
constexpr EAutomationTestFlags WoTReleaseStageSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;
constexpr uint64 WoTReleaseStageSpecSequence = 71;
constexpr TCHAR WoTReleaseStageSpecKeyId[] = TEXT("stage-test-release-key");

#if PLATFORM_MAC
bool WoTReleaseStageSpecHasOnlyAllowedMacXattrs(
	FAutomationTestBase& Test, const FString& Path, const FString& Relative)
{
	const FTCHARToUTF8 Utf8(*Path);
	const ssize_t Required = listxattr(Utf8.Get(), nullptr, 0, XATTR_NOFOLLOW);
	if (Required < 0)
	{
		Test.AddError(FString::Printf(
			TEXT("could not list staged xattrs for %s: %hs"),
			*Relative, strerror(errno)));
		return false;
	}
	if (Required > static_cast<ssize_t>(MAX_int32))
	{
		Test.AddError(TEXT("staged xattr name list exceeds the test limit: ")
			+ Relative);
		return false;
	}

	TArray<ANSICHAR> Names;
	Names.SetNumUninitialized(static_cast<int32>(Required));
	const ssize_t Read = Required == 0 ? 0
		: listxattr(Utf8.Get(), Names.GetData(),
			static_cast<size_t>(Names.Num()), XATTR_NOFOLLOW);
	if (Read != Required)
	{
		Test.AddError(FString::Printf(
			TEXT("staged xattr list changed while reading %s"), *Relative));
		return false;
	}

	bool bOk = true;
	bool bHasQuarantine = false;
	for (int32 Offset = 0; Offset < Names.Num();)
	{
		const ANSICHAR* Name = Names.GetData() + Offset;
		const int32 Remaining = Names.Num() - Offset;
		const int32 Length = static_cast<int32>(
			strnlen(Name, static_cast<size_t>(Remaining)));
		if (Length == Remaining)
		{
			Test.AddError(TEXT("staged xattr list is not NUL terminated: ") + Relative);
			return false;
		}
		const FString Attribute = UTF8_TO_TCHAR(Name);
		bHasQuarantine |= Attribute == TEXT("com.apple.quarantine");
		bOk &= Test.TestTrue(
			TEXT("staging copies no archive xattr: ") + Relative + TEXT(" ") + Attribute,
			Attribute == TEXT("com.apple.provenance"));
		Offset += Length + 1;
	}
	bOk &= Test.TestFalse(
		TEXT("staging never adds com.apple.quarantine: ") + Relative,
		bHasQuarantine);
	return bOk;
}

bool WoTReleaseStageSpecHasNoAppleDouble(
	FAutomationTestBase& Test, const FString& TreeRoot)
{
	TArray<FString> Paths;
	IFileManager::Get().FindFilesRecursive(
		Paths, *TreeRoot, TEXT("*"), true, true, false);
	bool bOk = true;
	for (const FString& Path : Paths)
	{
		FString Relative = Path;
		FPaths::MakePathRelativeTo(Relative, *(TreeRoot + TEXT("/")));
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		TArray<FString> Components;
		Relative.ParseIntoArray(Components, TEXT("/"), true);
		for (const FString& Component : Components)
		{
			bOk &= Test.TestFalse(
				TEXT("staging creates no AppleDouble entry: ") + Relative,
				IsAppleDoublePathComponent(Component));
		}
	}
	return bOk;
}
#endif

#if PLATFORM_MAC
constexpr TCHAR WoTReleaseStageSpecPlatform[] = TEXT("Mac");
constexpr TCHAR WoTReleaseStageSpecRoot[] = TEXT("WoT.app");
#elif PLATFORM_WINDOWS
constexpr TCHAR WoTReleaseStageSpecPlatform[] = TEXT("Win64");
constexpr TCHAR WoTReleaseStageSpecRoot[] = TEXT("Game");
#else
#error "release staging specs run only on Win64 and Mac"
#endif

TArray<uint8> WoTReleaseStageSpecAscii(FStringView Text)
{
	const FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

void WoTReleaseStageSpecAppend16(TArray<uint8>& Out, uint16 Value)
{
	Out.Add(static_cast<uint8>(Value & 0xffu));
	Out.Add(static_cast<uint8>((Value >> 8) & 0xffu));
}

void WoTReleaseStageSpecAppend32(TArray<uint8>& Out, uint32 Value)
{
	for (int32 Shift = 0; Shift < 32; Shift += 8)
	{
		Out.Add(static_cast<uint8>((Value >> Shift) & 0xffu));
	}
}

void WoTReleaseStageSpecPut32(TArray<uint8>& Out, int32 Offset, uint32 Value)
{
	for (int32 Shift = 0; Shift < 32; Shift += 8)
	{
		Out[Offset++] = static_cast<uint8>((Value >> Shift) & 0xffu);
	}
}

enum class EWoTReleaseStageSpecZipType : uint8
{
	File,
	Directory,
	UnixSymlink,
	DosReparse,
};

struct FWoTReleaseStageSpecZipInput
{
	FString Path;
	TArray<uint8> Bytes;
	EWoTReleaseStageSpecZipType Type = EWoTReleaseStageSpecZipType::File;
	uint32 Mode = 0644u;
	uint32 DeclaredSize = 0;
	bool bOverrideDeclaredSize = false;
};

struct FWoTReleaseStageSpecZipCentral
{
	TArray<uint8> Name;
	uint32 Crc = 0;
	uint32 CompressedSize = 0;
	uint32 DeclaredSize = 0;
	uint32 External = 0;
	uint32 LocalOffset = 0;
};

TArray<uint8> WoTReleaseStageSpecZip(
	const TArray<FWoTReleaseStageSpecZipInput>& Inputs,
	bool bCentralLocalDisagreement = false)
{
	TArray<uint8> Archive;
	TArray<FWoTReleaseStageSpecZipCentral> Central;
	for (const FWoTReleaseStageSpecZipInput& Input : Inputs)
	{
		FWoTReleaseStageSpecZipCentral Record;
		FString StoredPath = Input.Path;
		if (Input.Type == EWoTReleaseStageSpecZipType::Directory
			&& !StoredPath.EndsWith(TEXT("/")))
		{
			StoredPath += TEXT("/");
		}
		Record.Name = WoTReleaseStageSpecAscii(StoredPath);
		Record.Crc = FCrc::MemCrc32(Input.Bytes.GetData(), Input.Bytes.Num());
		Record.CompressedSize = static_cast<uint32>(Input.Bytes.Num());
		Record.DeclaredSize = Input.bOverrideDeclaredSize
			? Input.DeclaredSize : Record.CompressedSize;
		Record.LocalOffset = static_cast<uint32>(Archive.Num());
		if (Input.Type == EWoTReleaseStageSpecZipType::Directory)
		{
			Record.External = (0040000u | Input.Mode) << 16;
		}
		else if (Input.Type == EWoTReleaseStageSpecZipType::UnixSymlink)
		{
			Record.External = (0120000u | Input.Mode) << 16;
		}
		else if (Input.Type == EWoTReleaseStageSpecZipType::DosReparse)
		{
			Record.External = 0x400u;
		}
		else
		{
			Record.External = (0100000u | Input.Mode) << 16;
		}

		WoTReleaseStageSpecAppend32(Archive, 0x04034b50u);
		WoTReleaseStageSpecAppend16(Archive, 20);
		WoTReleaseStageSpecAppend16(Archive, 0x0800u);
		WoTReleaseStageSpecAppend16(Archive,
			Input.bOverrideDeclaredSize ? 8 : 0);
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend32(Archive, Record.Crc);
		WoTReleaseStageSpecAppend32(Archive, Record.CompressedSize);
		WoTReleaseStageSpecAppend32(Archive, Record.DeclaredSize);
		WoTReleaseStageSpecAppend16(Archive, static_cast<uint16>(Record.Name.Num()));
		WoTReleaseStageSpecAppend16(Archive, 0);
		Archive.Append(Record.Name);
		Archive.Append(Input.Bytes);
		Central.Add(MoveTemp(Record));
	}

	const uint32 CentralOffset = static_cast<uint32>(Archive.Num());
	for (const FWoTReleaseStageSpecZipCentral& Record : Central)
	{
		WoTReleaseStageSpecAppend32(Archive, 0x02014b50u);
		WoTReleaseStageSpecAppend16(Archive, 0x033fu);
		WoTReleaseStageSpecAppend16(Archive, 20);
		WoTReleaseStageSpecAppend16(Archive, 0x0800u);
		WoTReleaseStageSpecAppend16(Archive,
			Record.DeclaredSize != Record.CompressedSize ? 8 : 0);
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend32(Archive, Record.Crc);
		WoTReleaseStageSpecAppend32(Archive, Record.CompressedSize);
		WoTReleaseStageSpecAppend32(Archive, Record.DeclaredSize);
		WoTReleaseStageSpecAppend16(Archive, static_cast<uint16>(Record.Name.Num()));
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend16(Archive, 0);
		WoTReleaseStageSpecAppend32(Archive, Record.External);
		WoTReleaseStageSpecAppend32(Archive, Record.LocalOffset);
		Archive.Append(Record.Name);
	}
	const uint32 CentralSize = static_cast<uint32>(Archive.Num()) - CentralOffset;
	WoTReleaseStageSpecAppend32(Archive, 0x06054b50u);
	WoTReleaseStageSpecAppend16(Archive, 0);
	WoTReleaseStageSpecAppend16(Archive, 0);
	WoTReleaseStageSpecAppend16(Archive, static_cast<uint16>(Central.Num()));
	WoTReleaseStageSpecAppend16(Archive, static_cast<uint16>(Central.Num()));
	WoTReleaseStageSpecAppend32(Archive, CentralSize);
	WoTReleaseStageSpecAppend32(Archive, CentralOffset);
	WoTReleaseStageSpecAppend16(Archive, 0);
	if (bCentralLocalDisagreement && Central.Num() > 0)
	{
		WoTReleaseStageSpecPut32(Archive, CentralOffset + 24,
			Central[0].DeclaredSize + 1);
	}
	return Archive;
}

FWoTInstalledTreeEntry WoTReleaseStageSpecDirectory(
	const TCHAR* Path, uint32 Mode = 0755u)
{
	FWoTInstalledTreeEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::Directory;
	Entry.Mode = Mode;
	return Entry;
}

FWoTInstalledTreeEntry WoTReleaseStageSpecFile(
	const TCHAR* Path, TConstArrayView<uint8> Bytes)
{
	FWoTInstalledTreeEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::File;
	Entry.Mode = 0644u;
	Entry.Size = static_cast<uint64>(Bytes.Num());
	Entry.Sha256 = Sha256(Bytes);
	return Entry;
}

FWoTInstalledTreeEntry WoTReleaseStageSpecLink(
	const TCHAR* Path, const TCHAR* Target)
{
	FWoTInstalledTreeEntry Entry;
	Entry.Path = Path;
	Entry.Type = EWoTTreeEntryType::SymbolicLink;
	Entry.Mode = 0777u;
	Entry.LinkTarget = Target;
	return Entry;
}

struct FWoTReleaseStageSpecTree
{
	TArray<uint8> Content = WoTReleaseStageSpecAscii(TEXT("closed payload\n"));
	TArray<uint8> Game = WoTReleaseStageSpecAscii(TEXT("game bytes\n"));
	TArray<uint8> Updater = WoTReleaseStageSpecAscii(TEXT("updater bytes\n"));
	TArray<uint8> LockedChild = WoTReleaseStageSpecAscii(TEXT("locked child\n"));
	TArray<uint8> Padding;
	FWoTInstalledTreeManifest Inventory;
	TArray<uint8> InventoryBytes;
	TArray<uint8> InventorySignatureBytes;
	FWoTSha256 RootDigest;
	bool bRestrictiveDirectory = false;
	uint32 RestrictiveDirectoryMode = 0555u;
	bool bRestrictiveChild = true;
	bool bSymbolicLink = false;

	explicit FWoTReleaseStageSpecTree(bool bInRestrictiveDirectory = false,
		uint32 InRestrictiveDirectoryMode = 0555u,
		bool bInRestrictiveChild = true, bool bInSymbolicLink = false,
		int32 InPaddingBytes = 0)
		: bRestrictiveDirectory(bInRestrictiveDirectory)
		, RestrictiveDirectoryMode(InRestrictiveDirectoryMode)
		, bRestrictiveChild(bInRestrictiveChild)
		, bSymbolicLink(bInSymbolicLink)
	{
		Padding.SetNumUninitialized(InPaddingBytes);
		for (int32 Index = 0; Index < Padding.Num(); ++Index)
		{
			Padding[Index] = static_cast<uint8>((Index * 131 + 17) & 0xff);
		}
		Inventory.Format = WoTInstalledTreeV1Format;
		Inventory.Version = WoTInstalledTreeVersion;
		Inventory.Platform = WoTReleaseStageSpecPlatform;
		Inventory.Entries = {
			WoTReleaseStageSpecFile(TEXT("Content.bin"), Content),
			WoTReleaseStageSpecDirectory(TEXT("Contents")),
			WoTReleaseStageSpecDirectory(TEXT("Contents/MacOS")),
			WoTReleaseStageSpecFile(TEXT("Contents/MacOS/WoT"), Game),
		};
		if (bSymbolicLink)
		{
			Inventory.Entries.Add(WoTReleaseStageSpecLink(
				TEXT("Contents/MacOS/WoTLink"), TEXT("WoT")));
		}
		Inventory.Entries.Add(WoTReleaseStageSpecFile(
			TEXT("Contents/MacOS/WoTUpdater"), Updater));
		if (bRestrictiveDirectory)
		{
			Inventory.Entries.Add(WoTReleaseStageSpecDirectory(
				TEXT("Locked"), RestrictiveDirectoryMode));
			if (bRestrictiveChild)
			{
				Inventory.Entries.Add(WoTReleaseStageSpecFile(
					TEXT("Locked/child.bin"), LockedChild));
			}
		}
		if (!Padding.IsEmpty())
		{
			Inventory.Entries.Add(WoTReleaseStageSpecFile(
				TEXT("Padding.bin"), Padding));
		}
		TWoTBootstrapResult<FWoTSha256> Computed =
			ComputeInstalledTreeRoot(Inventory);
		check(!Computed.IsError());
		RootDigest = Computed.GetValue();
		FString Entries = FString::Printf(
			TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"Content.bin\",\"sha256\":\"%s\",\"size\":%d,\"type\":\"file\"},")
			TEXT("{\"link_target\":\"\",\"mode\":493,\"path\":\"Contents\",\"sha256\":\"\",\"size\":0,\"type\":\"directory\"},")
			TEXT("{\"link_target\":\"\",\"mode\":493,\"path\":\"Contents/MacOS\",\"sha256\":\"\",\"size\":0,\"type\":\"directory\"},")
			TEXT("{\"link_target\":\"\",\"mode\":420,\"path\":\"Contents/MacOS/WoT\",\"sha256\":\"%s\",\"size\":%d,\"type\":\"file\"}"),
			*Sha256(Content).ToHex(), Content.Num(), *Sha256(Game).ToHex(), Game.Num());
		if (bSymbolicLink)
		{
			Entries += TEXT(
				",{\"link_target\":\"WoT\",\"mode\":511,\"path\":\"Contents/MacOS/WoTLink\",\"sha256\":\"\",\"size\":0,\"type\":\"symlink\"}");
		}
		Entries += FString::Printf(
			TEXT(",{\"link_target\":\"\",\"mode\":420,\"path\":\"Contents/MacOS/WoTUpdater\",\"sha256\":\"%s\",\"size\":%d,\"type\":\"file\"}"),
			*Sha256(Updater).ToHex(), Updater.Num());
		if (bRestrictiveDirectory)
		{
			Entries += FString::Printf(
				TEXT(",{\"link_target\":\"\",\"mode\":%u,\"path\":\"Locked\",\"sha256\":\"\",\"size\":0,\"type\":\"directory\"}"),
				RestrictiveDirectoryMode);
			if (bRestrictiveChild)
			{
				Entries += FString::Printf(
					TEXT(",{\"link_target\":\"\",\"mode\":420,\"path\":\"Locked/child.bin\",\"sha256\":\"%s\",\"size\":%d,\"type\":\"file\"}"),
					*Sha256(LockedChild).ToHex(), LockedChild.Num());
			}
		}
		if (!Padding.IsEmpty())
		{
			Entries += FString::Printf(
				TEXT(",{\"link_target\":\"\",\"mode\":420,\"path\":\"Padding.bin\",\"sha256\":\"%s\",\"size\":%d,\"type\":\"file\"}"),
				*Sha256(Padding).ToHex(), Padding.Num());
		}
		const FString Json = FString::Printf(
			TEXT("{\"entries\":[%s],\"format\":\"wot-installed-tree-v1\",\"platform\":\"%s\",\"version\":1}"),
			*Entries, WoTReleaseStageSpecPlatform);
		InventoryBytes = WoTReleaseStageSpecAscii(Json);
		InventorySignatureBytes.SetNumZeroed(FWoTEd25519Signature::ByteCount);
	}
};

TArray<FWoTReleaseStageSpecZipInput> WoTReleaseStageSpecValidInputs(
	const FWoTReleaseStageSpecTree& Tree)
{
	const FString Root = WoTReleaseStageSpecRoot;
	TArray<FWoTReleaseStageSpecZipInput> Inputs = {
		{Root, {}, EWoTReleaseStageSpecZipType::Directory, 0755u},
		{Root + TEXT("/Content.bin"), Tree.Content,
			EWoTReleaseStageSpecZipType::File, 0644u},
		{Root + TEXT("/Contents"), {},
			EWoTReleaseStageSpecZipType::Directory, 0755u},
		{Root + TEXT("/Contents/MacOS"), {},
			EWoTReleaseStageSpecZipType::Directory, 0755u},
		{Root + TEXT("/Contents/MacOS/WoT"), Tree.Game,
			EWoTReleaseStageSpecZipType::File, 0644u},
	};
	if (Tree.bSymbolicLink)
	{
		Inputs.Add({Root + TEXT("/Contents/MacOS/WoTLink"),
			WoTReleaseStageSpecAscii(TEXT("WoT")),
			EWoTReleaseStageSpecZipType::UnixSymlink, 0777u});
	}
	Inputs.Add({Root + TEXT("/Contents/MacOS/WoTUpdater"), Tree.Updater,
		EWoTReleaseStageSpecZipType::File, 0644u});
	if (Tree.bRestrictiveDirectory)
	{
		Inputs.Add({Root + TEXT("/Locked"), {},
			EWoTReleaseStageSpecZipType::Directory,
			Tree.RestrictiveDirectoryMode});
		if (Tree.bRestrictiveChild)
		{
			Inputs.Add({Root + TEXT("/Locked/child.bin"), Tree.LockedChild,
				EWoTReleaseStageSpecZipType::File, 0644u});
		}
	}
	if (!Tree.Padding.IsEmpty())
	{
		Inputs.Add({Root + TEXT("/Padding.bin"), Tree.Padding,
			EWoTReleaseStageSpecZipType::File, 0644u});
	}
	Inputs.Add({Root + TEXT("/installed-tree-v1.json"), Tree.InventoryBytes,
		EWoTReleaseStageSpecZipType::File, 0644u});
	Inputs.Add({Root + TEXT("/installed-tree-v1.json.sig"),
		Tree.InventorySignatureBytes,
		EWoTReleaseStageSpecZipType::File, 0644u});
	return Inputs;
}

bool WoTReleaseStageSpecKey(FWoTEd25519PublicKey& OutPublic,
	evp_pkey_st*& OutPrivate)
{
	uint8 Seed[32];
	for (int32 Index = 0;
		Index < static_cast<int32>(UE_ARRAY_COUNT(Seed)); ++Index)
	{
		Seed[Index] = static_cast<uint8>(Index + 1);
	}
	// OpenSSL's stable NID for Ed25519.
	OutPrivate = EVP_PKEY_new_raw_private_key(1087, nullptr, Seed, sizeof(Seed));
	if (OutPrivate == nullptr) return false;
	size_t PublicLength = FWoTEd25519PublicKey::ByteCount;
	return EVP_PKEY_get_raw_public_key(
		OutPrivate, OutPublic.Bytes, &PublicLength) == 1
		&& PublicLength == FWoTEd25519PublicKey::ByteCount;
}

TArray<uint8> WoTReleaseStageSpecSign(
	evp_pkey_st* PrivateKey, TConstArrayView<uint8> Payload)
{
	const TArray<uint8> Framed = FrameSignatureInput(
		EWoTSignatureContext::ReleaseManifestV2, Payload);
	TArray<uint8> Signature;
	Signature.SetNumZeroed(FWoTEd25519Signature::ByteCount);
	evp_md_ctx_st* Context = EVP_MD_CTX_new();
	size_t SignatureLength = Signature.Num();
	if (Context == nullptr
		|| EVP_DigestSignInit(Context, nullptr, nullptr, nullptr, PrivateKey) != 1
		|| EVP_DigestSign(Context, Signature.GetData(), &SignatureLength,
			Framed.GetData(), Framed.Num()) != 1
		|| SignatureLength != FWoTEd25519Signature::ByteCount)
	{
		Signature.Reset();
	}
	if (Context != nullptr) EVP_MD_CTX_free(Context);
	return Signature;
}

struct FWoTReleaseStageSpecFixture
{
	FWoTAnnouncementV1 Announcement;
	FWoTKeyRing Ring;
	FWoTReleaseStagingMetadata Metadata;
	TArray<uint8> Archive;
	FWoTSha256 TreeRoot;
};

FWoTReleaseStageSpecFixture WoTReleaseStageSpecBuildFixture(
	const FWoTReleaseStageSpecTree& Tree, TArray<uint8> Archive,
	int64 ArchiveSizeAdjustment = 0, bool bWrongArchiveDigest = false)
{
	FWoTReleaseStageSpecFixture Fixture;
	Fixture.Archive = MoveTemp(Archive);
	Fixture.TreeRoot = Tree.RootDigest;
	FWoTEd25519PublicKey PublicKey;
	evp_pkey_st* PrivateKey = nullptr;
	check(WoTReleaseStageSpecKey(PublicKey, PrivateKey));
	FWoTSha256 ArchiveDigest = Sha256(Fixture.Archive);
	if (bWrongArchiveDigest) ArchiveDigest.Bytes[0] ^= 0x80u;
	const uint64 DeclaredArchiveSize = static_cast<uint64>(
		static_cast<int64>(Fixture.Archive.Num()) + ArchiveSizeAdjustment);
	const FString Manifest = FString::Printf(
		TEXT("{\"announcement_domain\":\"com.aleiby.wot.release.v1\",")
		TEXT("\"archive\":{\"name\":\"release.zip\",\"sha256\":\"%s\",\"size\":%llu,\"url\":\"https://github.com/aleiby/wot/releases/download/stage/release.zip\"},")
		TEXT("\"archive_root_id\":\"wot-archive-root-v1\",\"build_commit\":\"stage-test\",")
		TEXT("\"cache_contract_id\":\"wot-cache-contract-1\",\"cache_contract_version\":3,")
		TEXT("\"format\":\"wot-release-manifest-v2\",\"generated_utc\":\"2026-07-28T00:00:00+00:00\",")
		TEXT("\"human_version\":\"stage-test\",\"installed_tree_root\":{\"name\":\"%s\",\"sha256\":\"%s\",\"size\":%d},")
		TEXT("\"installed_tree_version\":1,\"key_id\":\"%s\",")
		TEXT("\"main_executable\":{\"name\":\"Contents/MacOS/WoT\",\"sha256\":\"%s\",\"size\":%d},")
		TEXT("\"manifest_version\":2,\"minimum_commit_service_protocol\":1,\"minimum_gate_protocol\":1,\"minimum_updater_protocol\":1,")
		TEXT("\"platform\":\"%s\",\"release_sequence\":%llu,\"stripped_audit_verdict\":\"pass\",")
		TEXT("\"target_attestation\":{\"configuration\":\"Shipping\",\"dedicated_server_startup_available\":false,\"launch_module\":\"WoTLaunch\",\"listen_server_present\":true,\"release_gate_enabled\":false,\"target_type\":\"Game\"}")
		TEXT("}"),
		*ArchiveDigest.ToHex(), DeclaredArchiveSize, WoTReleaseStageSpecRoot,
		*Tree.RootDigest.ToHex(), Tree.InventoryBytes.Num(), WoTReleaseStageSpecKeyId,
		*Sha256(Tree.Game).ToHex(), Tree.Game.Num(), WoTReleaseStageSpecPlatform,
		WoTReleaseStageSpecSequence);
	Fixture.Metadata.ReleaseManifestBytes = WoTReleaseStageSpecAscii(Manifest);
	Fixture.Metadata.ReleaseManifestSignature = WoTReleaseStageSpecSign(
		PrivateKey, Fixture.Metadata.ReleaseManifestBytes);
	EVP_PKEY_free(PrivateKey);
	Fixture.Metadata.AnnouncementBytes = WoTReleaseStageSpecAscii(TEXT("signed-announcement"));
	Fixture.Metadata.AnnouncementSignature.SetNumZeroed(64);
	Fixture.Metadata.KeyRingBytes = WoTReleaseStageSpecAscii(TEXT("signed-key-ring"));
	Fixture.Metadata.KeyRingSignature.SetNumZeroed(64);

	Fixture.Announcement.Format = WoTAnnouncementV1Format;
	Fixture.Announcement.Platform = WoTReleaseStageSpecPlatform;
	Fixture.Announcement.AnnouncementDomain = WoT::Bootstrap::AnnouncementDomain;
	Fixture.Announcement.ReleaseSequence = WoTReleaseStageSpecSequence;
	Fixture.Announcement.KeyId = WoTReleaseStageSpecKeyId;
	Fixture.Announcement.KeyRingEpoch = 1;
	Fixture.Announcement.MinimumGateProtocol = 1;
	Fixture.Announcement.Manifest.Url = TEXT("https://github.com/manifest.json");
	Fixture.Announcement.Manifest.Size = Fixture.Metadata.ReleaseManifestBytes.Num();
	Fixture.Announcement.Manifest.Sha256 = Sha256(Fixture.Metadata.ReleaseManifestBytes);
	Fixture.Ring.Format = TEXT("wot-release-key-ring-v1");
	Fixture.Ring.Epoch = 1;
	FWoTReleaseKey Key;
	Key.KeyId = WoTReleaseStageSpecKeyId;
	Key.PublicKey = PublicKey;
	Key.FirstSequence = 1;
	Key.LastSequence = 1000;
	Fixture.Ring.Keys.Add(Key);
	return Fixture;
}

class FWoTReleaseStageSpecInstall final
{
public:
	explicit FWoTReleaseStageSpecInstall(const FString& BaseDirectory = FString(),
		bool bUseRealCodeSeal = false)
	{
		if (!bUseRealCodeSeal)
		{
			CodeSealOverride = MakeUnique<
				FWoTReleaseStagingCodeSealVerifierOverrideForTesting>(
				[](const FString&) { return FWoTBootstrapStatus::Success(); });
		}
		FString Parent = BaseDirectory.IsEmpty()
			? FPlatformMisc::GetEnvironmentVariable(TEXT("TEMP")) : BaseDirectory;
		if (Parent.IsEmpty()) Parent = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".tmp")));
		InstallRoot = FPaths::Combine(Parent,
			TEXT("wot-stage-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UserStateRoot = InstallRoot + TEXT("-user");
		IFileManager::Get().MakeDirectory(*InstallRoot, true);
		IFileManager::Get().MakeDirectory(*UserStateRoot, true);
		TWoTBootstrapResult<FWoTUpdaterStateRoot> Resolved =
			ResolveUpdaterStateRoot(UserStateRoot, InstallRoot);
		check(!Resolved.IsError());
		Root = MoveTemp(Resolved).GetValue();
		IFileManager::Get().MakeDirectory(
			*FPaths::Combine(Root.InstallRoot, Root.LiveLeaf), true);
		FWoTBootstrapStatus Writable = ProbeUpdaterInstallWritable(Root);
		checkf(!Writable.IsError(), TEXT("writable fixture preflight failed: %s"),
			Writable.IsError() ? *Writable.GetError().ToString() : TEXT(""));
	}
	~FWoTReleaseStageSpecInstall()
	{
		IFileManager::Get().DeleteDirectory(*InstallRoot, false, true);
		IFileManager::Get().DeleteDirectory(*UserStateRoot, false, true);
	}
	FString InstallRoot;
	FString UserStateRoot;
	FWoTUpdaterStateRoot Root;
	TUniquePtr<FWoTReleaseStagingCodeSealVerifierOverrideForTesting>
		CodeSealOverride;
};

enum class EWoTReleaseStageSpecCheckpoint : uint8
{
	None,
	AppendExtra,
	ReplaceWithSymlink,
	ChmodFile,
	InterruptWithPartialMember,
	AppendExtraAfterEnumeration,
	FailBeforeSnapshotValidation,
	HoldInventoryScanner,
};

enum class EWoTReleaseStageSpecFetch : uint8
{
	Normal,
	InterruptFresh,
	InterruptAfterCheckpoint,
	FinishShort,
	InterruptRestartPastOldCommit,
	IgnoreRange,
	FullChangedValidator,
	ChangedValidator,
	CorruptComplete,
	TruncateComplete,
};

class FWoTReleaseStageSpecDownload final : public IWoTReleaseDownload
{
public:
	FWoTReleaseStageSpecDownload(const FWoTReleaseStageSpecFixture& InFixture,
		EWoTReleaseStageSpecFetch InFetch = EWoTReleaseStageSpecFetch::Normal,
		EWoTReleaseStageSpecCheckpoint InCheckpoint =
			EWoTReleaseStageSpecCheckpoint::None)
		: Metadata(InFixture.Metadata)
		, Archive(InFixture.Archive)
		, FetchScript(InFetch)
		, Checkpoint(InCheckpoint)
	{
	}

	~FWoTReleaseStageSpecDownload() override
	{
#if PLATFORM_WINDOWS
		if (InventoryScanner != INVALID_HANDLE_VALUE)
		{
			::CloseHandle(InventoryScanner);
		}
#endif
	}

	virtual FWoTBootstrapStatus Fetch(IWoTReleaseDownloadSink& Sink) override
	{
		RequestedOffsets.Add(0);
		FWoTReleaseDownloadResponse Response;
		Response.Status = 200;
		Response.BodyLength = Archive.Num();
		auto Started = Sink.OnResponseStart(Response);
		if (Started.IsError()) return Started;
		if (FetchScript == EWoTReleaseStageSpecFetch::InterruptFresh
			|| FetchScript == EWoTReleaseStageSpecFetch::FinishShort)
		{
			auto Written = Sink.Write(MakeArrayView(Archive.GetData(), Archive.Num() / 2));
			if (Written.IsError()) return Written;
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(EWoTBootstrapError::Io, TEXT("interrupted download")));
		}
		TArray<uint8> Body = Archive;
		if (FetchScript == EWoTReleaseStageSpecFetch::CorruptComplete) Body[0] ^= 0xff;
		return Sink.Write(Body);
	}

	virtual FWoTBootstrapStatus BeforeInstalledTreeSnapshotValidation(
		FStringView StagedTreeDir) override
	{
		bSnapshotValidationCheckpointRan = true;
		bInventoryObservedBeforeSnapshotValidation = IFileManager::Get().FileExists(
			*FPaths::Combine(
				FString(StagedTreeDir), TEXT("installed-tree-v1.json")));
		bInventorySignatureObservedBeforeSnapshotValidation =
			IFileManager::Get().FileExists(*FPaths::Combine(
				FString(StagedTreeDir), TEXT("installed-tree-v1.json.sig")));
#if PLATFORM_WINDOWS
		if (InventoryScanner != INVALID_HANDLE_VALUE)
		{
			// Cross an NTFS timestamp tick before and after the last external
			// close so the parent ChangeTime transition is deterministic.
			FPlatformProcess::SleepNoStats(0.3f);
			if (::CloseHandle(InventoryScanner) == 0)
			{
				InventoryScanner = INVALID_HANDLE_VALUE;
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("could not close the simulated scanner handle")));
			}
			InventoryScanner = INVALID_HANDLE_VALUE;
			bInventoryScannerReleased = true;
			FPlatformProcess::SleepNoStats(0.3f);
		}
#endif
		if (Checkpoint == EWoTReleaseStageSpecCheckpoint::FailBeforeSnapshotValidation)
		{
			bCleanupCancellationArmed = true;
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io,
				TEXT("scripted refusal before snapshot validation")));
		}
		if (Checkpoint != EWoTReleaseStageSpecCheckpoint::AppendExtraAfterEnumeration)
		{
			return FWoTBootstrapStatus::Success();
		}
		bSnapshotCheckpointRan = true;
		const TArray<uint8> Extra =
			WoTReleaseStageSpecAscii(TEXT("post-enumeration extra"));
		if (!FFileHelper::SaveArrayToFile(Extra,
			*FPaths::Combine(FString(StagedTreeDir), TEXT("late-extra.bin"))))
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io,
				TEXT("could not append post-enumeration file")));
		}
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus FetchSignedMetadata(
		FWoTReleaseStagingMetadata& OutMetadata) override
	{
		OutMetadata = Metadata;
		return FWoTBootstrapStatus::Success();
	}

	virtual FWoTBootstrapStatus AfterArchiveExtracted(
		FStringView StagedTreeDir) override
	{
		bAfterArchiveExtractedRan = true;
		const FString InventoryPath = FPaths::Combine(
			FString(StagedTreeDir), TEXT("installed-tree-v1.json"));
		bInventoryObservedAfterExtraction =
			IFileManager::Get().FileExists(*InventoryPath);
		bInventorySignatureObservedAfterExtraction =
			IFileManager::Get().FileExists(*FPaths::Combine(
				FString(StagedTreeDir), TEXT("installed-tree-v1.json.sig")));
#if PLATFORM_WINDOWS
		if (Checkpoint == EWoTReleaseStageSpecCheckpoint::HoldInventoryScanner
			&& bInventoryObservedAfterExtraction)
		{
			InventoryScanner = ::CreateFileW(*InventoryPath, GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (InventoryScanner == INVALID_HANDLE_VALUE)
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io,
					TEXT("could not open the simulated scanner handle"),
					InventoryPath));
			}
			bInventoryScannerOpened = true;
		}
#endif
		return FWoTBootstrapStatus::Success();
	}

	virtual void ReportProgress(uint64 CompletedBytes) override
	{
		Progress.Add(CompletedBytes);
	}

	virtual FWoTBootstrapStatus BeforeArchiveVerification(
		FStringView InArchivePath) override
	{
		bArchiveVerificationStarted = true;
		if (FetchScript != EWoTReleaseStageSpecFetch::TruncateComplete)
		{
			return FWoTBootstrapStatus::Success();
		}
		TArray<uint8> Truncated = Archive;
		Truncated.SetNum(Archive.Num() - 1);
		if (!FFileHelper::SaveArrayToFile(Truncated, *FString(InArchivePath)))
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::Io,
				TEXT("could not truncate the completed archive fixture")));
		}
		return FWoTBootstrapStatus::Success();
	}

	TArray<uint64> RequestedOffsets;
	TArray<uint64> Progress;
	bool bArchiveVerificationStarted = false;
	bool bCleanupCancellationArmed = false;

	virtual FWoTBootstrapStatus BeforeInstalledTreeVerification(
		FStringView StagedTreeDir) override
	{
		const FString Tree(StagedTreeDir);
		bVerificationCheckpointRan = true;
		bInventoryObservedBeforeVerification = IFileManager::Get().FileExists(
			*FPaths::Combine(Tree, TEXT("installed-tree-v1.json")));
		bInventorySignatureObservedBeforeVerification =
			IFileManager::Get().FileExists(*FPaths::Combine(
				Tree, TEXT("installed-tree-v1.json.sig")));
		const FString Main = FPaths::Combine(Tree, TEXT("Contents/MacOS/WoT"));
		if (Checkpoint == EWoTReleaseStageSpecCheckpoint::AppendExtra)
		{
			const TArray<uint8> Extra = WoTReleaseStageSpecAscii(TEXT("extra"));
			if (!FFileHelper::SaveArrayToFile(Extra,
				*FPaths::Combine(Tree, TEXT("unsigned-extra.bin"))))
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("could not append real extra file")));
			}
		}
		else if (Checkpoint == EWoTReleaseStageSpecCheckpoint::ChmodFile)
		{
#if PLATFORM_MAC
			const FTCHARToUTF8 Utf8(*Main);
			if (chmod(Utf8.Get(), 0600) != 0)
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("chmod fixture failed"), Main));
			}
#elif PLATFORM_WINDOWS
			if (!::SetFileAttributesW(*Main, FILE_ATTRIBUTE_READONLY))
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("mode fixture failed"), Main));
			}
#endif
		}
		else if (Checkpoint == EWoTReleaseStageSpecCheckpoint::ReplaceWithSymlink)
		{
#if PLATFORM_MAC
			const FTCHARToUTF8 MainUtf8(*Main);
			if (unlink(MainUtf8.Get()) != 0 || symlink("WoTUpdater", MainUtf8.Get()) != 0)
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("real symlink substitution failed"), Main));
			}
#elif PLATFORM_WINDOWS
			if (!::DeleteFileW(*Main)
				|| !::CreateSymbolicLinkW(*Main, TEXT("WoTUpdater"), 0))
			{
				const DWORD Error = ::GetLastError();
				if (Error == ERROR_PRIVILEGE_NOT_HELD)
				{
					bSymlinkPolicyDenied = true;
					return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
						EWoTBootstrapError::ServiceUnavailable,
						TEXT("Win64 policy denied CreateSymbolicLinkW")));
				}
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("real symlink substitution failed"), Main));
			}
#endif
		}
		else if (Checkpoint == EWoTReleaseStageSpecCheckpoint::InterruptWithPartialMember)
		{
			const TArray<uint8> Prefix = WoTReleaseStageSpecAscii(TEXT("part"));
			if (!FFileHelper::SaveArrayToFile(Prefix, *Main))
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::Io, TEXT("could not create partial member")));
			}
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("scripted extraction interruption")));
		}
		return FWoTBootstrapStatus::Success();
	}

	bool bSymlinkPolicyDenied = false;
	bool bSnapshotCheckpointRan = false;
	bool bAfterArchiveExtractedRan = false;
	bool bVerificationCheckpointRan = false;
	bool bSnapshotValidationCheckpointRan = false;
	bool bInventoryObservedAfterExtraction = false;
	bool bInventorySignatureObservedAfterExtraction = false;
	bool bInventoryObservedBeforeVerification = false;
	bool bInventorySignatureObservedBeforeVerification = false;
	bool bInventoryObservedBeforeSnapshotValidation = false;
	bool bInventorySignatureObservedBeforeSnapshotValidation = false;
#if PLATFORM_WINDOWS
	bool bInventoryScannerOpened = false;
	bool bInventoryScannerReleased = false;
#endif

private:
	FWoTReleaseStagingMetadata Metadata;
	TArray<uint8> Archive;
	EWoTReleaseStageSpecFetch FetchScript;
	EWoTReleaseStageSpecCheckpoint Checkpoint;
#if PLATFORM_WINDOWS
	HANDLE InventoryScanner = INVALID_HANDLE_VALUE;
#endif
};

int32 WoTReleaseStageSpecStagingChildren(const FWoTUpdaterStateRoot& Root)
{
	TArray<FString> Directories;
	IFileManager::Get().FindFiles(Directories, *(Root.StagingDir / TEXT("*")),
		false, true);
	return Directories.Num();
}

bool WoTReleaseStageSpecRefusedAndClean(FAutomationTestBase& Test,
	const TCHAR* Label, FWoTReleaseStageSpecFixture& Fixture,
	EWoTReleaseStageSpecCheckpoint Checkpoint = EWoTReleaseStageSpecCheckpoint::None)
{
	FWoTReleaseStageSpecInstall Install;
	FWoTReleaseStageSpecDownload Download(Fixture,
		EWoTReleaseStageSpecFetch::Normal, Checkpoint);
	TWoTBootstrapResult<FWoTStagedRelease> Staged = StageRelease(
		Fixture.Announcement, Fixture.Ring, Download, Install.Root);
#if PLATFORM_WINDOWS
	if (Checkpoint == EWoTReleaseStageSpecCheckpoint::ReplaceWithSymlink
		&& Download.bSymlinkPolicyDenied)
	{
		Test.AddInfo(TEXT("SKIP: Win64 policy denied real symlink creation"));
		return true;
	}
#endif
	const bool bRefused = Test.TestTrue(
		FString::Printf(TEXT("%s is refused by its actual check"), Label),
		Staged.IsError());
	bool bExpectedSnapshotRefusal = true;
	if (Checkpoint == EWoTReleaseStageSpecCheckpoint::AppendExtraAfterEnumeration)
	{
		bExpectedSnapshotRefusal = Test.TestTrue(
			FString::Printf(TEXT("%s reaches the post-enumeration checkpoint"), Label),
			Download.bSnapshotCheckpointRan);
		bExpectedSnapshotRefusal &= Test.TestTrue(
			FString::Printf(TEXT("%s is refused by directory snapshot revalidation"),
				Label),
			Staged.IsError() && Staged.GetError().Message.Contains(
				TEXT("directory changed during exact-tree verification"),
				ESearchCase::CaseSensitive));
	}
	const bool bClean = Test.TestEqual(
		FString::Printf(TEXT("%s leaves no staging directory"), Label),
		WoTReleaseStageSpecStagingChildren(Install.Root), 0);
	return bRefused && bExpectedSnapshotRefusal && bClean;
}

#if PLATFORM_WINDOWS
bool WoTReleaseStageSpecIcacls(
	const FString& Path, FStringView Arguments, FString& OutFailure)
{
	const FString Tool = FPaths::Combine(
		FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")),
		TEXT("System32/icacls.exe"));
	const FString Command = FString::Printf(
		TEXT("\"%s\" %.*s"), *Path, Arguments.Len(), Arguments.GetData());
	int32 ReturnCode = INDEX_NONE;
	FString StandardOutput;
	FString StandardError;
	const bool bRan = FPlatformProcess::ExecProcess(
		*Tool, *Command, &ReturnCode, &StandardOutput, &StandardError);
	if (!bRan || ReturnCode != 0)
	{
		OutFailure = FString::Printf(TEXT("icacls failed (%d): %s%s"),
			ReturnCode, *StandardOutput, *StandardError);
		return false;
	}
	return true;
}

bool WoTReleaseStageSpecFullSnapshot(
	const FString& Root, TArray<FString>& OutRows, FString& OutFailure)
{
	TArray<FString> Paths = {Root};
	IFileManager::Get().IterateDirectoryRecursively(*Root,
		[&Paths](const TCHAR* Path, bool)
		{
			Paths.Add(Path);
			return true;
		});
	for (const FString& Path : Paths)
	{
		WIN32_FILE_ATTRIBUTE_DATA Info = {};
		if (!::GetFileAttributesExW(
			*Path, GetFileExInfoStandard, &Info))
		{
			OutFailure = FString::Printf(
				TEXT("snapshot metadata failed for %s (%u)"),
				*Path, ::GetLastError());
			return false;
		}
		const bool bDirectory =
			(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		const SECURITY_INFORMATION SecurityInformation =
			OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION
			| DACL_SECURITY_INFORMATION;
		DWORD SecurityBytes = 0;
		(void)::GetFileSecurityW(
			*Path, SecurityInformation, nullptr, 0, &SecurityBytes);
		if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || SecurityBytes == 0)
		{
			OutFailure = FString::Printf(
				TEXT("snapshot ACL sizing failed for %s (%u)"),
				*Path, ::GetLastError());
			return false;
		}
		TArray<uint8> Security;
		Security.SetNumUninitialized(static_cast<int32>(SecurityBytes));
		if (!::GetFileSecurityW(*Path, SecurityInformation,
			reinterpret_cast<PSECURITY_DESCRIPTOR>(Security.GetData()),
			SecurityBytes, &SecurityBytes))
		{
			OutFailure = FString::Printf(
				TEXT("snapshot ACL read failed for %s (%u)"),
				*Path, ::GetLastError());
			return false;
		}
		FString Relative = Path == Root ? TEXT(".") : Path;
		if (Path != Root)
		{
			FPaths::MakePathRelativeTo(Relative, *(Root + TEXT("/")));
		}
		const uint64 Size = (static_cast<uint64>(Info.nFileSizeHigh) << 32)
			| static_cast<uint64>(Info.nFileSizeLow);
		const uint64 Created =
			(static_cast<uint64>(Info.ftCreationTime.dwHighDateTime) << 32)
			| static_cast<uint64>(Info.ftCreationTime.dwLowDateTime);
		const uint64 Written =
			(static_cast<uint64>(Info.ftLastWriteTime.dwHighDateTime) << 32)
			| static_cast<uint64>(Info.ftLastWriteTime.dwLowDateTime);
		FString ContentHash;
		if (!bDirectory)
		{
			const HANDLE ContentHandle = ::CreateFileW(*Path, FILE_READ_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (ContentHandle == INVALID_HANDLE_VALUE)
			{
				OutFailure = FString::Printf(
					TEXT("snapshot content open failed for %s (%u)"),
					*Path, ::GetLastError());
				return false;
			}
			ON_SCOPE_EXIT { ::CloseHandle(ContentHandle); };
			if (Size > static_cast<uint64>(MAX_int32))
			{
				OutFailure = TEXT("snapshot fixture file is too large: ") + Path;
				return false;
			}
			TArray<uint8> Bytes;
			Bytes.SetNumUninitialized(static_cast<int32>(Size));
			DWORD ReadBytes = 0;
			if (Size > 0 && (!::ReadFile(ContentHandle, Bytes.GetData(),
				static_cast<DWORD>(Size), &ReadBytes, nullptr)
				|| ReadBytes != static_cast<DWORD>(Size)))
			{
				OutFailure = FString::Printf(
					TEXT("snapshot content read failed for %s (%u)"),
					*Path, ::GetLastError());
				return false;
			}
			ContentHash = Sha256(Bytes).ToHex();
		}
		OutRows.Add(FString::Printf(
			TEXT("%s:%s:size=%llu:created=%llu:written=%llu:attributes=%u:acl=%s:sha=%s"),
			bDirectory ? TEXT("D") : TEXT("F"), *Relative,
			static_cast<unsigned long long>(Size),
			static_cast<unsigned long long>(Created),
			static_cast<unsigned long long>(Written),
			Info.dwFileAttributes,
			*BytesToHex(Security.GetData(), Security.Num()), *ContentHash));
	}
	OutRows.Sort();
	return true;
}
#endif
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageArchiveHostilesSpec,
	"WoT.UpdaterCore.Staging.ArchiveHostiles", WoTReleaseStageSpecFlags)

bool FWoTReleaseStageArchiveHostilesSpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree;
	const TArray<FWoTReleaseStageSpecZipInput> Valid =
		WoTReleaseStageSpecValidInputs(Tree);
	{
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Valid), 1, false);
		WoTReleaseStageSpecRefusedAndClean(*this, TEXT("declared archive size mismatch"),
			Fixture);
	}
	{
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Valid), 0, true);
		WoTReleaseStageSpecRefusedAndClean(*this, TEXT("declared archive digest mismatch"),
			Fixture);
	}
	{
		TArray<FWoTReleaseStageSpecZipInput> Inputs = Valid;
		Inputs.Add({FString(WoTReleaseStageSpecRoot) + TEXT("/../outside.bin"),
			WoTReleaseStageSpecAscii(TEXT("escape"))});
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Inputs));
		WoTReleaseStageSpecRefusedAndClean(*this, TEXT("traversal entry"), Fixture);
	}
	for (const EWoTReleaseStageSpecZipType LinkForm : {
		EWoTReleaseStageSpecZipType::UnixSymlink,
		EWoTReleaseStageSpecZipType::DosReparse})
	{
		TArray<FWoTReleaseStageSpecZipInput> Inputs = Valid;
		Inputs.Add({FString(WoTReleaseStageSpecRoot) + TEXT("/link"),
			WoTReleaseStageSpecAscii(TEXT("Content.bin")), LinkForm, 0777u});
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Inputs));
		WoTReleaseStageSpecRefusedAndClean(*this,
			LinkForm == EWoTReleaseStageSpecZipType::UnixSymlink
				? TEXT("Unix-mode symlink entry") : TEXT("DOS reparse link entry"),
			Fixture);
	}
	{
		TArray<FWoTReleaseStageSpecZipInput> Inputs = Valid;
		Inputs.Add(Valid[1]);
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Inputs));
		WoTReleaseStageSpecRefusedAndClean(*this, TEXT("duplicate ZIP name"), Fixture);
	}
	{
		TArray<FWoTReleaseStageSpecZipInput> Inputs = Valid;
		Inputs.Add({FString(WoTReleaseStageSpecRoot) + TEXT("/content.bin"),
			WoTReleaseStageSpecAscii(TEXT("case alias"))});
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Inputs));
		WoTReleaseStageSpecRefusedAndClean(*this, TEXT("case-folded ZIP name"), Fixture);
	}
	{
		TArray<FWoTReleaseStageSpecZipInput> Inputs = Valid;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FWoTReleaseStageSpecZipInput Bomb;
			Bomb.Path = FString::Printf(TEXT("%s/bomb%d.bin"),
				WoTReleaseStageSpecRoot, Index);
			Bomb.Bytes.Add(0);
			Bomb.DeclaredSize = 3000000000u;
			Bomb.bOverrideDeclaredSize = true;
			Inputs.Add(MoveTemp(Bomb));
		}
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Inputs));
		WoTReleaseStageSpecRefusedAndClean(*this,
			TEXT("declared decompression bomb over FWoTArchiveLimits"), Fixture);
	}
	{
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(Valid, true));
		WoTReleaseStageSpecRefusedAndClean(*this,
			TEXT("central/local header disagreement"), Fixture);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageInventoryArtifactsSpec,
	"WoT.UpdaterCore.Staging.InventoryArtifactsNeverMaterialized",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageInventoryArtifactsSpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree;
	auto InventoryInput = [](TArray<FWoTReleaseStageSpecZipInput>& Inputs)
		-> FWoTReleaseStageSpecZipInput*
	{
		const FString InventoryPath = FString(WoTReleaseStageSpecRoot)
			+ TEXT("/installed-tree-v1.json");
		return Inputs.FindByPredicate([&InventoryPath](
			const FWoTReleaseStageSpecZipInput& Input)
		{
			return Input.Path == InventoryPath;
		});
	};
	auto Refuses = [this](const TCHAR* Label,
		FWoTReleaseStageSpecFixture&& Fixture,
		EWoTBootstrapError ExpectedCode, const TCHAR* ExpectedMessage)
	{
		FWoTReleaseStageSpecInstall Install;
		FWoTReleaseStageSpecDownload Download(Fixture);
		TWoTBootstrapResult<FWoTStagedRelease> Staged = StageRelease(
			Fixture.Announcement, Fixture.Ring, Download, Install.Root);
		bool bOk = TestTrue(FString(Label) + TEXT(" is refused"), Staged.IsError());
		if (Staged.IsError())
		{
			bOk &= TestEqual(FString(Label) + TEXT(" keeps its error category"),
				Staged.GetError().Code, ExpectedCode);
			if (ExpectedMessage[0] != '\0')
			{
				bOk &= TestTrue(FString(Label) + TEXT(" keeps its refusal reason"),
					Staged.GetError().Message.Contains(
						ExpectedMessage, ESearchCase::CaseSensitive));
			}
		}
		bOk &= TestEqual(FString(Label) + TEXT(" leaves no staging directory"),
			WoTReleaseStageSpecStagingChildren(Install.Root), 0);
		return bOk;
	};
	{
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
		FWoTReleaseStageSpecInstall Install;
		FWoTReleaseStageSpecDownload Download(Fixture);
		TWoTBootstrapResult<FWoTStagedRelease> Staged = StageRelease(
			Fixture.Announcement, Fixture.Ring, Download, Install.Root);
		if (!TestFalse(TEXT("in-memory inventory artifacts still stage"),
				Staged.IsError()))
		{
			if (Staged.IsError()) AddError(Staged.GetError().ToString());
			return false;
		}
		TestTrue(TEXT("the artifact after-extraction hook ran"),
			Download.bAfterArchiveExtractedRan);
		TestTrue(TEXT("the artifact pre-verification hook ran"),
			Download.bVerificationCheckpointRan);
		TestTrue(TEXT("the artifact pre-snapshot hook ran"),
			Download.bSnapshotValidationCheckpointRan);
		TestFalse(TEXT("the inventory is absent after extraction"),
			Download.bInventoryObservedAfterExtraction);
		TestFalse(TEXT("the signature is absent after extraction"),
			Download.bInventorySignatureObservedAfterExtraction);
		TestFalse(TEXT("the inventory is absent before verification"),
			Download.bInventoryObservedBeforeVerification);
		TestFalse(TEXT("the signature is absent before verification"),
			Download.bInventorySignatureObservedBeforeVerification);
		TestFalse(TEXT("the inventory is absent before snapshot validation"),
			Download.bInventoryObservedBeforeSnapshotValidation);
		TestFalse(TEXT("the signature is absent before snapshot validation"),
			Download.bInventorySignatureObservedBeforeSnapshotValidation);
		TestFalse(TEXT("the final staged root has no inventory artifact"),
			IFileManager::Get().FileExists(*FPaths::Combine(
				Staged.GetValue().StagedTreeDir,
				TEXT("installed-tree-v1.json"))));
		TestFalse(TEXT("the final staged root has no signature artifact"),
			IFileManager::Get().FileExists(*FPaths::Combine(
				Staged.GetValue().StagedTreeDir,
				TEXT("installed-tree-v1.json.sig"))));
	}

	{
		TArray<FWoTReleaseStageSpecZipInput> Inputs =
			WoTReleaseStageSpecValidInputs(Tree);
		FWoTReleaseStageSpecZipInput* Inventory = InventoryInput(Inputs);
		if (!TestNotNull(TEXT("the valid fixture has an inventory entry"), Inventory))
		{
			return false;
		}
		Inventory->Bytes.Add(static_cast<uint8>(' '));
		Refuses(TEXT("an inventory with the wrong signed size"),
			WoTReleaseStageSpecBuildFixture(Tree, WoTReleaseStageSpecZip(Inputs)),
			EWoTBootstrapError::CorruptInstallation,
			TEXT("installed-tree inventory has the wrong signed size"));
	}
	{
		FWoTReleaseStageSpecTree WrongPlatformTree = Tree;
		FUTF8ToTCHAR Decoded(
			reinterpret_cast<const ANSICHAR*>(WrongPlatformTree.InventoryBytes.GetData()),
			WrongPlatformTree.InventoryBytes.Num());
		FString WrongPlatformJson = FString::ConstructFromPtrSize(
			Decoded.Get(), Decoded.Length());
#if PLATFORM_MAC
		const TCHAR* WrongPlatform = TEXT("Win64");
#else
		const TCHAR* WrongPlatform = TEXT("Mac");
#endif
		const int32 Replacements = WrongPlatformJson.ReplaceInline(
			*FString::Printf(TEXT("\"platform\":\"%s\""),
				WoTReleaseStageSpecPlatform),
			*FString::Printf(TEXT("\"platform\":\"%s\""), WrongPlatform),
			ESearchCase::CaseSensitive);
		if (!TestEqual(TEXT("the wrong-platform fixture changes one field"),
				Replacements, 1))
		{
			return false;
		}
		WrongPlatformTree.InventoryBytes =
			WoTReleaseStageSpecAscii(WrongPlatformJson);
		Refuses(TEXT("an inventory for another platform"),
			WoTReleaseStageSpecBuildFixture(WrongPlatformTree,
				WoTReleaseStageSpecZip(
					WoTReleaseStageSpecValidInputs(WrongPlatformTree))),
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("installed-tree inventory is for another platform"));
	}
	{
		FWoTReleaseStageSpecTree BadJsonTree = Tree;
		BadJsonTree.InventoryBytes = WoTReleaseStageSpecAscii(TEXT("{]"));
		Refuses(TEXT("a non-JSON inventory"),
			WoTReleaseStageSpecBuildFixture(BadJsonTree,
				WoTReleaseStageSpecZip(
					WoTReleaseStageSpecValidInputs(BadJsonTree))),
			EWoTBootstrapError::NonCanonicalJson, TEXT(""));
	}
	return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageCompleteCorruptionSpec,
	"WoT.UpdaterCore.Staging.CompleteCorruptionRetirement",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageCompleteCorruptionSpec::RunTest(const FString&)
{
    const FWoTReleaseStageSpecTree Tree;
    auto Fixture = WoTReleaseStageSpecBuildFixture(Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
    for (auto Fetch : {EWoTReleaseStageSpecFetch::CorruptComplete, EWoTReleaseStageSpecFetch::TruncateComplete, EWoTReleaseStageSpecFetch::Normal})
    {
        FWoTReleaseStageSpecInstall Install;
        FWoTReleaseStageSpecDownload Download(Fixture, Fetch);
        WoTSetStagingArchiveReadFailureForTesting(Fetch == EWoTReleaseStageSpecFetch::Normal);
        auto Refused = StageRelease(Fixture.Announcement, Fixture.Ring, Download, Install.Root);
        WoTSetStagingArchiveReadFailureForTesting(false);
        TestTrue(TEXT("corruption or failed read is refused"), Refused.IsError());
        TestFalse(TEXT("failed download is removed"), IFileManager::Get().DirectoryExists(*Install.Root.WorkDir));
        TestFalse(TEXT("failure publishes no ready"), IFileManager::Get().FileExists(*Install.Root.ReadyPath));
        FWoTReleaseStageSpecDownload Retry(Fixture);
        auto Retried = StageRelease(Fixture.Announcement, Fixture.Ring, Retry, Install.Root);
        TestFalse(TEXT("retry succeeds"), Retried.IsError());
        if (Retried.IsError()) AddError(Retried.GetError().ToString());
        TestTrue(TEXT("retry fetches from zero"), Retry.RequestedOffsets == TArray<uint64>{0});
    }
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageClosedTreeTamperSpec,
	"WoT.UpdaterCore.Staging.ClosedTreeTamper", WoTReleaseStageSpecFlags)

bool FWoTReleaseStageClosedTreeTamperSpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree;
	for (const TPair<EWoTReleaseStageSpecCheckpoint, const TCHAR*>& Case : {
		TPair<EWoTReleaseStageSpecCheckpoint, const TCHAR*>{
			EWoTReleaseStageSpecCheckpoint::AppendExtra, TEXT("real appended file")},
		TPair<EWoTReleaseStageSpecCheckpoint, const TCHAR*>{
			EWoTReleaseStageSpecCheckpoint::ReplaceWithSymlink,
			TEXT("real symlink substitution")},
		TPair<EWoTReleaseStageSpecCheckpoint, const TCHAR*>{
			EWoTReleaseStageSpecCheckpoint::ChmodFile, TEXT("real mode change")},
		TPair<EWoTReleaseStageSpecCheckpoint, const TCHAR*>{
			EWoTReleaseStageSpecCheckpoint::AppendExtraAfterEnumeration,
			TEXT("real post-enumeration file addition")},
	})
	{
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
		WoTReleaseStageSpecRefusedAndClean(*this, Case.Value, Fixture, Case.Key);
	}
	{
		// The child makes the historical cleanup order fail: unlinking it requires
		// widening its signed 0555 parent before recursion. Cleanup must use the
		// directory handle retained at creation, then prove the fresh root is gone.
		const FWoTReleaseStageSpecTree RestrictiveTree(true);
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			RestrictiveTree,
			WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(RestrictiveTree)));
		WoTReleaseStageSpecRefusedAndClean(*this,
			TEXT("post-enumeration tamper beneath a signed 0555 directory"),
			Fixture, EWoTReleaseStageSpecCheckpoint::AppendExtraAfterEnumeration);
	}
#if PLATFORM_MAC
	{
		// A 0000 empty directory cannot be reopened even for enumeration. The
		// pre-verification tamper forces refusal while cleanup still has the exact
		// directory handle that can widen it before recursive deletion.
		const FWoTReleaseStageSpecTree ZeroModeTree(true, 0000u, false);
		FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
			ZeroModeTree,
			WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(ZeroModeTree)));
		WoTReleaseStageSpecRefusedAndClean(*this,
			TEXT("real tamper beside a signed 0000 directory"), Fixture,
			EWoTReleaseStageSpecCheckpoint::AppendExtra);
	}
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageInventoryScannerSpec,
	"WoT.UpdaterCore.Staging.InventoryScannerCannotRefuse",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageInventoryScannerSpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree;
	FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
		Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
	FWoTReleaseStageSpecInstall Install(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()));
	FWoTReleaseStageSpecDownload Download(Fixture,
		EWoTReleaseStageSpecFetch::Normal,
		EWoTReleaseStageSpecCheckpoint::HoldInventoryScanner);
	TWoTBootstrapResult<FWoTStagedRelease> Staged = StageRelease(
		Fixture.Announcement, Fixture.Ring, Download, Install.Root);
	bool bOk = TestFalse(
		TEXT("a simulated inventory scanner cannot refuse staging"),
		Staged.IsError());
	if (Staged.IsError())
	{
		AddError(Staged.GetError().ToString());
	}
	bOk &= TestTrue(TEXT("the after-extraction hook ran"),
		Download.bAfterArchiveExtractedRan);
	bOk &= TestTrue(TEXT("the pre-verification hook ran"),
		Download.bVerificationCheckpointRan);
	bOk &= TestTrue(TEXT("the pre-snapshot-validation hook ran"),
		Download.bSnapshotValidationCheckpointRan);
	bOk &= TestFalse(TEXT("the inventory never existed after extraction"),
		Download.bInventoryObservedAfterExtraction);
	bOk &= TestFalse(TEXT("the inventory signature never existed after extraction"),
		Download.bInventorySignatureObservedAfterExtraction);
	bOk &= TestFalse(TEXT("the inventory never existed before verification"),
		Download.bInventoryObservedBeforeVerification);
	bOk &= TestFalse(TEXT("the inventory signature never existed before verification"),
		Download.bInventorySignatureObservedBeforeVerification);
	bOk &= TestFalse(TEXT("the inventory never existed before snapshot validation"),
		Download.bInventoryObservedBeforeSnapshotValidation);
	bOk &= TestFalse(
		TEXT("the inventory signature never existed before snapshot validation"),
		Download.bInventorySignatureObservedBeforeSnapshotValidation);
#if PLATFORM_WINDOWS
	bOk &= TestFalse(TEXT("there was no inventory for the scanner to open"),
		Download.bInventoryScannerOpened);
	bOk &= TestFalse(TEXT("there was no scanner handle to release"),
		Download.bInventoryScannerReleased);
#endif
	if (!Staged.IsError())
	{
		bOk &= TestFalse(TEXT("the final staged root has no inventory"),
			IFileManager::Get().FileExists(*FPaths::Combine(
				Staged.GetValue().StagedTreeDir,
				TEXT("installed-tree-v1.json"))));
		bOk &= TestFalse(TEXT("the final staged root has no inventory signature"),
			IFileManager::Get().FileExists(*FPaths::Combine(
				Staged.GetValue().StagedTreeDir,
				TEXT("installed-tree-v1.json.sig"))));
	}
	return bOk && !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageHappySpec,
	"WoT.UpdaterCore.Staging.HappyPath", WoTReleaseStageSpecFlags)

bool FWoTReleaseStageHappySpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree;
	const auto Fixture = WoTReleaseStageSpecBuildFixture(Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
	FWoTReleaseStageSpecInstall Install;
	FWoTReleaseStageSpecDownload Download(Fixture);
	auto Staged = StageRelease(Fixture.Announcement, Fixture.Ring, Download, Install.Root);
	if (Staged.IsError()) { AddError(Staged.GetError().ToString()); return false; }
	TestEqual(TEXT("published tree root"), Staged.GetValue().StagedTreeRoot, Tree.RootDigest);
	TestEqual(TEXT("fixed staged path"), Staged.GetValue().StagedTreeDir, Install.Root.StagedTreeDir);
	auto Ready = ReadWoTUpdateMarker(Install.Root, TEXT("ready"));
	TestTrue(TEXT("ready is published"), !Ready.IsError() && Ready.GetValue().IsSet());
	TestFalse(TEXT("process folder is retired"), IFileManager::Get().DirectoryExists(*Install.Root.WorkDir));
	TestFalse(TEXT("inventory is consumed before publication"), IFileManager::Get().FileExists(
		*FPaths::Combine(Install.Root.StagedTreeDir, TEXT("installed-tree-v1.json"))));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageStateRevalidationSpec,
	"WoT.UpdaterCore.Staging.PrepublicationStateRevalidation",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageStateRevalidationSpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree;
	const auto Fixture = WoTReleaseStageSpecBuildFixture(
		Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
	const FString FixtureRoot = FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()),
		TEXT("Automation/UpdateState/Prepublication"));
	for (const FWoTUpdateStateRecord Raised : {
		FWoTUpdateStateRecord{0, WoTReleaseStageSpecSequence + 1},
		FWoTUpdateStateRecord{Fixture.Ring.Epoch + 1, 0}})
	{
		FWoTReleaseStageSpecInstall Install(FixtureRoot);
		Install.CodeSealOverride.Reset();
		bool bRaisedAtSeal = false;
		FWoTReleaseStagingCodeSealVerifierOverrideForTesting RaiseState(
			[&](const FString&)
			{
				FWoTUpdateStateRecord State = Raised;
				bRaisedAtSeal = true;
				return MergeAndWriteWoTUpdateState(Install.Root, State);
			});
		FWoTReleaseStageSpecDownload Download(Fixture);
		const TWoTBootstrapResult<FWoTStagedRelease> Staged = StageRelease(
			Fixture.Announcement, Fixture.Ring, Download, Install.Root);
		TestTrue(TEXT("the state floor is raised after tree verification"),
			bRaisedAtSeal);
		TestTrue(TEXT("a newer observed floor refuses publication"),
			Staged.IsError()
			&& Staged.GetError().Code == EWoTBootstrapError::Rollback);
		TestFalse(TEXT("refusal publishes no ready marker"),
			IFileManager::Get().FileExists(*Install.Root.ReadyPath));
		TestFalse(TEXT("refusal publishes no staged tree"),
			IFileManager::Get().DirectoryExists(*Install.Root.StagedTreeDir));
		TestEqual(TEXT("refusal retires the process staging directory"),
			WoTReleaseStageSpecStagingChildren(Install.Root), 0);
	}
	return !HasAnyErrors();
}

#if PLATFORM_MAC
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageMacUnsignedSealSpec,
	"WoT.UpdaterCore.Staging.MacCodeSealRefusesUnsigned",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageMacUnsignedSealSpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree;
	const auto Fixture = WoTReleaseStageSpecBuildFixture(
		Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
	FWoTReleaseStageSpecInstall Install(FString(), true);
	FWoTReleaseStageSpecDownload Download(Fixture);
	auto Staged = StageRelease(
		Fixture.Announcement, Fixture.Ring, Download, Install.Root);
	TestTrue(TEXT("the real strict verifier refuses the unsigned bundle"),
		Staged.IsError()
		&& Staged.GetError().Code == EWoTBootstrapError::Crypto);
	TestFalse(TEXT("ready is never written"),
		IFileManager::Get().FileExists(*Install.Root.ReadyPath));
	TestFalse(TEXT("nothing is published"),
		IFileManager::Get().DirectoryExists(*Install.Root.StagedTreeDir));
	TestFalse(TEXT("the refused work tree is retired"),
		IFileManager::Get().DirectoryExists(*Install.Root.WorkDir));
	return !HasAnyErrors();
}
#endif


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageProbeCleanupSpec,
	"WoT.UpdaterCore.Staging.WritabilityProbeCleanup",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageProbeCleanupSpec::RunTest(const FString&)
{
	FWoTReleaseStageSpecInstall Install;
	const FString Orphan = FPaths::Combine(
		Install.Root.UpdateDir, TEXT(".probe-orphan"));
	if (!FFileHelper::SaveStringToFile(TEXT("orphan"), *Orphan))
	{
		AddError(TEXT("could not create the orphaned probe fixture"));
		return false;
	}
	FWoTBootstrapStatus Probed = ProbeUpdaterInstallWritable(Install.Root);
	if (!TestFalse(TEXT("the next writability probe succeeds"), Probed.IsError()))
	{
		if (Probed.IsError()) AddError(Probed.GetError().ToString());
		return false;
	}
	TestFalse(TEXT("the next writability probe removes an orphaned probe"),
		IFileManager::Get().FileExists(*Orphan));
	TArray<FString> Remaining;
	IFileManager::Get().FindFiles(Remaining,
		*FPaths::Combine(Install.Root.UpdateDir, TEXT(".probe-*")), true, false);
	TestEqual(TEXT("the successful probe leaves no probe files"),
		Remaining.Num(), 0);
	return !HasAnyErrors();
}

#if PLATFORM_WINDOWS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageLiveTreeNonMutatingSpec,
	"WoT.UpdaterCore.Staging.LiveTreeAccessCheckNeverMutates",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageLiveTreeNonMutatingSpec::RunTest(const FString&)
{
	FWoTReleaseStageSpecInstall Install;
	const FString LiveTree = FPaths::Combine(
		Install.Root.InstallRoot, Install.Root.LiveLeaf);
	const FString Sentinel = FPaths::Combine(LiveTree, TEXT("signed-payload.bin"));
	const FString Nested = FPaths::Combine(LiveTree, TEXT("nested"));
	const FString NestedPayload = FPaths::Combine(Nested, TEXT("payload.bin"));
	const FString NestedSibling = FPaths::Combine(Nested, TEXT("sibling.bin"));
	const TArray<uint8> SentinelBytes = {0u, 1u, 2u, 3u, 0xffu};
	IFileManager::Get().MakeDirectory(*Nested, true);
	if (!FFileHelper::SaveArrayToFile(SentinelBytes, *Sentinel)
		|| !FFileHelper::SaveArrayToFile(SentinelBytes, *NestedPayload)
		|| !FFileHelper::SaveArrayToFile(SentinelBytes, *NestedSibling))
	{
		AddError(TEXT("could not create the live-tree snapshot fixture"));
		return false;
	}
	auto Snapshot = [this, &LiveTree](TArray<FString>& Out)
	{
		FString Failure;
		if (!WoTReleaseStageSpecFullSnapshot(LiveTree, Out, Failure))
		{
			AddError(Failure);
			return false;
		}
		return true;
	};
	auto WithDeny = [this, &Snapshot](const FString& Path, FStringView Deny,
		TFunctionRef<bool()> Body)
	{
		TArray<FString> BeforeDeny;
		TArray<FString> AfterRestore;
		if (!Snapshot(BeforeDeny)) return false;
		FString Failure;
		if (!WoTReleaseStageSpecIcacls(Path, Deny, Failure))
		{
			AddError(Failure);
			return false;
		}
		bool bRestored = false;
		ON_SCOPE_EXIT
		{
			if (!bRestored)
			{
				FString Ignored;
				(void)WoTReleaseStageSpecIcacls(
					Path, TEXTVIEW("/remove:d *S-1-1-0"), Ignored);
			}
		};
		const bool bBody = Body();
		bRestored = WoTReleaseStageSpecIcacls(
			Path, TEXTVIEW("/remove:d *S-1-1-0"), Failure);
		if (!bRestored) AddError(Failure);
		if (bRestored && Snapshot(AfterRestore))
		{
			TestTrue(TEXT("ACL-scoped access probe and restoration leave the full live-tree snapshot unchanged"),
				AfterRestore == BeforeDeny);
		}
		return bBody && bRestored && AfterRestore == BeforeDeny;
	};

	TArray<FString> Before;
	TArray<FString> After;
	if (!Snapshot(Before)) return false;
	FWoTBootstrapStatus Success = ProbeUpdaterInstallWritable(Install.Root);
	TestFalse(TEXT("the non-mutating access check succeeds"), Success.IsError());
	if (!Snapshot(After)) return false;
	TestTrue(TEXT("success leaves the full live-tree snapshot unchanged"),
		After == Before);

	Before.Reset();
	After.Reset();
	if (!Snapshot(Before)) return false;
	int32 BetweenEntryHookCalls = 0;
	FWoTBootstrapStatus Interrupted = [&]()
	{
		WoTSetNativeDirectoryMutationWalkHookForTesting(
			[&BetweenEntryHookCalls]()
		{
			++BetweenEntryHookCalls;
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::ServiceUnavailable,
				TEXT("scripted walker-only interruption between live-tree entries")));
		});
		ON_SCOPE_EXIT
		{
			WoTSetNativeDirectoryMutationWalkHookForTesting({});
		};
		return ProbeUpdaterInstallWritable(Install.Root);
	}();
	TestTrue(TEXT("the interruption is observed between walked entries"),
		Interrupted.IsError() && BetweenEntryHookCalls >= 1);
	if (Interrupted.IsError())
	{
		TestTrue(TEXT("the walker-only interruption is the propagated failure"),
			Interrupted.GetError().Message.Contains(
				TEXT("walker-only interruption"), ESearchCase::CaseSensitive));
	}
	if (!Snapshot(After)) return false;
	TestTrue(TEXT("in-walk interruption leaves the full live-tree snapshot unchanged"),
		After == Before);

	// Staging never creates inside Game. Denying only AddFile/WriteData there
	// must not reject an otherwise renameable and recursively deletable tree.
	WithDeny(LiveTree, TEXTVIEW("/deny *S-1-1-0:(WD)"), [&]()
	{
		FWoTBootstrapStatus Probed = ProbeUpdaterInstallWritable(Install.Root);
		TestFalse(TEXT("create-file denial inside Game does not block activation"),
			Probed.IsError());
		return !Probed.IsError();
	});

	// Cleanup needs either DELETE on every entry or FILE_DELETE_CHILD on its
	// held parent. Deny both halves for one nested payload.
	WithDeny(Nested, TEXTVIEW("/deny *S-1-1-0:(DC)"), [&]()
	{
		return WithDeny(NestedPayload, TEXTVIEW("/deny *S-1-1-0:(D)"), [&]()
		{
			FWoTBootstrapStatus Refused = ProbeUpdaterInstallWritable(Install.Root);
			TestTrue(TEXT("nested entry without DELETE or parent delete-child is refused"),
				Refused.IsError());
			return Refused.IsError();
		});
	});

	WithDeny(Install.Root.InstallRoot, TEXTVIEW("/deny *S-1-1-0:(AD)"), [&]()
	{
		FWoTBootstrapStatus Refused = ProbeUpdaterInstallWritable(Install.Root);
		TestTrue(TEXT("install root without FILE_ADD_SUBDIRECTORY is refused"),
			Refused.IsError());
		return Refused.IsError();
	});

	WithDeny(LiveTree,
		TEXTVIEW("/deny *S-1-1-0:(OI)(CI)(WD,AD,WEA,WA,D,DC)"), [&]()
	{
		FWoTBootstrapStatus Refused = ProbeUpdaterInstallWritable(Install.Root);
		TestTrue(TEXT("the Win64 read-only funnel ACL still maps to not writable"),
			Refused.IsError());
		return Refused.IsError();
	});
	return !HasAnyErrors();
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageMetadataPreservationSpec,
	"WoT.UpdaterCore.Staging.PreservesLinksModesAndNoXattrs",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageMetadataPreservationSpec::RunTest(const FString&)
{
	const FWoTReleaseStageSpecTree Tree(false, 0755u, true, true);
	FWoTReleaseStageSpecFixture Fixture = WoTReleaseStageSpecBuildFixture(
		Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
	FWoTReleaseStageSpecInstall Install;
	FWoTReleaseStageSpecDownload Download(Fixture);
	TWoTBootstrapResult<FWoTStagedRelease> Staged = StageRelease(
		Fixture.Announcement, Fixture.Ring, Download, Install.Root);
#if PLATFORM_WINDOWS
	TestTrue(TEXT("Win64 explicitly skips unsupported archive symbolic links"),
		Staged.IsError());
	TestEqual(TEXT("the skipped Win64 symbolic-link tree is cleaned"),
		WoTReleaseStageSpecStagingChildren(Install.Root), 0);
#else
	if (!TestFalse(TEXT("the metadata-preserving tree stages"), Staged.IsError()))
	{
		if (Staged.IsError()) AddError(Staged.GetError().ToString());
		return false;
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> Root =
		OpenNativeDirectoryNoFollow(Staged.GetValue().StagedTreeDir);
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> MacOs = Root.IsError()
		? TWoTBootstrapResult<FWoTNativeDirectoryHandle>::Error(
			MoveTemp(Root).GetError())
		: OpenNativeDirectoryAtNoFollow(Root.GetValue(), TEXT("Contents/MacOS"));
	if (!TestFalse(TEXT("the staged link parent reopens"), MacOs.IsError()))
	{
		if (MacOs.IsError()) AddError(MacOs.GetError().ToString());
		return false;
	}
	TWoTBootstrapResult<FString> Target = ReadNativeSymbolicLinkAtNoFollow(
		MacOs.GetValue(), TEXT("WoTLink"));
	TWoTBootstrapResult<uint32> LinkMode = QueryNativeSymbolicLinkModeAtNoFollow(
		MacOs.GetValue(), TEXT("WoTLink"));
	TestFalse(TEXT("the staged symbolic link reopens without following"),
		Target.IsError() || LinkMode.IsError());
	if (!Target.IsError())
		TestEqual(TEXT("the staged symbolic-link target is exact"),
			Target.GetValue(), FString(TEXT("WoT")));
	if (!LinkMode.IsError())
		TestEqual(TEXT("the staged symbolic-link mode is exact"),
			LinkMode.GetValue(), uint32(0777u));
	TWoTBootstrapResult<FWoTNativeFileHandle> Main =
		OpenNativeFileAtReadNoFollow(Root.GetValue(), TEXT("Contents/MacOS/WoT"));
	TWoTBootstrapResult<uint32> MainMode = Main.IsError()
		? TWoTBootstrapResult<uint32>::Error(MoveTemp(Main).GetError())
		: QueryNativeMode(Main.GetValue());
	TestFalse(TEXT("the staged regular-file mode reopens"), MainMode.IsError());
	if (!MainMode.IsError())
		TestEqual(TEXT("the staged regular-file mode is exact"),
			MainMode.GetValue(), uint32(0644u));
	for (const FString& Relative : {
		FString(TEXT("Contents/MacOS/WoT")),
		FString(TEXT("Contents/MacOS/WoTLink")),
		FString(TEXT("Contents/MacOS/WoTUpdater"))})
	{
		const FString Path = FPaths::Combine(
			Staged.GetValue().StagedTreeDir, Relative);
		WoTReleaseStageSpecHasOnlyAllowedMacXattrs(*this, Path, Relative);
	}
	WoTReleaseStageSpecHasNoAppleDouble(*this, Staged.GetValue().StagedTreeDir);
#endif
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageYieldSpec,
	"WoT.UpdaterCore.Staging.HashYield",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageYieldSpec::RunTest(const FString&)
{
	TArray<uint8> Bytes;
	Bytes.SetNumZeroed(3 * 1024 * 1024 + 17);
	int32 YieldCount = 0;
	TWoTBootstrapResult<FWoTSha256> Yielded =
		WoTStagingHashWithYieldForTesting(Bytes, [&YieldCount]()
		{
			++YieldCount;
			return FWoTBootstrapStatus::Success();
		});
	TestEqual(TEXT("four hash chunks yield exactly between chunks"),
		YieldCount, 3);
	if (TestFalse(TEXT("chunked staging hash succeeds"), Yielded.IsError()))
	{
		TestEqual(TEXT("chunked staging hash equals the one-shot digest"),
			Yielded.GetValue(), Sha256(Bytes));
	}
	int32 CancellationChecks = 0;
	TWoTBootstrapResult<FWoTSha256> Cancelled =
		WoTStagingHashWithYieldForTesting(Bytes, [&CancellationChecks]()
		{
			++CancellationChecks;
			return CancellationChecks >= 2
				? FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::ServiceUnavailable,
					TEXT("scripted hash cancellation")))
				: FWoTBootstrapStatus::Success();
		});
	TestTrue(TEXT("chunked hashing stops on cooperative cancellation"),
		Cancelled.IsError());
	TestEqual(TEXT("hashing observes cancellation at the exact chunk boundary"),
		CancellationChecks, 2);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTReleaseStageCancellationPhasesSpec,
	"WoT.UpdaterCore.Staging.CooperativeCancellationPhases",
	WoTReleaseStageSpecFlags)

bool FWoTReleaseStageCancellationPhasesSpec::RunTest(const FString&)
{
    const FWoTReleaseStageSpecTree Tree;
    auto Fixture = WoTReleaseStageSpecBuildFixture(Tree,
        WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
    for (const bool bDuringExtraction : {false, true})
    {
        FWoTReleaseStageSpecInstall Install;
        FWoTReleaseStageSpecDownload Download(Fixture);
        bool bCancelled = false;
        auto Staged = StageRelease(Fixture.Announcement, Fixture.Ring, Download, Install.Root,
            [&]()
            {
                bCancelled |= bDuringExtraction ? Download.bAfterArchiveExtractedRan : Download.bArchiveVerificationStarted;
                return bCancelled ? FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
                    EWoTBootstrapError::ServiceUnavailable, TEXT("cancelled fixture"))) : FWoTBootstrapStatus::Success();
            });
        TestTrue(TEXT("cancellation is returned"), Staged.IsError() && bCancelled);
        TestFalse(TEXT("cancellation removes our work folder"), IFileManager::Get().DirectoryExists(*Install.Root.WorkDir));
        TestFalse(TEXT("cancellation never publishes ready"), IFileManager::Get().FileExists(*Install.Root.ReadyPath));
        FWoTReleaseStageSpecDownload Retry(Fixture);
        auto Retried = StageRelease(Fixture.Announcement, Fixture.Ring, Retry, Install.Root);
        TestFalse(TEXT("a fresh retry succeeds"), Retried.IsError());
        if (Retried.IsError()) AddError(Retried.GetError().ToString());
        TestTrue(TEXT("retry downloads the full object from zero"), Retry.RequestedOffsets == TArray<uint64>{0});
    }
    return !HasAnyErrors();
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateMarkerGrammarSpec,
    "WoT.Updater.Workspace.MarkerGrammar", WoTReleaseStageSpecFlags)
bool FWoTUpdateMarkerGrammarSpec::RunTest(const FString&)
{
    for (const TCHAR* Text : {TEXT("1\n"), TEXT("9223372036854775807\n")})
        TestFalse(TEXT("positive bounded sequence parses"), ParseWoTUpdateMarker(WoTReleaseStageSpecAscii(Text), false).IsError());
    for (const TCHAR* Text : {TEXT("0\n"), TEXT("01\n"), TEXT("-1\n"), TEXT("9223372036854775808\n"), TEXT("1\r\n"), TEXT("1\nextra"), TEXT("1"), TEXT(" 1\n")})
        TestTrue(TEXT("noncanonical marker refused"), ParseWoTUpdateMarker(WoTReleaseStageSpecAscii(Text), false).IsError());
    for (const TCHAR* Text : {TEXT("2 pending\n"), TEXT("2 started\n")})
        TestFalse(TEXT("trial state parses"), ParseWoTUpdateMarker(WoTReleaseStageSpecAscii(Text), true).IsError());
    TestTrue(TEXT("unknown trial state refused"), ParseWoTUpdateMarker(WoTReleaseStageSpecAscii(TEXT("2 ready\n")), true).IsError());
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTrialLifecycleSpec,
    "WoT.Updater.Workspace.TrialLifecycle", WoTReleaseStageSpecFlags)
bool FWoTUpdateTrialLifecycleSpec::RunTest(const FString&)
{
    FWoTReleaseStageSpecInstall Install;
    IFileManager::Get().MakeDirectory(*Install.Root.PreviousTreeDir, true);
    TestFalse(TEXT("pending written"), WriteWoTUpdateMarker(Install.Root, TEXT("trial"), {71, EWoTUpdateTrialState::Pending}).IsError());
    TestFalse(TEXT("game marks started"), WoTMarkUpdateTrialStarted(Install.Root).IsError());
    auto Trial = ReadWoTUpdateMarker(Install.Root, TEXT("trial"), true);
    TestTrue(TEXT("started is durable"), !Trial.IsError() && Trial.GetValue().IsSet() && Trial.GetValue()->State == EWoTUpdateTrialState::Started);
    TestFalse(TEXT("other sequence handoff is harmless"), WoTCompleteUpdateTrial(Install.Root, 70).IsError());
    TestTrue(TEXT("other sequence retains rollback"), IFileManager::Get().DirectoryExists(*Install.Root.PreviousTreeDir));
#if PLATFORM_WINDOWS
    HANDLE HeldTrial = ::CreateFileW(*Install.Root.TrialPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (TestTrue(TEXT("trial held against deletion"), HeldTrial != INVALID_HANDLE_VALUE))
    {
        TestTrue(TEXT("handoff reports trial deletion failure"), WoTCompleteUpdateTrial(Install.Root, 71).IsError());
        TestTrue(TEXT("failed trial deletion retains previous tree"), IFileManager::Get().DirectoryExists(*Install.Root.PreviousTreeDir));
        ::CloseHandle(HeldTrial);
    }
#endif
    TestFalse(TEXT("matching handoff completes trial"), WoTCompleteUpdateTrial(Install.Root, 71).IsError());
    TestFalse(TEXT("trial retired"), IFileManager::Get().FileExists(*Install.Root.TrialPath));
    TestFalse(TEXT("previous tree retired only after trial"), IFileManager::Get().DirectoryExists(*Install.Root.PreviousTreeDir));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateTornMarkerSpec,
    "WoT.Updater.Workspace.TornMarkerIsStale", WoTReleaseStageSpecFlags)
bool FWoTUpdateTornMarkerSpec::RunTest(const FString&)
{
    // A full disk, crash or power loss mid-write must never block later starts:
    // a malformed marker is stale and is dropped, and writes never tear.
    FWoTReleaseStageSpecInstall Install;
    IFileManager::Get().MakeDirectory(*Install.Root.UpdateDir, true);
    for (const TCHAR* Torn : {TEXT(""), TEXT("71 pen"), TEXT("71 started")})
    {
        FFileHelper::SaveStringToFile(Torn, *Install.Root.TrialPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        TestFalse(*FString::Printf(TEXT("torn trial '%s' never blocks startup"), Torn),
            WoTMarkUpdateTrialStarted(Install.Root).IsError());
        TestFalse(*FString::Printf(TEXT("torn trial '%s' is dropped"), Torn),
            IFileManager::Get().FileExists(*Install.Root.TrialPath));
    }
    const FString ReadyPath = FPaths::Combine(Install.Root.UpdateDir, TEXT("ready"));
    FFileHelper::SaveStringToFile(TEXT("7"), *ReadyPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    auto Ready = ReadWoTUpdateMarker(Install.Root, TEXT("ready"));
    TestTrue(TEXT("torn ready reads as absent"), !Ready.IsError() && !Ready.GetValue().IsSet());
    TestFalse(TEXT("torn ready is dropped"), IFileManager::Get().FileExists(*ReadyPath));

    const FString TempPath = FPaths::Combine(Install.Root.UpdateDir, TEXT("trial.tmp"));
    FFileHelper::SaveStringToFile(TEXT("left by a crash"), *TempPath);
    TestFalse(TEXT("a stale temporary doesn't block a write"),
        WriteWoTUpdateMarker(Install.Root, TEXT("trial"), {71, EWoTUpdateTrialState::Pending}).IsError());
    TestFalse(TEXT("a shorter rewrite replaces the marker whole"),
        WriteWoTUpdateMarker(Install.Root, TEXT("trial"), {7, EWoTUpdateTrialState::Started}).IsError());
    FString Written;
    FFileHelper::LoadFileToString(Written, *Install.Root.TrialPath);
    TestEqual(TEXT("marker holds exactly the new content"), Written, FString(TEXT("7 started\n")));
    TestFalse(TEXT("no temporary is left behind"), IFileManager::Get().FileExists(*TempPath));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateRollbackNoticeSpec,
    "WoT.Updater.Workspace.RollbackNotice", WoTReleaseStageSpecFlags)
bool FWoTUpdateRollbackNoticeSpec::RunTest(const FString&)
{
    FWoTReleaseStageSpecInstall Install;
    const FString NoticeRoot = FPaths::Combine(Install.UserStateRoot, TEXT("Preferences"));
    WoTSetUpdateFailureNoticeDirectoryForTesting(NoticeRoot);
    ON_SCOPE_EXIT { WoTSetUpdateFailureNoticeDirectoryForTesting(FString()); };
    IFileManager::Get().MakeDirectory(*Install.Root.UpdateDir, true);
    TestFalse(TEXT("rollback marker written"), WriteWoTUpdateMarker(Install.Root, TEXT("rolled-back"), {71}).IsError());
    FFileHelper::SaveStringToFile(TEXT("blocks notice directory"), *NoticeRoot);
    TestTrue(TEXT("notice persistence failure is returned"), WoTConsumeUpdateRollback(Install.Root, 70).IsError());
    TestTrue(TEXT("notice persistence failure retains rollback marker"), IFileManager::Get().FileExists(*Install.Root.RolledBackPath));
    IFileManager::Get().Delete(*NoticeRoot);
    TestFalse(TEXT("shared startup records the failure notice"), WoTConsumeUpdateRollback(Install.Root, 70).IsError());
    TestFalse(TEXT("recorded rollback marker retired"), IFileManager::Get().FileExists(*Install.Root.RolledBackPath));
    auto Failed = ReadUpdateFailedSequence(Install.Root);
    TestTrue(TEXT("pending notice retains failed sequence"), !Failed.IsError() && Failed.GetValue() == 71);
    FString NoticeDirectory;
    TestTrue(TEXT("notice path resolves"), WoTResolveUpdateFailureNoticeDirectory(Install.Root.UserStateRoot, NoticeDirectory));
    FFileHelper::SaveStringToFile(TEXT("WUF1\nfailed=71\nstate=seen\n"),
        *FPaths::Combine(NoticeDirectory, TEXT("WoTUpdateFailedNotice")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    Failed = ReadUpdateFailedSequence(Install.Root);
    TestTrue(TEXT("acknowledged notice retains failed sequence"), !Failed.IsError() && Failed.GetValue() == 71);
    TestFalse(TEXT("older announcement preserves failure"), ClearUpdateFailedSequenceForNewerRelease(Install.Root, 70).IsError());
    TestFalse(TEXT("equal announcement preserves failure"), ClearUpdateFailedSequenceForNewerRelease(Install.Root, 71).IsError());
    Failed = ReadUpdateFailedSequence(Install.Root);
    TestTrue(TEXT("only strictly newer can clear"), !Failed.IsError() && Failed.GetValue() == 71);
    TestFalse(TEXT("newer clears failure"), ClearUpdateFailedSequenceForNewerRelease(Install.Root, 72).IsError());
    Failed = ReadUpdateFailedSequence(Install.Root);
    TestTrue(TEXT("newer release re-enables staging"), !Failed.IsError() && Failed.GetValue() == 0);
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdatePublicationSpec,
    "WoT.Updater.ReleaseStaging.ReadyLastAndRetry", WoTReleaseStageSpecFlags)
bool FWoTUpdatePublicationSpec::RunTest(const FString&)
{
    const FWoTReleaseStageSpecTree Tree;
    auto Fixture = WoTReleaseStageSpecBuildFixture(Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
    FWoTReleaseStageSpecInstall Install;
    const FString Stale = FPaths::Combine(Install.Root.UpdateDir, TEXT("staging-99999998"));
    const FString Recent = FPaths::Combine(Install.Root.UpdateDir, TEXT("staging-99999999"));
    IFileManager::Get().MakeDirectory(*Stale, true);
    IFileManager::Get().MakeDirectory(*Recent, true);
#if PLATFORM_WINDOWS
    // UE's Windows SetTimeStamp opens only files, without BACKUP_SEMANTICS.
    HANDLE StaleDirectory = ::CreateFileW(*Stale, FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (!TestTrue(TEXT("stale fixture directory opens"), StaleDirectory != INVALID_HANDLE_VALUE)) return false;
    FILETIME OldTime;
    ::GetSystemTimeAsFileTime(&OldTime);
    ULARGE_INTEGER Ticks;
    Ticks.LowPart = OldTime.dwLowDateTime;
    Ticks.HighPart = OldTime.dwHighDateTime;
    Ticks.QuadPart -= 25ull * 60 * 60 * 10000000;
    OldTime.dwLowDateTime = Ticks.LowPart;
    OldTime.dwHighDateTime = Ticks.HighPart;
    TestTrue(TEXT("fixture directory timestamp set"), ::SetFileTime(StaleDirectory, nullptr, nullptr, &OldTime) != 0);
    ::CloseHandle(StaleDirectory);

#elif PLATFORM_MAC
	struct stat StaleInfo;
	const FTCHARToUTF8 StaleUtf8(*Stale);
	TestTrue(TEXT("stale fixture directory stat succeeds"),
		stat(StaleUtf8.Get(), &StaleInfo) == 0);
	struct utimbuf OldTimes;
	OldTimes.actime = StaleInfo.st_atime;
	OldTimes.modtime = StaleInfo.st_mtime - 25 * 60 * 60;
	TestTrue(TEXT("stale fixture directory timestamp set"),
		utime(StaleUtf8.Get(), &OldTimes) == 0);
#else
    IFileManager::Get().SetTimeStamp(*Stale, FDateTime::UtcNow() - FTimespan::FromHours(25));
#endif
    FWoTReleaseStageSpecDownload Interrupted(Fixture, EWoTReleaseStageSpecFetch::InterruptFresh);
    TestTrue(TEXT("partial download fails"), StageRelease(Fixture.Announcement, Fixture.Ring, Interrupted, Install.Root).IsError());
    TestFalse(TEXT("partial download is deleted"), IFileManager::Get().DirectoryExists(*Install.Root.WorkDir));
    TestFalse(TEXT("partial download has no ready"), IFileManager::Get().FileExists(*Install.Root.ReadyPath));
    TestFalse(TEXT("old crashed staging folder pruned"), IFileManager::Get().DirectoryExists(*Stale));
    TestTrue(TEXT("recent concurrent staging folder retained"), IFileManager::Get().DirectoryExists(*Recent));
    IFileManager::Get().MakeDirectory(*Install.Root.StagedTreeDir, true);
    FWoTReleaseStageSpecDownload Retry(Fixture);
    auto Staged = StageRelease(Fixture.Announcement, Fixture.Ring, Retry, Install.Root);
    TestFalse(TEXT("incomplete publication is replaced"), Staged.IsError());
    if (Staged.IsError()) AddError(Staged.GetError().ToString());
    TestTrue(TEXT("retry fetches from zero"), Retry.RequestedOffsets == TArray<uint64>{0});
    auto Ready = ReadWoTUpdateMarker(Install.Root, TEXT("ready"));
    TestTrue(TEXT("ready is published last"), !Ready.IsError() && Ready.GetValue().IsSet() && Ready.GetValue()->Sequence == 71);
    FWoTReleaseStageSpecDownload Other(Fixture);
    TestFalse(TEXT("another copy preserves published update"), StageRelease(Fixture.Announcement, Fixture.Ring, Other, Install.Root).IsError());
    TestTrue(TEXT("winner prevents another archive download"), Other.RequestedOffsets.IsEmpty());
    TestFalse(TEXT("own staging folder removed"), IFileManager::Get().DirectoryExists(*Install.Root.WorkDir));
    return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTUpdateFailedSequenceSpec,
    "WoT.Updater.ReleaseStaging.FailedSequenceEquality", WoTReleaseStageSpecFlags)
bool FWoTUpdateFailedSequenceSpec::RunTest(const FString&)
{
    const FWoTReleaseStageSpecTree Tree;
    auto Fixture = WoTReleaseStageSpecBuildFixture(Tree, WoTReleaseStageSpecZip(WoTReleaseStageSpecValidInputs(Tree)));
    for (const uint64 FailedSequence : {70ull, 71ull, 72ull})
    {
        FWoTReleaseStageSpecInstall Install;
        WoTSetUpdateFailureNoticeDirectoryForTesting(FPaths::Combine(Install.UserStateRoot, TEXT("Preferences")));
        ON_SCOPE_EXIT { WoTSetUpdateFailureNoticeDirectoryForTesting(FString()); };
        TestFalse(TEXT("failure recorded"), RecordUpdateFailedNotice(Install.Root, FailedSequence, 69).IsError());
        FWoTReleaseStageSpecDownload Download(Fixture);
        auto Staged = StageRelease(Fixture.Announcement, Fixture.Ring, Download, Install.Root);
        TestEqual(TEXT("only equal failed sequence is refused"), Staged.IsError(), FailedSequence == 71);
        if (FailedSequence != 71 && Staged.IsError()) AddError(Staged.GetError().ToString());
        TestEqual(TEXT("equal failed sequence never downloads"), Download.RequestedOffsets.IsEmpty(), FailedSequence == 71);
        auto Failed = ReadUpdateFailedSequence(Install.Root);
        TestTrue(TEXT("only authenticated newer release clears failure"), !Failed.IsError()
            && Failed.GetValue() == (FailedSequence < 71 ? 0 : FailedSequence));
    }
    return !HasAnyErrors();
}

#endif // WITH_AUTOMATION_TESTS
