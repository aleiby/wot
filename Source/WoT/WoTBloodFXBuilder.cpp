#include "WoTBloodFX.h"

#include "WoTRetailBuilderCanonical.h"
#include "World/WoTRuntimeClassDefaults.h"
#include "World/WoTRuntimeParticleProjection.h"
#include "World/WoTRuntimeWorldProperties.h"

namespace
{
struct FWoTBloodClassSpec
{
	const TCHAR* Name;
	const TCHAR* Identity;
	float BurstSeconds;
	float SelfDestroySeconds;
};

// Default-property anchors are ParticleSystems/BloodDamageRed1.uc:32-55,
// BloodDamageBlack1.uc:32-55, BloodBurstRed2.uc:32-64,
// BloodBurstBlack2.uc:32-64, BloodSprayRed.uc:12-22,
// BloodSprayBlack.uc:12-28, and BloodSpray01.uc:27-49.
// Burst/self-destroy timings come from PreBeginPlay/TriggerTimed methods, not
// CDO data, and intentionally remain the small native lifecycle policy layer.
constexpr FWoTBloodClassSpec GWoTBloodClasses[] = {
	{TEXT("BloodDamageRed1"), TEXT("ParticleSystems.BloodDamageRed1"), 0.1f, 2.1f},
	{TEXT("BloodDamageBlack1"), TEXT("ParticleSystems.BloodDamageBlack1"), 0.1f, 2.1f},
	{TEXT("BloodBurstRed2"), TEXT("ParticleSystems.BloodBurstRed2"), 0.1f, 2.1f},
	{TEXT("BloodBurstBlack2"), TEXT("ParticleSystems.BloodBurstBlack2"), 0.1f, 2.1f},
	{TEXT("BloodSprayRed"), TEXT("ParticleSystems.BloodSprayRed"), 0.2f, 2.2f},
	{TEXT("BloodSprayBlack"), TEXT("ParticleSystems.BloodSprayBlack"), 0.2f, 2.2f},
	{TEXT("BloodSpray01"), TEXT("ParticleSystems.BloodSpray01"), 0.0f, 0.0f},
};

FName WoTBloodLeaf(const FString& Identity)
{
	int32 Dot = INDEX_NONE;
	return FName(*(Identity.FindLastChar(TEXT('.'), Dot)
		? Identity.RightChop(Dot + 1) : Identity));
}

bool WoTBloodDecode(const FWoTRetailBuildContext& Context,
	const FWoTBloodClassSpec& Spec, FWoTBloodConfig& Out, FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	TArray<FString> Chain;
	if (!Defaults.GetClassChain(Spec.Identity, Chain))
	{
		OutError = FString::Printf(TEXT("blood class '%s' is missing"), Spec.Identity);
		return false;
	}
	FWoTRuntimeParticleSprayerConfiguration Particle;
	if (!WoTRuntimeReadParticleSprayerConfiguration(
		[&](FStringView Name, uint32 Index)
		{
			return Defaults.FindClassProperty(Spec.Identity, Name, Index);
		}, Particle))
	{
		OutError = FString::Printf(TEXT("blood class '%s' has malformed sprayer defaults"),
			Spec.Identity);
		return false;
	}
	Out.Spread = Particle.Spread;
	Out.Volume = Particle.Volume;
	Out.Gravity = Particle.Gravity;
	Out.PrimeCount = Particle.PrimeCount;
	Out.BurstSeconds = Spec.BurstSeconds;
	Out.SelfDestroySeconds = Spec.SelfDestroySeconds;
	if (const FWoTRuntimeWorldProperty* Gore =
		Defaults.FindClassProperty(Spec.Identity, TEXT("GoreLevel")))
	{
		uint8 Value = 0;
		if (!WoTRuntimeWorldPropertyByte(*Gore, Value))
		{
			OutError = FString::Printf(TEXT("blood class '%s' GoreLevel is malformed"),
				Spec.Identity);
			return false;
		}
		Out.GoreLevel = Value;
	}
	Out.Templates = MoveTemp(Particle.Templates);
	for (int32 Index = 0; Index < Particle.TextureIdentities.Num(); ++Index)
	{
		const FString& TextureIdentity = Particle.TextureIdentities[Index];
		FWoTRetailTextureFacts TextureFacts;
		if (!Context.FindTexture
			|| !Context.FindTexture(TextureIdentity, TextureFacts))
		{
			OutError = FString::Printf(
				TEXT("blood class '%s' texture '%s' is unavailable"),
				Spec.Identity, *TextureIdentity);
			return false;
		}
		const FName Texture = WoTBloodLeaf(TextureIdentity);
		Out.TextureNames.Add(Texture);
		Out.Templates[Index].TexWidth = static_cast<float>(TextureFacts.USize);
		Out.Templates[Index].TexHeight = static_cast<float>(TextureFacts.VSize);
	}
	if (const FWoTRuntimeWorldProperty* Decal =
		Defaults.FindClassProperty(Spec.Identity, TEXT("DecalType")))
	{
		if (Decal->Type != EWoTRuntimeWorldPropertyType::Object)
		{
			OutError = FString::Printf(TEXT("blood class '%s' DecalType is malformed"),
				Spec.Identity);
			return false;
		}
		Out.DecalTypeName = WoTBloodLeaf(Decal->ObjectIdentity);
	}
	if (const FWoTRuntimeWorldProperty* Percent =
		Defaults.FindClassProperty(Spec.Identity, TEXT("DecalPercent")))
	{
		if (!WoTRuntimeWorldPropertyFloat(*Percent, Out.DecalPercent))
		{
			OutError = FString::Printf(TEXT("blood class '%s' DecalPercent is malformed"),
				Spec.Identity);
			return false;
		}
	}
	return true;
}

bool WoTBloodTemplateEqual(const FWoTParticleTemplate& A,
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

void WoTBloodSerializeTemplate(TArray<uint8>& Out, const FWoTParticleTemplate& T)
{
	using namespace WoTRetailBuilderCanonical;
	for (float Value : {T.LifeSpan, T.Weight, T.MaxInitialVelocity,
		T.MinInitialVelocity, T.MaxDrawScale, T.MinDrawScale, T.MaxScaleGlow,
		T.MinScaleGlow})
	{
		Ascii(Out, " "); Float(Out, Value);
	}
	Ascii(Out, " "); Integer(Out, T.GrowPhase);
	for (float Value : {T.MaxGrowRate, T.MinGrowRate})
	{
		Ascii(Out, " "); Float(Out, Value);
	}
	Ascii(Out, " "); Integer(Out, T.FadePhase);
	for (float Value : {T.MaxFadeRate, T.MinFadeRate, T.TexWidth, T.TexHeight})
	{
		Ascii(Out, " "); Float(Out, Value);
	}
}
}

bool FWoTBloodFXBuilder::Build(const FWoTRetailBuildContext& Context,
	FWoTBloodSet& Out, FString& OutError)
{
	const FWoTRuntimeClassDefaults& Defaults = Context.ClassDefaults;
	if (!Defaults.IsLoaded())
	{
		OutError = TEXT("blood FX require loaded class defaults");
		return false;
	}
	FWoTBloodSet Candidate;
	for (const FWoTBloodClassSpec& Spec : GWoTBloodClasses)
	{
		FWoTBloodConfig Config;
		if (!WoTBloodDecode(Context, Spec, Config, OutError)) return false;
		Candidate.Configs.Add(FName(Spec.Name), MoveTemp(Config));
	}
	Out = MoveTemp(Candidate);
	OutError.Reset();
	return true;
}

void FWoTBloodFXBuilder::SerializeCanonical(const FWoTBloodSet& Set,
	TArray<uint8>& Out)
{
	using namespace WoTRetailBuilderCanonical;
	Out.Reset();
	Ascii(Out, "wot-blood-fx-v1\nconfigs "); Integer(Out, Set.Configs.Num());
	Ascii(Out, "\n");
	TArray<FName> Names;
	Set.Configs.GetKeys(Names);
	Names.Sort(NameLess);
	for (FName ConfigName : Names)
	{
		const FWoTBloodConfig& C = Set.Configs.FindChecked(ConfigName);
		Ascii(Out, "config "); Name(Out, ConfigName);
		for (float Value : {C.Spread, C.Volume, static_cast<float>(C.Gravity.X),
			static_cast<float>(C.Gravity.Y), static_cast<float>(C.Gravity.Z),
			C.PrimeCount, C.BurstSeconds, C.SelfDestroySeconds, C.DecalPercent})
		{
			Ascii(Out, " "); Float(Out, Value);
		}
		Ascii(Out, " "); Name(Out, C.DecalTypeName);
		Ascii(Out, " "); Integer(Out, C.GoreLevel);
		Ascii(Out, " templates "); Integer(Out, C.Templates.Num());
		Ascii(Out, "\n");
		for (int32 Index = 0; Index < C.Templates.Num(); ++Index)
		{
			Ascii(Out, "template "); Integer(Out, Index);
			Ascii(Out, " "); Name(Out, C.TextureNames.IsValidIndex(Index)
				? C.TextureNames[Index] : NAME_None);
			WoTBloodSerializeTemplate(Out, C.Templates[Index]);
			Ascii(Out, "\n");
		}
	}
}

FString FWoTBloodFXBuilder::CanonicalDigest(const FWoTBloodSet& Set)
{
	TArray<uint8> Bytes;
	SerializeCanonical(Set, Bytes);
	return Sha256(Bytes).ToHex();
}

bool FWoTBloodFXBuilder::AreEquivalent(const FWoTBloodSet& Built,
	const FWoTBloodSet& Expected, FString& OutError)
{
	if (Built.Configs.Num() != Expected.Configs.Num())
	{
		OutError = TEXT("blood config count differs"); return false;
	}
	for (const TPair<FName, FWoTBloodConfig>& Pair : Expected.Configs)
	{
		const FWoTBloodConfig* A = Built.Configs.Find(Pair.Key);
		const FWoTBloodConfig& B = Pair.Value;
		if (A == nullptr) { OutError = Pair.Key.ToString() + TEXT(" is missing"); return false; }
		auto F = [](float L, float R)
		{
			return WoTRetailBuilderCanonical::FloatBits(L)
				== WoTRetailBuilderCanonical::FloatBits(R);
		};
		if (!F(A->Spread, B.Spread) || !F(A->Volume, B.Volume)
			|| !A->Gravity.Equals(B.Gravity, 0.0) || !F(A->PrimeCount, B.PrimeCount)
			|| !F(A->BurstSeconds, B.BurstSeconds)
			|| !F(A->SelfDestroySeconds, B.SelfDestroySeconds)
			|| A->DecalTypeName != B.DecalTypeName
			|| !F(A->DecalPercent, B.DecalPercent)
			|| A->GoreLevel != B.GoreLevel
			|| A->TextureNames != B.TextureNames
			|| A->Templates.Num() != B.Templates.Num())
		{
			OutError = Pair.Key.ToString() + TEXT(" config differs"); return false;
		}
		for (int32 Index = 0; Index < A->Templates.Num(); ++Index)
		{
			if (!WoTBloodTemplateEqual(A->Templates[Index], B.Templates[Index]))
			{
				OutError = FString::Printf(TEXT("%s template %d differs"),
					*Pair.Key.ToString(), Index); return false;
			}
		}
	}
	OutError.Reset();
	return true;
}
