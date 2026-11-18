// Focused specs for the map-level centered light-response driver.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTLightWave.h"

#include "HAL/IConsoleManager.h"

namespace
{
constexpr int32 RetailReferenceTableSize = 16384;
constexpr double RetailReferencePi = 3.1415926535897932;

struct FRetailReferenceSinTable
{
	float Values[RetailReferenceTableSize];

	FRetailReferenceSinTable()
	{
		// Independent transcription of FGlobalMath's retail TrigFLOAT build.
		// This table deliberately does not call AWoTLightWave or share its data.
		for (int32 Index = 0; Index < RetailReferenceTableSize; ++Index)
		{
			const double Angle = static_cast<double>(static_cast<float>(Index))
				* 2.0 * RetailReferencePi
				/ static_cast<double>(
					static_cast<float>(RetailReferenceTableSize));
			Values[Index] = static_cast<float>(FPlatformMath::Sin(Angle));
		}
	}
};

const FRetailReferenceSinTable& RetailReferenceSinTable()
{
	static const FRetailReferenceSinTable Table;
	return Table;
}

FWoTLightWaveGroup MakeGroup(int32 Period, int32 Phase)
{
	FWoTLightWaveGroup Group;
	Group.Period = Period;
	Group.Phase = Phase;
	return Group;
}

float ExpectedWave(float TimeSeconds, int32 Period, int32 Phase)
{
	const double Theta = static_cast<double>(TimeSeconds) * 35.0 * 65536.0
		/ static_cast<double>(FMath::Max(Period, 1))
		+ static_cast<double>(Phase) * 256.0;
	const int32 RotatorUnits = static_cast<int32>(Theta);
	const int32 TableIndex = (RotatorUnits >> 2) & 16383;
	return 0.5f + 0.5f * RetailReferenceSinTable().Values[TableIndex];
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightWaveScalarsSpec,
	"WoT.LightWave.WaveformScalars",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightWaveScalarsSpec::RunTest(const FString&)
{
	const TArray<FWoTLightWaveGroup> Groups = {
		MakeGroup(99, 102), MakeGroup(134, 0),
		MakeGroup(83, 0), MakeGroup(134, 0)};
	constexpr float TimeSeconds = 1.375f;
	uint32 NoiseFrame = 40;
	const FWoTLightWaveParameterValues Values =
		AWoTLightWave::EvaluateParameters(
			TimeSeconds, false, Groups, NoiseFrame);

	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("Wave%d matches retail theta"), Index),
			FMath::IsNearlyEqual(
				Values.Waves[Index],
				ExpectedWave(
					TimeSeconds, Groups[Index].Period, Groups[Index].Phase),
				1.e-6f));
	}
	TestEqual(TEXT("one live tick advances one integer noise frame"),
		NoiseFrame, 41u);
	TestEqual(TEXT("transported noise frame is integer-valued"),
		Values.NoiseFrame, 41.0f);
	TestEqual(TEXT("live frame clears the frozen-default switch"),
		Values.Freeze, 0.0f);

	uint32 UnusedFrame = 0;
	const FWoTLightWaveParameterValues OneGroup =
		AWoTLightWave::EvaluateParameters(
			0.0f, false, {MakeGroup(100, 0)}, UnusedFrame);
	TestEqual(TEXT("phase-zero sine mean is s=0.5"), OneGroup.Waves[0], 0.5f);
	for (int32 Index = 1; Index < AWoTLightWave::MaxWaveformGroups; ++Index)
	{
		TestEqual(FString::Printf(TEXT("unused Wave%d stays at mean"), Index),
			OneGroup.Waves[Index], 0.5f);
	}
	const float CenteredCoefficient = 2.0f * OneGroup.Waves[0] - 1.0f;
	TestEqual(TEXT("s at mean produces exactly zero centered coefficient"),
		CenteredCoefficient, 0.0f);
	TestEqual(TEXT("Pulse centered algebra preserves literal mean"),
		0.60f + 0.39f * CenteredCoefficient, 0.60f);
	TestEqual(TEXT("SubtlePulse centered algebra preserves literal mean"),
		0.90f + 0.09f * CenteredCoefficient, 0.90f);

	TestTrue(TEXT("period zero uses retail divisor one"),
		FMath::IsNearlyEqual(
			AWoTLightWave::EvaluateWaveScalar(1.0f / 140.0f, 0, 0),
			1.0f, 1.e-6f));
	TestTrue(TEXT("phase is multiplied by 256 rotator units"),
		FMath::IsNearlyEqual(
			AWoTLightWave::EvaluateWaveScalar(0.0f, 100, 64),
			1.0f, 1.e-6f));
	TestEqual(TEXT("fractional rotator units truncate before SinTab lookup"),
		AWoTLightWave::EvaluateWaveScalar(
			100.0f / (35.0f * 65536.0f), 100, 0),
		0.5f);
	const AWoTLightWave* LegacyDefaults = GetDefault<AWoTLightWave>();
	TestNotNull(TEXT("legacy driver class default exists"), LegacyDefaults);
	if (LegacyDefaults)
	{
		TestEqual(TEXT("legacy serialized period default is retained"),
			LegacyDefaults->Period, 64.0f);
		TestEqual(TEXT("legacy serialized phase default is retained"),
			LegacyDefaults->Phase, 0.0f);
		TestTrue(TEXT("legacy actors select the compatibility path"),
			LegacyDefaults->Groups.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLightWaveFreezeNoiseSpec,
	"WoT.LightWave.FreezeAndNoise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLightWaveFreezeNoiseSpec::RunTest(const FString&)
{
	const TArray<FWoTLightWaveGroup> Groups = {
		MakeGroup(99, 102), MakeGroup(134, 0),
		MakeGroup(83, 0), MakeGroup(134, 0)};
	uint32 NoiseFrame = 12345;
	const FWoTLightWaveParameterValues Frozen =
		AWoTLightWave::EvaluateParameters(
			271.0f, true, Groups, NoiseFrame);
	for (int32 Index = 0; Index < AWoTLightWave::MaxWaveformGroups; ++Index)
	{
		TestEqual(FString::Printf(TEXT("frozen Wave%d uses MPC default"), Index),
			Frozen.Waves[Index], 0.5f);
	}
	TestEqual(TEXT("freeze holds internal noise frame"), NoiseFrame, 12345u);
	TestEqual(TEXT("freeze transports default noise frame"),
		Frozen.NoiseFrame, 0.0f);
	TestEqual(TEXT("freeze transports enabled default"), Frozen.Freeze, 1.0f);

	NoiseFrame = AWoTLightWave::NoiseFrameMask - 1u;
	const FWoTLightWaveParameterValues Last =
		AWoTLightWave::EvaluateParameters(0.0f, false, {}, NoiseFrame);
	TestEqual(TEXT("24-bit noise reaches its last frame"),
		NoiseFrame, AWoTLightWave::NoiseFrameMask);
	TestEqual(TEXT("last 24-bit integer is exactly representable"),
		Last.NoiseFrame,
		static_cast<float>(AWoTLightWave::NoiseFrameMask));
	const FWoTLightWaveParameterValues Wrapped =
		AWoTLightWave::EvaluateParameters(0.0f, false, {}, NoiseFrame);
	TestEqual(TEXT("noise frame wraps after 24 bits"), NoiseFrame, 0u);
	TestEqual(TEXT("wrapped noise scalar is zero"), Wrapped.NoiseFrame, 0.0f);

	IConsoleVariable* Freeze = IConsoleManager::Get().FindConsoleVariable(
		TEXT("wot.LightWaveFreeze"));
	TestNotNull(TEXT("golden-pose freeze cvar remains registered"), Freeze);
	if (Freeze)
	{
		const int32 Previous = Freeze->GetInt();
		Freeze->Set(1, ECVF_SetByCode);
		TestTrue(TEXT("actor observes the existing freeze cvar"),
			AWoTLightWave::IsFreezeEnabled());
		Freeze->Set(Previous, ECVF_SetByCode);
	}
	return true;
}

#endif // WITH_AUTOMATION_TESTS
