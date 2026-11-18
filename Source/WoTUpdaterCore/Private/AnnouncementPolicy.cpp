#include "WoTUpdaterCore/AnnouncementPolicy.h"


#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Ed25519.h"

namespace
{
FWoTBootstrapError WoTAnnouncementPolicyError(EWoTBootstrapError Code,
	FString Message, FString Path = FString())
{
	return MakeWoTBootstrapError(Code, MoveTemp(Message), MoveTemp(Path));
}

const TCHAR* WoTAnnouncementPolicyPlatform()
{
#if PLATFORM_WINDOWS
	return TEXT("Win64");
#elif PLATFORM_MAC
	return TEXT("Mac");
#else
#error "WoT announcement policy is defined for Win64 and Mac only"
#endif
}

TWoTBootstrapResult<FWoTEd25519PublicKey> WoTAnnouncementPolicyKey(
	const FWoTKeyRing& TrustedRing)
{
	using FResult = TWoTBootstrapResult<FWoTEd25519PublicKey>;
	if (TrustedRing.Keys.Num() != 1)
	{
		return FResult::Error(WoTAnnouncementPolicyError(EWoTBootstrapError::Crypto,
			TEXT("the announcement trust ring must contain exactly one dedicated key")));
	}
	const FWoTReleaseKey& Key = TrustedRing.Keys[0];
	if (Key.KeyId.IsEmpty() || TrustedRing.RevokedKeyIds.Contains(Key.KeyId))
	{
		return FResult::Error(WoTAnnouncementPolicyError(EWoTBootstrapError::Crypto,
			TEXT("the dedicated announcement key is unnamed or revoked"), Key.KeyId));
	}
	return FResult::Success(Key.PublicKey);
}

TWoTBootstrapResult<FWoTEd25519Signature> WoTAnnouncementPolicySignature(
	TConstArrayView<uint8> Bytes)
{
	using FResult = TWoTBootstrapResult<FWoTEd25519Signature>;
	if (Bytes.Num() != FWoTEd25519Signature::ByteCount)
	{
		return FResult::Error(WoTAnnouncementPolicyError(EWoTBootstrapError::Crypto,
			FString::Printf(TEXT("the detached announcement signature is %d bytes, not %d"),
				Bytes.Num(), FWoTEd25519Signature::ByteCount)));
	}
	FWoTEd25519Signature Signature;
	FMemory::Memcpy(Signature.Bytes, Bytes.GetData(), FWoTEd25519Signature::ByteCount);
	return FResult::Success(Signature);
}

TOptional<FWoTBootstrapError> WoTAnnouncementPolicyObjectRef(
	const FWoTCanonicalJsonValue& Parent, const TCHAR* Field,
	FWoTSignedObjectRef& OutRef)
{
	const FWoTCanonicalJsonValue* Object = nullptr;
	if (TOptional<FWoTBootstrapError> Error = ReadObjectField(
			Parent, Field, TEXT("announcement"), Object); Error.IsSet())
	{
		return Error;
	}
	static const TCHAR* const Fields[] = {TEXT("sha256"), TEXT("size"), TEXT("url")};
	const FString What = FString::Printf(TEXT("announcement.%s"), Field);
	if (TOptional<FWoTBootstrapError> Error =
			RequireExactMembers(*Object, Fields, *What); Error.IsSet())
	{
		return Error;
	}
	if (TOptional<FWoTBootstrapError> Error =
			ReadStringField(*Object, TEXT("url"), *What, OutRef.Url); Error.IsSet())
	{
		return Error;
	}
	if (!OutRef.Url.StartsWith(TEXT("https://"), ESearchCase::CaseSensitive))
	{
		return WoTAnnouncementPolicyError(EWoTBootstrapError::UnsafePath,
			FString::Printf(TEXT("%s.url is not HTTPS"), *What), OutRef.Url);
	}
	if (TOptional<FWoTBootstrapError> Error =
			ReadUnsignedField(*Object, TEXT("size"), *What, OutRef.Size); Error.IsSet())
	{
		return Error;
	}
	if (OutRef.Size == 0)
	{
		return WoTAnnouncementPolicyError(EWoTBootstrapError::InvalidNumber,
			FString::Printf(TEXT("%s.size is zero"), *What));
	}
	FString DigestHex;
	if (TOptional<FWoTBootstrapError> Error =
			ReadStringField(*Object, TEXT("sha256"), *What, DigestHex); Error.IsSet())
	{
		return Error;
	}
	TWoTBootstrapResult<FWoTSha256> Digest = FWoTSha256::FromHex(DigestHex);
	if (Digest.IsError())
	{
		return MoveTemp(Digest).GetError();
	}
	OutRef.Sha256 = Digest.GetValue();
	return {};
}

bool WoTAnnouncementPolicyAllowedField(FStringView Field)
{
	static const TCHAR* const Fields[] = {
		TEXT("announcement_domain"), TEXT("format"), TEXT("key_id"),
		TEXT("key_ring"), TEXT("key_ring_epoch"), TEXT("manifest"),
		TEXT("minimum_gate_protocol"), TEXT("platform"), TEXT("release_sequence")};
	for (const TCHAR* Expected : Fields)
	{
		if (Field.Equals(Expected, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

/**
 * Recover only authenticated ordering fields when the frozen envelope parser
 * failed solely because its manifest reference is missing or malformed.
 * This is called only after VerifyAnnouncementV1 authenticated the bytes.
 */
TWoTBootstrapResult<FWoTAnnouncementV1>
WoTAnnouncementPolicyParseUnusableManifest(TConstArrayView<uint8> Bytes,
	FStringView ExpectedDomain)
{
	using FResult = TWoTBootstrapResult<FWoTAnnouncementV1>;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(Bytes);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	if (!Root.IsObject())
	{
		return FResult::Error(WoTAnnouncementPolicyError(
			EWoTBootstrapError::UnknownField, TEXT("announcement is not an object")));
	}
	for (int32 Index = 0; Index < Root.Num(); ++Index)
	{
		if (!WoTAnnouncementPolicyAllowedField(Root.GetMemberKey(Index)))
		{
			return FResult::Error(WoTAnnouncementPolicyError(
				EWoTBootstrapError::UnknownField,
				TEXT("announcement contains an unknown field"),
				Root.GetMemberKey(Index)));
		}
	}
	static const TCHAR* const RequiredFields[] = {
		TEXT("announcement_domain"), TEXT("format"), TEXT("key_id"),
		TEXT("key_ring"), TEXT("key_ring_epoch"), TEXT("minimum_gate_protocol"),
		TEXT("platform"), TEXT("release_sequence")};
	for (const TCHAR* Required : RequiredFields)
	{
		if (Root.FindMember(Required) == nullptr)
		{
			return FResult::Error(WoTAnnouncementPolicyError(
				EWoTBootstrapError::UnknownField,
				TEXT("announcement is missing a required authority field"), Required));
		}
	}

	FWoTAnnouncementV1 Announcement;
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Root, TEXT("format"), TEXT("announcement"), Announcement.Format);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (!Announcement.Format.Equals(WoTAnnouncementV1Format,
			ESearchCase::CaseSensitive))
	{
		return FResult::Error(WoTAnnouncementPolicyError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the announcement format is not wot-announcement-v1")));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Root, TEXT("platform"), TEXT("announcement"), Announcement.Platform);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (!Announcement.Platform.Equals(WoTAnnouncementPolicyPlatform(),
			ESearchCase::CaseSensitive))
	{
		return FResult::Error(WoTAnnouncementPolicyError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the authenticated announcement is for another platform")));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(Root,
			TEXT("announcement_domain"), TEXT("announcement"),
			Announcement.AnnouncementDomain); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (!Announcement.AnnouncementDomain.Equals(FString(ExpectedDomain),
			ESearchCase::CaseSensitive))
	{
		return FResult::Error(WoTAnnouncementPolicyError(
			EWoTBootstrapError::UnsupportedProtocol,
			TEXT("the authenticated announcement is for another domain")));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadStringField(
			Root, TEXT("key_id"), TEXT("announcement"), Announcement.KeyId);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (Announcement.KeyId.IsEmpty())
	{
		return FResult::Error(WoTAnnouncementPolicyError(EWoTBootstrapError::Crypto,
			TEXT("the authenticated announcement names no release key")));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsignedField(Root,
			TEXT("release_sequence"), TEXT("announcement"),
			Announcement.ReleaseSequence); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsignedField(Root,
			TEXT("key_ring_epoch"), TEXT("announcement"), Announcement.KeyRingEpoch);
		Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadUnsigned32Field(Root,
			TEXT("minimum_gate_protocol"), TEXT("announcement"),
			Announcement.MinimumGateProtocol); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error =
			WoTAnnouncementPolicyObjectRef(Root, TEXT("key_ring"),
				Announcement.KeyRing); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	// This fallback is valid only when the manifest reference itself is the sole
	// unusable inner material. A valid reference means the original full parser
	// failed for some other reason and must remain refused.
	if (Root.FindMember(TEXT("manifest")) != nullptr)
	{
		FWoTSignedObjectRef Manifest;
		if (TOptional<FWoTBootstrapError> Error =
				WoTAnnouncementPolicyObjectRef(Root, TEXT("manifest"), Manifest);
			!Error.IsSet())
		{
			return FResult::Error(WoTAnnouncementPolicyError(
				EWoTBootstrapError::UnknownField,
				TEXT("the manifest reference is usable; another envelope field failed")));
		}
	}
	return FResult::Success(MoveTemp(Announcement));
}

FWoTUpdateAssessment WoTAnnouncementPolicyAssessment(EWoTUpdateDecision Decision)
{
	FWoTUpdateAssessment Assessment;
	Assessment.Decision = Decision;
	return Assessment;
}
}  // namespace

TWoTBootstrapResult<FWoTUpdateAssessment> AssessAnnouncement(
	IWoTAnnouncementTransport& Transport, const FWoTKeyRing& TrustedRing,
	FWoTUpdateStateRecord& State, const FWoTUpdaterStateRoot& Root,
	uint64 RunningReleaseSequence, bool bRecordState)
{
	using FResult = TWoTBootstrapResult<FWoTUpdateAssessment>;
	TArray<uint8> AnnouncementBytes;
	TArray<uint8> SignatureBytes;
	FWoTBootstrapStatus Fetched =
		Transport.FetchPair(AnnouncementBytes, SignatureBytes);
	if (Fetched.IsError())
	{
		return FResult::Success(WoTAnnouncementPolicyAssessment(
			EWoTUpdateDecision::ContinueCurrentAndLogUnavailable));
	}

	TWoTBootstrapResult<FWoTEd25519Signature> Signature =
		WoTAnnouncementPolicySignature(SignatureBytes);
	if (Signature.IsError())
	{
		return FResult::Error(MoveTemp(Signature).GetError());
	}
	TWoTBootstrapResult<FWoTEd25519PublicKey> AnnouncementKey =
		WoTAnnouncementPolicyKey(TrustedRing);
	if (AnnouncementKey.IsError())
	{
		return FResult::Error(MoveTemp(AnnouncementKey).GetError());
	}

	// VerifyAnnouncementV1 is deliberately the first consumer of network JSON.
	// It checks Ed25519 over the exact bytes before attempting canonical parsing.
	TWoTBootstrapResult<FWoTAnnouncementV1> Verified = VerifyAnnouncementV1(
		AnnouncementBytes, Signature.GetValue(), WoTAnnouncementPolicyPlatform(),
		WoT::Bootstrap::AnnouncementDomain, AnnouncementKey.GetValue());
	bool bManifestUsable = true;
	if (Verified.IsError())
	{
		// Crypto is the signature gate. Never parse or inspect bytes after it fails.
		if (Verified.GetError().Code == EWoTBootstrapError::Crypto)
		{
			return FResult::Error(MoveTemp(Verified).GetError());
		}
		TWoTBootstrapResult<FWoTAnnouncementV1> Partial =
			WoTAnnouncementPolicyParseUnusableManifest(AnnouncementBytes,
				WoT::Bootstrap::AnnouncementDomain);
		if (Partial.IsError())
		{
			return FResult::Error(MoveTemp(Verified).GetError());
		}
		Verified = MoveTemp(Partial);
		bManifestUsable = false;
	}

	FWoTAnnouncementV1 Announcement = MoveTemp(Verified).GetValue();
	const uint64 SequenceFloor = FMath::Max(
		RunningReleaseSequence, State.NewestAnnouncementSequence);
	if (Announcement.ReleaseSequence < SequenceFloor)
	{
		return FResult::Success(WoTAnnouncementPolicyAssessment(
			EWoTUpdateDecision::Current));
	}
	if (Announcement.ReleaseSequence == RunningReleaseSequence)
	{
		return FResult::Success(WoTAnnouncementPolicyAssessment(
			EWoTUpdateDecision::Current));
	}

	FString StateDiagnostic;
	if (bRecordState)
	{
		State.NewestAnnouncementSequence = FMath::Max(
			State.NewestAnnouncementSequence, Announcement.ReleaseSequence);
		const FWoTBootstrapStatus Saved =
			MergeAndWriteWoTUpdateState(Root, State, &StateDiagnostic);
		if (Saved.IsError() && StateDiagnostic.IsEmpty())
		{
			StateDiagnostic = Saved.GetError().ToString();
		}
	}

	if (!bManifestUsable
		|| Announcement.MinimumGateProtocol > WoT::Bootstrap::GateProtocol)
	{
		FWoTUpdateAssessment Assessment =
			WoTAnnouncementPolicyAssessment(EWoTUpdateDecision::StayBlocked);
		Assessment.NotifyOnlyDetail = MoveTemp(StateDiagnostic);
		return FResult::Success(MoveTemp(Assessment));
	}

	FWoTUpdateAssessment Assessment =
		WoTAnnouncementPolicyAssessment(EWoTUpdateDecision::MustStage);
	Assessment.Announcement.Emplace(MoveTemp(Announcement));
	Assessment.NotifyOnlyDetail = MoveTemp(StateDiagnostic);
	return FResult::Success(MoveTemp(Assessment));
}
