#include "WoTRuntimeAssets.h"

#include "WoTRetailIdentity.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeProceduralResource.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "../WoTFractalData.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../WoTBootstrapHeartbeat.h"
#include "../WoTPayloadSoundWave.h"
#include "../WoTPayloadSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/SoftObjectPath.h"

namespace
{
TWeakObjectPtr<UWoTRuntimeAssets> GWoTRuntimeAssets;
TArray<TWeakObjectPtr<UWoTRuntimeAssets>> GWoTRuntimeAssetProviders;
#if WITH_AUTOMATION_TESTS
bool GWoTRuntimePayloadTextureResolutionForTesting = false;
#endif

FString ObjectAssetPath(FStringView Input)
{
	FString Path(Input);
	Path.TrimStartAndEndInline();
	const int32 Quote = Path.Find(TEXT("'"));
	if (Quote != INDEX_NONE && Path.EndsWith(TEXT("'")))
	{
		Path = Path.Mid(Quote + 1, Path.Len() - Quote - 2);
	}
	return Path;
}

bool SplitObjectPath(FStringView Input, FString& OutPackagePath, FString& OutLeaf)
{
	const FString Path = ObjectAssetPath(Input);
	int32 Dot = INDEX_NONE;
	const FString PackagePath = Path.FindChar(TEXT('.'), Dot)
		? Path.Left(Dot) : Path;
	int32 Slash = INDEX_NONE;
	if (!PackagePath.FindLastChar(TEXT('/'), Slash)
		|| Slash + 1 >= PackagePath.Len())
	{
		return false;
	}
	OutPackagePath = PackagePath;
	OutLeaf = PackagePath.Mid(Slash + 1);
	if (Dot != INDEX_NONE)
	{
		const FString ObjectLeaf = Path.Mid(Dot + 1);
		if (!ObjectLeaf.Equals(OutLeaf, ESearchCase::IgnoreCase)) return false;
	}
	return !OutLeaf.IsEmpty();
}

bool UIAlias(FStringView IdView, FString& OutPackage, FString& OutName)
{
	const FString Id(IdView);
	// Authored from Tools/import/import_hud_textures.py:65-107. These are
	// source identities, not per-asset runtime data.
	if (Id.Equals(TEXT("StatusIconSelected"), ESearchCase::IgnoreCase))
	{
		OutPackage = TEXT("WoTBase"); OutName = TEXT("S"); return true;
	}
	if (Id.Equals(TEXT("StatusIconFrame"), ESearchCase::IgnoreCase))
	{
		OutPackage = TEXT("WoTBase"); OutName = TEXT("M"); return true;
	}
	if (Id.Equals(TEXT("M_SMyrddraal"), ESearchCase::IgnoreCase))
	{
		OutPackage = TEXT("WoTPawns"); OutName = Id; return true;
	}
	if (Id.StartsWith(TEXT("M_"), ESearchCase::IgnoreCase))
	{
		OutPackage = TEXT("Angreal"); OutName = Id; return true;
	}
	if (Id.StartsWith(TEXT("T32_"), ESearchCase::IgnoreCase))
	{
		OutPackage = TEXT("WOT"); OutName = Id; return true;
	}
	static const TCHAR* const WOTNames[] = {
		TEXT("SeekerFrame"), TEXT("I_Notch"), TEXT("D_Notch"),
		TEXT("I_Tainted"), TEXT("I_NotifyPickup"), TEXT("Turtle"),
		TEXT("TurtleDead"), TEXT("Warn"), TEXT("HeadsBase"),
		TEXT("HeadsBlack"), TEXT("HeadsBlue"), TEXT("HeadsRed"),
		TEXT("HeadsGreen"), TEXT("HeadsAesSedai"), TEXT("HeadsForsaken"),
		TEXT("HeadsHound"), TEXT("HeadsWC")};
	if (Id.StartsWith(TEXT("Crosshair"), ESearchCase::IgnoreCase))
	{
		const FString Suffix = Id.Mid(9);
		int32 Index = 0;
		if (LexTryParseString(Index, *Suffix) && Index >= 1 && Index <= 20)
		{
			OutPackage = TEXT("WOT"); OutName = Id; return true;
		}
	}
	for (const TCHAR* Name : WOTNames)
	{
		if (Id.Equals(Name, ESearchCase::IgnoreCase))
		{
			OutPackage = TEXT("WOT"); OutName = Id; return true;
		}
	}
	return false;
}

EWoTNeutralResourceKind NeutralKind(EWoTRuntimeAssetKind Kind)
{
	return Kind == EWoTRuntimeAssetKind::Sound
		? EWoTNeutralResourceKind::Sound : EWoTNeutralResourceKind::Texture;
}

FString OptionsKey(const FWoTRuntimeTextureOptions& Options)
{
	return FString::Printf(TEXT("%d:%d:%d:%d:%d:%d:%d"),
		Options.bLoadMips ? 1 : 0, Options.bSRGB ? 1 : 0,
		static_cast<int32>(Options.Filter), Options.LODBias,
		static_cast<int32>(Options.AddressX),
		static_cast<int32>(Options.AddressY),
		Options.bForcePaletteIndexZeroTransparent ? 1 : 0);
}

}

UTexture2D* WoTRuntimeMaterializeTexture(UObject* Outer,
	const FWoTTexturePayloadData& Payload,
	const FWoTRuntimeTextureOptions& Options)
{
	if (!Outer || Payload.Mips.IsEmpty() || Payload.Palette.Num() != 256)
	{
		return nullptr;
	}
	const int32 MipCount = Options.bLoadMips ? Payload.Mips.Num() : 1;
	const TConstArrayView<uint8> Bytes = Payload.PayloadBytes();
	UTexture2D* Texture = NewObject<UTexture2D>(Outer, NAME_None, RF_Transient);
	if (!Texture) return nullptr;
	TUniquePtr<FTexturePlatformData> PlatformData = MakeUnique<FTexturePlatformData>();
	PlatformData->SizeX = static_cast<int32>(Payload.Mips[0].Width);
	PlatformData->SizeY = static_cast<int32>(Payload.Mips[0].Height);
	PlatformData->SetNumSlices(1);
	PlatformData->PixelFormat = PF_B8G8R8A8;
	for (int32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
	{
		const FWoTTexturePayloadMip& SourceMip = Payload.Mips[MipIndex];
		uint64 PixelCount = 0;
		if (SourceMip.Width == 0 || SourceMip.Height == 0
			|| SourceMip.Width > MAX_int32 || SourceMip.Height > MAX_int32
			|| SourceMip.Width > MAX_uint64 / SourceMip.Height
			|| (PixelCount = static_cast<uint64>(SourceMip.Width)
				* SourceMip.Height) > 64ull * 1024ull * 1024ull
			|| SourceMip.StoredByteLength != PixelCount
			|| SourceMip.DataOffset > static_cast<uint64>(Bytes.Num())
			|| PixelCount > static_cast<uint64>(Bytes.Num()) - SourceMip.DataOffset)
		{
			return nullptr;
		}
		FTexture2DMipMap* DestinationMip = new FTexture2DMipMap(
			SourceMip.Width, SourceMip.Height, 1);
		PlatformData->Mips.Add(DestinationMip);
		DestinationMip->BulkData.Lock(LOCK_READ_WRITE);
		FColor* Destination = reinterpret_cast<FColor*>(DestinationMip->BulkData.Realloc(
			static_cast<int64>(PixelCount * sizeof(FColor))));
		if (!Destination)
		{
			DestinationMip->BulkData.Unlock();
			return nullptr;
		}
		for (uint64 Pixel = 0; Pixel < PixelCount; ++Pixel)
		{
			const uint8 PaletteIndex = Bytes[static_cast<int32>(
				SourceMip.DataOffset + Pixel)];
			FColor Color = Payload.Palette[PaletteIndex];
			Color.A = (Payload.bMasked
				|| Options.bForcePaletteIndexZeroTransparent) && PaletteIndex == 0
				? 0 : 255;
			Destination[Pixel] = Color;
		}
		DestinationMip->BulkData.Unlock();
	}
	Texture->SRGB = Options.bSRGB;
	Texture->Filter = Options.Filter;
	Texture->LODBias = Options.LODBias;
	Texture->AddressX = Options.AddressX;
	Texture->AddressY = Options.AddressY;
	Texture->NeverStream = true;
	Texture->SetPlatformData(PlatformData.Release());
	Texture->UpdateResource();
	return Texture;
}

void UWoTRuntimeAssets::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWoTPayloadSubsystem>();
	bShutdown = false;
	GWoTRuntimeAssetProviders.AddUnique(this);
	RefreshProviderBinding();
	PostWorldInitializationHandle = FWorldDelegates::OnPostWorldInitialization
		.AddUObject(this, &UWoTRuntimeAssets::HandlePostWorldInitialization);
	WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddUObject(
		this, &UWoTRuntimeAssets::HandleWorldTickStart);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(
		this, &UWoTRuntimeAssets::HandleWorldCleanup);
}

void UWoTRuntimeAssets::Deinitialize()
{
	ShutdownForPayloadDeactivation();
	GWoTRuntimeAssetProviders.Remove(this);
	RefreshProviderBinding();
	Super::Deinitialize();
}

void UWoTRuntimeAssets::RefreshProviderBindingWithOwner(
	UWoTPayloadSubsystem* AuthenticatedOwner)
{
	for (int32 Index = GWoTRuntimeAssetProviders.Num() - 1; Index >= 0; --Index)
	{
		UWoTRuntimeAssets* Provider = GWoTRuntimeAssetProviders[Index].Get();
		if (!Provider || Provider->bShutdown)
		{
			GWoTRuntimeAssetProviders.RemoveAt(Index);
		}
	}
	if (AuthenticatedOwner)
	{
		UGameInstance* OwnerGameInstance = AuthenticatedOwner->GetGameInstance();
		for (int32 Index = GWoTRuntimeAssetProviders.Num() - 1; Index >= 0; --Index)
		{
			UWoTRuntimeAssets* Provider = GWoTRuntimeAssetProviders[Index].Get();
			if (Provider && Provider->GetGameInstance() == OwnerGameInstance)
			{
				GWoTRuntimeAssets = Provider;
				return;
			}
		}
		GWoTRuntimeAssets.Reset();
		return;
	}
	GWoTRuntimeAssets = GWoTRuntimeAssetProviders.IsEmpty()
		? nullptr : GWoTRuntimeAssetProviders.Last();
}

void UWoTRuntimeAssets::RefreshProviderBinding()
{
	RefreshProviderBindingWithOwner(
		UWoTPayloadSubsystem::ActivePayloadOwnerForRuntimeAssets());
}

bool UWoTRuntimeAssets::ResolveCacheId(FStringView Path,
	EWoTRuntimeAssetKind Kind,
	TConstArrayView<FWoTNeutralResourceId> Resources,
	FString& OutLogicalId, FString* OutFailure)
{
	OutLogicalId.Reset();
	auto Fail = [OutFailure](const TCHAR* Message)
	{
		if (OutFailure) *OutFailure = Message;
		return false;
	};
	FString PackagePath;
	FString Leaf;
	if (!SplitObjectPath(Path, PackagePath, Leaf))
		return Fail(TEXT("malformed object path"));
	FString Package;
	FString Name = Leaf;
	FString Base;
	const TCHAR* Root = Kind == EWoTRuntimeAssetKind::Sound
		? TEXT("/Game/WoT/Sounds/") : TEXT("/Game/WoT/Textures/");
	if (PackagePath.StartsWith(Root, ESearchCase::IgnoreCase))
	{
		// "<Pkg>/<Name>" is the plain identity; "<Pkg>/<Group...>/<Name>" is the
		// exact identity of a name that collides inside its package
		// (WoTRetailIdentity.h). Both end in the asset leaf.
		const FString Tail = PackagePath.Mid(FCString::Strlen(Root));
		int32 Slash = INDEX_NONE;
		if (!Tail.FindLastChar(TEXT('/'), Slash) || Slash <= 0
			|| Slash + 1 >= Tail.Len()
			|| !Tail.Mid(Slash + 1).Equals(Leaf, ESearchCase::IgnoreCase)
			|| !WoTRetailLogicalBaseFromSubPath(Tail, Base))
			return Fail(TEXT("path does not have one package and one asset leaf"));
		int32 FirstSlash = INDEX_NONE;
		Tail.FindChar(TEXT('/'), FirstSlash);
		Package = Tail.Left(FirstSlash);
	}
	else if (Kind == EWoTRuntimeAssetKind::Texture
		&& PackagePath.StartsWith(TEXT("/Game/WoT/UI/"), ESearchCase::IgnoreCase)
		&& UIAlias(Leaf, Package, Name))
	{
		Base = Package + TEXT("__") + Name;
	}
	else
	{
		return Fail(TEXT("path is outside the supported gameplay roots"));
	}
	if (Package.IsEmpty() || Name.IsEmpty() || Package.Contains(TEXT("/"))
		|| Base.IsEmpty())
		return Fail(TEXT("empty or nested retail identity"));
	const FString Prefix = Kind == EWoTRuntimeAssetKind::Sound
		? TEXT("cache/sound/") : TEXT("cache/texture/");
	const FString Wanted = Prefix + Base;
	int32 Match = INDEX_NONE;
	for (int32 Index = 0; Index < Resources.Num(); ++Index)
	{
		if (Resources[Index].Kind != NeutralKind(Kind)
			|| !Resources[Index].LogicalId.Equals(Wanted, ESearchCase::IgnoreCase))
			continue;
		if (Match != INDEX_NONE)
			return Fail(TEXT("case-insensitive registry identity is ambiguous"));
		Match = Index;
	}
	if (Match == INDEX_NONE) return Fail(TEXT("identity is absent from the registry"));
	OutLogicalId = Resources[Match].LogicalId;
	if (OutFailure) OutFailure->Reset();
	return true;
}

bool UWoTRuntimeAssets::SplitCacheIdentity(FStringView LogicalId,
	EWoTRuntimeAssetKind Kind, FString& OutPackage, FString& OutName)
{
	const FString Prefix = Kind == EWoTRuntimeAssetKind::Sound
		? TEXT("cache/sound/") : TEXT("cache/texture/");
	if (!LogicalId.StartsWith(Prefix, ESearchCase::IgnoreCase)) return false;
	const FString Tail = FString(LogicalId.RightChop(Prefix.Len()));
	const int32 Separator = Tail.Find(TEXT("__"), ESearchCase::CaseSensitive);
	if (Separator <= 0 || Separator + 2 >= Tail.Len()) return false;
	OutPackage = Tail.Left(Separator);
	OutName = Tail.Mid(Separator + 2);
	return !OutPackage.IsEmpty() && !OutName.IsEmpty();
}

UWoTPayloadSoundWave* UWoTRuntimeAssets::ResolveSound(
	const FSoftObjectPath& Path, bool bLoop)
{
	if (bShutdown || !IsWoTPayloadBootstrapActive()) return nullptr;
	const FString Requested = Path.ToString();
	FString LogicalId;
	const TArray<FWoTNeutralResourceId> Resources =
		FWoTRuntimeRegistry::Get().ListResources(EWoTNeutralResourceKind::Sound);
	if (!ResolveCacheId(Requested, EWoTRuntimeAssetKind::Sound,
		Resources, LogicalId))
	{
		RecordResolution(true, false, Requested); return nullptr;
	}
	const FString Key = LogicalId.ToLower() + (bLoop ? TEXT(":1") : TEXT(":0"));
	if (TObjectPtr<UWoTPayloadSoundWave>* Existing = SoundCache.Find(Key))
	{
		RecordResolution(true, IsValid(*Existing), Requested); return *Existing;
	}
	UWoTPayloadSubsystem* PayloadSubsystem = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	const TSharedPtr<const FWoTSoundPayloadData> Payload = PayloadSubsystem
		? PayloadSubsystem->LoadSoundPayload(LogicalId) : nullptr;
	FString RetailPackage;
	FString RetailName;
	if (!Payload.IsValid() || Payload->FrameCount > MAX_int32
		|| !SplitCacheIdentity(LogicalId,
		EWoTRuntimeAssetKind::Sound, RetailPackage, RetailName))
	{
		RecordResolution(true, false, Requested); return nullptr;
	}
	UWoTRuntimeAssetContainer* WaveOuter =
		NewObject<UWoTRuntimeAssetContainer>(this, NAME_None, RF_Transient);
	UWoTPayloadSoundWave* Wave = NewObject<UWoTPayloadSoundWave>(
		WaveOuter, FName(*RetailName), RF_Transient);
	if (!Wave)
	{
		RecordResolution(true, false, Requested); return nullptr;
	}
	Wave->PayloadKey = LogicalId;
	Wave->PayloadChannels = Payload->Channels;
	Wave->PayloadSampleRate = static_cast<int32>(Payload->SampleRate);
	Wave->PayloadBitsPerSample = Payload->BitsPerSample;
	Wave->PayloadFrameCount = static_cast<int32>(Payload->FrameCount);
	Wave->bLooping = bLoop || !Payload->Loops.IsEmpty();
	if (!PayloadSubsystem->InjectSound(Wave, LogicalId))
	{
		Wave->MarkAsGarbage(); WaveOuter->MarkAsGarbage();
		RecordResolution(true, false, Requested); return nullptr;
	}
	RuntimeOuters.Add(WaveOuter);
	RuntimeSounds.Add(Wave);
	SoundCache.Add(Key, Wave);
	RecordResolution(true, true, Requested);
	return Wave;
}

UTexture2D* UWoTRuntimeAssets::ResolveTexture(const FSoftObjectPath& Path,
	const FWoTRuntimeTextureOptions& Options)
{
	if (bShutdown || !IsWoTPayloadBootstrapActive()) return nullptr;
	const FString Requested = Path.ToString();
	FString LogicalId;
	const TArray<FWoTNeutralResourceId> Resources =
		FWoTRuntimeRegistry::Get().ListResources(EWoTNeutralResourceKind::Texture);
	if (!ResolveCacheId(Requested, EWoTRuntimeAssetKind::Texture,
		Resources, LogicalId))
	{
		UTexture2D* Procedural = ResolveProceduralPlaceholder(Requested);
		RecordResolution(false, Procedural != nullptr, Requested);
		return Procedural;
	}
	const FString Key = LogicalId.ToLower() + TEXT(":") + OptionsKey(Options);
	if (TObjectPtr<UTexture2D>* Existing = TextureCache.Find(Key))
	{
		RecordResolution(false, IsValid(*Existing), Requested); return *Existing;
	}
	UWoTPayloadSubsystem* PayloadSubsystem = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	const TSharedPtr<const FWoTTexturePayloadData> Payload = PayloadSubsystem
		? PayloadSubsystem->LoadTexturePayload(LogicalId) : nullptr;
	UWoTRuntimeAssetContainer* TextureOuter =
		NewObject<UWoTRuntimeAssetContainer>(this, NAME_None, RF_Transient);
	UTexture2D* Texture = Payload.IsValid()
		? WoTRuntimeMaterializeTexture(TextureOuter, *Payload, Options) : nullptr;
	if (!Texture)
	{
		TextureOuter->MarkAsGarbage();
		RecordResolution(false, false, Requested); return nullptr;
	}
	FString RetailPackage;
	FString RetailName;
	if (!SplitCacheIdentity(LogicalId, EWoTRuntimeAssetKind::Texture,
		RetailPackage, RetailName)
		|| !Texture->Rename(*RetailName, TextureOuter,
			REN_DontCreateRedirectors | REN_DoNotDirty))
	{
		Texture->ReleaseResource();
		Texture->MarkAsGarbage();
		TextureOuter->MarkAsGarbage();
		RecordResolution(false, false, Requested); return nullptr;
	}
	RuntimeOuters.Add(TextureOuter);
	RuntimeTextures.Add(Texture);
	TextureCache.Add(Key, Texture);
	TexturePalettes.Add(FObjectKey(Texture), Payload->Palette);
	TextureMasked.Add(FObjectKey(Texture), Payload->bMasked);
	RecordResolution(false, true, Requested);
	return Texture;
}

UTexture2D* UWoTRuntimeAssets::ResolveProceduralPlaceholder(const FString& Requested)
{
	// The same "/Game/WoT/Textures/<Pkg>/<Name>" grammar, served from the
	// procedural domain: cache/procedural/<Pkg>__<Name>. The placeholder is
	// the authenticated mip-0 seed through its own palette; the fractal
	// subsystem discovers it by name and binds the live simulation.
	FString PackagePath;
	FString Leaf;
	if (!SplitObjectPath(Requested, PackagePath, Leaf)
		|| !PackagePath.StartsWith(TEXT("/Game/WoT/Textures/"), ESearchCase::IgnoreCase))
		return nullptr;
	const FString Tail = PackagePath.Mid(FCString::Strlen(TEXT("/Game/WoT/Textures/")));
	int32 Slash = INDEX_NONE;
	FString Base;
	if (!Tail.FindLastChar(TEXT('/'), Slash) || Slash <= 0 || Slash + 1 >= Tail.Len()
		|| !Tail.Mid(Slash + 1).Equals(Leaf, ESearchCase::IgnoreCase)
		|| !WoTRetailLogicalBaseFromSubPath(Tail, Base))
		return nullptr;
	const FString Wanted = TEXT("cache/procedural/") + Base;
	const FString Key = Wanted.ToLower() + TEXT(":procedural");
	if (TObjectPtr<UTexture2D>* Existing = TextureCache.Find(Key))
		return IsValid(*Existing) ? *Existing : nullptr;
	const FWoTNeutralResourceId* Match = nullptr;
	const TArray<FWoTNeutralResourceId> Resources =
		FWoTRuntimeRegistry::Get().ListResources(EWoTNeutralResourceKind::Procedural);
	for (const FWoTNeutralResourceId& Resource : Resources)
	{
		if (!Resource.LogicalId.Equals(Wanted, ESearchCase::IgnoreCase)) continue;
		if (Match != nullptr) return nullptr; // case-ambiguous registry identity
		Match = &Resource;
	}
	if (Match == nullptr) return nullptr;
	FWoTRuntimeProceduralResource Procedural;
	FWoTFractalTextureRecord Record;
	if (Procedural.LoadFirstUse(*Match).IsError() || !Procedural.BuildRecord(Record))
		return nullptr;
	UTexture2D* Texture = WoTRuntimeProceduralPlaceholderTexture(Record);
	if (!Texture) return nullptr;
	TArray<FColor> Palette;
	Palette.Reserve(256);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		Palette.Emplace(Record.PaletteRGBA[Index * 4], Record.PaletteRGBA[Index * 4 + 1],
			Record.PaletteRGBA[Index * 4 + 2], Record.PaletteRGBA[Index * 4 + 3]);
	}
	RuntimeTextures.Add(Texture);
	TextureCache.Add(Key, Texture);
	TexturePalettes.Add(FObjectKey(Texture), MoveTemp(Palette));
	TextureMasked.Add(FObjectKey(Texture), Procedural.Parameters().bMasked);
	return Texture;
}

UMaterialInterface* UWoTRuntimeAssets::ResolveDerivedMaterial(
	const FSoftObjectPath& Path)
{
	if (bShutdown || !IsWoTPayloadBootstrapActive()) return nullptr;
	const FString Requested = Path.ToString();
	if (TObjectPtr<UMaterialInterface>* Existing = MaterialCache.Find(Requested.ToLower()))
		return *Existing;
	FString PackagePath;
	FString AssetName;
	if (!SplitObjectPath(Requested, PackagePath, AssetName)
		|| !PackagePath.StartsWith(TEXT("/Game/WoT/Materials/Instances/"),
			ESearchCase::IgnoreCase))
		return nullptr;
	const TCHAR* Prefix = nullptr;
	const TCHAR* MasterPath = nullptr;
	if (AssetName.StartsWith(TEXT("MI_PSAddFB_"), ESearchCase::IgnoreCase))
	{
		Prefix = TEXT("MI_PSAddFB_");
		MasterPath = TEXT("/Game/WoT/Materials/M_WoT_ParticleAdd.M_WoT_ParticleAdd");
	}
	else if (AssetName.StartsWith(TEXT("MI_PSModFB_"), ESearchCase::IgnoreCase))
	{
		Prefix = TEXT("MI_PSModFB_");
		MasterPath = TEXT("/Game/WoT/Materials/M_WoT_ParticleMod.M_WoT_ParticleMod");
	}
	else if (AssetName.StartsWith(TEXT("MI_MeshFB_"), ESearchCase::IgnoreCase))
	{
		Prefix = TEXT("MI_MeshFB_");
		MasterPath = TEXT("/Game/WoT/Materials/M_WoT_MeshFBAddGlow.M_WoT_MeshFBAddGlow");
	}
	else if (AssetName.StartsWith(TEXT("MI_PSAdd_"), ESearchCase::IgnoreCase))
	{
		Prefix = TEXT("MI_PSAdd_");
		MasterPath = TEXT("/Game/WoT/Materials/M_WoT_ParticleAdd.M_WoT_ParticleAdd");
	}
	else if (AssetName.StartsWith(TEXT("MI_PSMod2x_"), ESearchCase::IgnoreCase))
	{
		Prefix = TEXT("MI_PSMod2x_");
		MasterPath = TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate");
	}
	else if (AssetName.StartsWith(TEXT("MI_PSMod_"), ESearchCase::IgnoreCase))
	{
		Prefix = TEXT("MI_PSMod_");
		MasterPath = TEXT("/Game/WoT/Materials/M_WoT_ParticleMod.M_WoT_ParticleMod");
	}
	else if (AssetName.StartsWith(TEXT("MI_Decal_"), ESearchCase::IgnoreCase))
	{
		Prefix = TEXT("MI_Decal_");
		MasterPath = TEXT("/Game/WoT/Materials/M_WoT_Decal.M_WoT_Decal");
	}
	else
	{
		return nullptr; // MI_Skin_* and arbitrary MI_<N> remain outside the authenticated derivation grammar.
	}
	// "<Pkg>__<Name>" or the grouped exact spelling "<Pkg>-2E<Group>...__<Name>"
	// (WoTRetailDerivedMaterialSuffix): a colliding export has no plain alias.
	const FString Identity = AssetName.Mid(FCString::Strlen(Prefix));
	FString TextureSubPath;
	if (!WoTRetailTextureSubPathFromDerivedSuffix(Identity, TextureSubPath)) return nullptr;
	int32 NameStart = INDEX_NONE;
	TextureSubPath.FindLastChar(TEXT('/'), NameStart);
	const FString RetailName = TextureSubPath.Mid(NameStart + 1);
	FWoTRuntimeTextureOptions Options;
	Options.bLoadMips = true;
	Options.bSRGB = false;
	Options.Filter = TF_Bilinear;
	UTexture2D* Texture = ResolveTexture(FSoftObjectPath(FString::Printf(
		TEXT("/Game/WoT/Textures/%s.%s"), *TextureSubPath, *RetailName)), Options);
	if (!Texture) return nullptr;
	UMaterialInterface* Master = Cast<UMaterialInterface>(
		FSoftObjectPath(MasterPath).TryLoad());
	if (!Master)
	{
		RecordMaterialFailure(MasterPath);
		return nullptr;
	}
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Master, this);
	if (!Material)
	{
		RecordMaterialFailure(Requested);
		return nullptr;
	}
	Material->SetTextureParameterValue(TEXT("Tex"), Texture);
	RuntimeMaterials.Add(Material);
	MaterialCache.Add(Requested.ToLower(), Material);
	return Material;
}

bool UWoTRuntimeAssets::ResolveKeyColor(
	const UTexture2D* Texture, FColor& OutKeyColor)
{
	return GWoTRuntimeAssets.IsValid()
		&& GWoTRuntimeAssets->GetKeyColor(Texture, OutKeyColor);
}

bool UWoTRuntimeAssets::ResolveTextureMasked(const UTexture2D* Texture)
{
	if (!Texture || !GWoTRuntimeAssets.IsValid()) return false;
	const bool* bMasked =
		GWoTRuntimeAssets->TextureMasked.Find(FObjectKey(Texture));
	return bMasked != nullptr && *bMasked;
}

FLinearColor UWoTRuntimeAssets::PaletteColorToMaterialColor(const FColor& Color)
{
	constexpr float Inv255 = 1.0f / 255.0f;
	return FLinearColor(Color.R * Inv255, Color.G * Inv255,
		Color.B * Inv255, 1.0f);
}

bool UWoTRuntimeAssets::GetPalette(
	const UTexture2D* Texture, TArray<FColor>& OutPalette) const
{
	if (!Texture) return false;
	if (const TArray<FColor>* Palette = TexturePalettes.Find(FObjectKey(Texture)))
	{
		OutPalette = *Palette; return true;
	}
	return false;
}

bool UWoTRuntimeAssets::GetKeyColor(
	const UTexture2D* Texture, FColor& OutKeyColor) const
{
	if (!Texture) return false;
	if (const TArray<FColor>* Palette = TexturePalettes.Find(FObjectKey(Texture));
		Palette && !Palette->IsEmpty())
	{
		OutKeyColor = (*Palette)[0]; return true;
	}
	return false;
}

USoundBase* UWoTRuntimeAssets::ResolveSoundPath(
	const FSoftObjectPath& Path, bool bLoop)
{
#if !WOT_GUARDED_BOOTSTRAP
	return Cast<USoundBase>(Path.TryLoad());
#else
	if (!IsWoTPayloadBootstrapActive())
		return Cast<USoundBase>(Path.TryLoad());
	UWoTRuntimeAssets* Assets = GWoTRuntimeAssets.Get();
	return Assets ? Assets->ResolveSound(Path, bLoop) : nullptr;
#endif
}

UTexture2D* UWoTRuntimeAssets::ResolveTexturePath(
	const FSoftObjectPath& Path, const FWoTRuntimeTextureOptions& Options)
{
#if !WOT_GUARDED_BOOTSTRAP
#if WITH_AUTOMATION_TESTS
	if (GWoTRuntimePayloadTextureResolutionForTesting
		&& IsWoTPayloadBootstrapActive())
	{
		UWoTRuntimeAssets* Assets = GWoTRuntimeAssets.Get();
		return Assets ? Assets->ResolveTexture(Path, Options) : nullptr;
	}
#endif
	// Cooked assets have no authenticated runtime payload metadata, so the
	// mask query intentionally reports false for this TryLoad path.
	return Cast<UTexture2D>(Path.TryLoad());
#else
	if (!IsWoTPayloadBootstrapActive())
	{
		// The inactive cooked fallback likewise has no payload mask metadata.
		return Cast<UTexture2D>(Path.TryLoad());
	}
	UWoTRuntimeAssets* Assets = GWoTRuntimeAssets.Get();
	return Assets ? Assets->ResolveTexture(Path, Options) : nullptr;
#endif
}

UMaterialInterface* UWoTRuntimeAssets::ResolveMaterialPath(
	const FSoftObjectPath& Path)
{
#if !WOT_GUARDED_BOOTSTRAP
#if WITH_AUTOMATION_TESTS
	if (GWoTRuntimePayloadTextureResolutionForTesting
		&& IsWoTPayloadBootstrapActive())
	{
		UWoTRuntimeAssets* Assets = GWoTRuntimeAssets.Get();
		return Assets ? Assets->ResolveDerivedMaterial(Path) : nullptr;
	}
#endif
	return Cast<UMaterialInterface>(Path.TryLoad());
#else
	if (!IsWoTPayloadBootstrapActive())
		return Cast<UMaterialInterface>(Path.TryLoad());
	UWoTRuntimeAssets* Assets = GWoTRuntimeAssets.Get();
	return Assets ? Assets->ResolveDerivedMaterial(Path) : nullptr;
#endif
}

UMaterialInstanceDynamic* UWoTRuntimeAssets::CreateModulatedSpriteMaterial(
	UObject* Outer, const FSoftObjectPath& TexturePath, FName RuntimeIdentity)
{
	if (!Outer) return nullptr;
	FWoTRuntimeTextureOptions Options;
	Options.bLoadMips = true;
	Options.bSRGB = false;
	Options.Filter = TF_Bilinear;
	UTexture2D* Texture = TexturePath.IsNull()
		? nullptr : ResolveTexturePath(TexturePath, Options);
	UMaterialInterface* Master = Cast<UMaterialInterface>(FSoftObjectPath(
		TEXT("/Game/WoT/Materials/M_WoT_MeshModulate.M_WoT_MeshModulate"))
		.TryLoad());
	if (!Master || (!Texture && RuntimeIdentity.IsNone())) return nullptr;
	UMaterialInstanceDynamic* Material =
		UMaterialInstanceDynamic::Create(Master, Outer, RuntimeIdentity);
	if (Material && Texture)
		Material->SetTextureParameterValue(TEXT("Tex"), Texture);
	return Material;
}

void UWoTRuntimeAssets::HandlePostWorldInitialization(
	UWorld* World, const UWorld::InitializationValues IVS)
{
	(void)IVS;
	if (!World || !World->IsGameWorld() || !IsWoTPayloadBootstrapActive()) return;
	TravelWorld = World;
	bTravelSummaryPending = true;
	bTravelSummaryClockStarted = false;
	TravelSummaryStartRealSeconds = 0.0;
	ResolvedSounds = FailedSounds = ResolvedTextures = FailedTextures = FailedMaterials = 0;
	FirstFailures.Reset();
}

void UWoTRuntimeAssets::HandleWorldTickStart(
	UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
	(void)TickType; (void)DeltaSeconds;
	if (!bTravelSummaryPending || World != TravelWorld.Get()) return;
	if (!AdvanceTravelSummaryClock(World->HasBegunPlay(), FPlatformTime::Seconds()))
	{
		return;
	}
	WoTRecordBootstrapHeartbeat(TEXT("assets-resolved"),
		EWoTBootstrapPhase::Ready, 0, {}, {}, TEXT("stage=5s ") + TravelSummary());
}

void UWoTRuntimeAssets::HandleWorldCleanup(
	UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	(void)bSessionEnded;
	(void)bCleanupResources;
	FinalizeTravelSummary(World);
}

bool UWoTRuntimeAssets::FinalizeTravelSummary(UWorld* ExpectedWorld)
{
	UWorld* World = TravelWorld.Get();
	if (!World || (ExpectedWorld && ExpectedWorld != World)) return false;
	WoTRecordBootstrapHeartbeat(TEXT("assets-resolved"),
		EWoTBootstrapPhase::Ready, 0, {}, {}, TEXT("stage=final ") + TravelSummary());
	TravelWorld.Reset();
	bTravelSummaryPending = false;
	bTravelSummaryClockStarted = false;
	TravelSummaryStartRealSeconds = 0.0;
#if WITH_AUTOMATION_TESTS
	++FinalTravelSummaryEmissionsForTesting;
#endif
	return true;
}

bool UWoTRuntimeAssets::AdvanceTravelSummaryClock(
	bool bWorldHasBegunPlay, double RealTimeSeconds)
{
	if (!bTravelSummaryPending || !bWorldHasBegunPlay) return false;
	if (!bTravelSummaryClockStarted)
	{
		bTravelSummaryClockStarted = true;
		TravelSummaryStartRealSeconds = RealTimeSeconds;
		return false;
	}
	if (RealTimeSeconds - TravelSummaryStartRealSeconds < 5.0) return false;
	bTravelSummaryPending = false;
	return true;
}

void UWoTRuntimeAssets::RecordResolution(
	bool bSound, bool bSuccess, FStringView Path)
{
	int32& Counter = bSound
		? (bSuccess ? ResolvedSounds : FailedSounds)
		: (bSuccess ? ResolvedTextures : FailedTextures);
	++Counter;
	if (!bSuccess && FirstFailures.Num() < 4)
		FirstFailures.Add(FString(Path).Replace(TEXT(" "), TEXT("%20")));
}

void UWoTRuntimeAssets::RecordMaterialFailure(FStringView Path)
{
	++FailedMaterials;
	if (FirstFailures.Num() < 4)
	{
		FirstFailures.Add(FString(Path).Replace(TEXT(" "), TEXT("%20")));
	}
}

FString UWoTRuntimeAssets::TravelSummary() const
{
	const FString Failures = FirstFailures.IsEmpty()
		? TEXT("-") : FString::Join(FirstFailures, TEXT(","));
	return FString::Printf(TEXT("sounds=%d failed_sounds=%d textures=%d failed_textures=%d failed_materials=%d failures=%s"),
		ResolvedSounds, FailedSounds, ResolvedTextures, FailedTextures,
		FailedMaterials, *Failures);
}

void UWoTRuntimeAssets::ReleaseRuntimeObjects()
{
	UWoTPayloadSubsystem* PayloadSubsystem = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UWoTPayloadSubsystem>() : nullptr;
	for (UWoTPayloadSoundWave* Wave : RuntimeSounds)
	{
		if (PayloadSubsystem && IsValid(Wave)) PayloadSubsystem->RemoveInjectedSound(Wave);
	}
	SoundCache.Empty();
	RuntimeSounds.Empty();
	TextureCache.Empty();
	TexturePalettes.Empty();
	TextureMasked.Empty();
	RuntimeTextures.Empty();
	MaterialCache.Empty();
	RuntimeMaterials.Empty();
	RuntimeOuters.Empty();
}

void UWoTRuntimeAssets::ShutdownForPayloadDeactivation()
{
	if (bShutdown) return;
	bShutdown = true;
	RefreshProviderBinding();
	// Game-instance subsystem teardown can precede UWorld cleanup during normal
	// engine exit. Finalize while the heartbeat channel and tracked world still
	// exist; the shared routine makes a later cleanup callback harmless.
	FinalizeTravelSummary(nullptr);
	if (PostWorldInitializationHandle.IsValid())
	{
		FWorldDelegates::OnPostWorldInitialization.Remove(PostWorldInitializationHandle);
		PostWorldInitializationHandle.Reset();
	}
	if (WorldTickStartHandle.IsValid())
	{
		FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
		WorldTickStartHandle.Reset();
	}
	if (WorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		WorldCleanupHandle.Reset();
	}
	ReleaseRuntimeObjects();
}

#if WITH_AUTOMATION_TESTS
void UWoTRuntimeAssets::ResetTravelStatsForTesting()
{
	ResolvedSounds = FailedSounds = ResolvedTextures = FailedTextures = FailedMaterials = 0;
	FirstFailures.Reset();
}

void UWoTRuntimeAssets::SetPayloadTextureResolutionForTesting(bool bEnabled)
{
	GWoTRuntimePayloadTextureResolutionForTesting = bEnabled;
}

FString UWoTRuntimeAssets::TravelSummaryForTesting() const
{
	return TravelSummary();
}

void UWoTRuntimeAssets::ResetProvidersForTesting()
{
	GWoTRuntimeAssetProviders.Reset();
	GWoTRuntimeAssets.Reset();
}

void UWoTRuntimeAssets::RegisterProviderForTesting(UWoTRuntimeAssets* Provider)
{
	GWoTRuntimeAssetProviders.AddUnique(Provider);
	RefreshProviderBindingWithOwner(nullptr);
}

void UWoTRuntimeAssets::RemoveProviderForTesting(UWoTRuntimeAssets* Provider)
{
	GWoTRuntimeAssetProviders.Remove(Provider);
	RefreshProviderBindingWithOwner(nullptr);
}

void UWoTRuntimeAssets::RefreshProviderBindingForTesting(
	UWoTPayloadSubsystem* AuthenticatedOwner)
{
	RefreshProviderBindingWithOwner(AuthenticatedOwner);
}

UWoTRuntimeAssets* UWoTRuntimeAssets::ProviderForTesting()
{
	return GWoTRuntimeAssets.Get();
}
#endif
