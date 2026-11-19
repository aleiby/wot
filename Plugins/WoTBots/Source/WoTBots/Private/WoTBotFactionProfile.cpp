#include "WoTBotFactionProfile.h"
#include "World/WoTWorldProjector.h"

#include "AI/WoTCreatureItemBinding.h"
#include "Payload/WoTRuntimeRegistry.h"
#include "Serialization/MemoryWriter.h"
#include "WoTCharacter.h"
#include "WoTFrontEnd.h"
#include "WoTGameMode.h"
#include "WoTPayloadSubsystem.h"
#include "WoTRetailText.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeWorldProperties.h"
#if !WOT_GUARDED_BOOTSTRAP
#include "RetailData/WoTDevRetailFamilies.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogWoTBotProfiles, Log, All);

namespace
{
constexpr const TCHAR* BotBaseIdentity = TEXT("WoTPawns.WOTBot");
constexpr uint32 ItemSlotCount = 16;

void LogUnboundClassOnce(const FString& Identity)
{
	static TSet<FString> Logged;
	const FString Key = Identity.ToLower();
	if (!Logged.Contains(Key))
	{
		Logged.Add(Key);
		UE_LOG(LogWoTBotProfiles, Log, TEXT("Skipping unbound bot profile class '%s'"), *Identity);
	}
}

void LogUnboundItemOnce(const FString& Identity)
{
	static TSet<FString> Logged;
	const FString Key = Identity.ToLower();
	if (!Logged.Contains(Key))
	{
		Logged.Add(Key);
		UE_LOG(LogWoTBotProfiles, Log, TEXT("Skipping unbound bot profile item '%s'"), *Identity);
	}
}

UClass* ResolvePlayerClass(const FString& BotIdentity)
{
	FString PublicIdentity = BotIdentity;
	const int32 Marker = PublicIdentity.Find(TEXT("#export-"),
		ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (Marker > 0) PublicIdentity.LeftInline(Marker);
	if (!PublicIdentity.RemoveFromEnd(TEXT("Bot"), ESearchCase::IgnoreCase)) return nullptr;
	// Validate against the existing playable choices before the login resolver,
	// whose fallback intentionally accepts unknown input as the default class.
	for (const FWoTFactionChoice& Choice : FWoTFrontEndModel::FactionChoices())
	{
		if (Choice.ClassOption.Equals(PublicIdentity, ESearchCase::IgnoreCase))
			return AWoTGameMode::ResolvePlayerClassOption(TEXT("?Class=") + Choice.ClassOption);
	}
	return nullptr;
}

bool ReadFloat(const FWoTRuntimeClassDefaults& Defaults, const FString& Identity,
	const TCHAR* Name, float& Out, FString& Error, uint32 Index = 0)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name, Index);
	if (Property == nullptr) return true;
	if (WoTRuntimeWorldPropertyFloat(*Property, Out) && FMath::IsFinite(Out)) return true;
	Error = FString::Printf(TEXT("bot profile '%s' property %s[%u] is malformed"),
		*Identity, Name, Index);
	return false;
}

bool ReadProfile(const FWoTRuntimeClassDefaults& Defaults, const FString& Identity,
	FWoTBotFactionProfile& Out, FString& Error)
{
	if (!ReadFloat(Defaults, Identity, TEXT("MinOffensiveAngrealUsageInterval"),
		Out.MinOffensiveUsageInterval, Error)
		|| !ReadFloat(Defaults, Identity, TEXT("MinDefensiveAngrealUsageInterval"),
			Out.MinDefensiveUsageInterval, Error)) return false;
	for (uint32 Index = 0; Index < ItemSlotCount; ++Index)
	{
		const FWoTRuntimeWorldProperty* Path = Defaults.FindClassProperty(
			Identity, TEXT("AII_AngrealInventoryStr"), Index);
		if (Path == nullptr) continue;
		// The shared nonempty string reader rejects the canonical empty string.
		if (Path->Type == EWoTRuntimeWorldPropertyType::String
			&& Path->Value.Num() == 2 && Path->Value[0] == 1 && Path->Value[1] == 0) continue;
		FString Source;
		if (!WoTRuntimeWorldPropertyUE1String(*Path, Source))
		{
			Error = FString::Printf(TEXT("bot profile '%s' item path [%u] is malformed"), *Identity, Index);
			return false;
		}
		// The world projection's retail-to-native item binding, the map pickups' own
		// map; the NPC weave table covers only NPC loadouts.
		UClass* ItemClass = WoTResolveNativePickupClass(Source);
		if (ItemClass == nullptr) ItemClass = WoTCreatureBoundWeave(FName(*Source));
		if (ItemClass == nullptr)
		{
			LogUnboundItemOnce(Source);
			continue;
		}
		float Priority = 0.0f;
		if (!ReadFloat(Defaults, Identity, TEXT("AII_Priority"), Priority, Error, Index)) return false;
		Out.Priorities.Add(ItemClass, Priority);
	}
	return true;
}

TArray<uint8> SourceKey(const FWoTRuntimeClassDefaults& Defaults, bool bPayload)
{
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	Writer << bPayload;
	if (bPayload)
	{
		// An owner can adopt another generation and an allocator can reuse the
		// graph's address. Authenticated content identities make that safe.
		for (const FWoTNeutralResourceId& Resource : FWoTRuntimeRegistry::Get().ListResources(
			EWoTNeutralResourceKind::ClassDefaults))
		{
			FString Identity = Resource.LogicalId;
			Writer << Identity;
			TOptional<FWoTCacheBlobDescriptor> Descriptor = FWoTRuntimeRegistry::Get().FindDescriptor(Resource);
			if (Descriptor.IsSet())
			{
				Writer << Descriptor->Schema;
				Writer << Descriptor->Size;
				Writer.Serialize(Descriptor->Sha256.Bytes, FWoTSha256::ByteCount);
			}
		}
	}
#if !WOT_GUARDED_BOOTSTRAP
	else
	{
		// Development defaults can be reset without a payload owner. Snapshot
		// only the typed inputs this decoder consumes so address reuse cannot
		// keep a previous fixture or reference tree's profiles alive.
		for (const FWoTFactionChoice& Choice : FWoTFrontEndModel::FactionChoices())
		{
			FString Identity = Choice.ClassOption + TEXT("Bot");
			Writer << Identity;
			bool bBound = Defaults.ClassIsA(Identity, BotBaseIdentity);
			Writer << bBound;
			auto Append = [&](const TCHAR* Name, uint32 Index = 0)
			{
				const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name, Index);
				bool bPresent = Property != nullptr;
				Writer << bPresent;
				if (!bPresent) return;
				uint8 Type = static_cast<uint8>(Property->Type);
				Writer << Type;
				TArray<uint8> Value = Property->Value;
				Writer << Value;
			};
			Append(TEXT("MinOffensiveAngrealUsageInterval"));
			Append(TEXT("MinDefensiveAngrealUsageInterval"));
			for (uint32 Index = 0; Index < ItemSlotCount; ++Index)
			{
				Append(TEXT("AII_AngrealInventoryStr"), Index);
				Append(TEXT("AII_Priority"), Index);
			}
		}
	}
#endif
	return Bytes;
}
}

bool WoTBotProfiles::Decode(const FWoTRuntimeClassDefaults& Defaults,
	TMap<UClass*, FWoTBotFactionProfile>& Out, FString& Error)
{
	Error.Reset();
	if (!Defaults.IsLoaded())
	{
		Error = TEXT("bot profiles require loaded class defaults");
		return false;
	}
	TArray<FString> Identities;
	Defaults.GetDerivedClassIdentities(BotBaseIdentity, Identities);
	Identities.Sort();
	TMap<UClass*, FWoTBotFactionProfile> Candidate;
	for (const FString& Identity : Identities)
	{
		if (Identity.Equals(BotBaseIdentity, ESearchCase::IgnoreCase)) continue;
		UClass* PlayerClass = ResolvePlayerClass(Identity);
		if (PlayerClass == nullptr)
		{
			LogUnboundClassOnce(Identity);
			continue;
		}
		FWoTBotFactionProfile Profile;
		if (!ReadProfile(Defaults, Identity, Profile, Error)) return false;
		Candidate.Add(PlayerClass, MoveTemp(Profile));
	}
	Out = MoveTemp(Candidate);
	return true;
}

const TMap<UClass*, FWoTBotFactionProfile>& WoTBotProfiles::Get()
{
	check(IsInGameThread());
	static TMap<UClass*, FWoTBotFactionProfile> Cached;
	static TWeakObjectPtr<UWoTPayloadSubsystem> CachedOwner;
	static const FWoTRuntimeClassDefaults* CachedDefaults = nullptr;
	static TArray<uint8> CachedKey;
	FString Error;
	const FWoTRuntimeClassDefaults* Defaults = UWoTPayloadSubsystem::ActiveRuntimeClassDefaults(Error);
	UWoTPayloadSubsystem* Owner = Defaults
		? UWoTPayloadSubsystem::ActivePayloadOwnerForRuntimeAssets() : nullptr;
	const bool bPayload = Defaults != nullptr;
#if !WOT_GUARDED_BOOTSTRAP
	if (Defaults == nullptr)
	{
		WoTDevRetailFamiliesEnsureActivated();
		Defaults = WoTDevRetailClassDefaults();
	}
#endif
	if (Defaults == nullptr)
	{
		Cached.Reset();
		CachedKey.Reset();
		CachedDefaults = nullptr;
		CachedOwner.Reset();
		return Cached;
	}
	TArray<uint8> Key = SourceKey(*Defaults, bPayload);
	if (CachedDefaults != Defaults || CachedOwner != Owner || CachedKey != Key)
	{
		Cached.Reset();
		if (!Decode(*Defaults, Cached, Error))
			UE_LOG(LogWoTBotProfiles, Warning, TEXT("Bot profiles unavailable: %s"), *Error);
		CachedDefaults = Defaults;
		CachedOwner = Owner;
		CachedKey = MoveTemp(Key);
	}
	return Cached;
}

FString WoTBotProfiles::DisplayName(UClass* PlayerClass)
{
	if (PlayerClass == nullptr || !FWoTRetailText::EnsureLoaded()) return FString();
	const FString* Name = FWoTRetailText::FindLocalized(PlayerClass, TEXT("TeamDescription"));
	return Name != nullptr ? *Name : FString();
}
