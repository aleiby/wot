#include "WoTBootstrapCore/InstalledTree.h"

#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/NativeHandles.h"

namespace
{
/** POSIX bits that grant more than "who may read/write/execute". */
constexpr uint32 SetUidBit = 0x800;
constexpr uint32 SetGidBit = 0x400;
constexpr uint32 StickyBit = 0x200;
constexpr uint32 PermissionMask = 0x1FF;

/**
 * Names the inventory may never contain.
 *
 * The inventory cannot describe itself — its own digest would then be an input to
 * itself — and a signature sidecar is proof ABOUT the tree, not part of it.
 */
const TCHAR* const SidecarNames[] = {
	TEXT("installed-tree-v1.json"),
	TEXT("installed-tree-v1.json.sig"),
};
}  // namespace

const TCHAR* LexToString(EWoTTreeEntryType Type)
{
	switch (Type)
	{
	case EWoTTreeEntryType::File:         return TEXT("file");
	case EWoTTreeEntryType::Directory:    return TEXT("directory");
	case EWoTTreeEntryType::SymbolicLink: return TEXT("symlink");
	}
	return TEXT("unknown");
}

bool IsInstalledTreeSidecarPath(FStringView Path)
{
	for (const TCHAR* Name : SidecarNames)
	{
		if (Path.Equals(Name, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

bool IsAppleDoublePathComponent(FStringView Component)
{
	return Component.Equals(TEXT("__MACOSX"), ESearchCase::CaseSensitive)
		|| Component.StartsWith(TEXT("._"), ESearchCase::CaseSensitive);
}

TWoTBootstrapResult<FWoTNativeFileIdentity> WoTReverifyHeldFileAfterMetadataChurn(
	const FWoTNativeFileHandle& File, const FWoTNativeFileIdentity& Recorded,
	uint64 VerifiedSize, const FWoTSha256& VerifiedSha256, uint32 VerifiedMode,
	FStringView RelativePath)
{
	using FResult = TWoTBootstrapResult<FWoTNativeFileIdentity>;
	auto Changed = [RelativePath](const TCHAR* Why)
	{
		return FResult::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation, Why, FString(RelativePath)));
	};
	TWoTBootstrapResult<FWoTNativeFileIdentity> Start = File.QueryCurrentIdentity();
	if (Start.IsError())
	{
		return FResult::Error(MoveTemp(Start).GetError());
	}
	// Further metadata churn since Recorded is fine; anything wider is not.
	if (Start.GetValue() != Recorded && !Start.GetValue().DiffersOnlyInChangeState(Recorded))
	{
		return Changed(TEXT("a verified file changed beyond metadata"));
	}
	TWoTBootstrapResult<TArray<uint8>> Bytes = File.ReadAll();
	if (Bytes.IsError())
	{
		return FResult::Error(MoveTemp(Bytes).GetError());
	}
	TWoTBootstrapResult<uint32> Mode = QueryNativeMode(File);
	if (Mode.IsError())
	{
		return FResult::Error(MoveTemp(Mode).GetError());
	}
	TWoTBootstrapResult<FWoTNativeFileIdentity> End = File.QueryCurrentIdentity();
	if (End.IsError())
	{
		return FResult::Error(MoveTemp(End).GetError());
	}
	if (End.GetValue() != Start.GetValue())
	{
		return Changed(TEXT("a verified file changed while it was re-hashed"));
	}
	if (static_cast<uint64>(Bytes.GetValue().Num()) != VerifiedSize
		|| Sha256(Bytes.GetValue()) != VerifiedSha256)
	{
		return Changed(TEXT("a verified file's bytes changed"));
	}
	if (Mode.GetValue() != VerifiedMode)
	{
		return Changed(TEXT("a verified file's mode changed"));
	}
	return FResult::Success(End.GetValue());
}

FWoTBootstrapStatus RequireSafeRelativePath(FStringView RelativePath)
{
	// One definition of "safe relative path", shared by the inventory reader and
	// every native walk, so the two can never disagree about what is inside a tree.
	if (RelativePath.IsEmpty())
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath, TEXT("the path is empty")));
	}
	if (RelativePath.Len() > 4096)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath, TEXT("the path is implausibly long")));
	}

	const FString Path(RelativePath);
	if (Path.Contains(TEXT("\\")))
	{
		// One separator, always. Accepting both would mean two spellings of one
		// path, and the digest is over the spelling.
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("backslashes are not path separators here"), Path));
	}
	if (Path.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath, TEXT("the path is absolute"), Path));
	}
	if (Path.Contains(TEXT(":")))
	{
		// Catches drive letters and NTFS alternate data streams alike.
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::UnsafePath,
			TEXT("the path carries a drive or stream qualifier"), Path));
	}

	TArray<FString> Components;
	Path.ParseIntoArray(Components, TEXT("/"), false);
	for (const FString& Component : Components)
	{
		if (Component.IsEmpty())
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the path has an empty component"), Path));
		}
		if (Component == TEXT(".") || Component == TEXT(".."))
		{
			// Never resolved, only refused: resolving would mean deciding what the
			// signer meant, and the signer had no business meaning this.
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the path contains a relative component"), Path));
		}
		if (Component.EndsWith(TEXT("."), ESearchCase::CaseSensitive)
			|| Component.EndsWith(TEXT(" "), ESearchCase::CaseSensitive))
		{
			// Windows silently strips these, so "a." and "a" would name one file
			// while hashing as two entries.
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("a path component ends with a dot or space"), Path));
		}
		for (const TCHAR Character : Component)
		{
			if (Character < 0x20 || Character == 0x7F)
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::UnsafePath,
					TEXT("a path component contains a control character"), Path));
			}
		}
	}
	return FWoTBootstrapStatus::Success();
}

TWoTBootstrapResult<FWoTInstalledTreeManifest> ParseInstalledTreeV1(
	TConstArrayView<uint8> CanonicalBytes)
{
	using FResult = TWoTBootstrapResult<FWoTInstalledTreeManifest>;

	TWoTBootstrapResult<FWoTCanonicalJsonValue> Document = ParseCanonicalJson(CanonicalBytes);
	if (Document.IsError())
	{
		return FResult::Error(MoveTemp(Document).GetError());
	}
	const FWoTCanonicalJsonValue& Root = Document.GetValue();

	static const TCHAR* const Fields[] = {
		TEXT("entries"), TEXT("format"), TEXT("platform"), TEXT("version")};
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(Root, Fields, TEXT("installed tree")); SchemaError.IsSet())
	{
		return FResult::Error(MoveTemp(SchemaError.GetValue()));
	}

	FWoTInstalledTreeManifest Inventory;
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Root, TEXT("format"), TEXT("installed tree"), Inventory.Format); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (!Inventory.Format.Equals(WoTInstalledTreeV1Format, ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("installed tree format '%s' is not '%s'"),
				*Inventory.Format, WoTInstalledTreeV1Format)));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsigned32Field(
			Root, TEXT("version"), TEXT("installed tree"), Inventory.Version); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (Inventory.Version != WoTInstalledTreeVersion)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("installed tree version %u is not %u"),
				Inventory.Version, WoTInstalledTreeVersion)));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Root, TEXT("platform"), TEXT("installed tree"), Inventory.Platform); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	const FWoTCanonicalJsonValue* const Entries = Root.FindMember(TEXT("entries"));
	if (Entries == nullptr || !Entries->IsArray())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("installed tree field 'entries' is not an array")));
	}

	static const TCHAR* const EntryFields[] = {
		TEXT("link_target"), TEXT("mode"), TEXT("path"), TEXT("sha256"),
		TEXT("size"), TEXT("type")};

	TSet<FString> SeenPaths;
	TSet<FString> SeenFoldedPaths;
	FString PreviousPath;
	for (int32 Index = 0; Index < Entries->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Element = Entries->GetElement(Index);
		const FString What = FString::Printf(TEXT("installed tree entry %d"), Index);
		if (TOptional<FWoTBootstrapError> SchemaError =
				RequireExactMembers(Element, EntryFields, *What); SchemaError.IsSet())
		{
			return FResult::Error(MoveTemp(SchemaError.GetValue()));
		}

		FWoTInstalledTreeEntry Entry;
		if (TOptional<FWoTBootstrapError> Error =
				ReadStringField(Element, TEXT("path"), *What, Entry.Path); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (FWoTBootstrapStatus PathStatus = RequireSafeRelativePath(Entry.Path);
			PathStatus.IsError())
		{
			return FResult::Error(MoveTemp(PathStatus).GetError());
		}
		TArray<FString> PathComponents;
		Entry.Path.ParseIntoArray(PathComponents, TEXT("/"), true);
		for (const FString& Component : PathComponents)
		{
			if (IsAppleDoublePathComponent(Component))
			{
				return FResult::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::UnsafePath,
					TEXT("the installed tree contains AppleDouble metadata"),
					Entry.Path));
			}
		}
		if (IsInstalledTreeSidecarPath(Entry.Path))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				TEXT("the inventory may not list itself or its signature"), Entry.Path));
		}
		if (SeenPaths.Contains(Entry.Path))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::DuplicateField,
				TEXT("an installed tree path appears twice"), Entry.Path));
		}
		// Two entries differing only by case would be one file on a case-insensitive
		// volume, so the inventory would describe a tree that cannot exist there.
		const FString Folded = Entry.Path.ToLower();
		if (SeenFoldedPaths.Contains(Folded))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::DuplicateField,
				TEXT("two installed tree paths differ only by case"), Entry.Path));
		}
		// Canonical order is part of the format, so the derived root digest depends
		// only on the tree and never on the emitter's iteration order.
		if (!PreviousPath.IsEmpty()
			&& Entry.Path.Compare(PreviousPath, ESearchCase::CaseSensitive) <= 0)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::NonCanonicalJson,
				FString::Printf(TEXT("installed tree path '%s' does not follow '%s'"),
					*Entry.Path, *PreviousPath)));
		}
		SeenPaths.Add(Entry.Path);
		SeenFoldedPaths.Add(Folded);
		PreviousPath = Entry.Path;

		FString TypeText;
		if (TOptional<FWoTBootstrapError> Error =
				ReadStringField(Element, TEXT("type"), *What, TypeText); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TypeText == TEXT("file"))
		{
			Entry.Type = EWoTTreeEntryType::File;
		}
		else if (TypeText == TEXT("directory"))
		{
			Entry.Type = EWoTTreeEntryType::Directory;
		}
		else if (TypeText == TEXT("symlink"))
		{
			Entry.Type = EWoTTreeEntryType::SymbolicLink;
		}
		else
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
				FString::Printf(TEXT("%s has unknown type '%s'"), *What, *TypeText)));
		}

		uint64 Mode = 0;
		if (TOptional<FWoTBootstrapError> Error =
				ReadUnsignedField(Element, TEXT("mode"), *What, Mode); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if ((Mode & ~static_cast<uint64>(PermissionMask | SetUidBit | SetGidBit | StickyBit)) != 0)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
				FString::Printf(TEXT("%s has mode bits outside the permission set"), *What)));
		}
		if ((Mode & (SetUidBit | SetGidBit | StickyBit)) != 0)
		{
			// Refused rather than masked: a signer that asked for setuid asked for
			// privilege escalation, and quietly dropping the bit would install
			// something other than what was signed.
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
				FString::Printf(TEXT("%s requests setuid/setgid/sticky"), *What), Entry.Path));
		}
		Entry.Mode = static_cast<uint32>(Mode);

		if (TOptional<FWoTBootstrapError> Error =
				ReadUnsignedField(Element, TEXT("size"), *What, Entry.Size); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}

		FString DigestHex;
		if (TOptional<FWoTBootstrapError> Error =
				ReadStringField(Element, TEXT("sha256"), *What, DigestHex); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error =
				ReadStringField(Element, TEXT("link_target"), *What, Entry.LinkTarget);
			Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}

		switch (Entry.Type)
		{
		case EWoTTreeEntryType::File:
		{
			TWoTBootstrapResult<FWoTSha256> Digest = FWoTSha256::FromHex(DigestHex);
			if (Digest.IsError())
			{
				return FResult::Error(MoveTemp(Digest).GetError());
			}
			Entry.Sha256 = Digest.GetValue();
			if (!Entry.LinkTarget.IsEmpty())
			{
				return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
					FString::Printf(TEXT("%s is a file but names a link target"), *What)));
			}
			break;
		}
		case EWoTTreeEntryType::Directory:
			if (Entry.Size != 0 || !DigestHex.IsEmpty() || !Entry.LinkTarget.IsEmpty())
			{
				return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
					FString::Printf(TEXT("%s is a directory with file fields set"), *What)));
			}
			break;
		case EWoTTreeEntryType::SymbolicLink:
			if (Entry.Size != 0 || !DigestHex.IsEmpty())
			{
				return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
					FString::Printf(TEXT("%s is a link with file fields set"), *What)));
			}
			// A link may only point inside the tree. An escaping link is how a
			// signed inventory would otherwise describe somebody else's file.
			if (FWoTBootstrapStatus LinkStatus = RequireSafeRelativePath(Entry.LinkTarget);
				LinkStatus.IsError())
			{
				return FResult::Error(MoveTemp(LinkStatus).GetError());
			}
			break;
		}

		Inventory.Entries.Add(MoveTemp(Entry));
	}

	return FResult::Success(MoveTemp(Inventory));
}

TWoTBootstrapResult<FWoTSha256> ComputeInstalledTreeRoot(
	const FWoTInstalledTreeManifest& Inventory)
{
	using FResult = TWoTBootstrapResult<FWoTSha256>;

	// A length-prefixed projection rather than a delimiter-joined string: two
	// different trees must not be able to produce one byte stream by choosing
	// names that contain the delimiter.
	FWoTSha256Hasher Hasher;

	const auto AppendText = [&Hasher](const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		const uint64 Length = static_cast<uint64>(Utf8.Length());
		uint8 Prefix[8];
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Prefix[Index] = static_cast<uint8>((Length >> (Index * 8)) & 0xFF);
		}
		Hasher.Update(TConstArrayView<uint8>(Prefix, 8));
		Hasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	};
	const auto AppendNumber = [&Hasher](uint64 Value)
	{
		uint8 Bytes[8];
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Bytes[Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xFF);
		}
		Hasher.Update(TConstArrayView<uint8>(Bytes, 8));
	};

	AppendText(FString(WoTInstalledTreeV1Format));
	AppendNumber(Inventory.Version);
	AppendText(Inventory.Platform);
	AppendNumber(static_cast<uint64>(Inventory.Entries.Num()));

	FString PreviousPath;
	for (const FWoTInstalledTreeEntry& Entry : Inventory.Entries)
	{
		// The root digest is only meaningful over a canonical inventory, so an
		// unordered one is refused instead of quietly hashed into a value that no
		// other implementation would reproduce.
		if (!PreviousPath.IsEmpty()
			&& Entry.Path.Compare(PreviousPath, ESearchCase::CaseSensitive) <= 0)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::NonCanonicalJson,
				TEXT("installed tree entries are not in canonical order"), Entry.Path));
		}
		PreviousPath = Entry.Path;

		AppendText(Entry.Path);
		AppendText(FString(LexToString(Entry.Type)));
		AppendNumber(Entry.Mode);
		AppendNumber(Entry.Size);
		Hasher.Update(TConstArrayView<uint8>(Entry.Sha256.GetData(), Entry.Sha256.Num()));
		AppendText(Entry.LinkTarget);
	}

	return FResult::Success(Hasher.Finish());
}
