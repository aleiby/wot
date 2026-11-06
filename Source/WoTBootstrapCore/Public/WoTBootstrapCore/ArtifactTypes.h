// The sole definition site for the stable bootstrap protocol constants and for
// every public artifact and process-identity struct.
//
// Ownership rule: aliases, version-suffixed alternatives for unversioned
// formats, and parallel game/updater/service definitions are forbidden. Later
// tasks extend THIS header before their first use of a completed struct — the
// canonical names are FWoTKeyRing, FWoTReleaseManifestV2, FWoTReleaseIdentity,
// FWoTInstalledTreeManifest, FWoTComponentIdentity, and
// FWoTProcessStartIdentity. Every other public header in this module
// includes this one rather than restating a type.
#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "WoTBootstrapCore/Error.h"

namespace WoT::Bootstrap
{
/** Wire/format versions. Bumping one is a compatibility decision, never a cleanup. */
inline constexpr uint32 GateProtocol = 1;
inline constexpr uint32 CommitServiceProtocol = 1;
inline constexpr uint32 UpdaterProtocol = 1;
inline constexpr uint32 CacheContractVersion = 3;
inline constexpr uint32 ReleaseManifestVersion = 2;

/** Signature domain separators. Distinct domains cannot be substituted for each other. */
inline constexpr TCHAR AnnouncementDomain[] = TEXT("com.aleiby.wot.release.v1");
inline constexpr TCHAR CacheContractSignatureContext[] = TEXT("com.aleiby.wot.cache-contract.v3");

/**
 * The one pinned retail installer. Identity is compiled in, not configured: the
 * name, byte size, and SHA-256 are the whole acceptance test for the
 * player-supplied adjacent file.
 */
inline constexpr TCHAR InstallerName[] = TEXT("setup_the_wheel_of_time_333b_(54846).exe");
inline constexpr uint64 InstallerSize = 1029509080;
inline constexpr TCHAR InstallerSha256[] = TEXT("0ad6474ebc4b5d367b27197fd856733beb45179dc6d6c094641521b16b63f8bb");
}

/**
 * The closed set of signature domains.
 *
 * Domain separation is why a signature over one artifact cannot be replayed as a
 * signature over another: the context is mixed into the signed input, so a valid
 * announcement signature presented as a release-manifest signature fails. Values
 * are part of the signed bytes and therefore never renumbered.
 */
enum class EWoTSignatureContext : uint8
{
	AnnouncementV1,
	ComponentIdentityV1,
	ReleaseKeyRingV1,
	FullPackageV1,
	RecoveryDomainV1,
	ReleaseManifestV2,
	InstalledTreeV1,
	CacheContractV3,
};

/** Stable ASCII spelling of a signature domain, for evidence and diagnostics. */
WOTBOOTSTRAPCORE_API const TCHAR* LexToString(EWoTSignatureContext Context);

namespace WoT::Bootstrap::Detail
{
/** Lower-case hex of a fixed-width field. Shared by every digest and key type. */
WOTBOOTSTRAPCORE_API FString FixedBytesToHex(const uint8* Bytes, int32 Count);

/**
 * Parse exactly `Count` bytes of lower-case hex.
 *
 * Upper case is refused rather than accepted: artifacts have one spelling, and a
 * field that round-trips differently than it arrived cannot be re-signed.
 */
WOTBOOTSTRAPCORE_API FWoTBootstrapStatus FixedBytesFromHex(
	FStringView Hex, uint8* OutBytes, int32 Count);
}

/** A SHA-256 digest. Fixed width by construction: a digest is never a loose blob. */
struct FWoTSha256
{
	static constexpr int32 ByteCount = 32;

	uint8 Bytes[ByteCount] = {};

	const uint8* GetData() const { return Bytes; }
	int32 Num() const { return ByteCount; }

	/** Lower-case, 64 characters — the only spelling that appears in artifacts. */
	WOTBOOTSTRAPCORE_API FString ToHex() const;

	/** Requires exactly 64 lower-case hex characters; upper case is not canonical. */
	WOTBOOTSTRAPCORE_API static TWoTBootstrapResult<FWoTSha256> FromHex(FStringView Hex);

	WOTBOOTSTRAPCORE_API bool operator==(const FWoTSha256& Other) const;
	bool operator!=(const FWoTSha256& Other) const { return !(*this == Other); }
};

/** A raw Ed25519 public key. Never a PEM, DER, or certificate — 32 bytes exactly. */
struct FWoTEd25519PublicKey
{
	static constexpr int32 ByteCount = 32;

	uint8 Bytes[ByteCount] = {};

	const uint8* GetData() const { return Bytes; }
	int32 Num() const { return ByteCount; }

	WOTBOOTSTRAPCORE_API FString ToHex() const;
	WOTBOOTSTRAPCORE_API static TWoTBootstrapResult<FWoTEd25519PublicKey> FromHex(FStringView Hex);

	WOTBOOTSTRAPCORE_API bool operator==(const FWoTEd25519PublicKey& Other) const;
	bool operator!=(const FWoTEd25519PublicKey& Other) const { return !(*this == Other); }
};

/** A raw Ed25519 signature: 64 bytes, R || S. */
struct FWoTEd25519Signature
{
	static constexpr int32 ByteCount = 64;

	uint8 Bytes[ByteCount] = {};

	const uint8* GetData() const { return Bytes; }
	int32 Num() const { return ByteCount; }

	WOTBOOTSTRAPCORE_API FString ToHex() const;
	WOTBOOTSTRAPCORE_API static TWoTBootstrapResult<FWoTEd25519Signature> FromHex(FStringView Hex);

	WOTBOOTSTRAPCORE_API bool operator==(const FWoTEd25519Signature& Other) const;
	bool operator!=(const FWoTEd25519Signature& Other) const { return !(*this == Other); }
};

/**
 * One release-signing key and the release sequence range it is trusted for.
 *
 * The range is what makes key rotation auditable rather than retroactive: a key
 * retired at sequence N cannot be used to sign release N+1 even if its bytes
 * remain in the ring, and it still validates the releases it did sign.
 */
struct FWoTReleaseKey
{
	FString KeyId;
	FWoTEd25519PublicKey PublicKey;
	uint64 FirstSequence = 0;
	uint64 LastSequence = 0;
};

/**
 * An ordered position in one announcement domain.
 *
 * Ordering is per-domain and lexicographic on (Domain, Value). "Newer" is never
 * inferred from a build commit or a timestamp: only a strictly greater Value
 * inside the SAME domain is newer, and a different domain is never adopted from
 * the network — only an offline-root full-package transition can move domains.
 */
struct FWoTDomainSequence
{
	FString Domain;
	uint64 Value = 0;
};

/** A referenced object: where it is, how big it is, and what it must hash to. */
struct FWoTSignedObjectRef
{
	FString Url;
	uint64 Size = 0;
	FWoTSha256 Sha256;
};

/**
 * The permanently frozen launch-time envelope.
 *
 * This is the only thing a client fetches before it knows anything, so its field
 * set never grows: a v1 client must be able to parse every future v1
 * announcement, or an old build could not learn that it is obsolete. Everything
 * variable lives behind the two references below.
 */
struct FWoTAnnouncementV1
{
	FString Format;
	FString Platform;
	FString AnnouncementDomain;
	uint64 ReleaseSequence = 0;
	FString KeyId;
	uint64 KeyRingEpoch = 0;
	uint32 MinimumGateProtocol = 0;
	FWoTSignedObjectRef KeyRing;
	FWoTSignedObjectRef Manifest;
};

/** An executable or tree component named by the manifest. */
struct FWoTManifestComponentRef
{
	FString Name;
	uint64 Size = 0;
	FWoTSha256 Sha256;
};

/** What the release claims about how it was built. Checked, never trusted. */
struct FWoTTargetAttestation
{
	FString TargetType;
	FString Configuration;
	FString LaunchModule;
	bool bListenServerPresent = false;
	bool bDedicatedServerStartupAvailable = false;
	bool bReleaseGateEnabled = false;
};

/**
 * The inner, release-key-signed manifest.
 *
 * Authorizes code installation; the announcement alone never does. The
 * `Minimum*` fields are how a release refuses to be applied by a build too old to
 * apply it correctly — the client exits to a manual upgrade rather than guessing.
 */
struct FWoTReleaseManifestV2
{
	FString Format;
	uint32 ManifestVersion = 0;
	FString Platform;
	FString AnnouncementDomain;
	uint64 ReleaseSequence = 0;
	FString HumanVersion;
	FString KeyId;
	FString BuildCommit;
	FString GeneratedUtc;

	FWoTSignedObjectRef Archive;
	FString ArchiveName;
	FString ArchiveRootId;

	FWoTManifestComponentRef InstalledTreeRoot;
	FWoTManifestComponentRef MainExecutable;

	FWoTTargetAttestation Attestation;
	FString StrippedAuditVerdict;
	FString CacheContractId;

	uint32 MinimumGateProtocol = 0;
	uint32 MinimumCommitServiceProtocol = 0;
	uint32 MinimumUpdaterProtocol = 0;
	uint32 InstalledTreeVersion = 0;
	uint32 CacheContractVersion = 0;
};

/**
 * A signed set of release keys at one epoch.
 *
 * `Epoch` is monotonic: a ring that verifies but carries an older epoch than the
 * one already installed is a rollback attempt, not an update, and callers compare
 * it against stored state rather than trusting the ring alone.
 */
struct FWoTKeyRing
{
	FString Format;
	uint64 Epoch = 0;
	TArray<FWoTReleaseKey> Keys;
	TArray<FString> RevokedKeyIds;
};

// --- Installed identity ------------------------------------------------------

/**
 * What release an installation IS.
 *
 * Ordering alone is not an identity: two domains may legitimately reach the same
 * numeric sequence, and the same sequence on another platform is another build. So
 * platform and the manifest digest stay bound to the ordering position, and
 * equality means all three agree.
 */
struct FWoTReleaseIdentity
{
	FWoTDomainSequence Ordering;
	FString Platform;
	FWoTSha256 ManifestSha256;
};

enum class EWoTTreeEntryType : uint8
{
	File,
	Directory,
	SymbolicLink,
};

WOTBOOTSTRAPCORE_API const TCHAR* LexToString(EWoTTreeEntryType Type);

/**
 * One entry in a signed installed-tree inventory.
 *
 * `Path` is always tree-relative, forward-slashed, and normalized. It is never a
 * path the process may hand to the OS unchecked — every consumer walks it
 * component by component through link-refusing handles.
 */
struct FWoTInstalledTreeEntry
{
	FString Path;
	EWoTTreeEntryType Type = EWoTTreeEntryType::File;

	/** POSIX permission bits. Setuid/setgid/sticky are refused, not masked off. */
	uint32 Mode = 0;

	uint64 Size = 0;

	/** Content digest for files; unused for directories and links. */
	FWoTSha256 Sha256;

	/** Relative, non-escaping target for a symbolic link; empty otherwise. */
	FString LinkTarget;
};

/**
 * The complete inventory of an installed tree.
 *
 * Entries are canonically ordered and unique, so the derived root digest is a
 * function of the tree's content and shape alone — which is what makes
 * "is this installation still what we signed" answerable.
 */
struct FWoTInstalledTreeManifest
{
	FString Format;
	uint32 Version = 0;
	FString Platform;
	TArray<FWoTInstalledTreeEntry> Entries;
};

/**
 * A separately identified component inside a release.
 *
 * The candidate root executable is bound as its own component rather than as a
 * tree entry, because it is promoted by a different transaction than the tree it
 * ships beside.
 */
struct FWoTComponentIdentity
{
	FString ComponentId;
	FString Name;
	uint64 Size = 0;
	FWoTSha256 Sha256;
};

// --- Native identity ---------------------------------------------------------

/**
 * What the OS says a held object IS, independent of any path.
 *
 * A path is a name, and names are racy: the thing a name refers to can be swapped
 * between two calls. These fields come from an open handle, so they keep answering
 * for the object we actually hold even after its name has been given to something
 * else.
 */
struct FWoTNativeFileIdentity
{
	/** Volume/device the object lives on. */
	uint64 VolumeId = 0;

	/** Stable file id within that volume (FILE_ID_INFO / inode). */
	uint64 FileIdLow = 0;
	uint64 FileIdHigh = 0;

	uint64 Size = 0;

	/** Change counter: ctime/change time. Detects mutation without re-reading. */
	uint64 ChangeState = 0;

	/** Hard links to this object. More than one means another name reaches it. */
	uint32 LinkCount = 0;

	EWoTTreeEntryType Type = EWoTTreeEntryType::File;

	/**
	 * Strict equality: the same object, AND nothing about it has changed.
	 *
	 * Includes LinkCount and ChangeState, so it is mutation-sensitive by design —
	 * a new hard link or an in-place rewrite makes it false.
	 */
	WOTBOOTSTRAPCORE_API bool operator==(const FWoTNativeFileIdentity& Other) const;
	bool operator!=(const FWoTNativeFileIdentity& Other) const { return !(*this == Other); }

	/**
	 * The weaker question: is this the same object, whatever has happened to its
	 * names?
	 *
	 * Unlinking or renaming over a held file legitimately changes its link count
	 * and change time while leaving the bytes we hold untouched. Callers asking
	 * "am I still holding what I verified" want this; callers asking "has anything
	 * about it moved" want operator==.
	 */
	WOTBOOTSTRAPCORE_API bool IsSameObject(const FWoTNativeFileIdentity& Other) const;

	/**
	 * Same volume, file id, type, size and link count; ONLY ChangeState moved.
	 *
	 * That is what the OS's own metadata writes look like (macOS provenance/macl
	 * xattrs on launch, NTFS Zone.Identifier/SmartScreen streams, ACL and
	 * attribute edits), but it is equally what a same-length in-place rewrite
	 * looks like. So it is never a reason to accept on its own: a caller that sees
	 * it re-hashes the file or re-lists the directory and accepts only if those
	 * still equal what was verified (WoTReverifyHeldFileAfterMetadataChurn).
	 */
	WOTBOOTSTRAPCORE_API bool DiffersOnlyInChangeState(
		const FWoTNativeFileIdentity& Other) const;
};

/** Opaque per-platform handle state. Defined only in the platform sources. */
struct FWoTNativeHandleState;

/**
 * A held, link-refusing OS handle.
 *
 * Move-only because it owns a live descriptor: copying one would produce two
 * owners of a single kernel object, and closing either would invalidate the other.
 * The class deliberately exposes no path — once opened, an object is referred to
 * by handle, never by name.
 */
class WOTBOOTSTRAPCORE_API FWoTNativeFileHandle final
{
public:
	FWoTNativeFileHandle();
	~FWoTNativeFileHandle();

	FWoTNativeFileHandle(FWoTNativeFileHandle&& Other);
	FWoTNativeFileHandle& operator=(FWoTNativeFileHandle&& Other);

	FWoTNativeFileHandle(const FWoTNativeFileHandle&) = delete;
	FWoTNativeFileHandle& operator=(const FWoTNativeFileHandle&) = delete;

	bool IsValid() const;

	/** The identity captured when the handle was opened. */
	const FWoTNativeFileIdentity& GetIdentity() const;

	/** Re-reads identity from the live handle, to detect mutation under us. */
	TWoTBootstrapResult<FWoTNativeFileIdentity> QueryCurrentIdentity() const;

	/** An independent handle to the SAME object, not to the same path. */
	TWoTBootstrapResult<FWoTNativeFileHandle> DuplicateForRead() const;

	/** Reads the whole held object. The bytes come from the handle, never a path. */
	TWoTBootstrapResult<TArray<uint8>> ReadAll() const;

private:
	friend struct FWoTNativeHandleAccess;
	TUniquePtr<FWoTNativeHandleState> State;
};

/** A held directory handle used to walk further components without following links. */
class WOTBOOTSTRAPCORE_API FWoTNativeDirectoryHandle final
{
public:
	FWoTNativeDirectoryHandle();
	~FWoTNativeDirectoryHandle();

	FWoTNativeDirectoryHandle(FWoTNativeDirectoryHandle&& Other);
	FWoTNativeDirectoryHandle& operator=(FWoTNativeDirectoryHandle&& Other);

	FWoTNativeDirectoryHandle(const FWoTNativeDirectoryHandle&) = delete;
	FWoTNativeDirectoryHandle& operator=(const FWoTNativeDirectoryHandle&) = delete;

	bool IsValid() const;
	const FWoTNativeFileIdentity& GetIdentity() const;
	TWoTBootstrapResult<FWoTNativeFileIdentity> QueryCurrentIdentity() const;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> DuplicateForRead() const;

private:
	friend struct FWoTNativeHandleAccess;
	TUniquePtr<FWoTNativeHandleState> State;
};

/**
 * The running image, held open.
 *
 * Captured as a handle so that later checks describe the code that is actually
 * executing, not whatever now sits at the executable's path.
 */
class WOTBOOTSTRAPCORE_API FWoTNativeExecutableIdentity final
{
public:
	FWoTNativeExecutableIdentity();
	~FWoTNativeExecutableIdentity();

	FWoTNativeExecutableIdentity(FWoTNativeExecutableIdentity&& Other);
	FWoTNativeExecutableIdentity& operator=(FWoTNativeExecutableIdentity&& Other);

	FWoTNativeExecutableIdentity(const FWoTNativeExecutableIdentity&) = delete;
	FWoTNativeExecutableIdentity& operator=(const FWoTNativeExecutableIdentity&) = delete;

	bool IsValid() const;
	const FWoTNativeFileIdentity& GetIdentity() const;
	TWoTBootstrapResult<FWoTNativeFileIdentity> QueryCurrentIdentity() const;

	/** The image path as reported by the OS. Diagnostic only; never re-opened. */
	const FString& GetReportedPath() const;

private:
	friend struct FWoTNativeHandleAccess;
	TUniquePtr<FWoTNativeHandleState> State;
	FString ReportedPath;
};

// --- Process identity --------------------------------------------------------

/**
 * Which process, exactly.
 *
 * A pid alone is not an identity: pids are reused, and a permit checked against a
 * bare pid would follow the number to whatever process inherited it. Start time and
 * the held image identity are what make this answer for one specific process.
 */
struct FWoTProcessStartIdentity
{
	uint64 ProcessId = 0;
	uint64 ProcessStartTime = 0;
	FWoTNativeFileIdentity Image;

	bool operator==(const FWoTProcessStartIdentity& Other) const
	{
		return ProcessId == Other.ProcessId
			&& ProcessStartTime == Other.ProcessStartTime && Image == Other.Image;
	}
	bool operator!=(const FWoTProcessStartIdentity& Other) const { return !(*this == Other); }
};
