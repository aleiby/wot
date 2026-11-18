#include "WoTBootstrapCore/ReleaseManifest.h"

#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Ed25519.h"

namespace
{
TOptional<FWoTBootstrapError> ReadComponentRef(const FWoTCanonicalJsonValue& Parent,
	const TCHAR* Field, const TCHAR* What, FWoTManifestComponentRef& OutRef)
{
	const FWoTCanonicalJsonValue* Object = nullptr;
	if (TOptional<FWoTBootstrapError> FieldError =
			ReadObjectField(Parent, Field, What, Object); FieldError.IsSet())
	{
		return FieldError;
	}

	static const TCHAR* const RefFields[] = {TEXT("name"), TEXT("sha256"), TEXT("size")};
	const FString RefWhat = FString::Printf(TEXT("%s.%s"), What, Field);
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(*Object, RefFields, *RefWhat); SchemaError.IsSet())
	{
		return SchemaError;
	}
	if (TOptional<FWoTBootstrapError> NameError =
			ReadStringField(*Object, TEXT("name"), *RefWhat, OutRef.Name); NameError.IsSet())
	{
		return NameError;
	}
	if (TOptional<FWoTBootstrapError> SizeError =
			ReadUnsignedField(*Object, TEXT("size"), *RefWhat, OutRef.Size); SizeError.IsSet())
	{
		return SizeError;
	}

	FString DigestHex;
	if (TOptional<FWoTBootstrapError> DigestError =
			ReadStringField(*Object, TEXT("sha256"), *RefWhat, DigestHex); DigestError.IsSet())
	{
		return DigestError;
	}
	TWoTBootstrapResult<FWoTSha256> Digest = FWoTSha256::FromHex(DigestHex);
	if (Digest.IsError())
	{
		return MoveTemp(Digest).GetError();
	}
	OutRef.Sha256 = Digest.GetValue();
	return {};
}

TOptional<FWoTBootstrapError> ReadArchive(const FWoTCanonicalJsonValue& Parent,
	FWoTSignedObjectRef& OutRef, FString& OutName)
{
	const FWoTCanonicalJsonValue* Object = nullptr;
	if (TOptional<FWoTBootstrapError> FieldError =
			ReadObjectField(Parent, TEXT("archive"), TEXT("release manifest"), Object);
		FieldError.IsSet())
	{
		return FieldError;
	}

	static const TCHAR* const Fields[] = {
		TEXT("name"), TEXT("sha256"), TEXT("size"), TEXT("url")};
	if (TOptional<FWoTBootstrapError> SchemaError = RequireExactMembers(
			*Object, Fields, TEXT("release manifest.archive")); SchemaError.IsSet())
	{
		return SchemaError;
	}
	if (TOptional<FWoTBootstrapError> NameError = ReadStringField(
			*Object, TEXT("name"), TEXT("release manifest.archive"), OutName);
		NameError.IsSet())
	{
		return NameError;
	}
	// The archive name is used to build a local path, so it must be a bare file
	// name. A signed manifest is not permission to write outside the staging root.
	if (OutName.IsEmpty() || OutName.Contains(TEXT("/")) || OutName.Contains(TEXT("\\"))
		|| OutName.Contains(TEXT("..")) || OutName.Contains(TEXT(":")))
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the archive name must be a bare file name"), OutName);
	}
	if (TOptional<FWoTBootstrapError> UrlError = ReadStringField(
			*Object, TEXT("url"), TEXT("release manifest.archive"), OutRef.Url);
		UrlError.IsSet())
	{
		return UrlError;
	}
	if (!OutRef.Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			TEXT("the archive url is not HTTPS"), OutRef.Url);
	}
	if (TOptional<FWoTBootstrapError> SizeError = ReadUnsignedField(
			*Object, TEXT("size"), TEXT("release manifest.archive"), OutRef.Size);
		SizeError.IsSet())
	{
		return SizeError;
	}
	if (OutRef.Size == 0)
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::InvalidNumber,
			TEXT("the archive size is zero"));
	}

	FString DigestHex;
	if (TOptional<FWoTBootstrapError> DigestError = ReadStringField(
			*Object, TEXT("sha256"), TEXT("release manifest.archive"), DigestHex);
		DigestError.IsSet())
	{
		return DigestError;
	}
	TWoTBootstrapResult<FWoTSha256> Digest = FWoTSha256::FromHex(DigestHex);
	if (Digest.IsError())
	{
		return MoveTemp(Digest).GetError();
	}
	OutRef.Sha256 = Digest.GetValue();
	return {};
}

TOptional<FWoTBootstrapError> ReadAttestation(const FWoTCanonicalJsonValue& Parent,
	FWoTTargetAttestation& Out)
{
	const FWoTCanonicalJsonValue* Object = nullptr;
	if (TOptional<FWoTBootstrapError> FieldError = ReadObjectField(
			Parent, TEXT("target_attestation"), TEXT("release manifest"), Object);
		FieldError.IsSet())
	{
		return FieldError;
	}

	static const TCHAR* const Fields[] = {
		TEXT("configuration"), TEXT("dedicated_server_startup_available"),
		TEXT("launch_module"), TEXT("listen_server_present"),
		TEXT("release_gate_enabled"), TEXT("target_type")};
	const TCHAR* const What = TEXT("release manifest.target_attestation");
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(*Object, Fields, What); SchemaError.IsSet())
	{
		return SchemaError;
	}
	if (TOptional<FWoTBootstrapError> Error =
			ReadStringField(*Object, TEXT("target_type"), What, Out.TargetType); Error.IsSet())
	{
		return Error;
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			*Object, TEXT("configuration"), What, Out.Configuration); Error.IsSet())
	{
		return Error;
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			*Object, TEXT("launch_module"), What, Out.LaunchModule); Error.IsSet())
	{
		return Error;
	}
	if (TOptional<FWoTBootstrapError> Error = ReadBoolField(
			*Object, TEXT("listen_server_present"), What, Out.bListenServerPresent);
		Error.IsSet())
	{
		return Error;
	}
	if (TOptional<FWoTBootstrapError> Error = ReadBoolField(
			*Object, TEXT("dedicated_server_startup_available"), What,
			Out.bDedicatedServerStartupAvailable); Error.IsSet())
	{
		return Error;
	}
	if (TOptional<FWoTBootstrapError> Error = ReadBoolField(
			*Object, TEXT("release_gate_enabled"), What, Out.bReleaseGateEnabled);
		Error.IsSet())
	{
		return Error;
	}
	return {};
}
}  // namespace

bool SupportsManifestProtocols(const FWoTReleaseManifestV2& Manifest)
{
	// Every one of these is a way the release can say "a build older than X cannot
	// apply me correctly". Refusing is the safe answer; guessing is not.
	return Manifest.MinimumGateProtocol <= WoT::Bootstrap::GateProtocol
		&& Manifest.MinimumCommitServiceProtocol <= WoT::Bootstrap::CommitServiceProtocol
		&& Manifest.MinimumUpdaterProtocol <= WoT::Bootstrap::UpdaterProtocol
		&& Manifest.CacheContractVersion <= WoT::Bootstrap::CacheContractVersion
		&& Manifest.InstalledTreeVersion <= 1;
}

TWoTBootstrapResult<FWoTReleaseManifestV2> VerifyReleaseManifestV2(
	TConstArrayView<uint8> Json, const FWoTEd25519Signature& Signature,
	const FWoTAnnouncementV1& Announcement, const FWoTKeyRing& Ring)
{
	using FResult = TWoTBootstrapResult<FWoTReleaseManifestV2>;

	// The key is chosen by the ALREADY AUTHENTICATED announcement, not by anything
	// in these bytes: a manifest must not get to nominate the key that vouches for
	// it. Selection also enforces the key's sequence window and revocation.
	TWoTBootstrapResult<FWoTReleaseKey> SigningKey =
		SelectReleaseKey(Ring, Announcement.KeyId, Announcement.ReleaseSequence);
	if (SigningKey.IsError())
	{
		return FResult::Error(MoveTemp(SigningKey).GetError());
	}

	TWoTBootstrapResult<bool> Verified = VerifyEd25519(SigningKey.GetValue().PublicKey,
		EWoTSignatureContext::ReleaseManifestV2, Json, Signature);
	if (Verified.IsError())
	{
		return FResult::Error(MoveTemp(Verified).GetError());
	}
	if (!Verified.GetValue())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
			FString::Printf(TEXT("the release manifest signature does not verify under key '%s'"),
				*SigningKey.GetValue().KeyId)));
	}

	TWoTBootstrapResult<FWoTCanonicalJsonValue> Document = ParseCanonicalJson(Json);
	if (Document.IsError())
	{
		return FResult::Error(MoveTemp(Document).GetError());
	}
	const FWoTCanonicalJsonValue& Root = Document.GetValue();

	static const TCHAR* const Fields[] = {
		TEXT("announcement_domain"), TEXT("archive"), TEXT("archive_root_id"),
		TEXT("build_commit"), TEXT("cache_contract_id"), TEXT("cache_contract_version"),
		TEXT("format"), TEXT("generated_utc"), TEXT("human_version"),
		TEXT("installed_tree_root"), TEXT("installed_tree_version"), TEXT("key_id"),
		TEXT("main_executable"), TEXT("manifest_version"),
		TEXT("minimum_commit_service_protocol"), TEXT("minimum_gate_protocol"),
		TEXT("minimum_updater_protocol"), TEXT("platform"), TEXT("release_sequence"),
		TEXT("stripped_audit_verdict"), TEXT("target_attestation")};
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(Root, Fields, TEXT("release manifest")); SchemaError.IsSet())
	{
		return FResult::Error(MoveTemp(SchemaError.GetValue()));
	}

	FWoTReleaseManifestV2 Manifest;
	const TCHAR* const What = TEXT("release manifest");

	if (TOptional<FWoTBootstrapError> Error =
			ReadStringField(Root, TEXT("format"), What, Manifest.Format); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (!Manifest.Format.Equals(WoTReleaseManifestV2Format, ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("release manifest format '%s' is not '%s'"),
				*Manifest.Format, WoTReleaseManifestV2Format)));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsigned32Field(
			Root, TEXT("manifest_version"), What, Manifest.ManifestVersion); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (Manifest.ManifestVersion != WoT::Bootstrap::ReleaseManifestVersion)
	{
		// v1 exists only as a frozen developer oracle. Production never
		// emits or accepts it, so there is no compatibility branch to attack.
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("release manifest version %u is not %u"),
				Manifest.ManifestVersion, WoT::Bootstrap::ReleaseManifestVersion)));
	}

	if (TOptional<FWoTBootstrapError> Error =
			ReadStringField(Root, TEXT("platform"), What, Manifest.Platform); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Root, TEXT("announcement_domain"), What, Manifest.AnnouncementDomain);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsignedField(
			Root, TEXT("release_sequence"), What, Manifest.ReleaseSequence); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error =
			ReadStringField(Root, TEXT("key_id"), What, Manifest.KeyId); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	// The manifest must agree with the envelope that authenticated. A disagreement
	// is not a newer release; it is a substituted one, so it is refused in either
	// direction rather than resolved by preferring one side.
	if (!Manifest.AnnouncementDomain.Equals(Announcement.AnnouncementDomain,
			ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the release manifest domain does not match the announcement")));
	}
	if (!Manifest.Platform.Equals(Announcement.Platform, ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the release manifest platform does not match the announcement")));
	}
	if (Manifest.ReleaseSequence != Announcement.ReleaseSequence)
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Rollback,
			FString::Printf(
				TEXT("the release manifest sequence %llu does not match the announced %llu"),
				Manifest.ReleaseSequence, Announcement.ReleaseSequence)));
	}
	if (!Manifest.KeyId.Equals(Announcement.KeyId, ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
			TEXT("the release manifest key id does not match the announcement")));
	}

	for (const TPair<const TCHAR*, FString*>& Text : {
			TPair<const TCHAR*, FString*>{TEXT("human_version"), &Manifest.HumanVersion},
			TPair<const TCHAR*, FString*>{TEXT("build_commit"), &Manifest.BuildCommit},
			TPair<const TCHAR*, FString*>{TEXT("generated_utc"), &Manifest.GeneratedUtc},
			TPair<const TCHAR*, FString*>{TEXT("archive_root_id"), &Manifest.ArchiveRootId},
			TPair<const TCHAR*, FString*>{TEXT("stripped_audit_verdict"),
				&Manifest.StrippedAuditVerdict},
			TPair<const TCHAR*, FString*>{TEXT("cache_contract_id"),
				&Manifest.CacheContractId}})
	{
		if (TOptional<FWoTBootstrapError> Error =
				ReadStringField(Root, Text.Key, What, *Text.Value); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
	}

	if (TOptional<FWoTBootstrapError> Error =
			ReadArchive(Root, Manifest.Archive, Manifest.ArchiveName); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadComponentRef(Root,
			TEXT("installed_tree_root"), What, Manifest.InstalledTreeRoot); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadComponentRef(Root,
			TEXT("main_executable"), What, Manifest.MainExecutable); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	if (TOptional<FWoTBootstrapError> Error = ReadAttestation(Root, Manifest.Attestation);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	for (const TPair<const TCHAR*, uint32*>& Protocol : {
			TPair<const TCHAR*, uint32*>{TEXT("minimum_gate_protocol"),
				&Manifest.MinimumGateProtocol},
			TPair<const TCHAR*, uint32*>{TEXT("minimum_commit_service_protocol"),
				&Manifest.MinimumCommitServiceProtocol},
			TPair<const TCHAR*, uint32*>{TEXT("minimum_updater_protocol"),
				&Manifest.MinimumUpdaterProtocol},
			TPair<const TCHAR*, uint32*>{TEXT("installed_tree_version"),
				&Manifest.InstalledTreeVersion},
			TPair<const TCHAR*, uint32*>{TEXT("cache_contract_version"),
				&Manifest.CacheContractVersion}})
	{
		if (TOptional<FWoTBootstrapError> Error =
				ReadUnsigned32Field(Root, Protocol.Key, What, *Protocol.Value); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
	}

	// Note what is NOT done here: an unsupported protocol is a successful parse of
	// a valid manifest this build cannot apply. That distinction is the difference
	// between "manual full package" and "metadata failure", so the caller asks
	// SupportsManifestProtocols separately rather than getting an error here.
	return FResult::Success(MoveTemp(Manifest));
}
