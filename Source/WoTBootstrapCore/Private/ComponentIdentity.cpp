#include "WoTBootstrapCore/ComponentIdentity.h"

bool FWoTNativeFileIdentity::operator==(const FWoTNativeFileIdentity& Other) const
{
	// Every field, including LinkCount and ChangeState: an object that gained a
	// hard link or was rewritten in place is not the object we validated, even
	// though its volume and file id are unchanged.
	return VolumeId == Other.VolumeId && FileIdLow == Other.FileIdLow
		&& FileIdHigh == Other.FileIdHigh && Size == Other.Size
		&& ChangeState == Other.ChangeState && LinkCount == Other.LinkCount
		&& Type == Other.Type;
}

bool FWoTNativeFileIdentity::IsSameObject(const FWoTNativeFileIdentity& Other) const
{
	// Deliberately excludes LinkCount and ChangeState. Removing or replacing a
	// NAME touches both while leaving the held object's bytes exactly where they
	// were, so including them here would report a name change as a content change.
	// Size is compared for files only: an APFS/HFS directory's st_size tracks its
	// entries, so adding a child (UpdaterState under the user-state root) would
	// otherwise make the same directory look like a different object. NTFS
	// reports directory size 0, which hid this on Win64.
	return VolumeId == Other.VolumeId && FileIdLow == Other.FileIdLow
		&& FileIdHigh == Other.FileIdHigh && Type == Other.Type
		&& (Type == EWoTTreeEntryType::Directory || Size == Other.Size);
}

bool FWoTNativeFileIdentity::DiffersOnlyInChangeState(
	const FWoTNativeFileIdentity& Other) const
{
	return VolumeId == Other.VolumeId && FileIdLow == Other.FileIdLow
		&& FileIdHigh == Other.FileIdHigh && Size == Other.Size
		&& LinkCount == Other.LinkCount && Type == Other.Type
		&& ChangeState != Other.ChangeState;
}

TWoTBootstrapResult<FWoTComponentIdentity> ParseComponentIdentityV1(
	const FWoTCanonicalJsonValue& Document)
{
	using FResult = TWoTBootstrapResult<FWoTComponentIdentity>;

	static const TCHAR* const Fields[] = {
		TEXT("component_id"), TEXT("format"), TEXT("name"), TEXT("sha256"), TEXT("size")};
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(Document, Fields, TEXT("component identity"));
		SchemaError.IsSet())
	{
		return FResult::Error(MoveTemp(SchemaError.GetValue()));
	}

	FString Format;
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Document, TEXT("format"), TEXT("component identity"), Format); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (!Format.Equals(WoTComponentIdentityV1Format, ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("component identity format '%s' is not '%s'"),
				*Format, WoTComponentIdentityV1Format)));
	}

	FWoTComponentIdentity Component;
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(Document,
			TEXT("component_id"), TEXT("component identity"), Component.ComponentId);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (Component.ComponentId.IsEmpty())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("a component identity must name a component")));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Document, TEXT("name"), TEXT("component identity"), Component.Name);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	// The name becomes a file name under a staging root, so it is a bare name.
	if (Component.Name.IsEmpty() || Component.Name.Contains(TEXT("/"))
		|| Component.Name.Contains(TEXT("\\")) || Component.Name.Contains(TEXT(".."))
		|| Component.Name.Contains(TEXT(":")))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("a component name must be a bare file name"), Component.Name));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsignedField(
			Document, TEXT("size"), TEXT("component identity"), Component.Size);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	FString DigestHex;
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Document, TEXT("sha256"), TEXT("component identity"), DigestHex); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	TWoTBootstrapResult<FWoTSha256> Digest = FWoTSha256::FromHex(DigestHex);
	if (Digest.IsError())
	{
		return FResult::Error(MoveTemp(Digest).GetError());
	}
	Component.Sha256 = Digest.GetValue();

	return FResult::Success(MoveTemp(Component));
}

FWoTBootstrapStatus MatchComponentIdentity(const FWoTComponentIdentity& Expected,
	const FWoTNativeFileIdentity& Actual, const FWoTSha256& ActualContentSha256)
{
	// A component must be an ordinary file. A directory or link of the right size
	// and digest is still not the executable the release describes.
	if (Actual.Type != EWoTTreeEntryType::File)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			FString::Printf(TEXT("component '%s' is a %s, not a file"),
				*Expected.ComponentId, LexToString(Actual.Type))));
	}
	if (Actual.Size != Expected.Size)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			FString::Printf(TEXT("component '%s' is %llu bytes, expected %llu"),
				*Expected.ComponentId, Actual.Size, Expected.Size)));
	}
	if (ActualContentSha256 != Expected.Sha256)
	{
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
			EWoTBootstrapError::CorruptInstallation,
			FString::Printf(TEXT("component '%s' content does not match its identity"),
				*Expected.ComponentId)));
	}
	return FWoTBootstrapStatus::Success();
}
