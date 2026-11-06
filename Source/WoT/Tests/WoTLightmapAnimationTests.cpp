// Version-5 animated light-map responses: the decoder's bounds, the retail
// per-frame recomposition, and the runtime BSP owner's texture updates.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#include "../WoTBSPPayload.h"
#include "../WoTDynamicLightSubsystem.h"
#include "../WoTLightWave.h"
#include "../World/WoTRuntimeBSPRenderer.h"
#include "../Payload/WoTRuntimeRegistry.h"
#include "WoTRetailReader/BSPWorld.h"
#include "WoTRetailReader/Contract.h"
#include "WoTRetailReader/ProductionContract.h"
#include "WoTRetailReader/Testing/ReferencePaths.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	struct FQuad
	{
		uint32 A = 0;
		uint32 B = 0;
		uint32 C = 0;
		uint32 D = 0;
	};

	struct FRectSpec
	{
		uint32 Surface = 0;
		TArray<uint8> RGB;
	};

	struct FTermSpec
	{
		uint8 Kind = 0;
		uint8 Group = 255;
		uint8 Effect = 0;
		uint32 Lights = 0;
		TArray<FRectSpec> Rects;
	};

	FQuad MakeQuad(const uint32 A, const uint32 B, const uint32 C, const uint32 D)
	{
		FQuad Result;
		Result.A = A; Result.B = B; Result.C = C; Result.D = D;
		return Result;
	}

	FRectSpec MakeRect(const uint32 Surface, const TArray<uint8>& RGB)
	{
		FRectSpec Result;
		Result.Surface = Surface;
		Result.RGB = RGB;
		return Result;
	}

	FTermSpec MakeTerm(const uint8 Kind, const uint8 Group, const uint8 Effect,
		const uint32 Lights, TArray<FRectSpec> Rects)
	{
		FTermSpec Result;
		Result.Kind = Kind; Result.Group = Group; Result.Effect = Effect;
		Result.Lights = Lights;
		Result.Rects = MoveTemp(Rects);
		return Result;
	}

	void AppendU16(TArray<uint8>& Out, const uint16 Value)
	{
		Out.Add(static_cast<uint8>(Value));
		Out.Add(static_cast<uint8>(Value >> 8));
	}

	void AppendU32(TArray<uint8>& Out, const uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
			Out.Add(static_cast<uint8>(Value >> Shift));
	}

	void AppendFloat(TArray<uint8>& Out, const float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(float));
		AppendU32(Out, Bits);
	}

	void AppendText(TArray<uint8>& Out, const ANSICHAR* Value)
	{
		Out.Append(reinterpret_cast<const uint8*>(Value),
			static_cast<int32>(FCStringAnsi::Strlen(Value)));
	}

	/** The version-5 animation trailer (WoTWorldEncodePart layout). */
	TArray<uint8> MakeTrailer(const TArray<FQuad>& Surfaces,
		const TArray<FQuad>& Groups, const TArray<FTermSpec>& Terms)
	{
		TArray<uint8> Body;
		AppendU32(Body, 0);
		AppendU32(Body, static_cast<uint32>(Surfaces.Num()));
		for (const FQuad& Surface : Surfaces)
		{
			AppendU32(Body, Surface.A);
			AppendU32(Body, Surface.B);
			AppendU32(Body, Surface.C);
			AppendU32(Body, Surface.D);
		}
		AppendU32(Body, static_cast<uint32>(Groups.Num()));
		for (const FQuad& Group : Groups)
		{
			Body.Add(static_cast<uint8>(Group.A));
			Body.Add(static_cast<uint8>(Group.B));
			Body.Add(static_cast<uint8>(Group.C));
			Body.Add(0);
			AppendU32(Body, Group.D);
		}
		AppendU32(Body, static_cast<uint32>(Terms.Num()));
		for (const FTermSpec& Spec : Terms)
		{
			Body.Add(Spec.Kind);
			Body.Add(Spec.Group);
			Body.Add(Spec.Effect);
			Body.Add(0);
			AppendU32(Body, Spec.Lights);
			AppendU32(Body, static_cast<uint32>(Spec.Rects.Num()));
			for (const FRectSpec& RectSpec : Spec.Rects)
			{
				AppendU32(Body, RectSpec.Surface);
				Body.Append(RectSpec.RGB);
			}
		}
		const uint32 Count = static_cast<uint32>(Body.Num());
		Body[0] = static_cast<uint8>(Count);
		Body[1] = static_cast<uint8>(Count >> 8);
		Body[2] = static_cast<uint8>(Count >> 16);
		Body[3] = static_cast<uint8>(Count >> 24);
		return Body;
	}

	/** One lit triangle in one section over an 8x8 atlas, then the trailer. */
	TArray<uint8> MakeAnimatedPayload(const TArray<uint8>& Trailer, const bool bWithAnimation)
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>("WOTPBSP"), 8);
		AppendU16(Bytes, 5);
		AppendU16(Bytes, static_cast<uint16>(2 | 4 | (bWithAnimation ? 16 : 0)));
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 1);
		AppendU16(Bytes, 2);
		AppendU16(Bytes, 1);
		const float Bounds[] = {0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0.0f};
		for (const float Value : Bounds) AppendFloat(Bytes, Value);
		const ANSICHAR* Slot = "WorldT__Stone";
		AppendU16(Bytes, static_cast<uint16>(FCStringAnsi::Strlen(Slot)));
		AppendU16(Bytes, 0);
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 2);
		Bytes.Add(1); Bytes.Add(0); Bytes.Add(0); Bytes.Add(0);
		AppendText(Bytes, Slot);
		const ANSICHAR* Package = "WorldT";
		const ANSICHAR* Name = "Stone";
		const ANSICHAR* Source = "WorldT.Detail.Stone";
		AppendU16(Bytes, static_cast<uint16>(FCStringAnsi::Strlen(Package)));
		AppendU16(Bytes, static_cast<uint16>(FCStringAnsi::Strlen(Name)));
		AppendU16(Bytes, static_cast<uint16>(FCStringAnsi::Strlen(Source)));
		AppendU16(Bytes, 0);
		AppendU32(Bytes, 0);
		AppendText(Bytes, Package);
		AppendText(Bytes, Name);
		AppendText(Bytes, Source);
		const float Positions[] = {0.0f, 0.0f, 0.0f, 100.0f, 0.0f, 0.0f, 0.0f, 100.0f, 0.0f};
		for (const float Value : Positions) AppendFloat(Bytes, Value);
		const float Normals[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
		for (const float Value : Normals) AppendFloat(Bytes, Value);
		const float UV0[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
		for (const float Value : UV0) AppendFloat(Bytes, Value);
		const float UV1[] = {0.2f, 0.2f, 0.5f, 0.2f, 0.2f, 0.5f};
		for (const float Value : UV1) AppendFloat(Bytes, Value);
		for (uint32 Index = 0; Index < 3; ++Index) AppendU32(Bytes, Index);
		AppendU32(Bytes, 8);
		AppendU32(Bytes, 8);
		AppendU32(Bytes, 8 * 8 * 4);
		for (int32 Pixel = 0; Pixel < 64; ++Pixel)
		{
			Bytes.Add(100); Bytes.Add(120); Bytes.Add(140); Bytes.Add(255);
		}
		Bytes.Append(Trailer);
		return Bytes;
	}

	/** A version-6 sky-rest trailer (BSPWorld.cpp WoTWorldBuildSkyRestTrailer). */
	struct FSkyRestGroupSpec
	{
		uint8 Role = 1;
		const ANSICHAR* Slot = "";
		int32 Triangles = 1;
	};

	TArray<uint8> MakeSkyRestTrailer(const uint32 SkyZone, const uint32 Rays,
		const uint32 Covered, const TArray<FQuad>& Surfaces,
		const TArray<FSkyRestGroupSpec>& Groups)
	{
		TArray<uint8> Body;
		AppendU32(Body, 0);
		AppendU32(Body, SkyZone);
		AppendU32(Body, Rays);
		AppendU32(Body, Covered);
		AppendU32(Body, static_cast<uint32>(Surfaces.Num()));
		for (const FQuad& Surface : Surfaces)
		{
			AppendU32(Body, Surface.A);
			AppendU32(Body, Surface.B);
			AppendU32(Body, Surface.C);
			AppendU32(Body, Surface.D);
		}
		AppendU32(Body, static_cast<uint32>(Groups.Num()));
		for (const FSkyRestGroupSpec& Group : Groups)
		{
			Body.Add(Group.Role);
			Body.Add(0);
			AppendU16(Body, static_cast<uint16>(FCStringAnsi::Strlen(Group.Slot)));
			AppendText(Body, Group.Slot);
			AppendU32(Body, static_cast<uint32>(Group.Triangles));
			for (int32 Triangle = 0; Triangle < Group.Triangles; ++Triangle)
			{
				for (const float Value : {0.0f, 0.0f, 1.0f}) AppendFloat(Body, Value);
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const float X = 10.0f * static_cast<float>(Corner == 1);
					const float Y = 10.0f * static_cast<float>(Corner == 2);
					for (const float Value : {X, Y, 500.0f + Triangle, 0.5f, 0.25f,
						0.125f, 0.0625f}) AppendFloat(Body, Value);
				}
			}
		}
		const uint32 Count = static_cast<uint32>(Body.Num());
		Body[0] = static_cast<uint8>(Count);
		Body[1] = static_cast<uint8>(Count >> 8);
		Body[2] = static_cast<uint8>(Count >> 16);
		Body[3] = static_cast<uint8>(Count >> 24);
		return Body;
	}

	TArray<uint8> Ramp(const int32 Count, const int32 Start, const int32 Step)
	{
		TArray<uint8> Out;
		for (int32 Index = 0; Index < Count; ++Index)
			Out.Add(static_cast<uint8>(Start + Index * Step));
		return Out;
	}

	bool Decodes(const TArray<uint8>& Payload, FWoTBSPPayloadData& Out, FString& Error)
	{
		return UWoTBSPPayloadLibrary::DecodePayload(Payload, Out, Error);
	}

	// Independent transcription of the UE1 retail integer-angle waveform. Keep
	// this out of the production evaluator so the mover material transport spec
	// can catch a shared arithmetic, type, or Period-vs-Period+1 regression.
	float IndependentMoverWaveContribution(const uint8 Type, const float TimeSeconds,
		const uint8 Period, const uint8 Phase)
	{
		if (Type == 3)
		{
			const double Units = static_cast<double>(TimeSeconds) * 35.0 * 65536.0
				/ (static_cast<double>(Period) + 1.0)
				+ static_cast<double>(static_cast<uint32>(Phase) << 8);
			return (static_cast<int64>(Units) & 1) ? 0.0f : 1.0f;
		}
		const double Units = static_cast<double>(TimeSeconds) * 35.0 * 65536.0
			/ static_cast<double>(FMath::Max(static_cast<int32>(Period), 1))
			+ static_cast<double>(static_cast<uint32>(Phase) << 8);
		const int32 TableIndex = (static_cast<int32>(Units) >> 2) & 16383;
		const double Angle = static_cast<double>(static_cast<float>(TableIndex))
			* 2.0 * 3.1415926535897932
			/ static_cast<double>(static_cast<float>(16384));
		const float SinTheta = static_cast<float>(FMath::Sin(Angle));
		return Type == 7 ? 0.90f + 0.09f * SinTheta
			: 0.60f + 0.39f * SinTheta;
	}

	float IndependentMoverWaveNormalized(const uint8 Type, const float TimeSeconds,
		const uint8 Period, const uint8 Phase)
	{
		const float Contribution = IndependentMoverWaveContribution(
			Type, TimeSeconds, Period, Phase);
		if (Type == 3) return (Contribution - 0.50f) / 0.50f;
		if (Type == 7) return (Contribution - 0.90f) / 0.09f;
		return (Contribution - 0.60f) / 0.39f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP11MoverSharedEvaluatorSpec,
	"WoT.P11.MoverLightmap.SharedEvaluatorParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoTP11MoverSharedEvaluatorSpec::RunTest(const FString&)
{
	struct FCase
	{
		uint8 Type;
		uint8 Period;
		uint8 Phase;
		float Time;
	};
	const FCase Cases[] = {
		{2, 100, 0, 0.0f}, {2, 100, 0, 0.347f},
		{7, 83, 0, 1.75f}, {7, 134, 17, 3.125f},
		{3, 0, 0, 0.0f}, {3, 0, 0, 1.0f / (35.0f * 65536.0f)},
		{3, 7, 3, 0.019f}, {3, 7, 3, 0.020f},
	};
	for (const FCase& Case : Cases)
	{
		FWoTLightWaveGroup Group;
		Group.Type = Case.Type;
		Group.Period = Case.Period;
		Group.Phase = Case.Phase;
		uint32 NoiseFrame = 0;
		const FWoTLightWaveParameterValues Parameters =
			AWoTLightWave::EvaluateParameters(
				Case.Time, false, {Group}, NoiseFrame);
		const float MaterialTransport = 2.0f * Parameters.Waves[0] - 1.0f;
		const float Independent = IndependentMoverWaveNormalized(
			Case.Type, Case.Time, Case.Period, Case.Phase);
		TestTrue(FString::Printf(
			TEXT("mover transport matches independent retail oracle: type=%u period=%u phase=%u time=%.6f"),
			Case.Type, Case.Period, Case.Phase, Case.Time),
			FMath::IsNearlyEqual(MaterialTransport, Independent, 0.000001f));
	}

	UMaterialInterface* GeneratedMoverMIC = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/MI_WoT_MoverWaveRuntime.MI_WoT_MoverWaveRuntime"));
	if (!TestNotNull(TEXT("generated mover-wave MIC loads"), GeneratedMoverMIC))
	{
		return false;
	}
	#if WITH_EDITORONLY_DATA
	for (int32 Index = 0; Index < 6; ++Index)
	{
		bool bUseDelta = false;
		FGuid ExpressionGuid;
		TestTrue(FString::Printf(TEXT("generated MIC exposes UseDelta%d"), Index),
			GeneratedMoverMIC->GetStaticSwitchParameterValue(
				FHashedMaterialParameterInfo(*FString::Printf(
					TEXT("UseDelta%d"), Index)), bUseDelta, ExpressionGuid));
		TestTrue(FString::Printf(TEXT("generated MIC compiles UseDelta%d on"), Index),
			bUseDelta);
	}
	#endif

	FWoTLightWaveGroup Blink;
	Blink.Type = 3;
	Blink.Period = 7;
	Blink.Phase = 0;
	const float BlinkUnitSeconds = 8.0f / (35.0f * 65536.0f);
	for (const float Units : {0.5f, 1.5f, 2.5f})
	{
		uint32 NoiseFrame = 0;
		const float TimeSeconds = Units * BlinkUnitSeconds;
		const FWoTLightWaveParameterValues Parameters =
			AWoTLightWave::EvaluateParameters(
				TimeSeconds, false, {Blink}, NoiseFrame);
		const float MaterialCoefficient =
			2.0f * Parameters.Waves[0] - 1.0f;
		const float ActualContribution = 0.50f + 0.25f * MaterialCoefficient;
		const float ExpectedContribution = 0.50f + 0.25f
			* IndependentMoverWaveNormalized(3, TimeSeconds, 7, 0);
		TestEqual(FString::Printf(
			TEXT("generated MIC Blink contribution at %.1f Period+1 units"), Units),
			ActualContribution, ExpectedContribution);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTP11MoverStaticMeanSpec,
	"WoT.P11.MoverLightmap.StaticMeanInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWoTP11MoverStaticMeanSpec::RunTest(const FString&)
{
	TestEqual(TEXT("steady mean remains the authored contribution"),
		UWoTDynamicLightSubsystem::RetailWaveMean(1), 1.0f);
	TestEqual(TEXT("steady response amplitude remains zero"),
		UWoTDynamicLightSubsystem::RetailWaveAmplitude(1), 0.0f);
	TestEqual(TEXT("steady evaluator remains constant"),
		UWoTDynamicLightSubsystem::EvaluateRetailWaveform(
			1, 123.0f, 0, 255, 0, 0), 1.0f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightmapAnimationDecodeSpec,
	"WoT.LightmapAnimation.Version5Decode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightmapAnimationDecodeSpec::RunTest(const FString&)
{
	// Surface 0 sits at (1,1) 2x2 with its gutter inside the 8x8 atlas;
	// surface 1 has no light map (an all-zero placement).
	const TArray<FQuad> Surfaces = {MakeQuad(1, 1, 2, 2), MakeQuad(0, 0, 0, 0)};
	const TArray<FQuad> Groups = {MakeQuad(2, 64, 0, 3), MakeQuad(7, 32, 5, 1)};
	const TArray<uint8> RGB = Ramp(12, 10, 2);
	TArray<FTermSpec> Terms;
	Terms.Add(MakeTerm(0, 0, 0, 3, {MakeRect(0, RGB)}));
	Terms.Add(MakeTerm(0, 1, 0, 1, {MakeRect(0, RGB)}));
	Terms.Add(MakeTerm(1, 255, 3, 2, {MakeRect(0, RGB)}));
	Terms.Add(MakeTerm(2, 1, 3, 1, {MakeRect(0, RGB)}));
	const TArray<uint8> Payload = MakeAnimatedPayload(
		MakeTrailer(Surfaces, Groups, Terms), true);
	FWoTBSPPayloadData Decoded;
	FString Error;
	if (!TestTrue(TEXT("version-5 animated payload decodes"), Decodes(Payload, Decoded, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("format version"), static_cast<int32>(Decoded.FormatVersion), 5);
	TestTrue(TEXT("animation present"), Decoded.HasLightmapAnimation());
	TestEqual(TEXT("surface table"), Decoded.AnimationSurfaces.Num(), 2);
	TestEqual(TEXT("group table"), Decoded.AnimationGroups.Num(), 2);
	TestEqual(TEXT("term table"), Decoded.AnimationTerms.Num(), 4);
	if (Decoded.AnimationSurfaces.Num() == 2 && Decoded.AnimationGroups.Num() == 2
		&& Decoded.AnimationTerms.Num() == 4)
	{
		TestTrue(TEXT("surface 0 placement"),
			Decoded.AnimationSurfaces[0].X == 1 && Decoded.AnimationSurfaces[0].Y == 1
			&& Decoded.AnimationSurfaces[0].W == 2 && Decoded.AnimationSurfaces[0].H == 2);
		TestTrue(TEXT("group 1 key"),
			Decoded.AnimationGroups[1].Type == 7 && Decoded.AnimationGroups[1].Period == 32
			&& Decoded.AnimationGroups[1].Phase == 5 && Decoded.AnimationGroups[1].LightCount == 1);
		TestTrue(TEXT("intersection term row"),
			Decoded.AnimationTerms[3].Kind == 2 && Decoded.AnimationTerms[3].Group == 1
			&& Decoded.AnimationTerms[3].Effect == 3 && Decoded.AnimationTerms[3].LightCount == 1
			&& Decoded.AnimationTerms[3].Rects.Num() == 1
			&& Decoded.AnimationTerms[3].Rects[0].SurfaceIndex == 0
			&& Decoded.AnimationTerms[3].Rects[0].RGB == RGB);
		TestEqual(TEXT("effect term has no group"),
			static_cast<int32>(Decoded.AnimationTerms[2].Group), 255);
	}

	// Without flag 16 the same bytes are trailing garbage; a version-4 header
	// may not carry the flag; bad tables are refused before any bytes are
	// adopted.
	FWoTBSPPayloadData Rejected;
	TestFalse(TEXT("unflagged trailing bytes are rejected"),
		Decodes(MakeAnimatedPayload(MakeTrailer(Surfaces, Groups, Terms), false), Rejected, Error));
	TArray<uint8> Stale = Payload;
	Stale[8] = 4;
	TestFalse(TEXT("a version-4 header rejects the animation flag"),
		Decodes(Stale, Rejected, Error));
	TArray<FTermSpec> Disordered;
	Disordered.Add(MakeTerm(1, 255, 3, 2, {MakeRect(0, RGB)}));
	Disordered.Add(MakeTerm(0, 0, 0, 3, {MakeRect(0, RGB)}));
	Disordered.Add(MakeTerm(0, 1, 0, 1, {MakeRect(0, RGB)}));
	TestFalse(TEXT("terms out of material slot order are rejected"),
		Decodes(MakeAnimatedPayload(MakeTrailer(Surfaces, Groups, Disordered), true),
			Rejected, Error));
	TArray<FTermSpec> Unplaced;
	Unplaced.Add(MakeTerm(0, 0, 0, 3, {MakeRect(1, RGB)}));
	Unplaced.Add(MakeTerm(0, 1, 0, 1, {MakeRect(0, RGB)}));
	TestFalse(TEXT("a rect on an unplaced surface is rejected"),
		Decodes(MakeAnimatedPayload(MakeTrailer(Surfaces, Groups, Unplaced), true),
			Rejected, Error));
	TArray<FTermSpec> Short;
	Short.Add(MakeTerm(0, 0, 0, 3, {MakeRect(0, Ramp(11, 10, 2))}));
	Short.Add(MakeTerm(0, 1, 0, 1, {MakeRect(0, RGB)}));
	TestFalse(TEXT("a short rect is rejected"),
		Decodes(MakeAnimatedPayload(MakeTrailer(Surfaces, Groups, Short), true),
			Rejected, Error));
	const TArray<FQuad> Overlapping = {MakeQuad(1, 1, 2, 2), MakeQuad(2, 2, 2, 2)};
	TArray<FTermSpec> Both;
	Both.Add(MakeTerm(0, 0, 0, 3, {MakeRect(0, RGB), MakeRect(1, RGB)}));
	Both.Add(MakeTerm(0, 1, 0, 1, {MakeRect(0, RGB)}));
	TestFalse(TEXT("overlapping placements are rejected"),
		Decodes(MakeAnimatedPayload(MakeTrailer(Overlapping, Groups, Both), true),
			Rejected, Error));
	return true;
}

// Version 6 adds the sky-rest trailer (flag 32) after any animation
// trailer; each trailer is located by its own byte count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPSkyRestDecodeSpec,
	"WoT.LightmapAnimation.Version6SkyRestDecode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPSkyRestDecodeSpec::RunTest(const FString&)
{
	const TArray<FQuad> Surfaces = {MakeQuad(252, 1107888, 0, 0),
		MakeQuad(887, 0, 3, 0), MakeQuad(900, 0, 0, 7)};
	const TArray<FSkyRestGroupSpec> Groups = {
		{2, "", 2}, {1, "NatureT__SkyMod442", 3}, {3, "", 1}};
	const TArray<uint8> SkyRest = MakeSkyRestTrailer(2, 4096u * 2048u, 4104868u,
		Surfaces, Groups);
	auto Payload = [](const uint16 Version, const uint16 Flags, const TArray<uint8>& Tail)
	{
		TArray<uint8> Bytes = MakeAnimatedPayload(Tail, (Flags & 16) != 0);
		Bytes[8] = static_cast<uint8>(Version);
		Bytes[10] = static_cast<uint8>(Flags);
		return Bytes;
	};
	FWoTBSPPayloadData Decoded;
	FString Error;
	if (!TestTrue(TEXT("a version-6 sky-rest payload decodes"),
		Decodes(Payload(6, 2 | 4 | 32, SkyRest), Decoded, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("format version"), static_cast<int32>(Decoded.FormatVersion), 6);
	TestTrue(TEXT("the census is present"), Decoded.SkyRest.bPresent);
	TestEqual(TEXT("sky zone"), Decoded.SkyRest.SkyZone, 2u);
	TestEqual(TEXT("ray count"), Decoded.SkyRest.RayCount, 4096u * 2048u);
	TestEqual(TEXT("covered rays"), Decoded.SkyRest.CoveredRays, 4104868u);
	if (TestEqual(TEXT("surface rows"), Decoded.SkyRest.Surfaces.Num(), 3))
	{
		TestTrue(TEXT("visible row"), Decoded.SkyRest.Surfaces[0].Surface == 252
			&& Decoded.SkyRest.Surfaces[0].VisibleRays == 1107888);
		TestEqual(TEXT("blocker rays"), Decoded.SkyRest.Surfaces[1].BlockerRays, 3u);
		TestEqual(TEXT("window rays"), Decoded.SkyRest.Surfaces[2].FakeBackdropRays, 7u);
	}
	if (TestEqual(TEXT("groups"), Decoded.SkyRest.Groups.Num(), 3))
	{
		TestTrue(TEXT("blocker group"),
			Decoded.SkyRest.Groups[0].Role == EWoTBSPSkyRestRole::Blocker
			&& Decoded.SkyRest.Groups[0].Slot.IsEmpty()
			&& Decoded.SkyRest.Groups[0].Positions.Num() == 6);
		TestTrue(TEXT("visible group keeps its world slot"),
			Decoded.SkyRest.Groups[1].Role == EWoTBSPSkyRestRole::Visible
			&& Decoded.SkyRest.Groups[1].Slot == TEXT("NatureT__SkyMod442")
			&& Decoded.SkyRest.Groups[1].Positions.Num() == 9);
		TestTrue(TEXT("vertex streams"),
			Decoded.SkyRest.Groups[1].Positions[4].Equals(FVector3f(10, 0, 501))
			&& Decoded.SkyRest.Groups[1].UV1[4].Equals(FVector2f(0.125f, 0.0625f))
			&& Decoded.SkyRest.Groups[1].Normals[4].Equals(FVector3f(0, 0, 1)));
	}
	// Animation and sky rest together: each trailer by its own byte count.
	const TArray<FQuad> Placements = {MakeQuad(1, 1, 2, 2)};
	TArray<FTermSpec> Terms;
	Terms.Add(MakeTerm(1, 255, 3, 2, {MakeRect(0, Ramp(12, 10, 2))}));
	TArray<uint8> Both = MakeTrailer(Placements, {}, Terms);
	Both.Append(SkyRest);
	FWoTBSPPayloadData Combined;
	TestTrue(TEXT("animation then sky rest decode"),
		Decodes(Payload(6, 2 | 4 | 16 | 32, Both), Combined, Error)
		&& Combined.HasLightmapAnimation() && Combined.SkyRest.bPresent
		&& Combined.SkyRest.Groups.Num() == 3);

	FWoTBSPPayloadData Rejected;
	TestFalse(TEXT("a version-5 header rejects flag 32"),
		Decodes(Payload(5, 2 | 4 | 32, SkyRest), Rejected, Error));
	TestFalse(TEXT("flag 32 requires the atlas"),
		Decodes(Payload(6, 2 | 32, SkyRest), Rejected, Error));
	TestFalse(TEXT("unflagged trailer bytes are rejected"),
		Decodes(Payload(6, 2 | 4, SkyRest), Rejected, Error));
	TestFalse(TEXT("covered rays over the ray count are rejected"),
		Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(2, 10, 11, Surfaces, Groups)),
			Rejected, Error));
	TestFalse(TEXT("sky zone 0 is rejected"),
		Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(0, 10, 5, Surfaces, Groups)),
			Rejected, Error));
	const TArray<FQuad> Unsorted = {MakeQuad(887, 0, 3, 0), MakeQuad(252, 1, 0, 0)};
	TestFalse(TEXT("unsorted surface rows are rejected"),
		Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(2, 10, 5, Unsorted, Groups)),
			Rejected, Error));
	TestFalse(TEXT("a visible group without a slot is rejected"),
		Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(2, 10, 5, Surfaces,
			{{1, "", 1}})), Rejected, Error));
	TestFalse(TEXT("a blocker group with a slot is rejected"),
		Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(2, 10, 5, Surfaces,
			{{2, "Pkg__Wall", 1}})), Rejected, Error));
	TestFalse(TEXT("an unknown role is rejected"),
		Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(2, 10, 5, Surfaces,
			{{4, "", 1}})), Rejected, Error));
	TArray<uint8> Truncated = SkyRest;
	Truncated.SetNum(Truncated.Num() - 4);
	TestFalse(TEXT("a truncated trailer is rejected"),
		Decodes(Payload(6, 2 | 4 | 32, Truncated), Rejected, Error));

	// Slot text follows the strict native reader and Tools/convert/bsp_payload.py
	// _decode_bsp_text_at: exact UTF-8 round trip, no NUL, no control characters.
	FWoTBSPPayloadData Accented;
	TestTrue(TEXT("a valid multi-byte UTF-8 slot decodes"),
		Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(2, 10, 5, Surfaces,
			{{1, "Sky\xc3\xa9", 1}})), Accented, Error)
		&& Accented.SkyRest.Groups.Num() == 1
		&& Accented.SkyRest.Groups[0].Slot == FString(TEXT("Sky\u00e9")));
	struct FMalformedSlot { const TCHAR* Label; const ANSICHAR* Slot; };
	static const FMalformedSlot Malformed[] = {
		{TEXT("a truncated multi-byte sequence"), "Sky\xc3"},
		{TEXT("an overlong encoding"), "Sky\xc0\xaf"},
		{TEXT("a UTF-8 encoded surrogate"), "Sky\xed\xa0\x80"},
		{TEXT("a stray continuation byte"), "Sky\x80"},
		{TEXT("a control character"), "Sky\x01"},
		{TEXT("a DEL character"), "Sky\x7f"},
	};
	for (const FMalformedSlot& Case : Malformed)
	{
		TestFalse(FString::Printf(TEXT("%s in a sky-rest slot is rejected"), Case.Label),
			Decodes(Payload(6, 2 | 4 | 32, MakeSkyRestTrailer(2, 10, 5, Surfaces,
				{{1, Case.Slot, 1}})), Rejected, Error));
	}
	// An embedded NUL: build a slot then clear its middle byte in place.
	TArray<uint8> WithNul = MakeSkyRestTrailer(2, 10, 5, Surfaces, {{1, "SkyXNul", 1}});
	bool bPatched = false;
	for (int32 Index = 0; Index + 7 <= WithNul.Num() && !bPatched; ++Index)
	{
		if (FMemory::Memcmp(WithNul.GetData() + Index, "SkyXNul", 7) == 0)
		{
			WithNul[Index + 3] = 0;
			bPatched = true;
		}
	}
	TestTrue(TEXT("the NUL fixture was patched"), bPatched);
	TestFalse(TEXT("an embedded NUL in a sky-rest slot is rejected"),
		Decodes(Payload(6, 2 | 4 | 32, WithNul), Rejected, Error));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightmapAnimationComposeSpec,
	"WoT.LightmapAnimation.Recomposition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightmapAnimationComposeSpec::RunTest(const FString&)
{
	// base + z*R at the waveform endpoints reproduces the retail hi/lo
	// compositions the response encodes: z=+1 -> base+R, z=-1 -> base-R,
	// clamped to the atlas range; a frozen frame is the literal base.
	TArray<uint8> Base;
	for (int32 Pixel = 0; Pixel < 64; ++Pixel)
	{
		Base.Add(100); Base.Add(120); Base.Add(140); Base.Add(255);
	}
	FWoTBSPAnimationSurface Surface;
	Surface.X = 1; Surface.Y = 1; Surface.W = 2; Surface.H = 2;
	FWoTBSPAnimationTerm Wave;
	Wave.Kind = 0; Wave.Group = 0; Wave.Effect = 0; Wave.LightCount = 1;
	FWoTBSPAnimationRect WaveRect;
	WaveRect.SurfaceIndex = 0;
	WaveRect.RGB = {10, 20, 30, 10, 20, 30, 10, 20, 30, 200, 200, 200};
	TArray<const FWoTBSPAnimationTerm*> Terms;
	Terms.Add(&Wave);
	TArray<const FWoTBSPAnimationRect*> Rects;
	Rects.Add(&WaveRect);
	FWoTLightmapAnimationFrame Frame;
	Frame.bFrozen = false;
	Frame.Wave[0] = 1.0f;
	TArray<uint8> Out;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	if (!TestEqual(TEXT("composed block is W*H*4"), Out.Num(), 16)) return false;
	// The atlas is BGRA (100,120,140) = RGB (140,120,100); responses are RGB.
	TestEqual(TEXT("z=+1 adds the response (R)"), static_cast<int32>(Out[2]), 150);
	TestEqual(TEXT("z=+1 adds the response (G)"), static_cast<int32>(Out[1]), 140);
	TestEqual(TEXT("z=+1 adds the response (B)"), static_cast<int32>(Out[0]), 130);
	TestEqual(TEXT("alpha stays opaque"), static_cast<int32>(Out[3]), 255);
	TestEqual(TEXT("z=+1 ceilings at 254"), static_cast<int32>(Out[3 * 4 + 2]), 254);
	Frame.Wave[0] = -1.0f;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	TestEqual(TEXT("z=-1 subtracts the response (R)"), static_cast<int32>(Out[2]), 130);
	TestEqual(TEXT("z=-1 floors at 0"), static_cast<int32>(Out[3 * 4 + 0]), 0);
	Frame.Wave[0] = 0.5f;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	TestEqual(TEXT("z=0.5 rounds to nearest"), static_cast<int32>(Out[2]), 145);
	Frame.Wave[0] = 0.0f;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	TestEqual(TEXT("z=0 is the literal base"), static_cast<int32>(Out[2]), 140);
	Frame.bFrozen = true;
	Frame.Wave[0] = 1.0f;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	TestEqual(TEXT("frozen frame is the literal base"), static_cast<int32>(Out[2]), 140);

	// Shimmer terms read the retail random tables by the surface-local texel
	// key (WoT.LightmapAnimation.NativeShimmerRandoms covers the tables): an
	// effect term ignores the waveform, an intersection term multiplies both,
	// and a frame without tables composes the shimmer at its mean.
	FWoTLightmapShimmerRandoms Randoms;
	Randoms.Reset(WoTLightmapShimmerSeed);
	Randoms.Tick(1.0f);
	FWoTBSPAnimationTerm Effect;
	Effect.Kind = 1; Effect.Group = 255; Effect.Effect = 3; Effect.LightCount = 1;
	Terms.Reset();
	Terms.Add(&Effect);
	Frame.bFrozen = false;
	Frame.Wave[0] = 0.0f;
	Frame.Randoms = &Randoms;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	const float Noise = 2.0f * Randoms.Random(0) - 1.0f;
	TestEqual(TEXT("a watery effect term applies the continuous random at key 0"),
		static_cast<int32>(Out[2]),
		FMath::Clamp(FMath::RoundToInt(140.0f + Noise * 10.0f), 0, 254));
	const float Noise3 = 2.0f * Randoms.Random(3) - 1.0f;
	TestEqual(TEXT("texel 3 reads key 3 (surface-local, row-major)"),
		static_cast<int32>(Out[3 * 4 + 2]),
		FMath::Clamp(FMath::RoundToInt(140.0f + Noise3 * 200.0f), 0, 254));
	FWoTBSPAnimationTerm Intersection;
	Intersection.Kind = 2; Intersection.Group = 0; Intersection.Effect = 3;
	Intersection.LightCount = 1;
	Terms.Reset();
	Terms.Add(&Intersection);
	Frame.Wave[0] = 0.5f;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	TestEqual(TEXT("intersection term multiplies wave and random"),
		static_cast<int32>(Out[2]),
		FMath::Clamp(FMath::RoundToInt(140.0f + 0.5f * Noise * 10.0f), 0, 254));
	Frame.Randoms = nullptr;
	Frame.Wave[0] = 1.0f;
	WoTLightmapComposeSurface(Base, 8, Surface, Terms, Rects, Frame, Out);
	TestEqual(TEXT("no tables: shimmer terms compose at their mean"),
		static_cast<int32>(Out[2]), 140);

	// The runtime owner: decode, adopt, and recompose into the shared texture.
	const TArray<FQuad> Surfaces = {MakeQuad(1, 1, 2, 2)};
	const TArray<FQuad> Groups = {MakeQuad(2, 64, 0, 1)};
	TArray<FTermSpec> Specs;
	Specs.Add(MakeTerm(0, 0, 0, 1, {MakeRect(0, WaveRect.RGB)}));
	FWoTBSPPayloadData Decoded;
	FString Error;
	if (!TestTrue(TEXT("owner payload decodes"),
		Decodes(MakeAnimatedPayload(MakeTrailer(Surfaces, Groups, Specs), true), Decoded, Error)))
	{
		AddError(Error);
		return false;
	}
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTRuntimeBSPWorld* Owner = World ? World->SpawnActor<AWoTRuntimeBSPWorld>() : nullptr;
	UTexture2D* Lightmap = WoTRuntimeBSPMaterializeLightmap(Decoded);
	if (TestNotNull(TEXT("owner spawned"), Owner) && TestNotNull(TEXT("lightmap"), Lightmap))
	{
		TestFalse(TEXT("owner is inert before adoption"), Owner->IsActorTickEnabled());
		Owner->SetLightmapAnimation(Decoded, Lightmap);
		TestTrue(TEXT("owner adopted the animation"), Owner->HasLightmapAnimation());
		TestEqual(TEXT("one affected surface"), Owner->GetAnimatedSurfaceCount(), 1);
		TestTrue(TEXT("owner ticks while animated"), Owner->IsActorTickEnabled());
		TestEqual(TEXT("driver groups follow the trailer"),
			Owner->GetAnimationGroups().Num(), 1);
		Owner->RecomposeLightmap(0.25f, false);
		const uint32 FrameA = Owner->GetAnimationFrameForTesting();
		Owner->RecomposeLightmap(0.5f, false);
		TestEqual(TEXT("noise frame advances per live recomposition"),
			Owner->GetAnimationFrameForTesting(), FrameA + 1u);
		Owner->RecomposeLightmap(0.75f, true);
		const uint32 Frozen = Owner->GetAnimationFrameForTesting();
		Owner->RecomposeLightmap(1.0f, true);
		TestEqual(TEXT("frozen recompositions hold the frame"),
			Owner->GetAnimationFrameForTesting(), Frozen);
		// The same retail sine drives the MPC and the recomposer.
		const float Scalar = AWoTLightWave::EvaluateWaveScalar(0.25f, 64.0f, 0.0f);
		TestTrue(TEXT("wave scalar is in range"), Scalar >= 0.0f && Scalar <= 1.0f);
		FWoTBSPPayloadData Unanimated;
		Owner->SetLightmapAnimation(Unanimated, Lightmap);
		TestFalse(TEXT("an unanimated payload disarms the owner"), Owner->HasLightmapAnimation());
		TestFalse(TEXT("a disarmed owner stops ticking"), Owner->IsActorTickEnabled());
	}
	if (World) WoTTest::DestroySpecWorld(World);
	return true;
}

// The runtime BSP owner has two per-frame duties (the animated
// light-map recomposition and the WoTSceneTime pan driver) behind one tick
// predicate. A world with panned Entry-family sections but no light waves
// must keep ticking even when the light-map path disarms.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTRuntimeBSPTickPredicateSpec,
	"WoT.LightmapAnimation.TickPredicateCoversSceneTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTRuntimeBSPTickPredicateSpec::RunTest(const FString&)
{
	const TArray<FQuad> Surfaces = {MakeQuad(1, 1, 2, 2)};
	const TArray<FQuad> Groups = {MakeQuad(2, 64, 0, 1)};
	TArray<FTermSpec> Specs;
	Specs.Add(MakeTerm(0, 0, 0, 1, {MakeRect(0, TArray<uint8>({
		10, 20, 30, 10, 20, 30, 10, 20, 30, 10, 20, 30}))}));
	FWoTBSPPayloadData Animated;
	FString Error;
	if (!TestTrue(TEXT("animated payload decodes"),
		Decodes(MakeAnimatedPayload(MakeTrailer(Surfaces, Groups, Specs), true),
			Animated, Error)))
	{
		AddError(Error);
		return false;
	}
	UMaterialInterface* EntryMaster = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/WoT/Materials/M_WoT_EntryTranslucent.M_WoT_EntryTranslucent"));
	if (!TestNotNull(TEXT("Entry-family master is available"), EntryMaster)) return false;
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTRuntimeBSPWorld* Owner = World ? World->SpawnActor<AWoTRuntimeBSPWorld>() : nullptr;
	UTexture2D* Lightmap = WoTRuntimeBSPMaterializeLightmap(Animated);
	UMaterialInstanceDynamic* Panned = Owner != nullptr
		? UMaterialInstanceDynamic::Create(EntryMaster, Owner) : nullptr;
	if (TestNotNull(TEXT("owner spawned"), Owner) && TestNotNull(TEXT("lightmap"), Lightmap)
		&& TestNotNull(TEXT("panned instance"), Panned))
	{
		const FWoTBSPPayloadData Unanimated;
		TestFalse(TEXT("neither duty: no tick"), Owner->ShouldTick());
		Owner->RefreshTickEnabled();
		TestFalse(TEXT("neither duty: tick stays off"), Owner->IsActorTickEnabled());

		// Panned, no light waves (the materializer registers pans, then any
		// light-map adoption on an unanimated atlas disarms only its own duty).
		Owner->SceneTimeMaterials.Add(Panned);
		Owner->RefreshTickEnabled();
		TestTrue(TEXT("pans only: predicate holds"), Owner->ShouldTick());
		TestTrue(TEXT("pans only: ticks"), Owner->IsActorTickEnabled());
		TestTrue(TEXT("pans only: starts ticking at BeginPlay"),
			Owner->PrimaryActorTick.bStartWithTickEnabled);
		Owner->SetLightmapAnimation(Unanimated, Lightmap);
		TestFalse(TEXT("pans only: no light-map animation"), Owner->HasLightmapAnimation());
		TestTrue(TEXT("pans only: the light-map disarm keeps the pan tick"),
			Owner->IsActorTickEnabled());
		Panned->SetScalarParameterValue(TEXT("WoTSceneTime"), -1.0f);
		Owner->Tick(0.016f);
		float SceneTime = -2.0f;
		Panned->GetScalarParameterValue(
			FHashedMaterialParameterInfo(TEXT("WoTSceneTime")), SceneTime);
		TestEqual(TEXT("pans only: Tick drives WoTSceneTime from the world clock"),
			SceneTime, static_cast<float>(World->GetTimeSeconds()));

		// Both duties, then each one removed in turn.
		Owner->SetLightmapAnimation(Animated, Lightmap);
		TestTrue(TEXT("both: animation adopted"), Owner->HasLightmapAnimation());
		TestTrue(TEXT("both: ticks"), Owner->IsActorTickEnabled());
		Owner->SceneTimeMaterials.Reset();
		Owner->RefreshTickEnabled();
		TestTrue(TEXT("light waves only: still ticks"), Owner->IsActorTickEnabled());
		Owner->SetLightmapAnimation(Unanimated, Lightmap);
		TestFalse(TEXT("neither again: tick off"), Owner->IsActorTickEnabled());
		TestFalse(TEXT("neither again: BeginPlay would not tick"),
			Owner->PrimaryActorTick.bStartWithTickEnabled);
	}
	if (World) WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightmapShimmerRandomsSpec,
	"WoT.LightmapAnimation.NativeShimmerRandoms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightmapShimmerRandomsSpec::RunTest(const FString&)
{
	// Render/Src/UnRandom.cpp FGlobalRandoms::Tick correlation and cadence
	// vectors: seeded replay, per-frame RandomBases regeneration, Randoms
	// continuity within a 35 Hz server tick, the 16-entry re-target window,
	// the bounded drift step, the >= 16-tick wraparound, the 256 key mask,
	// and (through the composer) Torch/Fire sharing one table on the
	// surface-local key regardless of atlas placement.
	FWoTLightmapShimmerRandoms A;
	FWoTLightmapShimmerRandoms B;
	A.Reset(WoTLightmapShimmerSeed);
	B.Reset(WoTLightmapShimmerSeed);
	A.Tick(1.0f);
	B.Tick(1.0f);
	bool bReplays = true;
	bool bInRange = true;
	for (int32 Index = 0; Index < FWoTLightmapShimmerRandoms::N_RANDS; ++Index)
	{
		bReplays = bReplays && A.RandomBases[Index] == B.RandomBases[Index]
			&& A.Randoms[Index] == B.Randoms[Index];
		bInRange = bInRange && A.RandomBases[Index] >= 0.0f && A.RandomBases[Index] <= 1.0f
			&& A.Randoms[Index] >= 0.0f && A.Randoms[Index] <= 1.0f;
	}
	TestTrue(TEXT("the documented seed replays both tables"), bReplays);
	TestTrue(TEXT("both tables stay in [0, 1]"), bInRange);
	TestEqual(TEXT("t=1.0 is server tick 35"), A.GetLastTicks(), 35u);
	TestEqual(TEXT("RandomBase key wraps at 256"), A.RandomBase(300), A.RandomBase(44));
	TestEqual(TEXT("Random key wraps at 256"), A.Random(511), A.Random(255));

	// Same server tick: the bases regenerate, the continuous table holds.
	float Bases0[FWoTLightmapShimmerRandoms::N_RANDS];
	float Randoms0[FWoTLightmapShimmerRandoms::N_RANDS];
	FMemory::Memcpy(Bases0, A.RandomBases, sizeof(Bases0));
	FMemory::Memcpy(Randoms0, A.Randoms, sizeof(Randoms0));
	A.Tick(1.01f);
	int32 BaseChanges = 0;
	int32 RandomChanges = 0;
	for (int32 Index = 0; Index < FWoTLightmapShimmerRandoms::N_RANDS; ++Index)
	{
		BaseChanges += A.RandomBases[Index] != Bases0[Index] ? 1 : 0;
		RandomChanges += A.Randoms[Index] != Randoms0[Index] ? 1 : 0;
	}
	TestEqual(TEXT("still server tick 35"), A.GetLastTicks(), 35u);
	TestTrue(TEXT("RandomBases regenerate every rendered frame"), BaseChanges > 200);
	TestEqual(TEXT("Randoms hold within one 35 Hz tick"), RandomChanges, 0);

	// One server tick later: entries [m, r) = [48, 64) re-target and hold
	// this tick; every other entry drifts by delta*1 with |delta| <= 1/16.
	float Randoms1[FWoTLightmapShimmerRandoms::N_RANDS];
	FMemory::Memcpy(Randoms1, A.Randoms, sizeof(Randoms1));
	A.Tick(1.03f);
	TestEqual(TEXT("t=1.03 is server tick 36"), A.GetLastTicks(), 36u);
	int32 Moved = 0;
	float MaxStep = 0.0f;
	bool bWindowHeld = true;
	for (int32 Index = 0; Index < FWoTLightmapShimmerRandoms::N_RANDS; ++Index)
	{
		const float Step = FMath::Abs(A.Randoms[Index] - Randoms1[Index]);
		if (Index >= 48 && Index < 64)
		{
			bWindowHeld = bWindowHeld && Step == 0.0f;
		}
		else
		{
			Moved += Step > 0.0f ? 1 : 0;
			MaxStep = FMath::Max(MaxStep, Step);
		}
	}
	TestTrue(TEXT("the re-targeted window holds for its own tick"), bWindowHeld);
	TestTrue(TEXT("the other entries drift"), Moved > 200);
	TestTrue(TEXT("a drift step is bounded by (base - random) / 16"),
		MaxStep <= 1.0f / 16.0f + 1e-5f);

	// A gap of >= 16 server ticks (RAND_CYCLE) takes the wraparound branch:
	// the continuous table is regenerated (steps beyond one drift).
	float Randoms2[FWoTLightmapShimmerRandoms::N_RANDS];
	FMemory::Memcpy(Randoms2, A.Randoms, sizeof(Randoms2));
	A.Tick(1.03f + 17.0f / 35.0f);
	int32 Regenerated = 0;
	int32 LargeSteps = 0;
	for (int32 Index = 0; Index < FWoTLightmapShimmerRandoms::N_RANDS; ++Index)
	{
		const float Step = FMath::Abs(A.Randoms[Index] - Randoms2[Index]);
		Regenerated += Step > 0.0f ? 1 : 0;
		LargeSteps += Step > 1.0f / 16.0f + 1e-5f ? 1 : 0;
	}
	TestTrue(TEXT("wraparound regenerates the continuous table"), Regenerated > 200);
	TestTrue(TEXT("wraparound steps exceed one drift"), LargeSteps > 50);

	// Composer: Torch (effect 1) and Fire (effect 2) share RandomBase on the
	// same key, so their deltas scale with their responses (20 vs 100).
	TArray<uint8> Base;
	for (int32 Pixel = 0; Pixel < 64; ++Pixel)
	{
		Base.Add(100); Base.Add(120); Base.Add(140); Base.Add(255);
	}
	FWoTBSPAnimationSurface Surface;
	Surface.X = 1; Surface.Y = 1; Surface.W = 2; Surface.H = 2;
	FWoTBSPAnimationTerm Torch;
	Torch.Kind = 1; Torch.Group = 255; Torch.Effect = 1; Torch.LightCount = 1;
	FWoTBSPAnimationTerm Fire;
	Fire.Kind = 1; Fire.Group = 255; Fire.Effect = 2; Fire.LightCount = 1;
	FWoTBSPAnimationTerm Water;
	Water.Kind = 1; Water.Group = 255; Water.Effect = 3; Water.LightCount = 1;
	FWoTBSPAnimationRect TorchRect;
	TorchRect.SurfaceIndex = 0;
	TorchRect.RGB.Init(20, 12);
	FWoTBSPAnimationRect FireRect;
	FireRect.SurfaceIndex = 0;
	FireRect.RGB.Init(100, 12);
	FWoTLightmapAnimationFrame Frame;
	Frame.bFrozen = false;
	Frame.Randoms = &A;
	TArray<uint8> TorchOut;
	TArray<uint8> FireOut;
	TArray<uint8> WaterOut;
	TArray<const FWoTBSPAnimationTerm*> TorchTerms;
	TorchTerms.Add(&Torch);
	TArray<const FWoTBSPAnimationTerm*> FireTerms;
	FireTerms.Add(&Fire);
	TArray<const FWoTBSPAnimationTerm*> WaterTerms;
	WaterTerms.Add(&Water);
	TArray<const FWoTBSPAnimationRect*> TorchRects;
	TorchRects.Add(&TorchRect);
	TArray<const FWoTBSPAnimationRect*> FireRects;
	FireRects.Add(&FireRect);
	WoTLightmapComposeSurface(Base, 8, Surface, TorchTerms, TorchRects, Frame, TorchOut);
	WoTLightmapComposeSurface(Base, 8, Surface, FireTerms, FireRects, Frame, FireOut);
	WoTLightmapComposeSurface(Base, 8, Surface, WaterTerms, FireRects, Frame, WaterOut);
	bool bShared = true;
	bool bWaterDiffers = false;
	for (int32 Texel = 0; Texel < 4; ++Texel)
	{
		const int32 TorchDelta = static_cast<int32>(TorchOut[Texel * 4 + 2]) - 140;
		const int32 FireDelta = static_cast<int32>(FireOut[Texel * 4 + 2]) - 140;
		const int32 Expected = FMath::RoundToInt(
			(2.0f * A.RandomBase(Texel) - 1.0f) * 100.0f);
		bShared = bShared && FMath::Abs(TorchDelta * 5 - FireDelta) <= 5
			&& FireDelta == Expected;
		bWaterDiffers = bWaterDiffers
			|| static_cast<int32>(WaterOut[Texel * 4 + 2]) - 140 != FireDelta;
	}
	TestTrue(TEXT("Torch and Fire read the same RandomBase per key"), bShared);
	TestTrue(TEXT("WateryShimmer reads the continuous table instead"), bWaterDiffers);

	// Placement independence: the same surface data at another atlas
	// position composes identically (the key is surface-local).
	FWoTBSPAnimationSurface MovedSurface = Surface;
	MovedSurface.X = 5;
	MovedSurface.Y = 5;
	TArray<uint8> MovedOut;
	WoTLightmapComposeSurface(Base, 8, MovedSurface, FireTerms, FireRects, Frame, MovedOut);
	TestTrue(TEXT("shimmer keys are surface-local, not atlas placement"), MovedOut == FireOut);
	return true;
}

namespace
{
	/** Keeps only the requested map's BSP objects from a world-set import. */
	class FWoTArena12ObjectWriter final : public IWoTObjectWriter
	{
	public:
		FWoTBootstrapStatus WriteObject(FStringView LogicalId, FStringView Schema,
			TConstArrayView<uint8> Bytes, const FWoTSha256&, uint64 MaximumBytes) override
		{
			if (static_cast<uint64>(Bytes.Num()) > MaximumBytes)
			{
				return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
					EWoTBootstrapError::SizeLimit, TEXT("upload batch writer overflow")));
			}
			if (Schema == TEXT("wot-bsp-v3")
				&& LogicalId.StartsWith(TEXT("cache/bsp/Arena_12/"), ESearchCase::CaseSensitive))
			{
				TArray<uint8>& Copy = Objects.Add(FString(LogicalId));
				Copy.Append(Bytes.GetData(), Bytes.Num());
			}
			return FWoTBootstrapStatus::Success();
		}
		TMap<FString, TArray<uint8>> Objects;
	};

	class FWoTArena12Progress final : public IWoTProgressSink
	{
	public:
		FWoTBootstrapStatus ReportProgress(FStringView, uint64, uint64) override
		{
			return FWoTBootstrapStatus::Success();
		}
		bool IsCancellationRequested() const override { return false; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightmapArena12UploadBatchSpec,
	"WoT.LightmapAnimation.RealRetailArena12UploadBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightmapArena12UploadBatchSpec::RunTest(const FString&)
{
	if (!WoTReferenceTreeExists())
	{
		AddWarning(WoTReferenceMissingWarning(TEXT("original")));
		return true;
	}
	// The production-sized trailer: Arena_12 (four waveform groups, a watery
	// shimmer effect and its intersection, 191 affected surfaces) imported by
	// the native reader from the retail package. One live frame must be ONE
	// UpdateTextureRegions call with one region per affected surface, from
	// persistent staging bounded by three atlas pages.
	const FString RetailRoot = WoTReferencePath(TEXT("original"));
	if (!IFileManager::Get().FileExists(*(RetailRoot / TEXT("Maps/Arena_12.wot"))))
	{
		AddError(TEXT("required original/Maps/Arena_12.wot is missing"));
		return false;
	}
	TWoTBootstrapResult<FWoTRetailContract> LoadedContract = WoTLoadProductionRetailContract();
	if (!TestFalse(TEXT("production retail contract loads"), LoadedContract.IsError()))
	{
		AddError(LoadedContract.GetError().ToString());
		return false;
	}
	// The importer only accepts the complete nineteen-map contract; the
	// writer keeps just the Arena_12 BSP objects.
	FWoTRetailContract WorldContract;
	TArray<FWoTWorldPackageBytesForTesting> WorldSources;
	for (const FWoTRequiredFact& Fact : LoadedContract.GetValue().Facts)
	{
		const bool bMultiplayerWorldFact = (Fact.Domain == EWoTFactDomain::BSP
			|| Fact.Domain == EWoTFactDomain::World)
			&& !Fact.SourcePath.Equals(TEXT("Maps/WheelOfTime.wot"), ESearchCase::IgnoreCase);
		if (!bMultiplayerWorldFact) continue;
		WorldContract.Facts.Add(Fact);
		if (Fact.Domain != EWoTFactDomain::World) continue;
		FWoTWorldPackageBytesForTesting& Source = WorldSources.AddDefaulted_GetRef();
		Source.SourcePath = Fact.SourcePath;
		if (!TestTrue(Fact.SourcePath + TEXT(" loads"),
			FFileHelper::LoadFileToArray(Source.Bytes, *(RetailRoot / Fact.SourcePath))))
			return false;
	}
	if (!TestEqual(TEXT("nineteen map packages"), WorldSources.Num(), 19)) return false;
	FWoTArena12ObjectWriter Writer;
	FWoTArena12Progress Progress;
	TWoTBootstrapResult<FWoTWorldSetResult> Imported =
		ImportRequiredWorldSetPackageBytesForTesting(
			WorldSources, WorldContract, Writer, Progress);
	if (!TestFalse(TEXT("native importer builds the Arena_12 BSP objects"), Imported.IsError()))
	{
		AddError(Imported.GetError().ToString());
		return false;
	}
	FWoTBSPPayloadData Animated;
	bool bFound = false;
	for (const FWoTWorldDescriptor& Descriptor : Imported.GetValue().Worlds)
	{
		if (Descriptor.MapIdentity != TEXT("Arena_12")) continue;
		for (const FWoTBSPPartDescriptor& Part : Descriptor.BSPParts)
		{
			TArray<uint8>* Bytes = Writer.Objects.Find(Part.LogicalId);
			if (!TestNotNull(*(Part.LogicalId + TEXT(" object")), Bytes)) continue;
			TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Decoded =
				FWoTRuntimeRegistry::DecodeObjectBytesForTesting(MoveTemp(*Bytes),
					TEXT("wot-bsp-static-mesh-v1"), TEXT("bsp"));
			if (!TestFalse(Part.LogicalId + TEXT(" frame decodes"), Decoded.IsError())) continue;
			FWoTBSPPayloadData Payload;
			FString Error;
			if (!TestTrue(Part.LogicalId + TEXT(" decodes"),
				UWoTBSPPayloadLibrary::DecodePayload(Decoded.GetValue().Payload(), Payload, Error)))
			{
				AddError(Error);
				continue;
			}
			if (Payload.HasLightmapAnimation())
			{
				TestFalse(TEXT("exactly one Arena_12 part carries the trailer"), bFound);
				Animated = MoveTemp(Payload);
				bFound = true;
			}
		}
	}
	if (!TestTrue(TEXT("the Arena_12 atlas carrier has the animation trailer"), bFound))
		return false;
	TestEqual(TEXT("Arena_12 atlas width"), static_cast<int32>(Animated.LightmapWidth), 1024);
	TestEqual(TEXT("Arena_12 atlas height"), static_cast<int32>(Animated.LightmapHeight), 1024);
	TestEqual(TEXT("Arena_12 waveform groups"), Animated.AnimationGroups.Num(), 4);
	TestEqual(TEXT("Arena_12 response terms"), Animated.AnimationTerms.Num(), 6);

	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTRuntimeBSPWorld* Owner = World ? World->SpawnActor<AWoTRuntimeBSPWorld>() : nullptr;
	UTexture2D* Lightmap = WoTRuntimeBSPMaterializeLightmap(Animated);
	if (TestNotNull(TEXT("owner spawned"), Owner) && TestNotNull(TEXT("lightmap"), Lightmap))
	{
		Owner->SetLightmapAnimation(Animated, Lightmap);
		TestTrue(TEXT("Arena_12 trailer adopted"), Owner->HasLightmapAnimation());
		TestEqual(TEXT("Arena_12 affected surfaces"), Owner->GetAnimatedSurfaceCount(), 191);
		TestEqual(TEXT("no upload before the first frame"),
			Owner->GetTotalUploadCallCount(), static_cast<uint64>(0));
		Owner->RecomposeLightmap(1.0f, false);
		TestEqual(TEXT("one live frame is one UpdateTextureRegions batch"),
			Owner->GetLastUploadBatchCount(), 1);
		TestEqual(TEXT("one region per affected surface"),
			Owner->GetLastUploadRegionCount(), Owner->GetAnimatedSurfaceCount());
		Owner->RecomposeLightmap(1.02f, false);
		Owner->RecomposeLightmap(1.04f, false);
		// -nullrhi creates no texture resource, so the batches stage without
		// an enqueue; with a resource every live frame is exactly one call.
		TestEqual(TEXT("three live frames, one call each when a resource exists"),
			Owner->GetTotalUploadCallCount(),
			static_cast<uint64>(Lightmap->GetResource() != nullptr ? 3 : 0));
		TestEqual(TEXT("no frame dropped with three staging slots"),
			Owner->GetSkippedUploadFrameCount(), 0u);
		const uint64 AtlasBytes = 1024ull * 1024ull * 4ull;
		const uint64 Staged = Owner->GetUploadStagingBytes();
		TestTrue(TEXT("staging is persistent and bounded by three atlas pages"),
			Staged >= AtlasBytes
			&& Staged <= 3ull * (AtlasBytes + 191ull * sizeof(FUpdateTextureRegion2D)));
		Owner->RecomposeLightmap(1.06f, false);
		TestEqual(TEXT("a further frame allocates no new staging"),
			Owner->GetUploadStagingBytes(), Staged);
		Owner->RecomposeLightmap(1.08f, true);
		TestEqual(TEXT("the first frozen frame stages the base once"),
			Owner->GetLastUploadBatchCount(), 1);
		Owner->RecomposeLightmap(1.10f, true);
		TestEqual(TEXT("a held frozen frame stages nothing"),
			Owner->GetLastUploadBatchCount(), 0);
		TestEqual(TEXT("the shimmer tables advanced with the world clock"),
			Owner->GetShimmerRandoms().GetLastTicks(), 37u);
	}
	if (World) WoTTest::DestroySpecWorld(World);
	return !HasAnyErrors();
}

#endif
