#include "WoTBootstrapCore/KeyRing.h"


TWoTBootstrapResult<FWoTKeyRing> ParseKeyRing(const FWoTCanonicalJsonValue& Document,
	FStringView ExpectedFormat)
{
	using FResult = TWoTBootstrapResult<FWoTKeyRing>;

	static const TCHAR* const RingFields[] = {
		TEXT("epoch"), TEXT("format"), TEXT("keys"), TEXT("revoked_key_ids")};
	if (TOptional<FWoTBootstrapError> SchemaError =
			RequireExactMembers(Document, RingFields, TEXT("key ring")); SchemaError.IsSet())
	{
		return FResult::Error(MoveTemp(SchemaError.GetValue()));
	}

	FWoTKeyRing Ring;
	if (TOptional<FWoTBootstrapError> FormatError =
			ReadStringField(Document, TEXT("format"), TEXT("key ring"), Ring.Format);
		FormatError.IsSet())
	{
		return FResult::Error(MoveTemp(FormatError.GetValue()));
	}
	if (!Ring.Format.Equals(FString(ExpectedFormat), ESearchCase::CaseSensitive))
	{
		// Covers two different faults with one rule: a format this build was never
		// written against, and a well-formed ring of the WRONG ROLE. Refusing both
		// is why a release ring cannot be presented where a root ring is expected.
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnsupportedProtocol,
			FString::Printf(TEXT("key ring format '%s' is not '%.*s'"),
				*Ring.Format, ExpectedFormat.Len(), ExpectedFormat.GetData())));
	}
	if (TOptional<FWoTBootstrapError> EpochError =
			ReadUnsignedField(Document, TEXT("epoch"), TEXT("key ring"), Ring.Epoch);
		EpochError.IsSet())
	{
		return FResult::Error(MoveTemp(EpochError.GetValue()));
	}

	const FWoTCanonicalJsonValue* const Keys = Document.FindMember(TEXT("keys"));
	if (Keys == nullptr || !Keys->IsArray())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("key ring field 'keys' is not an array")));
	}

	static const TCHAR* const KeyFields[] = {
		TEXT("first_sequence"), TEXT("key_id"), TEXT("last_sequence"), TEXT("public_key")};
	TSet<FString> SeenKeyIds;
	for (int32 Index = 0; Index < Keys->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Entry = Keys->GetElement(Index);
		if (TOptional<FWoTBootstrapError> SchemaError =
				RequireExactMembers(Entry, KeyFields, TEXT("release key")); SchemaError.IsSet())
		{
			return FResult::Error(MoveTemp(SchemaError.GetValue()));
		}

		FWoTReleaseKey Key;
		if (TOptional<FWoTBootstrapError> IdError =
				ReadStringField(Entry, TEXT("key_id"), TEXT("release key"), Key.KeyId);
			IdError.IsSet())
		{
			return FResult::Error(MoveTemp(IdError.GetValue()));
		}
		if (Key.KeyId.IsEmpty())
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
				TEXT("a release key id must not be empty")));
		}
		if (SeenKeyIds.Contains(Key.KeyId))
		{
			// Two entries with one id would make "which key signed this" ambiguous.
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::DuplicateField,
				FString::Printf(TEXT("release key id '%s' appears twice"), *Key.KeyId)));
		}
		SeenKeyIds.Add(Key.KeyId);

		FString PublicKeyHex;
		if (TOptional<FWoTBootstrapError> KeyError =
				ReadStringField(Entry, TEXT("public_key"), TEXT("release key"), PublicKeyHex);
			KeyError.IsSet())
		{
			return FResult::Error(MoveTemp(KeyError.GetValue()));
		}
		TWoTBootstrapResult<FWoTEd25519PublicKey> PublicKey =
			FWoTEd25519PublicKey::FromHex(PublicKeyHex);
		if (PublicKey.IsError())
		{
			return FResult::Error(MoveTemp(PublicKey).GetError());
		}
		Key.PublicKey = PublicKey.GetValue();

		if (TOptional<FWoTBootstrapError> FirstError = ReadUnsignedField(Entry,
				TEXT("first_sequence"), TEXT("release key"), Key.FirstSequence);
			FirstError.IsSet())
		{
			return FResult::Error(MoveTemp(FirstError.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> LastError = ReadUnsignedField(Entry,
				TEXT("last_sequence"), TEXT("release key"), Key.LastSequence);
			LastError.IsSet())
		{
			return FResult::Error(MoveTemp(LastError.GetValue()));
		}
		if (Key.FirstSequence > Key.LastSequence)
		{
			// An inverted window trusts nothing, so it is more likely a mistake in
			// the ceremony than an intentional restriction. Refuse it loudly.
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
				FString::Printf(TEXT("release key '%s' has an inverted sequence range"),
					*Key.KeyId)));
		}

		Ring.Keys.Add(MoveTemp(Key));
	}

	const FWoTCanonicalJsonValue* const Revoked = Document.FindMember(TEXT("revoked_key_ids"));
	if (Revoked == nullptr || !Revoked->IsArray())
	{
		return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
			TEXT("key ring field 'revoked_key_ids' is not an array")));
	}
	for (int32 Index = 0; Index < Revoked->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Entry = Revoked->GetElement(Index);
		if (Entry.GetKind() != EWoTCanonicalJsonKind::String)
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::UnknownField,
				TEXT("every revoked key id must be a string")));
		}
		Ring.RevokedKeyIds.Add(Entry.GetString());
	}

	return FResult::Success(MoveTemp(Ring));
}

TWoTBootstrapResult<FWoTReleaseKey> SelectReleaseKey(const FWoTKeyRing& Ring,
	FStringView KeyId, uint64 ReleaseSequence)
{
	using FResult = TWoTBootstrapResult<FWoTReleaseKey>;

	// Revocation is checked before presence: a revoked id must stay unusable even
	// while its entry remains in the ring, or revoking would mean rewriting it.
	for (const FString& Revoked : Ring.RevokedKeyIds)
	{
		if (Revoked.Equals(FString(KeyId), ESearchCase::CaseSensitive))
		{
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
				FString::Printf(TEXT("release key '%.*s' is revoked"),
					KeyId.Len(), KeyId.GetData())));
		}
	}

	for (const FWoTReleaseKey& Key : Ring.Keys)
	{
		if (!Key.KeyId.Equals(FString(KeyId), ESearchCase::CaseSensitive))
		{
			continue;
		}
		if (ReleaseSequence < Key.FirstSequence || ReleaseSequence > Key.LastSequence)
		{
			// The window is inclusive on both ends, and there is no fallback to
			// another key: a retired key must not sign a later release.
			return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
				FString::Printf(
					TEXT("release key '%s' covers [%llu, %llu], not sequence %llu"),
					*Key.KeyId, Key.FirstSequence, Key.LastSequence, ReleaseSequence)));
		}
		return FResult::Success(Key);
	}

	return FResult::Error(MakeWoTBootstrapError(EWoTBootstrapError::Crypto,
		FString::Printf(TEXT("no release key '%.*s' in the ring"),
			KeyId.Len(), KeyId.GetData())));
}

FWoTBootstrapStatus RequireRingEpochAtLeast(const FWoTKeyRing& Ring, uint64 MinimumEpoch)
{
	if (Ring.Epoch < MinimumEpoch)
	{
		// A correctly signed ring at an older epoch is exactly what an attacker
		// replays to reinstate a revoked key, so it is a rollback, not staleness.
		return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(EWoTBootstrapError::Rollback,
			FString::Printf(TEXT("key ring epoch %llu is older than the installed %llu"),
				Ring.Epoch, MinimumEpoch)));
	}
	return FWoTBootstrapStatus::Success();
}
