#include "WoTBootstrapCore/Announcement.h"

#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Ed25519.h"

namespace
{
/** `url`, `size`, `sha256` — the closed shape of both references. */
TOptional<FWoTBootstrapError> ReadObjectRef(const FWoTCanonicalJsonValue& Parent,
	const TCHAR* Field, const TCHAR* What, FWoTSignedObjectRef& OutRef)
{
	const FWoTCanonicalJsonValue* Object = nullptr;
	if (TOptional<FWoTBootstrapError> FieldError =
			ReadObjectField(Parent, Field, What, Object); FieldError.IsSet())
	{
		return FieldError;
	}

	static const TCHAR* const RefFields[] = {TEXT("sha256"), TEXT("size"), TEXT("url")};
	const FString RefWhat = FString::Printf(TEXT("%s.%s"), What, Field);
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(*Object, RefFields, *RefWhat); SchemaError.IsSet())
	{
		return SchemaError;
	}
	if (TOptional<FWoTBootstrapError> UrlError =
			ReadStringField(*Object, TEXT("url"), *RefWhat, OutRef.Url); UrlError.IsSet())
	{
		return UrlError;
	}
	if (!OutRef.Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))
	{
		// A signed announcement may not point at plaintext. The object is hashed,
		// so this is not about secrecy — it is about not teaching the client that
		// downgrading the transport is ever acceptable.
		return MakeWoTBootstrapError(EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("%s.url is not HTTPS"), *RefWhat), OutRef.Url);
	}
	if (TOptional<FWoTBootstrapError> SizeError =
			ReadUnsignedField(*Object, TEXT("size"), *RefWhat, OutRef.Size); SizeError.IsSet())
	{
		return SizeError;
	}
	if (OutRef.Size == 0)
	{
		return MakeWoTBootstrapError(EWoTBootstrapError::InvalidNumber,
			FString::Printf(TEXT("%s.size is zero"), *RefWhat));
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
}  // namespace

TWoTBootstrapResult<FWoTAnnouncementV1> VerifyAnnouncementV1(TConstArrayView<uint8> Json,
	const FWoTEd25519Signature& Signature, FStringView Platform,
	FStringView CompiledDomain, const FWoTEd25519PublicKey& AnnouncementKey)
{
	using FResult = TWoTBootstrapResult<FWoTAnnouncementV1>;

	// Signature first, over the exact bytes, before any field is believed. Parsing
	// unauthenticated input into decisions is how a parser becomes an attack
	// surface; here it only ever becomes a diagnostic.
	TWoTBootstrapResult<bool> Verified = VerifyEd25519(AnnouncementKey,
		EWoTSignatureContext::AnnouncementV1, Json, Signature);
	if (Verified.IsError())
	{
		return FResult::Error(MoveTemp(Verified).GetError());
	}
	if (!Verified.GetValue())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
			TEXT("the announcement signature does not verify under the embedded key")));
	}

	TWoTBootstrapResult<FWoTCanonicalJsonValue> Document = ParseCanonicalJson(Json);
	if (Document.IsError())
	{
		return FResult::Error(MoveTemp(Document).GetError());
	}

	// Frozen field set. It never grows: a v1 client must parse every future v1
	// announcement, or an obsolete build could not learn that it is obsolete.
	static const TCHAR* const Fields[] = {
		TEXT("announcement_domain"), TEXT("format"), TEXT("key_id"),
		TEXT("key_ring"), TEXT("key_ring_epoch"), TEXT("manifest"),
		TEXT("minimum_gate_protocol"), TEXT("platform"), TEXT("release_sequence")};
	const FWoTCanonicalJsonValue& Root = Document.GetValue();
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(Root, Fields, TEXT("announcement")); SchemaError.IsSet())
	{
		return FResult::Error(MoveTemp(SchemaError.GetValue()));
	}

	FWoTAnnouncementV1 Announcement;
	if (TOptional<FWoTBootstrapError> FormatError = ReadStringField(
			Root, TEXT("format"), TEXT("announcement"), Announcement.Format);
		FormatError.IsSet())
	{
		return FResult::Error(MoveTemp(FormatError.GetValue()));
	}
	if (!Announcement.Format.Equals(WoTAnnouncementV1Format, ESearchCase::CaseSensitive))
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("announcement format '%s' is not '%s'"),
				*Announcement.Format, WoTAnnouncementV1Format)));
	}

	if (TOptional<FWoTBootstrapError> PlatformError = ReadStringField(
			Root, TEXT("platform"), TEXT("announcement"), Announcement.Platform);
		PlatformError.IsSet())
	{
		return FResult::Error(MoveTemp(PlatformError.GetValue()));
	}
	if (!Announcement.Platform.Equals(FString(Platform), ESearchCase::CaseSensitive))
	{
		// A correctly signed announcement for the other platform is a valid
		// signature over something that is not ours.
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("announcement platform '%s' is not '%.*s'"),
				*Announcement.Platform, Platform.Len(), Platform.GetData())));
	}

	if (TOptional<FWoTBootstrapError> DomainError = ReadStringField(Root,
			TEXT("announcement_domain"), TEXT("announcement"),
			Announcement.AnnouncementDomain); DomainError.IsSet())
	{
		return FResult::Error(MoveTemp(DomainError.GetValue()));
	}
	if (!Announcement.AnnouncementDomain.Equals(FString(CompiledDomain),
			ESearchCase::CaseSensitive))
	{
		// A different domain is never adopted from the network. Only an
		// offline-root full-package transition can move this client's domain.
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("announcement domain '%s' is not the compiled '%.*s'"),
				*Announcement.AnnouncementDomain, CompiledDomain.Len(),
				CompiledDomain.GetData())));
	}

	if (TOptional<FWoTBootstrapError> KeyIdError = ReadStringField(
			Root, TEXT("key_id"), TEXT("announcement"), Announcement.KeyId);
		KeyIdError.IsSet())
	{
		return FResult::Error(MoveTemp(KeyIdError.GetValue()));
	}
	if (Announcement.KeyId.IsEmpty())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
			TEXT("the announcement names no signing key id for its manifest")));
	}

	if (TOptional<FWoTBootstrapError> SequenceError = ReadUnsignedField(Root,
			TEXT("release_sequence"), TEXT("announcement"), Announcement.ReleaseSequence);
		SequenceError.IsSet())
	{
		return FResult::Error(MoveTemp(SequenceError.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> EpochError = ReadUnsignedField(Root,
			TEXT("key_ring_epoch"), TEXT("announcement"), Announcement.KeyRingEpoch);
		EpochError.IsSet())
	{
		return FResult::Error(MoveTemp(EpochError.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> GateError = ReadUnsigned32Field(Root,
			TEXT("minimum_gate_protocol"), TEXT("announcement"),
			Announcement.MinimumGateProtocol); GateError.IsSet())
	{
		return FResult::Error(MoveTemp(GateError.GetValue()));
	}

	if (TOptional<FWoTBootstrapError> RingError = ReadObjectRef(Root, TEXT("key_ring"),
			TEXT("announcement"), Announcement.KeyRing); RingError.IsSet())
	{
		return FResult::Error(MoveTemp(RingError.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> ManifestError = ReadObjectRef(Root, TEXT("manifest"),
			TEXT("announcement"), Announcement.Manifest); ManifestError.IsSet())
	{
		return FResult::Error(MoveTemp(ManifestError.GetValue()));
	}

	return FResult::Success(MoveTemp(Announcement));
}
