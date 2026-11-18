#include "WoTFireWall.h"

#include "../WoTRetailBuilderCanonical.h"
#include "../World/WoTRuntimeClassDefaults.h"
#include "../World/WoTRuntimeParticleProjection.h"
#include "../World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTFireWallSprayerSpec
{
	const TCHAR* Identity;
	const TCHAR* OffsetProperty;
	float FadeLifeSpan;
};

// Defaults: FireWallSprayer1.uc:90-111, FireWallSprayer2.uc:12-29,
// FireWallSprayer3.uc:12-27, FireWallSlab.uc:194-214, FireWall.uc:45-49,
// FireWallInventory.uc:15-27. Fade values are Destroyed() method behavior
// (FireWallSlab.uc:104-122), so they remain native policy.
constexpr FWoTFireWallSprayerSpec GWoTFireWallSprayers[] = {
	{TEXT("WoTTraps.FireWallSprayer1"), TEXT("FlameSprayerOffset1"), 1.5f},
	{TEXT("WoTTraps.FireWallSprayer2"), TEXT("FlameSprayerOffset2"), 1.7f},
	{TEXT("WoTTraps.FireWallSprayer3"), TEXT("FlameSprayerOffset3"), 1.0f},
};

FName WoTFireWallLeaf(const FString& Identity)
{
	int32 Dot = INDEX_NONE;
	return FName(*(Identity.FindLastChar(TEXT('.'), Dot)
		? Identity.RightChop(Dot + 1) : Identity));
}

FName WoTFireWallTextureSlug(const FString& Identity)
{
	int32 First = INDEX_NONE;
	int32 Last = INDEX_NONE;
	if (!Identity.FindChar(TEXT('.'), First) || !Identity.FindLastChar(TEXT('.'), Last))
	{
		return FName(*Identity);
	}
	return FName(*(Identity.Left(First) + TEXT("__") + Identity.RightChop(Last + 1)));
}

bool WoTFireWallReadFloat(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, float& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name);
	if (Property == nullptr || !WoTRuntimeWorldPropertyFloat(*Property, Out))
	{
		Error = FString::Printf(TEXT("fire-wall '%s' property %s is missing or malformed"),
			Identity, Name); return false;
	}
	return true;
}

bool WoTFireWallReadInteger(const FWoTRuntimeClassDefaults& Defaults,
	const TCHAR* Identity, const TCHAR* Name, int32& Out, FString& Error)
{
	const FWoTRuntimeWorldProperty* Property = Defaults.FindClassProperty(Identity, Name);
	if (Property == nullptr || !WoTRuntimeWorldPropertyInteger(*Property, Out))
	{
		Error = FString::Printf(TEXT("fire-wall '%s' property %s is missing or malformed"),
			Identity, Name); return false;
	}
	return true;
}

bool WoTFireWallDecodeSprayer(const FWoTRetailBuildContext& Context,
	const FWoTFireWallSprayerSpec& Spec, FWoTFireWallSprayer& Out, FString& Error)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	TArray<FString> Chain;
	if (!Defaults.GetClassChain(Spec.Identity, Chain))
	{
		Error = FString::Printf(TEXT("fire-wall sprayer class '%s' is missing"), Spec.Identity);
		return false;
	}
	FWoTRuntimeParticleSprayerConfiguration Config;
	if (!WoTRuntimeReadParticleSprayerConfiguration(
		[&](FStringView Name, uint32 Index)
		{
			return Defaults.FindClassProperty(Spec.Identity, Name, Index);
		}, Config))
	{
		Error = FString::Printf(TEXT("fire-wall sprayer '%s' is malformed"), Spec.Identity);
		return false;
	}
	Out.ClassIdentity = FName(Spec.Identity);
	Out.Spread = Config.Spread;
	Out.Volume = Config.Volume;
	Out.MinVolume = Config.MinVolume;
	Out.VolumeScalePct = Config.VolumeScalePct;
	Out.VisibilityRadius = Config.VisibilityRadius;
	Out.VisibilityHeight = Config.VisibilityHeight;
	Out.Gravity = Config.Gravity;
	Out.FadeLifeSpan = Spec.FadeLifeSpan;
	const FWoTRuntimeWorldProperty* Style =
		Defaults.FindClassProperty(Spec.Identity, TEXT("Style"));
	if (Style != nullptr)
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*Style, Value))
		{
			Error = FString::Printf(TEXT("fire-wall sprayer '%s' Style is malformed"),
				Spec.Identity); return false;
		}
		Out.bModulateOnly = Value == 4; // Engine.Actor STY_Modulated.
	}
	const FWoTRuntimeWorldProperty* Offset = Defaults.FindClassProperty(
		TEXT("WoTTraps.FireWallSlab"), Spec.OffsetProperty);
	if (Offset == nullptr || !WoTRuntimeWorldPropertyVector(*Offset, Out.Offset))
	{
		Error = FString::Printf(TEXT("FireWallSlab property %s is missing or malformed"),
			Spec.OffsetProperty); return false;
	}
	for (int32 Index = 0; Index < Config.Templates.Num(); ++Index)
	{
		const FString& TextureIdentity = Config.TextureIdentities[Index];
		FWoTRetailTextureFacts TextureFacts;
		if (!Context.FindTexture
			|| !Context.FindTexture(TextureIdentity, TextureFacts))
		{
			Error = FString::Printf(
				TEXT("fire-wall sprayer '%s' texture '%s' is unavailable"),
				Spec.Identity, *TextureIdentity);
			return false;
		}
		FWoTFireWallTemplate& Target = Out.Templates.AddDefaulted_GetRef();
		Target.Template = Config.Templates[Index];
		Target.TextureIdentity = WoTFireWallTextureSlug(TextureIdentity);
		Target.Template.TexWidth = static_cast<float>(TextureFacts.USize);
		Target.Template.TexHeight = static_cast<float>(TextureFacts.VSize);
	}
	return true;
}

bool WoTFireWallTemplateEqual(const FWoTParticleTemplate& A,
	const FWoTParticleTemplate& B)
{
	using WoTRetailBuilderCanonical::FloatBits;
	return FloatBits(A.LifeSpan) == FloatBits(B.LifeSpan)
		&& FloatBits(A.Weight) == FloatBits(B.Weight)
		&& FloatBits(A.MaxInitialVelocity) == FloatBits(B.MaxInitialVelocity)
		&& FloatBits(A.MinInitialVelocity) == FloatBits(B.MinInitialVelocity)
		&& FloatBits(A.MaxDrawScale) == FloatBits(B.MaxDrawScale)
		&& FloatBits(A.MinDrawScale) == FloatBits(B.MinDrawScale)
		&& FloatBits(A.MaxScaleGlow) == FloatBits(B.MaxScaleGlow)
		&& FloatBits(A.MinScaleGlow) == FloatBits(B.MinScaleGlow)
		&& A.GrowPhase == B.GrowPhase
		&& FloatBits(A.MaxGrowRate) == FloatBits(B.MaxGrowRate)
		&& FloatBits(A.MinGrowRate) == FloatBits(B.MinGrowRate)
		&& A.FadePhase == B.FadePhase
		&& FloatBits(A.MaxFadeRate) == FloatBits(B.MaxFadeRate)
		&& FloatBits(A.MinFadeRate) == FloatBits(B.MinFadeRate)
		&& FloatBits(A.TexWidth) == FloatBits(B.TexWidth)
		&& FloatBits(A.TexHeight) == FloatBits(B.TexHeight);
}

void WoTFireWallSerializeTemplate(TArray<uint8>& Out,
	const FWoTFireWallTemplate& Entry)
{
	using namespace WoTRetailBuilderCanonical;
	Name(Out, Entry.TextureIdentity);
	const FWoTParticleTemplate& T = Entry.Template;
	for (float Value : {T.LifeSpan, T.Weight, T.MaxInitialVelocity,
		T.MinInitialVelocity, T.MaxDrawScale, T.MinDrawScale,
		T.MaxScaleGlow, T.MinScaleGlow})
	{
		Ascii(Out, " "); Float(Out, Value);
	}
	Ascii(Out, " "); Integer(Out, T.GrowPhase);
	Ascii(Out, " "); Float(Out, T.MaxGrowRate);
	Ascii(Out, " "); Float(Out, T.MinGrowRate);
	Ascii(Out, " "); Integer(Out, T.FadePhase);
	for (float Value : {T.MaxFadeRate, T.MinFadeRate, T.TexWidth, T.TexHeight})
	{
		Ascii(Out, " "); Float(Out, Value);
	}
}
}

bool FWoTFireWallBuilder::Build(const FWoTRetailBuildContext& Context,
	FWoTFireWallSet& Out, FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("fire-wall requires loaded class defaults"); return false;
	}
	FWoTFireWallSet Candidate;
	for (const FWoTFireWallSprayerSpec& Spec : GWoTFireWallSprayers)
	{
		FWoTFireWallSprayer& Sprayer = Candidate.Sprayers.AddDefaulted_GetRef();
		if (!WoTFireWallDecodeSprayer(Context, Spec, Sprayer, OutError)) return false;
	}
	if (!WoTFireWallReadInteger(Defaults, TEXT("WoTTraps.FireWallSlab"),
		TEXT("Health"), Candidate.Defaults.Health, OutError)
		|| !WoTFireWallReadFloat(Defaults, TEXT("WoTTraps.FireWallSlab"),
			TEXT("DamageAmount"), Candidate.Defaults.DamageAmount, OutError)
		|| !WoTFireWallReadFloat(Defaults, TEXT("WoTTraps.FireWallSprayer1"),
			TEXT("DamageInterval"), Candidate.Defaults.DamageInterval, OutError)
		|| !WoTFireWallReadInteger(Defaults, TEXT("WoTTraps.FireWallSlab"),
			TEXT("SlabWidth"), Candidate.Defaults.SlabWidth, OutError)
		|| !WoTFireWallReadFloat(Defaults, TEXT("WoTTraps.FireWallSlab"),
			TEXT("DrawScale"), Candidate.Defaults.SlabDrawScale, OutError)
		|| !WoTFireWallReadFloat(Defaults, TEXT("WoTTraps.FireWallSlab"),
			TEXT("CollisionRadius"), Candidate.Defaults.CollisionRadius, OutError)
		|| !WoTFireWallReadFloat(Defaults, TEXT("WoTTraps.FireWallSlab"),
			TEXT("CollisionHeight"), Candidate.Defaults.CollisionHeight, OutError)
		|| !WoTFireWallReadInteger(Defaults, TEXT("WoTTraps.FireWall"),
			TEXT("MaxSlabs"), Candidate.Defaults.MaxSlabs, OutError)
		|| !WoTFireWallReadInteger(Defaults, TEXT("WoTTraps.FireWallInventory"),
			TEXT("Count"), Candidate.Defaults.InventoryCount, OutError)) return false;
	const FWoTRuntimeWorldProperty* Mesh = Defaults.FindClassProperty(
		TEXT("WoTTraps.FireWallSlab"), TEXT("Mesh"));
	if (Mesh == nullptr || Mesh->Type != EWoTRuntimeWorldPropertyType::Object)
	{
		OutError = TEXT("FireWallSlab Mesh is missing or malformed"); return false;
	}
	Candidate.Defaults.SlabMesh = WoTFireWallLeaf(Mesh->ObjectIdentity);
	// Native bindings remain explicit policy: FireWall -> AWoTFireWallSlab and
	// FireWallInventory -> AWoTFireWall. FireWallSlab TrapType=None remains the
	// PORT_NOTES compatibility correction in the native constructor.
	Out = MoveTemp(Candidate);
	OutError.Reset(); return true;
}

void FWoTFireWallBuilder::SerializeCanonical(const FWoTFireWallSet& Set,
	TArray<uint8>& Out)
{
	using namespace WoTRetailBuilderCanonical;
	Out.Reset(); Ascii(Out, "wot-fire-wall-v1\nsprayers ");
	Integer(Out, Set.Sprayers.Num()); Ascii(Out, "\n");
	for (const FWoTFireWallSprayer& S : Set.Sprayers)
	{
		Ascii(Out, "sprayer "); Name(Out, S.ClassIdentity);
		for (float Value : {static_cast<float>(S.Offset.X), static_cast<float>(S.Offset.Y),
			static_cast<float>(S.Offset.Z), S.Spread, S.Volume, S.MinVolume,
			S.VolumeScalePct, S.VisibilityRadius, S.VisibilityHeight,
			static_cast<float>(S.Gravity.X), static_cast<float>(S.Gravity.Y),
			static_cast<float>(S.Gravity.Z), S.FadeLifeSpan})
		{
			Ascii(Out, " "); Float(Out, Value);
		}
		Ascii(Out, " "); Bool(Out, S.bModulateOnly); Ascii(Out, " templates ");
		Integer(Out, S.Templates.Num()); Ascii(Out, "\n");
		for (const FWoTFireWallTemplate& T : S.Templates)
		{
			Ascii(Out, "template "); WoTFireWallSerializeTemplate(Out, T); Ascii(Out, "\n");
		}
	}
	const FWoTFireWallDefaults& D = Set.Defaults;
	Ascii(Out, "defaults "); Integer(Out, D.Health); Ascii(Out, " "); Float(Out, D.DamageAmount);
	Ascii(Out, " "); Float(Out, D.DamageInterval); Ascii(Out, " "); Integer(Out, D.SlabWidth);
	Ascii(Out, " "); Name(Out, D.SlabMesh); Ascii(Out, " "); Float(Out, D.SlabDrawScale);
	Ascii(Out, " "); Float(Out, D.CollisionRadius); Ascii(Out, " "); Float(Out, D.CollisionHeight);
	Ascii(Out, " "); Integer(Out, D.MaxSlabs); Ascii(Out, " "); Integer(Out, D.InventoryCount);
	Ascii(Out, "\n");
}

FString FWoTFireWallBuilder::CanonicalDigest(const FWoTFireWallSet& Set)
{
	TArray<uint8> Bytes; SerializeCanonical(Set, Bytes); return Sha256(Bytes).ToHex();
}

bool FWoTFireWallBuilder::AreEquivalent(const FWoTFireWallSet& Built,
	const FWoTFireWallSet& Expected, FString& OutError)
{
	using WoTRetailBuilderCanonical::FloatBits;
	auto F = [](float A, float B) { return FloatBits(A) == FloatBits(B); };
	if (Built.Sprayers.Num() != Expected.Sprayers.Num())
	{
		OutError = TEXT("fire-wall sprayer count differs"); return false;
	}
	for (int32 Index = 0; Index < Built.Sprayers.Num(); ++Index)
	{
		const FWoTFireWallSprayer& A = Built.Sprayers[Index];
		const FWoTFireWallSprayer& B = Expected.Sprayers[Index];
		if (A.ClassIdentity != B.ClassIdentity || !A.Offset.Equals(B.Offset, 0.0)
			|| !F(A.Spread, B.Spread) || !F(A.Volume, B.Volume)
			|| !F(A.MinVolume, B.MinVolume) || !F(A.VolumeScalePct, B.VolumeScalePct)
			|| !F(A.VisibilityRadius, B.VisibilityRadius)
			|| !F(A.VisibilityHeight, B.VisibilityHeight)
			|| !A.Gravity.Equals(B.Gravity, 0.0) || A.bModulateOnly != B.bModulateOnly
			|| !F(A.FadeLifeSpan, B.FadeLifeSpan)
			|| A.Templates.Num() != B.Templates.Num())
		{
			OutError = FString::Printf(TEXT("fire-wall sprayer %d differs"), Index); return false;
		}
		for (int32 T = 0; T < A.Templates.Num(); ++T)
		{
			if (A.Templates[T].TextureIdentity != B.Templates[T].TextureIdentity
				|| !WoTFireWallTemplateEqual(A.Templates[T].Template,
					B.Templates[T].Template))
			{
				OutError = FString::Printf(TEXT("fire-wall sprayer %d template %d differs"),
					Index, T); return false;
			}
		}
	}
	const FWoTFireWallDefaults& A = Built.Defaults;
	const FWoTFireWallDefaults& B = Expected.Defaults;
	if (A.Health != B.Health || !F(A.DamageAmount, B.DamageAmount)
		|| !F(A.DamageInterval, B.DamageInterval) || A.SlabWidth != B.SlabWidth
		|| A.SlabMesh != B.SlabMesh || !F(A.SlabDrawScale, B.SlabDrawScale)
		|| !F(A.CollisionRadius, B.CollisionRadius)
		|| !F(A.CollisionHeight, B.CollisionHeight)
		|| A.MaxSlabs != B.MaxSlabs || A.InventoryCount != B.InventoryCount)
	{
		OutError = TEXT("fire-wall defaults differ"); return false;
	}
	OutError.Reset(); return true;
}
