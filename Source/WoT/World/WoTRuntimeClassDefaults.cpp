#include "WoTRuntimeClassDefaults.h"

#include "WoTRuntimeWorldProperties.h"

#include "Containers/StringConv.h"

namespace
{
class FClassDefaultsCursor
{
public:
	explicit FClassDefaultsCursor(TConstArrayView<uint8> InBytes)
		: Bytes(InBytes) {}
	bool Skip(uint32 Count)
	{
		if (!CanRead(Count)) return false;
		Offset += static_cast<int32>(Count); return true;
	}
	bool ReadU8(uint8& Out)
	{
		if (!CanRead(1)) return false;
		Out = Bytes[Offset++]; return true;
	}
	bool ReadU16(uint16& Out)
	{
		if (!CanRead(2)) return false;
		Out = static_cast<uint16>(Bytes[Offset])
			| (static_cast<uint16>(Bytes[Offset + 1]) << 8);
		Offset += 2; return true;
	}
	bool ReadU32(uint32& Out)
	{
		if (!CanRead(4)) return false;
		Out = static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4; return true;
	}
	bool ReadString(FString& Out)
	{
		uint32 Count = 0;
		if (!ReadU32(Count) || Count > 1024u * 1024u || !CanRead(Count))
			return false;
		if (Count == 0) { Out.Empty(); return true; }
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset),
			static_cast<int32>(Count));
		if (Converted.Length() <= 0) return false;
		Out = FString(Converted.Length(), Converted.Get());
		FTCHARToUTF8 RoundTrip(*Out);
		if (RoundTrip.Length() != static_cast<int32>(Count)
			|| FMemory::Memcmp(RoundTrip.Get(), Bytes.GetData() + Offset, Count) != 0)
			return false;
		Offset += static_cast<int32>(Count); return true;
	}
	int32 Tell() const { return Offset; }
	bool SetOffset(int32 Value)
	{
		if (Value < Offset || Value > Bytes.Num()) return false;
		Offset = Value; return true;
	}
	bool AtEnd() const { return Offset == Bytes.Num(); }
	TConstArrayView<uint8> View() const { return Bytes; }
private:
	bool CanRead(uint64 Count) const
	{
		return Offset >= 0 && static_cast<uint64>(Offset) <= static_cast<uint64>(Bytes.Num())
			&& Count <= static_cast<uint64>(Bytes.Num()) - static_cast<uint64>(Offset);
	}
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

bool DecodeClassDefaultsObject(const FWoTRuntimeVerifiedObject& Object,
	TArray<FWoTRuntimeClassDefaultRecord>& Out, FString& Error)
{
	const TConstArrayView<uint8> Payload = Object.Payload();
	FClassDefaultsCursor Cursor(Payload);
	uint16 Version = 0;
	uint16 Flags = 0;
	uint32 Count = 0;
	if (Payload.Num() < 16
		|| FMemory::Memcmp(Payload.GetData(), "WOTDEF3", 8) != 0
		|| !Cursor.Skip(8) || !Cursor.ReadU16(Version)
		|| !Cursor.ReadU16(Flags) || !Cursor.ReadU32(Count)
		|| Version != 3 || Flags != 0 || Count > 4000000)
	{
		Error = TEXT("class-default payload framing is malformed");
		return false;
	}
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		FWoTRuntimeClassDefaultRecord Record;
		uint8 Placeholder = 0;
		if (!Cursor.ReadString(Record.Identity) || Record.Identity.IsEmpty()
			|| !Cursor.ReadU32(Record.ExportIndex) || Record.ExportIndex == 0
			|| !Cursor.ReadString(Record.SuperclassIdentity)
			|| !Cursor.ReadU8(Placeholder) || Placeholder > 1)
		{
			Error = FString::Printf(TEXT("class-default record %u is malformed"), Index);
			return false;
		}
		Record.bPlaceholder = Placeholder != 0;
		int32 PropertyOffset = Cursor.Tell();
		if (!WoTRuntimeWorldDecodeProperties(Payload, PropertyOffset,
			Record.Properties, Error, nullptr, true,
			EWoTStructMemberPolicy::ReflectedGeneric)
			|| !Cursor.SetOffset(PropertyOffset))
		{
			Error = FString::Printf(TEXT("%s: %s"), *Record.Identity, *Error);
			return false;
		}
		Out.Add(MoveTemp(Record));
	}
	if (!Cursor.AtEnd())
	{
		Error = TEXT("class-default payload has a trailing byte range");
		return false;
	}
	return true;
}
}

bool FWoTRuntimeClassDefaults::LoadActiveGeneration(FString& OutError)
{
	if (bLoaded)
	{
		OutError = TEXT("class defaults were loaded more than once");
		return false;
	}
	const TArray<FWoTNeutralResourceId> Resources =
		FWoTRuntimeRegistry::Get().ListResources(
			EWoTNeutralResourceKind::ClassDefaults);
	if (Resources.Num() != 20)
	{
		OutError = FString::Printf(
			TEXT("active generation has %d class-default objects, expected 20"),
			Resources.Num());
		return false;
	}
	TSet<FString> ExpectedResources = {
		TEXT("cache/class_default/Angreal"), TEXT("cache/class_default/Core"),
		TEXT("cache/class_default/Editor"), TEXT("cache/class_default/Engine"),
		TEXT("cache/class_default/Environment"), TEXT("cache/class_default/Fire"),
		TEXT("cache/class_default/IpDrv"), TEXT("cache/class_default/IpServer"),
		TEXT("cache/class_default/Legend"), TEXT("cache/class_default/MachinShin"),
		TEXT("cache/class_default/ParticleSystems"), TEXT("cache/class_default/UBrowser"),
		TEXT("cache/class_default/UWindow"), TEXT("cache/class_default/WinDrv"),
		TEXT("cache/class_default/WOT"), TEXT("cache/class_default/WoTBase"),
		TEXT("cache/class_default/WoTBrowser"), TEXT("cache/class_default/WoTDecorations"),
		TEXT("cache/class_default/WoTPawns"), TEXT("cache/class_default/WoTTraps")};
	for (const FWoTNeutralResourceId& Resource : Resources)
	{
		if (ExpectedResources.Remove(Resource.LogicalId) != 1)
		{
			OutError = TEXT("active generation has an unexpected class-default object");
			return false;
		}
	}
	if (!ExpectedResources.IsEmpty())
	{
		OutError = TEXT("active generation is missing a required class-default object");
		return false;
	}
	TArray<FWoTRuntimeClassDefaultRecord> Decoded;
	TArray<FWoTRuntimeVerifiedObject> Objects;
	Objects.Reserve(Resources.Num());
	for (const FWoTNeutralResourceId& Resource : Resources)
	{
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
			FWoTRuntimeRegistry::Get().ReadObject(Resource,
				256ull * 1024ull * 1024ull,
				TEXT("ue1-class-defaults-v2"), TEXT("class_default"));
		if (Read.IsError())
		{
			OutError = TEXT("an authenticated class-default object could not be read");
			return false;
		}
		FWoTRuntimeVerifiedObject Object = MoveTemp(Read).GetValue();
		if (!DecodeClassDefaultsObject(Object, Decoded, OutError)) return false;
		Objects.Add(MoveTemp(Object));
	}
	if (!AdoptRecords(MoveTemp(Decoded), OutError)) return false;
	VerifiedObjects = MoveTemp(Objects);
	return true;
}

bool FWoTRuntimeClassDefaults::LoadFixture(
	TArray<FWoTRuntimeClassDefaultRecord> Records, FString& OutError)
{
	if (bLoaded)
	{
		OutError = TEXT("class-default fixture was loaded more than once");
		return false;
	}
	return AdoptRecords(MoveTemp(Records), OutError);
}

bool FWoTRuntimeClassDefaults::LoadObjects(
	TArray<FWoTRuntimeVerifiedObject> Objects,
	const TCHAR* InvalidInputError, FString& OutError)
{
	if (bLoaded || Objects.Num() != 20)
	{
		OutError = InvalidInputError;
		return false;
	}
	TArray<FWoTRuntimeClassDefaultRecord> Decoded;
	for (const FWoTRuntimeVerifiedObject& Object : Objects)
	{
		if (!DecodeClassDefaultsObject(Object, Decoded, OutError)) return false;
	}
	if (!AdoptRecords(MoveTemp(Decoded), OutError)) return false;
	VerifiedObjects = MoveTemp(Objects);
	return true;
}

#if !WOT_GUARDED_BOOTSTRAP
bool FWoTRuntimeClassDefaults::LoadObjectsForDevelopment(
	TArray<FWoTRuntimeVerifiedObject> Objects, FString& OutError)
{
	return LoadObjects(MoveTemp(Objects),
		TEXT("class-default development requires exactly twenty fresh objects"),
		OutError);
}
#endif

#if WITH_AUTOMATION_TESTS
bool FWoTRuntimeClassDefaults::LoadObjectsForTesting(
	TArray<FWoTRuntimeVerifiedObject> Objects, FString& OutError)
{
	return LoadObjects(MoveTemp(Objects),
		TEXT("class-default automation requires exactly twenty fresh objects"),
		OutError);
}

bool FWoTRuntimeClassDefaults::SetClassStructIntegerForTesting(
	FStringView ClassIdentity, FStringView PropertyName, uint32 ArrayIndex,
	FStringView MemberName, int32 Value)
{
	const FString Folded = FString(ClassIdentity).ToLower();
	FString Resolved = Folded;
	if (const FString* Alias = FoldedAliasToFoldedIdentity.Find(Folded))
		Resolved = *Alias;
	FWoTRuntimeClassDefaultRecord* Record =
		RecordsByFoldedIdentity.Find(Resolved);
	if (!Record) return false;
	for (FWoTRuntimeWorldProperty& Property : Record->Properties)
	{
		if (Property.ArrayIndex != ArrayIndex
			|| !FStringView(Property.Name).Equals(PropertyName,
				ESearchCase::IgnoreCase)
			|| Property.Type != EWoTRuntimeWorldPropertyType::Struct)
			continue;
		for (FWoTRuntimeWorldProperty& Member : Property.StructMembers)
		{
			if (!FStringView(Member.Name).Equals(MemberName,
				ESearchCase::IgnoreCase)) continue;
			Member.Type = EWoTRuntimeWorldPropertyType::Integer;
			Member.Value.SetNumUninitialized(sizeof(Value));
			FMemory::Memcpy(Member.Value.GetData(), &Value, sizeof(Value));
			return true;
		}
	}
	return false;
}
#endif

bool FWoTRuntimeClassDefaults::AdoptRecords(
	TArray<FWoTRuntimeClassDefaultRecord> Records, FString& OutError)
{
	TMap<FString, FWoTRuntimeClassDefaultRecord> Candidate;
	for (FWoTRuntimeClassDefaultRecord& Record : Records)
	{
		if (Record.Identity.IsEmpty())
		{
			OutError = TEXT("a class-default record has no identity"); return false;
		}
		const FString Key = Record.Identity.ToLower();
		if (Candidate.Contains(Key))
		{
			OutError = TEXT("class-default identities collide case-insensitively");
			return false;
		}
		Candidate.Add(Key, MoveTemp(Record));
	}
	TMap<FString, FString> Aliases;
	{
		TMap<FString, int32> BaseCounts;
		TMap<FString, FString> BaseToKey;
		for (const TPair<FString, FWoTRuntimeClassDefaultRecord>& Pair : Candidate)
		{
			int32 Marker = INDEX_NONE;
			if (!Pair.Key.FindChar(TEXT('#'), Marker) || Marker <= 0
				|| !Pair.Key.Mid(Marker).StartsWith(TEXT("#export-"))) continue;
			const FString Base = Pair.Key.Left(Marker);
			BaseCounts.FindOrAdd(Base) += 1;
			BaseToKey.Add(Base, Pair.Key);
		}
		for (const TPair<FString, int32>& Count : BaseCounts)
		{
			// Only a unique disambiguated class stands in for its plain name,
			// and never when a record already owns the plain name.
			if (Count.Value == 1 && !Candidate.Contains(Count.Key))
				Aliases.Add(Count.Key, BaseToKey.FindChecked(Count.Key));
		}
	}
	auto FindCandidate = [&Candidate, &Aliases](const FString& Identity)
		-> const FWoTRuntimeClassDefaultRecord*
	{
		const FString Folded = Identity.ToLower();
		if (const FWoTRuntimeClassDefaultRecord* Direct = Candidate.Find(Folded))
			return Direct;
		const FString* Aliased = Aliases.Find(Folded);
		return Aliased != nullptr ? Candidate.Find(*Aliased) : nullptr;
	};
	for (const TPair<FString, FWoTRuntimeClassDefaultRecord>& Pair : Candidate)
	{
		TSet<FString> Seen;
		const FWoTRuntimeClassDefaultRecord* Current = &Pair.Value;
		while (Current != nullptr && !Current->SuperclassIdentity.IsEmpty())
		{
			const FString CurrentKey = Current->Identity.ToLower();
			if (Seen.Contains(CurrentKey))
			{
				OutError = TEXT("class-default superclass graph contains a cycle");
				return false;
			}
			Seen.Add(CurrentKey);
			Current = FindCandidate(Current->SuperclassIdentity);
			if (Current == nullptr)
			{
				OutError = FString::Printf(TEXT("class-default superclass '%s' is absent"),
					*Pair.Value.SuperclassIdentity);
				return false;
			}
		}
	}
	RecordsByFoldedIdentity = MoveTemp(Candidate);
	FoldedAliasToFoldedIdentity = MoveTemp(Aliases);
	PublicIdentityByLeaf.Reset();
	FoldedAncestorsByFoldedIdentity.Reset();
	RelatedClassLeavesCache.Reset();
#if WITH_AUTOMATION_TESTS
	RelationshipResolutionCountForTesting = 0;
#endif
	for (const TPair<FString, FWoTRuntimeClassDefaultRecord>& Pair
		: RecordsByFoldedIdentity)
	{
		const FString Public = PublicIdentity(Pair.Value.Identity);
		int32 Dot = INDEX_NONE;
		const FName Leaf(*(
			Public.FindLastChar(TEXT('.'), Dot) ? Public.Mid(Dot + 1) : Public));
		if (FString* Existing = PublicIdentityByLeaf.Find(Leaf))
		{
			if (!Existing->Equals(Public, ESearchCase::IgnoreCase))
			{
				Existing->Reset();
			}
		}
		else
		{
			PublicIdentityByLeaf.Add(Leaf, Public);
		}

		TSet<FString>& Ancestors =
			FoldedAncestorsByFoldedIdentity.FindOrAdd(Pair.Key);
		const FWoTRuntimeClassDefaultRecord* Current = &Pair.Value;
		while (Current)
		{
			Ancestors.Add(PublicIdentity(Current->Identity).ToLower());
			Current = Current->SuperclassIdentity.IsEmpty() ? nullptr
				: FindRecord(Current->SuperclassIdentity);
		}
	}
	bLoaded = true;
	return true;
}

const FWoTRuntimeClassDefaultRecord* FWoTRuntimeClassDefaults::FindRecord(
	FStringView Identity) const
{
	const FString Folded = FString(Identity).ToLower();
	if (const FWoTRuntimeClassDefaultRecord* Direct =
		RecordsByFoldedIdentity.Find(Folded)) return Direct;
	const FString* Aliased = FoldedAliasToFoldedIdentity.Find(Folded);
	return Aliased != nullptr ? RecordsByFoldedIdentity.Find(*Aliased) : nullptr;
}

FString FWoTRuntimeClassDefaults::PublicIdentity(const FString& Identity)
{
	const int32 Marker = Identity.Find(TEXT("#export-"),
		ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	return Marker > 0 ? Identity.Left(Marker) : Identity;
}

const FWoTRuntimeWorldProperty* FWoTRuntimeClassDefaults::FindProperty(
	const FWoTRuntimeWorldActor& Actor, FStringView Name, uint32 ArrayIndex) const
{
	if (const FWoTRuntimeWorldProperty* Instance =
		WoTRuntimeWorldFindProperty(Actor, Name, ArrayIndex)) return Instance;
	return FindClassProperty(Actor.ClassIdentity, Name, ArrayIndex);
}

const FWoTRuntimeWorldProperty* FWoTRuntimeClassDefaults::FindClassProperty(
	FStringView ClassIdentity, FStringView Name, uint32 ArrayIndex) const
{
	const FWoTRuntimeClassDefaultRecord* Current = FindRecord(ClassIdentity);
	while (Current != nullptr)
	{
		for (const FWoTRuntimeWorldProperty& Property : Current->Properties)
		{
			if (Property.ArrayIndex == ArrayIndex
				&& FStringView(Property.Name).Equals(Name, ESearchCase::IgnoreCase))
				return &Property;
		}
		Current = Current->SuperclassIdentity.IsEmpty() ? nullptr
			: FindRecord(Current->SuperclassIdentity);
	}
	return nullptr;
}

const FWoTRuntimeWorldProperty* FWoTRuntimeClassDefaults::FindOwnClassProperty(
	FStringView ClassIdentity, FStringView Name, uint32 ArrayIndex) const
{
	const FWoTRuntimeClassDefaultRecord* Record = FindRecord(ClassIdentity);
	if (Record == nullptr) return nullptr;
	for (const FWoTRuntimeWorldProperty& Property : Record->Properties)
	{
		if (Property.ArrayIndex == ArrayIndex
			&& FStringView(Property.Name).Equals(Name, ESearchCase::IgnoreCase))
		{
			return &Property;
		}
	}
	return nullptr;
}

bool FWoTRuntimeClassDefaults::ClassIsA(
	FStringView ClassIdentity, FStringView BaseIdentity) const
{
	const FWoTRuntimeClassDefaultRecord* Record = FindRecord(ClassIdentity);
	if (!Record) return false;
	const TSet<FString>* Ancestors =
		FoldedAncestorsByFoldedIdentity.Find(Record->Identity.ToLower());
	if (!Ancestors) return false;
	const FWoTRuntimeClassDefaultRecord* Base = FindRecord(BaseIdentity);
	const FString FoldedBase = Base
		? PublicIdentity(Base->Identity).ToLower()
		: FString(BaseIdentity).ToLower();
	return Ancestors->Contains(FoldedBase);
}

bool FWoTRuntimeClassDefaults::GetClassChain(
	FStringView ClassIdentity, TArray<FString>& OutMostSpecificFirst) const
{
	OutMostSpecificFirst.Reset();
	const FWoTRuntimeClassDefaultRecord* Current = FindRecord(ClassIdentity);
	while (Current != nullptr)
	{
		// Rule matching and diagnostics use the public Pkg.Name form.
		OutMostSpecificFirst.Add(PublicIdentity(Current->Identity));
		Current = Current->SuperclassIdentity.IsEmpty() ? nullptr
			: FindRecord(Current->SuperclassIdentity);
	}
	return !OutMostSpecificFirst.IsEmpty();
}

bool FWoTRuntimeClassDefaults::FindClassIdentityByLeaf(
	FStringView Leaf, FString& OutIdentity) const
{
	OutIdentity.Reset();
	const FString* Identity = PublicIdentityByLeaf.Find(FName(*FString(Leaf)));
	if (!Identity || Identity->IsEmpty()) return false;
	OutIdentity = *Identity;
	return true;
}

bool FWoTRuntimeClassDefaults::ClassesAreRelatedByLeaf(
	const FName FirstLeaf, const FName SecondLeaf) const
{
	if (const TMap<FName, bool>* SecondMap =
		RelatedClassLeavesCache.Find(FirstLeaf))
	{
		if (const bool* Cached = SecondMap->Find(SecondLeaf))
		{
			return *Cached;
		}
	}
#if WITH_AUTOMATION_TESTS
	++RelationshipResolutionCountForTesting;
#endif
	const FString* FirstIdentity = PublicIdentityByLeaf.Find(FirstLeaf);
	const FString* SecondIdentity = PublicIdentityByLeaf.Find(SecondLeaf);
	const bool bRelated = FirstIdentity && !FirstIdentity->IsEmpty()
		&& SecondIdentity && !SecondIdentity->IsEmpty()
		&& (ClassIsA(*FirstIdentity, *SecondIdentity)
			|| ClassIsA(*SecondIdentity, *FirstIdentity));
	RelatedClassLeavesCache.FindOrAdd(FirstLeaf).Add(SecondLeaf, bRelated);
	RelatedClassLeavesCache.FindOrAdd(SecondLeaf).Add(FirstLeaf, bRelated);
	return bRelated;
}

void FWoTRuntimeClassDefaults::GetDerivedClassIdentities(
	FStringView BaseIdentity, TArray<FString>& OutIdentities) const
{
	OutIdentities.Reset();
	for (const TPair<FString, FWoTRuntimeClassDefaultRecord>& Pair
		: RecordsByFoldedIdentity)
	{
		const FString Identity = PublicIdentity(Pair.Value.Identity);
		if (ClassIsA(Identity, BaseIdentity))
		{
			OutIdentities.AddUnique(Identity);
		}
	}
}
