#include "WoTRetailReader/Contract.h"

#include "WoTRetailReader/Generated/CacheContractV3.generated.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Ed25519.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTBootstrapCore/KeyRing.h"

namespace
{
FWoTBootstrapError ContractError(EWoTBootstrapError Code, FString Message,
	FString Path)
{
	return MakeWoTBootstrapError(Code, MoveTemp(Message), MoveTemp(Path));
}

TOptional<FWoTBootstrapError> RequireContractShape(
	const FWoTCanonicalJsonValue& Object, TConstArrayView<const TCHAR*> Fields,
	const FString& Path)
{
	// Keep all artifact codecs on the shared closed-schema primitive. The explicit
	// scan below adds the stable machine-readable field path required by cache-v3.
	if (TOptional<FWoTBootstrapError> Shared =
			RequireExactMembers(Object, Fields, *Path); !Shared.IsSet())
	{
		return {};
	}
	if (!Object.IsObject())
	{
		return ContractError(EWoTBootstrapError::UnknownField,
			Path + TEXT(" must be an object"), Path);
	}
	for (int32 MemberIndex = 0; MemberIndex < Object.Num(); ++MemberIndex)
	{
		const FString& Key = Object.GetMemberKey(MemberIndex);
		bool bKnown = false;
		for (const TCHAR* Field : Fields)
		{
			bKnown |= Key.Equals(Field, ESearchCase::CaseSensitive);
		}
		if (!bKnown)
		{
			const FString FieldPath = Path + TEXT(".") + Key;
			return ContractError(EWoTBootstrapError::UnknownField,
				FieldPath + TEXT(" is an unknown field"), FieldPath);
		}
	}
	for (const TCHAR* Field : Fields)
	{
		if (Object.FindMember(Field) == nullptr)
		{
			const FString FieldPath = Path + TEXT(".") + Field;
			return ContractError(EWoTBootstrapError::UnknownField,
				FieldPath + TEXT(" is missing"), FieldPath);
		}
	}
	return {};
}

TOptional<FWoTBootstrapError> ReadContractString(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const FString& Path,
	FString& OutValue)
{
	const FString FieldPath = Path + TEXT(".") + Field;
	if (TOptional<FWoTBootstrapError> Shared =
			ReadStringField(Object, Field, *Path, OutValue); Shared.IsSet())
	{
		return ContractError(Shared.GetValue().Code,
			FieldPath + TEXT(" must be a string"), FieldPath);
	}
	if (OutValue.IsEmpty())
	{
		return ContractError(EWoTBootstrapError::UnknownField,
			FieldPath + TEXT(" must not be empty"), FieldPath);
	}
	return {};
}

TOptional<FWoTBootstrapError> ValidateSourceExportIdentity(
	const FString& Value, const FString& FieldPath)
{
	TArray<FString> Components;
	Value.ParseIntoArray(Components, TEXT("."), false);
	for (const FString& Component : Components)
	{
		if (Component.IsEmpty())
		{
			return ContractError(EWoTBootstrapError::UnknownField,
				FieldPath + TEXT(" must not contain an empty export identity component"),
				FieldPath);
		}
	}
	return {};
}

TOptional<FWoTBootstrapError> ValidateSourcePackageIdentity(
	const FString& Value, const FString& FieldPath)
{
	if (Value.Contains(TEXT("\\"), ESearchCase::CaseSensitive))
	{
		return ContractError(EWoTBootstrapError::UnknownField,
			FieldPath + TEXT(" must use forward-slash package components"), FieldPath);
	}
	TArray<FString> Components;
	Value.ParseIntoArray(Components, TEXT("/"), false);
	for (const FString& Component : Components)
	{
		if (Component.IsEmpty())
		{
			return ContractError(EWoTBootstrapError::UnknownField,
				FieldPath + TEXT(" must not contain an empty package component"), FieldPath);
		}
	}
	if (Components.IsEmpty())
	{
		return ContractError(EWoTBootstrapError::UnknownField,
			FieldPath + TEXT(" must name a package component"), FieldPath);
	}

	// The final component may carry one file extension. The extension is not
	// part of the UE1 package identity; every identity-bearing component is.
	FString LeafIdentity = Components.Last();
	int32 ExtensionDot = INDEX_NONE;
	if (LeafIdentity.FindLastChar(TEXT('.'), ExtensionDot))
	{
		if (ExtensionDot == 0 || ExtensionDot == LeafIdentity.Len() - 1)
		{
			return ContractError(EWoTBootstrapError::UnknownField,
				FieldPath + TEXT(" has an invalid package filename extension"), FieldPath);
		}
		LeafIdentity.LeftInline(ExtensionDot);
	}
	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num() - 1;
		++ComponentIndex)
	{
		if (Components[ComponentIndex].Contains(TEXT("."), ESearchCase::CaseSensitive))
		{
			return ContractError(EWoTBootstrapError::UnknownField,
				FieldPath + TEXT(" must not contain a dotted package identity component"),
				FieldPath);
		}
	}
	if (LeafIdentity.Contains(TEXT("."), ESearchCase::CaseSensitive))
	{
		return ContractError(EWoTBootstrapError::UnknownField,
			FieldPath + TEXT(" must not contain a dotted package identity component"),
			FieldPath);
	}
	return {};
}

TOptional<FWoTBootstrapError> ReadContractUnsigned(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const FString& Path,
	uint64& OutValue)
{
	const FString FieldPath = Path + TEXT(".") + Field;
	if (TOptional<FWoTBootstrapError> Shared =
			ReadUnsignedField(Object, Field, *Path, OutValue); Shared.IsSet())
	{
		return ContractError(Shared.GetValue().Code,
			FieldPath + TEXT(" must be a non-negative integer"), FieldPath);
	}
	return {};
}

TOptional<FWoTBootstrapError> ReadContractObject(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const FString& Path,
	const FWoTCanonicalJsonValue*& OutValue)
{
	const FString FieldPath = Path + TEXT(".") + Field;
	if (TOptional<FWoTBootstrapError> Shared =
			ReadObjectField(Object, Field, *Path, OutValue); Shared.IsSet())
	{
		return ContractError(Shared.GetValue().Code,
			FieldPath + TEXT(" must be an object"), FieldPath);
	}
	return {};
}

TOptional<FWoTBootstrapError> ReadContractArray(
	const FWoTCanonicalJsonValue& Object, const TCHAR* Field, const FString& Path,
	const FWoTCanonicalJsonValue*& OutValue)
{
	const FString FieldPath = Path + TEXT(".") + Field;
	OutValue = Object.FindMember(Field);
	if (OutValue == nullptr || !OutValue->IsArray())
	{
		return ContractError(EWoTBootstrapError::UnknownField,
			FieldPath + TEXT(" must be an array"), FieldPath);
	}
	return {};
}

TOptional<EWoTFactDomain> ParseDomain(FStringView Name)
{
	static_assert(UE_ARRAY_COUNT(WoT::Retail::Generated::DomainNames)
		== static_cast<int32>(EWoTFactDomain::FrontEndWorld) + 1,
		"generated cache-v3 domains must match EWoTFactDomain");
	for (int32 DomainIndex = 0;
		DomainIndex < UE_ARRAY_COUNT(WoT::Retail::Generated::DomainNames); ++DomainIndex)
	{
		if (Name == WoT::Retail::Generated::DomainNames[DomainIndex])
		{
			return static_cast<EWoTFactDomain>(DomainIndex);
		}
	}
	return {};
}

FString AuditKey(FStringView Consumer, FStringView Rule)
{
	return FString(Consumer) + TEXT("\x1f") + FString(Rule);
}

int32 CompareUtf8(FStringView Left, FStringView Right)
{
	const FTCHARToUTF8 LeftUtf8(Left.GetData(), Left.Len());
	const FTCHARToUTF8 RightUtf8(Right.GetData(), Right.Len());
	const int32 Common = FMath::Min(LeftUtf8.Length(), RightUtf8.Length());
	const int32 Compared = FMemory::Memcmp(LeftUtf8.Get(), RightUtf8.Get(), Common);
	return Compared != 0 ? Compared : LeftUtf8.Length() - RightUtf8.Length();
}

FWoTSha256 StartupCriticalLogicalIdsDigest(TConstArrayView<FString> LogicalIds)
{
	FWoTSha256Hasher Hasher;
	for (const FString& LogicalId : LogicalIds)
	{
		const FTCHARToUTF8 Utf8(*LogicalId);
		const uint64 Length = static_cast<uint64>(Utf8.Length());
		uint8 Prefix[8];
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Prefix); ++Index)
		{
			Prefix[Index] = static_cast<uint8>((Length >> (Index * 8)) & 0xff);
		}
		Hasher.Update(MakeArrayView(Prefix));
		Hasher.Update(TConstArrayView<uint8>(
			reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	}
	return Hasher.Finish();
}
}  // namespace

FWoTBootstrapStatus ValidateFactClosure(const FWoTRetailContract& Contract)
{
	TSet<FString> AuditBindings;
	for (int32 AuditIndex = 0; AuditIndex < Contract.AuditSet.Num(); ++AuditIndex)
	{
		const FWoTRetailAuditBinding& Binding = Contract.AuditSet[AuditIndex];
		const FString Path = FString::Printf(TEXT("contract.payload.audit_set[%d]"), AuditIndex);
		if (Binding.RuntimeConsumer.IsEmpty())
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
				Path + TEXT(".runtime_consumer must not be empty"),
				Path + TEXT(".runtime_consumer")));
		}
		if (Binding.AuditRule.IsEmpty())
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
				Path + TEXT(".audit_rule must not be empty"), Path + TEXT(".audit_rule")));
		}
		const FString Key = AuditKey(Binding.RuntimeConsumer, Binding.AuditRule);
		if (AuditBindings.Contains(Key))
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::DuplicateField,
				Path + TEXT(" duplicates a consumer/audit pair"), Path));
		}
		AuditBindings.Add(Key);
	}

	TSet<FString> FactIds;
	TMap<FString, int32> OutputCounts;
	for (int32 FactIndex = 0; FactIndex < Contract.Facts.Num(); ++FactIndex)
	{
		const FWoTRequiredFact& Fact = Contract.Facts[FactIndex];
		const FString Path = FString::Printf(TEXT("contract.payload.facts[%d]"), FactIndex);
		if (Fact.FactId.IsEmpty())
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
				Path + TEXT(".fact_id must not be empty"), Path + TEXT(".fact_id")));
		}
		if (FactIds.Contains(Fact.FactId))
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::DuplicateField,
				Path + TEXT(".fact_id duplicates '") + Fact.FactId + TEXT("'"),
				Path + TEXT(".fact_id")));
		}
		FactIds.Add(Fact.FactId);
		const struct { const FString* Value; const TCHAR* Field; } RequiredStrings[] = {
			{&Fact.Importer, TEXT("importer")},
			{&Fact.RuntimeConsumer, TEXT("runtime_consumer")},
			{&Fact.AuditRule, TEXT("audit_rule")},
			{&Fact.SourcePath, TEXT("source.package")},
			{&Fact.SourceExport, TEXT("source.export")},
		};
		for (const auto& Required : RequiredStrings)
		{
			if (Required.Value->IsEmpty())
			{
				const FString FieldPath = Path + TEXT(".") + Required.Field;
				return FWoTBootstrapStatus::Error(ContractError(
					EWoTBootstrapError::UnknownField,
					FieldPath + TEXT(" must not be empty"), FieldPath));
			}
		}
		if (TOptional<FWoTBootstrapError> Error = ValidateSourcePackageIdentity(
				Fact.SourcePath, Path + TEXT(".source.package")); Error.IsSet())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ValidateSourceExportIdentity(
				Fact.SourceExport, Path + TEXT(".source.export")); Error.IsSet())
		{
			return FWoTBootstrapStatus::Error(MoveTemp(Error.GetValue()));
		}
		if (!AuditBindings.Contains(AuditKey(Fact.RuntimeConsumer, Fact.AuditRule)))
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
				Path + TEXT(".runtime_consumer is absent from the matching audit set"),
				Path + TEXT(".runtime_consumer")));
		}
		const bool bGeneric = Fact.Importer.Equals(
			WoT::Retail::Generated::GenericImporter, ESearchCase::CaseSensitive);
		if (bGeneric && Fact.Outputs.Num() != 0)
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
				Path + TEXT(".outputs must be empty for a generic authored fact"),
				Path + TEXT(".outputs")));
		}
		if (!bGeneric && Fact.Outputs.Num() == 0)
		{
			return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
				Path + TEXT(".outputs must name a typed importer output"),
				Path + TEXT(".outputs")));
		}
		TSet<FString> LogicalIds;
		for (int32 OutputIndex = 0; OutputIndex < Fact.Outputs.Num(); ++OutputIndex)
		{
			const FWoTRequiredFactOutput& Output = Fact.Outputs[OutputIndex];
			const FString OutputPath = FString::Printf(TEXT("%s.outputs[%d]"), *Path, OutputIndex);
			if (Output.LogicalId.IsEmpty())
			{
				return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
					OutputPath + TEXT(".logical_id must not be empty"),
					OutputPath + TEXT(".logical_id")));
			}
			if (Output.Schema.IsEmpty())
			{
				return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
					OutputPath + TEXT(".schema must not be empty"),
					OutputPath + TEXT(".schema")));
			}
			if (Output.MaxDecodedSize == 0)
			{
				return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::InvalidNumber,
					OutputPath + TEXT(".max_decoded_size must be greater than zero"),
					OutputPath + TEXT(".max_decoded_size")));
			}
			if (LogicalIds.Contains(Output.LogicalId))
			{
				return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::DuplicateField,
					OutputPath + TEXT(".logical_id is duplicated"),
					OutputPath + TEXT(".logical_id")));
			}
			LogicalIds.Add(Output.LogicalId);
			OutputCounts.FindOrAdd(Output.LogicalId) += 1;
		}
	}
	const FString ClosurePath = TEXT("contract.payload.startup_critical_logical_ids");
	if (Contract.StartupCriticalLogicalIds.IsEmpty())
	{
		return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::UnknownField,
			ClosurePath + TEXT(" must not be empty"), ClosurePath));
	}
	for (int32 Index = 0; Index < Contract.StartupCriticalLogicalIds.Num(); ++Index)
	{
		const FString& LogicalId = Contract.StartupCriticalLogicalIds[Index];
		const FString Path = FString::Printf(TEXT("%s[%d]"), *ClosurePath, Index);
		if (LogicalId.IsEmpty())
		{
			return FWoTBootstrapStatus::Error(ContractError(
				EWoTBootstrapError::UnknownField, Path + TEXT(" must not be empty"), Path));
		}
		if (Index > 0 && CompareUtf8(Contract.StartupCriticalLogicalIds[Index - 1],
			LogicalId) >= 0)
		{
			return FWoTBootstrapStatus::Error(ContractError(
				EWoTBootstrapError::DuplicateField,
				Path + TEXT(" must be strictly sorted and unique"), Path));
		}
		const int32* Count = OutputCounts.Find(LogicalId);
		if (Count == nullptr || *Count != 1)
		{
			return FWoTBootstrapStatus::Error(ContractError(
				EWoTBootstrapError::UnknownField,
				Path + TEXT(" must resolve to exactly one declared fact output"), Path));
		}
	}
	const FString DigestPath =
		TEXT("contract.payload.startup_critical_logical_ids_sha256");
	if (StartupCriticalLogicalIdsDigest(Contract.StartupCriticalLogicalIds)
		!= Contract.StartupCriticalLogicalIdsSha256)
	{
		return FWoTBootstrapStatus::Error(ContractError(EWoTBootstrapError::Crypto,
			DigestPath + TEXT(" does not match the canonical logical-ID closure"),
			DigestPath));
	}
	return FWoTBootstrapStatus::Success();
}

namespace
{
TWoTBootstrapResult<FWoTRetailContract> ParseRetailContractPayloadValue(
	const FWoTCanonicalJsonValue& Payload, const FString& RootPath)
{
	using FResult = TWoTBootstrapResult<FWoTRetailContract>;
	if (TOptional<FWoTBootstrapError> Error = RequireContractShape(Payload,
			WoT::Retail::Generated::PayloadFields, RootPath); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	FWoTRetailContract Contract;
	if (TOptional<FWoTBootstrapError> Error = ReadContractString(Payload,
			TEXT("format"), RootPath, Contract.Format); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (!Contract.Format.Equals(WoT::Retail::Generated::Format, ESearchCase::CaseSensitive))
	{
		const FString FieldPath = RootPath + TEXT(".format");
		return FResult::Error(ContractError(EWoTBootstrapError::UnsupportedProtocol,
			FieldPath + TEXT(" has an unsupported value"), FieldPath));
	}
	uint64 ContractVersion = 0;
	if (TOptional<FWoTBootstrapError> Error = ReadContractUnsigned(Payload,
			TEXT("contract_version"), RootPath, ContractVersion); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (ContractVersion != WoT::Retail::Generated::ContractVersion)
	{
		const FString FieldPath = RootPath + TEXT(".contract_version");
		return FResult::Error(ContractError(EWoTBootstrapError::UnsupportedProtocol,
			FieldPath + TEXT(" has an unsupported value"), FieldPath));
	}
	Contract.ContractVersion = static_cast<uint32>(ContractVersion);
	if (TOptional<FWoTBootstrapError> Error = ReadContractString(Payload,
			TEXT("contract_id"), RootPath, Contract.ContractId); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadContractString(Payload,
			TEXT("key_id"), RootPath, Contract.KeyId); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	if (TOptional<FWoTBootstrapError> Error = ReadContractUnsigned(Payload,
			TEXT("release_sequence"), RootPath, Contract.ReleaseSequence); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	const FWoTCanonicalJsonValue* StartupCriticalLogicalIds = nullptr;
	if (TOptional<FWoTBootstrapError> Error = ReadContractArray(Payload,
			TEXT("startup_critical_logical_ids"), RootPath,
			StartupCriticalLogicalIds); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	for (int32 Index = 0; Index < StartupCriticalLogicalIds->Num(); ++Index)
	{
		const FString Path = FString::Printf(
			TEXT("%s.startup_critical_logical_ids[%d]"), *RootPath, Index);
		const FWoTCanonicalJsonValue& Value = StartupCriticalLogicalIds->GetElement(Index);
		if (Value.GetKind() != EWoTCanonicalJsonKind::String
			|| Value.GetString().IsEmpty())
		{
			return FResult::Error(ContractError(EWoTBootstrapError::UnknownField,
				Path + TEXT(" must be a non-empty string"), Path));
		}
		Contract.StartupCriticalLogicalIds.Add(Value.GetString());
	}
	FString StartupCriticalDigestHex;
	if (TOptional<FWoTBootstrapError> Error = ReadContractString(Payload,
			TEXT("startup_critical_logical_ids_sha256"), RootPath,
			StartupCriticalDigestHex); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	TWoTBootstrapResult<FWoTSha256> StartupCriticalDigest =
		FWoTSha256::FromHex(StartupCriticalDigestHex);
	if (StartupCriticalDigest.IsError())
	{
		const FString Path = RootPath
			+ TEXT(".startup_critical_logical_ids_sha256");
		return FResult::Error(ContractError(EWoTBootstrapError::UnknownField,
			Path + TEXT(" must be 64 lower-case hex characters"), Path));
	}
	Contract.StartupCriticalLogicalIdsSha256 = StartupCriticalDigest.GetValue();

	const FWoTCanonicalJsonValue* Audits = nullptr;
	if (TOptional<FWoTBootstrapError> Error = ReadContractArray(Payload,
			TEXT("audit_set"), RootPath, Audits); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	for (int32 AuditIndex = 0; AuditIndex < Audits->Num(); ++AuditIndex)
	{
		const FWoTCanonicalJsonValue& AuditValue = Audits->GetElement(AuditIndex);
		const FString AuditPath = FString::Printf(
			TEXT("%s.audit_set[%d]"), *RootPath, AuditIndex);
		if (TOptional<FWoTBootstrapError> Error = RequireContractShape(AuditValue,
				WoT::Retail::Generated::AuditFields, AuditPath); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		FWoTRetailAuditBinding Audit;
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(AuditValue,
				TEXT("audit_rule"), AuditPath, Audit.AuditRule); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(AuditValue,
				TEXT("runtime_consumer"), AuditPath, Audit.RuntimeConsumer); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		Contract.AuditSet.Add(MoveTemp(Audit));
	}

	const FWoTCanonicalJsonValue* Facts = nullptr;
	if (TOptional<FWoTBootstrapError> Error = ReadContractArray(Payload,
			TEXT("facts"), RootPath, Facts); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	for (int32 FactIndex = 0; FactIndex < Facts->Num(); ++FactIndex)
	{
		const FWoTCanonicalJsonValue& FactValue = Facts->GetElement(FactIndex);
		const FString FactPath = FString::Printf(
			TEXT("%s.facts[%d]"), *RootPath, FactIndex);
		if (TOptional<FWoTBootstrapError> Error = RequireContractShape(FactValue,
				WoT::Retail::Generated::FactFields, FactPath); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		FWoTRequiredFact Fact;
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(FactValue,
				TEXT("fact_id"), FactPath, Fact.FactId); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		FString DomainName;
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(FactValue,
				TEXT("domain"), FactPath, DomainName); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		TOptional<EWoTFactDomain> Domain = ParseDomain(DomainName);
		if (!Domain.IsSet())
		{
			const FString FieldPath = FactPath + TEXT(".domain");
			return FResult::Error(ContractError(EWoTBootstrapError::UnsupportedProtocol,
				FieldPath + TEXT(" is unknown"), FieldPath));
		}
		Fact.Domain = Domain.GetValue();
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(FactValue,
				TEXT("importer"), FactPath, Fact.Importer); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(FactValue,
				TEXT("runtime_consumer"), FactPath, Fact.RuntimeConsumer); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(FactValue,
				TEXT("audit_rule"), FactPath, Fact.AuditRule); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}

		const FWoTCanonicalJsonValue* Source = nullptr;
		if (TOptional<FWoTBootstrapError> Error = ReadContractObject(FactValue,
				TEXT("source"), FactPath, Source); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		const FString SourceObjectPath = FactPath + TEXT(".source");
		if (TOptional<FWoTBootstrapError> Error = RequireContractShape(*Source,
				WoT::Retail::Generated::SourceFields, SourceObjectPath); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(*Source,
				TEXT("package"), SourceObjectPath, Fact.SourcePath); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		if (TOptional<FWoTBootstrapError> Error = ReadContractString(*Source,
				TEXT("export"), SourceObjectPath, Fact.SourceExport); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}

		const FWoTCanonicalJsonValue* Outputs = nullptr;
		if (TOptional<FWoTBootstrapError> Error = ReadContractArray(FactValue,
				TEXT("outputs"), FactPath, Outputs); Error.IsSet())
		{
			return FResult::Error(MoveTemp(Error.GetValue()));
		}
		for (int32 OutputIndex = 0; OutputIndex < Outputs->Num(); ++OutputIndex)
		{
			const FWoTCanonicalJsonValue& OutputValue = Outputs->GetElement(OutputIndex);
			const FString OutputPath = FString::Printf(
				TEXT("%s.outputs[%d]"), *FactPath, OutputIndex);
			if (TOptional<FWoTBootstrapError> Error = RequireContractShape(OutputValue,
					WoT::Retail::Generated::OutputFields, OutputPath); Error.IsSet())
			{
				return FResult::Error(MoveTemp(Error.GetValue()));
			}
			FWoTRequiredFactOutput Output;
			if (TOptional<FWoTBootstrapError> Error = ReadContractString(OutputValue,
					TEXT("logical_id"), OutputPath, Output.LogicalId); Error.IsSet())
			{
				return FResult::Error(MoveTemp(Error.GetValue()));
			}
			if (TOptional<FWoTBootstrapError> Error = ReadContractString(OutputValue,
					TEXT("schema"), OutputPath, Output.Schema); Error.IsSet())
			{
				return FResult::Error(MoveTemp(Error.GetValue()));
			}
			if (TOptional<FWoTBootstrapError> Error = ReadContractUnsigned(OutputValue,
					TEXT("max_decoded_size"), OutputPath, Output.MaxDecodedSize);
				Error.IsSet())
			{
				return FResult::Error(MoveTemp(Error.GetValue()));
			}
			if (Output.MaxDecodedSize == 0)
			{
				const FString FieldPath = OutputPath + TEXT(".max_decoded_size");
				return FResult::Error(ContractError(EWoTBootstrapError::InvalidNumber,
					FieldPath + TEXT(" must be greater than zero"), FieldPath));
			}
			Fact.Outputs.Add(MoveTemp(Output));
		}
		Contract.Facts.Add(MoveTemp(Fact));
	}

	FWoTBootstrapStatus Closure = ValidateFactClosure(Contract);
	if (Closure.IsError())
	{
		FWoTBootstrapError Error = MoveTemp(Closure).GetError();
		if (RootPath == TEXT("payload")
			&& Error.Path.StartsWith(TEXT("contract.payload"), ESearchCase::CaseSensitive))
		{
			Error.Path = RootPath + Error.Path.RightChop(16);
		}
		return FResult::Error(MoveTemp(Error));
	}
	return FResult::Success(MoveTemp(Contract));
}
}  // namespace

TWoTBootstrapResult<FWoTRetailContract> ParseRetailContractPayload(
	TArrayView<const uint8> CanonicalPayloadBytes)
{
	using FResult = TWoTBootstrapResult<FWoTRetailContract>;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(CanonicalPayloadBytes);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	return ParseRetailContractPayloadValue(Parsed.GetValue(), TEXT("payload"));
}

TWoTBootstrapResult<FWoTRetailContract> ParseRetailContract(
	TArrayView<const uint8> CanonicalBytes, const FWoTKeyRing& Keys)
{
	using FResult = TWoTBootstrapResult<FWoTRetailContract>;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed = ParseCanonicalJson(CanonicalBytes);
	if (Parsed.IsError())
	{
		return FResult::Error(MoveTemp(Parsed).GetError());
	}
	const FWoTCanonicalJsonValue& Envelope = Parsed.GetValue();
	if (TOptional<FWoTBootstrapError> Error = RequireContractShape(Envelope,
			WoT::Retail::Generated::EnvelopeFields, TEXT("contract")); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	const FWoTCanonicalJsonValue* Payload = nullptr;
	if (TOptional<FWoTBootstrapError> Error = ReadContractObject(
			Envelope, TEXT("payload"), TEXT("contract"), Payload); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}

	TWoTBootstrapResult<FWoTRetailContract> Structural =
		ParseRetailContractPayloadValue(*Payload, TEXT("contract.payload"));
	if (Structural.IsError())
	{
		return FResult::Error(MoveTemp(Structural).GetError());
	}
	FWoTRetailContract Contract = MoveTemp(Structural).GetValue();

	FString SignatureHex;
	if (TOptional<FWoTBootstrapError> Error = ReadContractString(Envelope,
			TEXT("signature"), TEXT("contract"), SignatureHex); Error.IsSet())
	{
		return FResult::Error(MoveTemp(Error.GetValue()));
	}
	TWoTBootstrapResult<FWoTEd25519Signature> Signature =
		FWoTEd25519Signature::FromHex(SignatureHex);
	if (Signature.IsError())
	{
		FWoTBootstrapError Error = MoveTemp(Signature).GetError();
		Error.Path = TEXT("contract.signature");
		return FResult::Error(MoveTemp(Error));
	}
	TWoTBootstrapResult<FWoTReleaseKey> SigningKey =
		SelectReleaseKey(Keys, Contract.KeyId, Contract.ReleaseSequence);
	if (SigningKey.IsError())
	{
		return FResult::Error(MoveTemp(SigningKey).GetError());
	}
	TWoTBootstrapResult<TArray<uint8>> PayloadBytes = WriteCanonicalJson(*Payload);
	if (PayloadBytes.IsError())
	{
		return FResult::Error(MoveTemp(PayloadBytes).GetError());
	}
	TWoTBootstrapResult<bool> Verified = VerifyEd25519(SigningKey.GetValue().PublicKey,
		EWoTSignatureContext::CacheContractV3, PayloadBytes.GetValue(), Signature.GetValue());
	if (Verified.IsError())
	{
		return FResult::Error(MoveTemp(Verified).GetError());
	}
	if (!Verified.GetValue())
	{
		return FResult::Error(ContractError(EWoTBootstrapError::Crypto,
			TEXT("cache contract signature does not verify under the selected release key"),
			TEXT("contract.signature")));
	}
	return FResult::Success(MoveTemp(Contract));
}
