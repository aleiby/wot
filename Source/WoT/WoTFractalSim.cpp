#include "WoTFractalSim.h"

namespace
{
enum EWoTDrop : uint8
{
	DropFixedDepth = 0,
	DropPhaseSpot = 1,
	DropShallowSpot = 2,
	DropHalfAmpl = 3,
	DropRandomMover = 4,
	DropFixedRandomSpot = 5,
	DropWhirlyThing = 6,
	DropBigWhirly = 7,
	DropHorizontalLine = 8,
	DropVerticalLine = 9,
	DropDiagonalLine1 = 10,
	DropDiagonalLine2 = 11,
	DropHorizontalOsc = 12,
	DropVerticalOsc = 13,
	DropDiagonalOsc1 = 14,
	DropDiagonalOsc2 = 15,
	DropRainDrops = 16,
	DropAreaClamp = 17,
	DropLeakyTap = 18,
	DropDrippyTap = 19,
	DropWhirlyBack = 64,
	DropBigWhirlyBack = 65,
};

constexpr double RetailPi = 3.1415926535897932;

bool IsPowerOfTwoAtLeast8(const int32 Value)
{
	return Value >= 8 && FMath::IsPowerOfTwo(Value);
}

// Rate limiter shared by the three texture sims: a step is owed once the banked
// time reaches the shortest frame interval; banked time beyond the longest
// interval carries over, capped at one long interval. Expression order is kept
// so the float results match the sims' earlier inline versions exactly.
}

bool WoTAdvanceRateLimitedTexture(float& BankedSeconds,
	const float SlowestFramesPerSecond, const float FastestFramesPerSecond,
	const float DeltaSeconds, TFunctionRef<void()> Step)
{
	const float ShortestInterval = 1.0f / FMath::Clamp(FastestFramesPerSecond, 0.01f, 100.0f);
	const float LongestInterval = 1.0f / FMath::Clamp(SlowestFramesPerSecond, 0.01f, 100.0f);
	BankedSeconds += DeltaSeconds;
	if (BankedSeconds < ShortestInterval)
	{
		return false;
	}

	const bool bSpendAll = BankedSeconds < LongestInterval;
	Step();
	const float CarryOver = bSpendAll
		? 0.0f
		: BankedSeconds - LongestInterval;
	BankedSeconds = CarryOver > LongestInterval ? LongestInterval : CarryOver;
	return true;
}

void FWoTFractalRandomStream::Initialize(const uint32 Seed)
{
	Table.SetNumUninitialized(512);
	uint32 State = Seed;
	for (uint8& Byte : Table)
	{
		// The retail Win32 appRand implementation used the MSVC recurrence.
		State = 214013u * State + 2531011u;
		Byte = static_cast<uint8>(State >> 16);
	}
	Index = 0;
}

uint8 FWoTFractalRandomStream::Next()
{
	check(Table.Num() == 512);
	const uint32 Source = (Index + 4 * (1 + 31)) & 0xfcu;
	const uint32 Dest = (Index + 4) & 0xfcu;
	uint32 SourceWord = 0;
	uint32 DestWord = 0;
	for (uint32 Byte = 0; Byte < 4; ++Byte)
	{
		SourceWord |= static_cast<uint32>(Table[Source + Byte]) << (Byte * 8);
		DestWord |= static_cast<uint32>(Table[Dest + Byte]) << (Byte * 8);
	}
	DestWord ^= SourceWord;
	for (uint32 Byte = 0; Byte < 4; ++Byte)
	{
		Table[Dest + Byte] = static_cast<uint8>(DestWord >> (Byte * 8));
	}
	Index = Dest;
	return static_cast<uint8>(SourceWord);
}

bool FWoTWaterFractalSim::Initialize(const FWoTFractalTextureRecord& InDefinition)
{
	bValid = false;
	if (InDefinition.FractalClass != EWoTFractalClass::Wet
		&& InDefinition.FractalClass != EWoTFractalClass::Wave)
	{
		return false;
	}
	if (!IsPowerOfTwoAtLeast8(InDefinition.Width)
		|| !IsPowerOfTwoAtLeast8(InDefinition.Height)
		|| InDefinition.InitialBitmap.Num() != InDefinition.Width * InDefinition.Height
		|| InDefinition.Drops.Num() > 256)
	{
		return false;
	}
	if (InDefinition.FractalClass == EWoTFractalClass::Wet
		&& (!IsPowerOfTwoAtLeast8(InDefinition.SourceWidth)
			|| !IsPowerOfTwoAtLeast8(InDefinition.SourceHeight)
			|| InDefinition.SourceWidth > InDefinition.Width
			|| InDefinition.SourceHeight > InDefinition.Height
			|| InDefinition.SourceIndices.Num()
				!= InDefinition.SourceWidth * InDefinition.SourceHeight))
	{
		return false;
	}

	Definition = InDefinition;
	if (!RandomStream)
	{
		RandomStream = &LocalRandom;
	}
	XDimension = Definition.Width / 2;
	YDimension = Definition.Height / 2;
	UBits = FMath::FloorLog2(Definition.Width);
	BuildTables();
	InitializeSourceBitmap();
	bValid = true;
	Reset();
	return true;
}

void FWoTWaterFractalSim::SetRandomStream(
	FWoTFractalRandomStream* InRandomStream)
{
	RandomStream = InRandomStream ? InRandomStream : &LocalRandom;
}

void FWoTWaterFractalSim::BuildTables()
{
	PhaseTable.SetNumUninitialized(256);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const float Angle = (static_cast<float>(Index) / 256.0f) * 6.2831853f;
		const float Value = static_cast<float>(127.45f
			+ 127.5f * FMath::Sin(static_cast<double>(Angle)));
		PhaseTable[Index] = static_cast<uint8>(FMath::RoundToInt(Value));
	}

	WaveTable.SetNumUninitialized(1536);
	for (int32 Index = 0; Index < WaveTable.Num(); ++Index)
	{
		const int32 Value = (Index / 2 - 256) + ((Index - 512) < 256 ? 1 : 0);
		WaveTable[Index] = static_cast<uint8>(FMath::Clamp(Value, 0, 255));
	}

	RenderTable.SetNumUninitialized(1024);
	if (Definition.FractalClass == EWoTFractalClass::Wet)
	{
		for (int32 Index = 0; Index < RenderTable.Num(); ++Index)
		{
			const int32 Value = static_cast<int32>((Index - 511)
				* (static_cast<float>(Definition.WaveAmp) / 512.0f));
			RenderTable[Index] = static_cast<uint8>(FMath::Clamp(Value, -128, 127) & 255);
		}
	}
	else
	{
		const float Lamp = static_cast<float>(RetailPi * Definition.BumpMapLight / 255.0);
		const float Viewer = static_cast<float>(RetailPi * Definition.BumpMapAngle / 255.0);
		const float PhongRadius = static_cast<float>(Definition.PhongSize) / 512.0f;
		for (int32 Index = 0; Index < RenderTable.Num(); ++Index)
		{
			const float X = (static_cast<float>(Definition.WaveAmp) / 255.0f)
				* (512.0f - static_cast<float>(Index)) / 196.0f;
			const double FakeAtan = static_cast<double>(3.1415f * 0.5f)
				* static_cast<double>(X) / (static_cast<double>(FMath::Abs(X)) + 1.0);
			const float Normal = static_cast<float>(FakeAtan + RetailPi * 0.5f);
			int32 TempLight = static_cast<int32>((256 - Definition.PhongRange / 2)
				* FMath::Cos(static_cast<double>(Normal - Lamp)));
			const float Reflected = Normal * 2.0f - Lamp;
			if (PhongRadius > 0.0f
				&& FMath::Square(Reflected - Viewer) < FMath::Square(PhongRadius))
			{
				TempLight += static_cast<int32>((Definition.PhongRange * 2)
					* (PhongRadius - FMath::Abs(Reflected - Viewer)) / PhongRadius);
			}
			RenderTable[Index] = static_cast<uint8>(FMath::Clamp(TempLight, 0, 255));
		}
	}
}

void FWoTWaterFractalSim::InitializeSourceBitmap()
{
	SourceBitmap.Reset();
	if (Definition.FractalClass != EWoTFractalClass::Wet)
	{
		return;
	}
	UpdateSourceIndices(Definition.SourceIndices);
}

bool FWoTWaterFractalSim::UpdateSourceIndices(
	const TConstArrayView<uint8> InSourceIndices, const int32 SourceSlot)
{
	if (Definition.FractalClass != EWoTFractalClass::Wet
		|| SourceSlot != 0
		|| InSourceIndices.Num() != Definition.SourceWidth * Definition.SourceHeight)
	{
		return false;
	}
	SourceBitmap.SetNumUninitialized(Definition.Width * Definition.Height);
	const int32 UScaler = UBits - FMath::FloorLog2(Definition.SourceWidth);
	const int32 VScaler = FMath::FloorLog2(Definition.Height)
		- FMath::FloorLog2(Definition.SourceHeight);
	for (int32 V = 0; V < Definition.Height; ++V)
	{
		for (int32 U = 0; U < Definition.Width; ++U)
		{
			SourceBitmap[V * Definition.Width + U] = InSourceIndices[
				(V >> VScaler) * Definition.SourceWidth + (U >> UScaler)];
		}
	}
	return true;
}

void FWoTWaterFractalSim::Reset()
{
	if (!bValid)
	{
		return;
	}
	Drops = Definition.Drops;
	SourceFields.Init(128, Definition.Width * Definition.Height / 2);
	OutputIndices = Definition.InitialBitmap;
	WaveParity = 0;
	GlobalPhase = 0;
	PrimeCurrent = 0;
	StepCount = 0;
	Accumulator = 0.0f;
	if (RandomStream == &LocalRandom)
	{
		LocalRandom.Initialize(1);
	}
}

void FWoTWaterFractalSim::Prime(const FBeforeStep& BeforeStep)
{
	while (PrimeCurrent < FMath::Max(Definition.PrimeCount, 0))
	{
		++PrimeCurrent;
		ConstantTimeTick(BeforeStep);
	}
}

int32 FWoTWaterFractalSim::TickTexture(
	const float DeltaSeconds, const FBeforeStep& BeforeStep)
{
	if (!bValid)
	{
		return 0;
	}
	const int32 Before = StepCount;
	Prime(BeforeStep);
	if (Definition.MaxFrameRate == 0.0f)
	{
		ConstantTimeTick(BeforeStep);
	}
	else
	{
		if (!WoTAdvanceRateLimitedTexture(Accumulator, Definition.MinFrameRate,
			Definition.MaxFrameRate, DeltaSeconds,
			[this, &BeforeStep]() { ConstantTimeTick(BeforeStep); }))
		{
			return StepCount - Before;
		}
	}
	return StepCount - Before;
}

void FWoTWaterFractalSim::ConstantTimeTick(const FBeforeStep& BeforeStep)
{
	if (!bValid)
	{
		return;
	}
	if (BeforeStep)
	{
		// UWetTexture locks its SourceTexture at this texture's old
		// LastUpdateTime before WaterRedrawDrops/CalculateWater.
		BeforeStep();
	}
	WaterRedrawDrops();
	CalculateWater();
	if (Definition.FractalClass == EWoTFractalClass::Wet)
	{
		ApplyWetTexture();
	}
	++StepCount;
}

int32 FWoTWaterFractalSim::FieldIndex(const bool bFieldB, const int32 X, const int32 Y) const
{
	return ((Y & (YDimension - 1)) << UBits)
		+ (bFieldB ? XDimension : 0) + (X & (XDimension - 1));
}

int32 FWoTWaterFractalSim::PixelIndex(const int32 X, const int32 Y) const
{
	return (Y & (Definition.Height - 1)) * Definition.Width
		+ (X & (Definition.Width - 1));
}

void FWoTWaterFractalSim::Output4Pix(const int32 DestIndex,
	const uint8 SourceA, const uint8 SourceC, const uint8 SourceE, const uint8 SourceG,
	const uint8 SourceB, const uint8 SourceD, const uint8 SourceF, const uint8 SourceH,
	const int32 Dest1, const int32 Dest2, const int32 Dest3, const int32 Dest4)
{
	const int32 WaveIndex = 512 + static_cast<int32>(SourceE) + SourceG + SourceF + SourceH
		- (static_cast<int32>(SourceFields[DestIndex]) << 1);
	SourceFields[DestIndex] = WaveTable[WaveIndex];
	const int32 EA = static_cast<int32>(SourceE) - SourceA;
	const int32 FB = static_cast<int32>(SourceF) - SourceB;
	const int32 GC = static_cast<int32>(SourceG) - SourceC;
	const int32 HD = static_cast<int32>(SourceH) - SourceD;
	OutputIndices[Dest2] = RenderTable[512 + GC + HD];
	OutputIndices[Dest3] = RenderTable[512 + FB + HD];
	OutputIndices[Dest1] = RenderTable[512 + (FB + HD + EA + GC) / 2];
	OutputIndices[Dest4] = RenderTable[512 + HD + HD];
}

void FWoTWaterFractalSim::CalculateWater()
{
	++WaveParity;
	if ((WaveParity & 1) == 0)
	{
		for (int32 Y = 0; Y < YDimension; ++Y)
		{
			for (int32 X = 0; X < XDimension; ++X)
			{
				Output4Pix(FieldIndex(false, X, Y),
					SourceFields[FieldIndex(true, X - 3, Y - 1)],
					SourceFields[FieldIndex(true, X - 2, Y - 1)],
					SourceFields[FieldIndex(true, X - 1, Y - 1)],
					SourceFields[FieldIndex(true, X, Y - 1)],
					SourceFields[FieldIndex(true, X - 3, Y)],
					SourceFields[FieldIndex(true, X - 2, Y)],
					SourceFields[FieldIndex(true, X - 1, Y)],
					SourceFields[FieldIndex(true, X, Y)],
					PixelIndex(2 * X - 1, 2 * Y - 1),
					PixelIndex(2 * X, 2 * Y - 1),
					PixelIndex(2 * X - 1, 2 * Y),
					PixelIndex(2 * X, 2 * Y));
			}
		}
	}
	else
	{
		for (int32 Y = 0; Y < YDimension; ++Y)
		{
			for (int32 X = 0; X < XDimension; ++X)
			{
				Output4Pix(FieldIndex(true, X, Y),
					SourceFields[FieldIndex(false, X - 2, Y)],
					SourceFields[FieldIndex(false, X - 1, Y)],
					SourceFields[FieldIndex(false, X, Y)],
					SourceFields[FieldIndex(false, X + 1, Y)],
					SourceFields[FieldIndex(false, X - 2, Y + 1)],
					SourceFields[FieldIndex(false, X - 1, Y + 1)],
					SourceFields[FieldIndex(false, X, Y + 1)],
					SourceFields[FieldIndex(false, X + 1, Y + 1)],
					PixelIndex(2 * X, 2 * Y),
					PixelIndex(2 * X + 1, 2 * Y),
					PixelIndex(2 * X, 2 * Y + 1),
					PixelIndex(2 * X + 1, 2 * Y + 1));
			}
		}
	}
}

uint8 FWoTWaterFractalSim::SpeedRand()
{
	check(RandomStream);
	return RandomStream->Next();
}

void FWoTWaterFractalSim::WaterRedrawDrops()
{
	const int32 U2Mask = XDimension - 1;
	const int32 V2Mask = YDimension - 1;
	++GlobalPhase;

	auto PutBoth = [this, U2Mask, V2Mask](const int32 X, const int32 Y,
		const uint8 ValueA, const uint8 ValueB)
	{
		SourceFields[FieldIndex(false, X & U2Mask, Y & V2Mask)] = ValueA;
		SourceFields[FieldIndex(true, X & U2Mask, Y & V2Mask)] = ValueB;
	};
	auto PutSame = [&PutBoth](const int32 X, const int32 Y, const uint8 Value)
	{
		PutBoth(X, Y, Value, Value);
	};

	for (FWoTFractalDrop& Drop : Drops)
	{
		const uint8 XNow = Drop.X;
		const uint8 YNow = Drop.Y;
		switch (Drop.Type)
		{
		case DropFixedDepth:
			PutSame(XNow, YNow, Drop.ByteD);
			break;
		case DropPhaseSpot:
			Drop.Depth = static_cast<uint8>(Drop.Depth + Drop.ByteD);
			PutSame(XNow, YNow, PhaseTable[Drop.Depth]);
			break;
		case DropHalfAmpl:
		{
			Drop.Depth = static_cast<uint8>(Drop.Depth + Drop.ByteD);
			const uint8 Depth = static_cast<uint8>(FMath::Max<int32>(
				128, PhaseTable[Drop.Depth]));
			PutSame(XNow, YNow, Depth);
			break;
		}
		case DropShallowSpot:
			Drop.Depth = static_cast<uint8>(Drop.Depth + Drop.ByteD);
			PutSame(XNow, YNow, static_cast<uint8>(64 + (PhaseTable[Drop.Depth] >> 1)));
			break;
		case DropRandomMover:
		{
			const uint8 XMinus = SpeedRand() & 3;
			const uint8 XPlus = SpeedRand() & 3;
			const uint8 YMinus = SpeedRand() & 3;
			const uint8 YPlus = SpeedRand() & 3;
			Drop.X = static_cast<uint8>(U2Mask & (XNow - XMinus + XPlus));
			Drop.Y = static_cast<uint8>(V2Mask & (YNow - YMinus + YPlus));
			PutBoth(XNow, YNow, 128 + 57, 128 - 57);
			break;
		}
		case DropFixedRandomSpot:
		{
			const uint8 ValueA = SpeedRand();
			const uint8 ValueB = SpeedRand();
			PutBoth(XNow, YNow, ValueA, ValueB);
			break;
		}
		case DropWhirlyThing:
		case DropWhirlyBack:
		case DropBigWhirly:
		case DropBigWhirlyBack:
		{
			uint32 Phase16 = static_cast<uint32>(Drop.ByteA) | (static_cast<uint32>(Drop.ByteB) << 8);
			const uint32 Delta16 = static_cast<uint32>(Drop.ByteC)
				| (static_cast<uint32>(Drop.ByteD) << 8);
			if (Drop.Type == DropWhirlyBack || Drop.Type == DropBigWhirlyBack)
			{
				Phase16 = (Phase16 - Delta16) & 0xffffu;
			}
			else
			{
				Phase16 = (Phase16 + Delta16) & 0xffffu;
			}
			Drop.ByteA = static_cast<uint8>(Phase16);
			Drop.ByteB = static_cast<uint8>(Phase16 >> 8);
			const int32 Shift = (Drop.Type == DropBigWhirly
				|| Drop.Type == DropBigWhirlyBack) ? 3 : 4;
			const int32 X = XNow + (PhaseTable[Drop.ByteB] >> Shift);
			const int32 Y = YNow + (PhaseTable[static_cast<uint8>(Drop.ByteB + 64)] >> Shift);
			PutSame(X, Y, Drop.Depth);
			break;
		}
		case DropHorizontalLine:
		case DropVerticalLine:
		case DropDiagonalLine1:
		case DropDiagonalLine2:
		case DropHorizontalOsc:
		case DropVerticalOsc:
		case DropDiagonalOsc1:
		case DropDiagonalOsc2:
		{
			uint8 Depth = Drop.Depth;
			if (Drop.Type >= DropHorizontalOsc)
			{
				Drop.Depth = static_cast<uint8>(Drop.Depth + Drop.ByteC);
				Depth = PhaseTable[Drop.Depth];
			}
			const int32 Segment = Drop.ByteD >> 1;
			for (int32 T = 0; T <= Segment; ++T)
			{
				int32 X = XNow;
				int32 Y = YNow;
				switch (Drop.Type)
				{
				case DropHorizontalLine:
				case DropHorizontalOsc: X += T; break;
				case DropVerticalLine:
				case DropVerticalOsc: Y += T; break;
				case DropDiagonalLine1:
				case DropDiagonalOsc1: X -= T; Y += T; break;
				default: X += T; Y += T; break;
				}
				PutSame(X, Y, Depth);
			}
			break;
		}
		case DropAreaClamp:
		{
			const int32 Segment = Drop.ByteD >> 1;
			for (int32 V = 0; V < Segment; ++V)
			{
				for (int32 U = 0; U < Segment; ++U)
				{
					PutSame(XNow + U, YNow + V, Drop.Depth);
				}
			}
			break;
		}
		case DropRainDrops:
			if ((SpeedRand() & 15) == 0)
			{
				const uint8 Spray = Drop.ByteD;
				const int32 Y = YNow + ((SpeedRand() * Spray) >> 8);
				const int32 X = XNow + ((SpeedRand() * Spray) >> 8);
				PutBoth(X, Y, Drop.Depth, 255 ^ Drop.Depth);
			}
			break;
		case DropLeakyTap:
			Drop.ByteA = static_cast<uint8>(Drop.ByteA + Drop.ByteD);
			if (Drop.ByteA <= Drop.ByteD)
			{
				PutBoth(XNow, YNow, Drop.Depth, 255 ^ Drop.Depth);
			}
			break;
		case DropDrippyTap:
			Drop.ByteA = static_cast<uint8>(Drop.ByteA + Drop.ByteD);
			if (Drop.ByteA <= Drop.ByteD)
			{
				Drop.ByteA = SpeedRand();
				PutBoth(XNow, YNow, Drop.Depth, 255 ^ Drop.Depth);
			}
			break;
		default:
			break;
		}
	}
}

void FWoTWaterFractalSim::ApplyWetTexture()
{
	for (int32 V = 0; V < Definition.Height; ++V)
	{
		const int32 Row = V * Definition.Width;
		for (int32 U = 0; U < Definition.Width; ++U)
		{
			const int32 Pixel = Row + U;
			OutputIndices[Pixel] = SourceBitmap[
				Row + ((U + OutputIndices[Pixel]) & (Definition.Width - 1))];
		}
	}
}

namespace
{
enum EWoTAuthoredSpark : uint8
{
	SparkBurn = 0,
	SparkPulse = 2,
	SparkSignal = 3,
	SparkBlaze = 4,
	SparkOzHasSpoken = 5,
	SparkCylinder3D = 10,
	SparkEels = 16,
	SparkStars = 22,
	SparkLineLightning = 23,
	SparkRampLightning = 24,
	SparkSphereLightning = 25,
};

enum EWoTInternalSpark : uint8
{
	SparkDrifter = 32,
	SparkDriftSlow = 33,
	SparkSpawnedEel = 38,
};

constexpr int32 StarCutoff = 38;

int32 SignedByte(const uint8 Value)
{
	return static_cast<int32>(static_cast<int8>(Value));
}
}

bool FWoTFireFractalSim::IsAuthoredSparkTypeSupported(const uint8 Type)
{
	switch (Type)
	{
	case SparkBurn:
	case SparkPulse:
	case SparkSignal:
	case SparkBlaze:
	case SparkOzHasSpoken:
	case SparkCylinder3D:
	case SparkEels:
	case SparkStars:
	case SparkLineLightning:
	case SparkRampLightning:
	case SparkSphereLightning:
		return true;
	default:
		return false;
	}
}

bool FWoTFireFractalSim::Initialize(
	const FWoTFractalTextureRecord& InDefinition)
{
	bValid = false;
	if (InDefinition.FractalClass != EWoTFractalClass::Fire
		|| !IsPowerOfTwoAtLeast8(InDefinition.Width)
		|| !IsPowerOfTwoAtLeast8(InDefinition.Height)
		|| InDefinition.InitialBitmap.Num()
			!= InDefinition.Width * InDefinition.Height
		|| InDefinition.SparksLimit < 0
		|| InDefinition.SparksLimit > 8192
		|| InDefinition.Sparks.Num() > InDefinition.SparksLimit)
	{
		return false;
	}
	for (const FWoTFractalSpark& Spark : InDefinition.Sparks)
	{
		if (!IsAuthoredSparkTypeSupported(Spark.Type))
		{
			return false;
		}
	}

	Definition = InDefinition;
	if (!RandomStream)
	{
		RandomStream = &LocalRandom;
	}
	UBits = FMath::FloorLog2(Definition.Width);
	BuildTables();
	bValid = true;
	Reset();
	return true;
}

void FWoTFireFractalSim::SetRandomStream(
	FWoTFractalRandomStream* InRandomStream)
{
	RandomStream = InRandomStream ? InRandomStream : &LocalRandom;
}

void FWoTFireFractalSim::BuildTables()
{
	PhaseTable.SetNumUninitialized(256);
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const float Angle = (static_cast<float>(Index) / 256.0f) * 6.2831853f;
		const float Value = static_cast<float>(127.45f
			+ 127.5f * FMath::Sin(static_cast<double>(Angle)));
		PhaseTable[Index] = static_cast<uint8>(FMath::RoundToInt(Value));
	}

	// UFireTexture::PostLoad. Assignment to BYTE
	// truncates the positive clamped result; there is no fitted heat curve.
	RenderTable.SetNumUninitialized(1024);
	for (int32 Index = 0; Index < RenderTable.Num(); ++Index)
	{
		const double Value = static_cast<double>(Index) / 4.0 + 1.0
			- static_cast<double>(255 - Definition.RenderHeat) / 16.0;
		RenderTable[Index] = static_cast<uint8>(FMath::Clamp(Value, 0.0, 255.0));
	}
}

void FWoTFireFractalSim::Reset()
{
	if (!bValid)
	{
		return;
	}
	Sparks = Definition.Sparks;
	Sparks.Reserve(Definition.SparksLimit);
	OutputIndices = Definition.InitialBitmap;
	GlobalPhase = 0;
	PrimeCurrent = 0;
	StepCount = 0;
	Accumulator = 0.0f;
	// UFireTexture's constructor starts optimistic and the first post pass
	// disables the scan if no SPARK_Stars record exists.
	bStarStatus = true;
	if (RandomStream == &LocalRandom)
	{
		LocalRandom.Initialize(1);
	}
}

void FWoTFireFractalSim::Prime(const FBeforeStep& BeforeStep)
{
	while (PrimeCurrent < FMath::Max(Definition.PrimeCount, 0))
	{
		++PrimeCurrent;
		ConstantTimeTick(BeforeStep);
	}
}

int32 FWoTFireFractalSim::TickTexture(
	const float DeltaSeconds, const FBeforeStep& BeforeStep)
{
	if (!bValid)
	{
		return 0;
	}
	const int32 Before = StepCount;
	Prime(BeforeStep);
	if (Definition.MaxFrameRate == 0.0f)
	{
		ConstantTimeTick(BeforeStep);
	}
	else
	{
		if (!WoTAdvanceRateLimitedTexture(Accumulator, Definition.MinFrameRate,
			Definition.MaxFrameRate, DeltaSeconds,
			[this, &BeforeStep]() { ConstantTimeTick(BeforeStep); }))
		{
			return StepCount - Before;
		}
	}
	return StepCount - Before;
}

void FWoTFireFractalSim::ConstantTimeTick(const FBeforeStep& BeforeStep)
{
	if (!bValid)
	{
		return;
	}
	if (BeforeStep)
	{
		BeforeStep();
	}
	// Engine authority: UFireTexture::ConstantTimeTick.
	RedrawSparks();
	if (Definition.bRising)
	{
		CalculateWrapFire();
	}
	else
	{
		CalculateSlowFire();
	}
	PostDrawSparks();
	++StepCount;
}

uint8 FWoTFireFractalSim::SpeedRand()
{
	check(RandomStream);
	return RandomStream->Next();
}

void FWoTFireFractalSim::PutPixel(
	const int32 X, const int32 Y, const uint8 Value)
{
	OutputIndices[(Y & (Definition.Height - 1)) * Definition.Width
		+ (X & (Definition.Width - 1))] = Value;
}

void FWoTFireFractalSim::MoveSpark(FWoTFractalSpark& Spark)
{
	// The original C MoveSpark. Each axis consumes a draw even
	// for a zero speed; preserving that call count keeps the global stream exact.
	const int32 XSpeed = SignedByte(Spark.ByteA);
	if ((SpeedRand() & 127) < FMath::Abs(XSpeed))
	{
		Spark.X = static_cast<uint8>((Spark.X + (XSpeed < 0 ? -1 : 1))
			& (Definition.Width - 1));
	}
	const int32 YSpeed = SignedByte(Spark.ByteB);
	if ((SpeedRand() & 127) < FMath::Abs(YSpeed))
	{
		Spark.Y = static_cast<uint8>((Spark.Y + (YSpeed < 0 ? -1 : 1))
			& (Definition.Height - 1));
	}
}

void FWoTFireFractalSim::DrawFlashRamp(
	uint8 X, uint8 Y, uint8 XLength, uint8 YLength,
	uint8 Color1, uint8 Color2)
{
	// Reimplements the original DrawFlashRamp (scalar). The low bit
	// of each length carries direction, and every jitter byte is retained.
	if (((YLength & 1) && (static_cast<int32>(YLength) * 2 >= XLength))
		|| ((XLength & 1) && (static_cast<int32>(YLength) * 2 < XLength)))
	{
		X = static_cast<uint8>(X + ((XLength & 1) ? -XLength : XLength));
		Y = static_cast<uint8>(Y + ((YLength & 1) ? -YLength : YLength));
		XLength ^= 1;
		YLength ^= 1;
		Swap(Color1, Color2);
	}

	const int32 MajorLength = 1 | FMath::Max<int32>(XLength, YLength);
	uint8 FlashArray[256];
	int32 FlashPosition = 0;
	for (int32 Flash = 0; Flash < MajorLength; ++Flash)
	{
		FlashArray[Flash] = SpeedRand();
		FlashPosition += FlashArray[Flash];
	}

	const int32 YStep = (YLength & 1) ? -1 : 1;
	const int32 RealYLength = (YLength & 1) ? -YLength : YLength;
	const int32 XStep = (XLength & 1) ? -1 : 1;
	const int32 RealXLength = (XLength & 1) ? -XLength : XLength;
	int32 RampColor = static_cast<int32>(static_cast<uint32>(Color1) << 23);
	const int32 ColorSlope = static_cast<int32>(
		(static_cast<int64>(static_cast<int32>(Color2) - Color1) * (1ll << 23))
		/ MajorLength);
	auto AdvanceColor = [&RampColor, ColorSlope]()
	{
		RampColor = static_cast<int32>(static_cast<uint32>(RampColor)
			+ static_cast<uint32>(ColorSlope));
		return static_cast<uint8>(RampColor >> 23);
	};

	if (XLength >= YLength)
	{
		int32 YPosition = static_cast<int32>(Y) << 6;
		const int32 FlashBias = (RealYLength * 64 - FlashPosition) / MajorLength;
		for (int32 Flash = 0; Flash < XLength; ++Flash)
		{
			YPosition += FlashArray[Flash] + FlashBias;
			PutPixel(X, YPosition >> 6, AdvanceColor());
			X = static_cast<uint8>(X + XStep);
		}
	}
	else
	{
		int32 XPosition = static_cast<int32>(X) << 6;
		const int32 FlashBias = (RealXLength * 64 - FlashPosition) / MajorLength;
		for (int32 Flash = 0; Flash < YLength; ++Flash)
		{
			XPosition += FlashArray[Flash] + FlashBias;
			PutPixel(XPosition >> 6, Y, AdvanceColor());
			Y = static_cast<uint8>(Y + YStep);
		}
	}
}

void FWoTFireFractalSim::RedrawSparks()
{
	// Shipped authored cases from the original RedrawSparks.
	// Newly emitted sparks are appended and therefore run later in this same
	// loop; a killed spark's last-element replacement is skipped until next tick.
	++GlobalPhase;
	for (int32 SparkIndex = 0; SparkIndex < Sparks.Num(); ++SparkIndex)
	{
		FWoTFractalSpark& Spark = Sparks[SparkIndex];
		bool bKill = false;
		switch (Spark.Type)
		{
		case SparkBurn:
			PutPixel(Spark.X, Spark.Y, SpeedRand());
			break;
		case SparkPulse:
			PutPixel(Spark.X, Spark.Y, Spark.Heat);
			Spark.Heat = static_cast<uint8>(Spark.Heat + Spark.ByteD);
			break;
		case SparkSignal:
			if (Spark.Heat > Spark.ByteC)
			{
				PutPixel(Spark.X, Spark.Y, Spark.Heat);
			}
			Spark.Heat = static_cast<uint8>(Spark.Heat + Spark.ByteD);
			if (Spark.Heat < Spark.ByteD)
			{
				Spark.Heat = SpeedRand();
			}
			break;
		case SparkBlaze:
			if (Sparks.Num() < Definition.SparksLimit && 128 > SpeedRand())
			{
				FWoTFractalSpark Spawned;
				Spawned.Type = SparkDrifter;
				Spawned.Heat = Spark.Heat;
				Spawned.X = Spark.X;
				Spawned.Y = Spark.Y;
				Spawned.ByteA = SpeedRand();
				Spawned.ByteB = SpeedRand();
				Spawned.ByteC = Spark.ByteC;
				Spawned.ByteD = Spark.ByteD;
				Sparks.Add(Spawned);
			}
			break;
		case SparkOzHasSpoken:
			if (Sparks.Num() < Definition.SparksLimit && 128 > SpeedRand())
			{
				FWoTFractalSpark Spawned;
				Spawned.Type = SparkDriftSlow;
				Spawned.Heat = Spark.Heat;
				Spawned.X = Spark.X;
				Spawned.Y = Spark.Y;
				Spawned.ByteA = static_cast<uint8>((SpeedRand() & 127) - 63);
				Spawned.ByteB = static_cast<uint8>(-127);
				Spawned.ByteD = 2;
				Sparks.Add(Spawned);
			}
			break;
		case SparkCylinder3D:
			if (((Spark.ByteA + 64) & 255) < 128)
			{
				const uint8 Heat = static_cast<uint8>(FMath::Min<int32>(
					PhaseTable[(Spark.ByteA + 64) & 255] + Spark.Heat, 255));
				const uint8 X = static_cast<uint8>(Spark.X
					+ ((PhaseTable[Spark.ByteA] * Spark.ByteB) >> 8));
				PutPixel(X, Spark.Y, Heat);
			}
			Spark.ByteA = static_cast<uint8>(Spark.ByteA + Spark.ByteD);
			break;
		case SparkEels:
			if (SpeedRand() < 20 && Sparks.Num() < Definition.SparksLimit)
			{
				FWoTFractalSpark Spawned;
				Spawned.Type = SparkSpawnedEel;
				Spawned.Heat = Spark.Heat;
				Spawned.X = static_cast<uint8>((Spark.X + (SpeedRand() & 31))
					& (Definition.Width - 1));
				Spawned.Y = static_cast<uint8>((Spark.Y + (SpeedRand() & 31))
					& (Definition.Height - 1));
				Spawned.ByteA = SpeedRand();
				Spawned.ByteB = SpeedRand();
				Spawned.ByteC = Spark.ByteC;
				Sparks.Add(Spawned);
			}
			// The retail expression is `SpeedRand() & 15 == 15`; C precedence
			// makes it `SpeedRand() & 1`, not a one-in-sixteen test.
			if ((SpeedRand() & 1) != 0)
			{
				Spark.X = static_cast<uint8>((Spark.X + (SpeedRand() & 15) - 7)
					& (Definition.Width - 1));
			}
			if ((SpeedRand() & 1) != 0)
			{
				Spark.Y = static_cast<uint8>((Spark.Y + (SpeedRand() & 15) - 7)
					& (Definition.Height - 1));
			}
			break;
		case SparkStars:
			PutPixel(Spark.X, Spark.Y, Spark.ByteB);
			break;
		case SparkLineLightning:
		case SparkRampLightning:
			if (Spark.Heat == 0)
			{
				break;
			}
			if (Spark.ByteC > 0)
			{
				--Spark.ByteC;
				DrawFlashRamp(Spark.X, Spark.Y, Spark.ByteA, Spark.ByteB,
					Spark.Heat,
					Spark.Type == SparkLineLightning
						? Spark.Heat : static_cast<uint8>(Spark.Heat >> 3));
			}
			else if (SpeedRand() >= Spark.ByteD)
			{
				Spark.ByteC = static_cast<uint8>((1 + SpeedRand()) & 5);
			}
			break;
		case SparkSphereLightning:
			if (SpeedRand() >= Spark.ByteD)
			{
				const uint8 Angle = SpeedRand();
				const int32 Radius = Spark.ByteC;
				const int32 XDisplacement =
					(Radius * PhaseTable[Angle]) >> 8;
				const int32 YDisplacement =
					(Radius * PhaseTable[static_cast<uint8>(Angle + 64)]) >> 8;
				int32 XLength = XDisplacement - Radius / 2;
				int32 YLength = YDisplacement - Radius / 2;
				XLength = XLength <= 0 ? (-XLength) | 1 : XLength & ~1;
				YLength = YLength <= 0 ? (-YLength) | 1 : YLength & ~1;
				DrawFlashRamp(Spark.X, Spark.Y,
					static_cast<uint8>(XLength), static_cast<uint8>(YLength),
					Spark.Heat, static_cast<uint8>(Spark.Heat >> 2));
			}
			break;
		case SparkDrifter:
			Spark.Heat = static_cast<uint8>(Spark.Heat - 5);
			if (Spark.Heat < 251)
			{
				PutPixel(Spark.X, Spark.Y, Spark.Heat);
				MoveSpark(Spark);
			}
			else
			{
				bKill = true;
			}
			break;
		case SparkDriftSlow:
			Spark.Heat = static_cast<uint8>(Spark.Heat - Spark.ByteD);
			if (Spark.Heat > Spark.ByteD)
			{
				PutPixel(Spark.X, Spark.Y, Spark.Heat);
				MoveSpark(Spark);
			}
			else
			{
				bKill = true;
			}
			break;
		case SparkSpawnedEel:
			Spark.ByteC = static_cast<uint8>(Spark.ByteC - 1);
			if (Spark.ByteC < 255)
			{
				PutPixel(Spark.X, Spark.Y, Spark.Heat);
				MoveSpark(Spark);
			}
			else
			{
				bKill = true;
			}
			break;
		default:
			checkNoEntry();
			break;
		}

		if (bKill)
		{
			Sparks[SparkIndex] = Sparks.Last();
			Sparks.Pop(EAllowShrinking::No);
		}
	}
}

void FWoTFireFractalSim::CalculateWrapFire()
{
	// The original C CalcWrapFire. This is deliberately in-place:
	// the final two rows consume the already-updated wrapped rows 0 and 1.
	const int32 Width = Definition.Width;
	const int32 Height = Definition.Height;
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 BelowY = (Y + 1) % Height;
		const int32 LowerY = (Y + 2) % Height;
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 Sum = OutputIndices[BelowY * Width + X]
				+ OutputIndices[BelowY * Width + ((X - 1) & (Width - 1))]
				+ OutputIndices[BelowY * Width + ((X + 1) & (Width - 1))]
				+ OutputIndices[LowerY * Width + X];
			OutputIndices[Y * Width + X] = RenderTable[Sum];
		}
	}
}

void FWoTFireFractalSim::CalculateSlowFire()
{
	// The original C CalcSlowFire. BelowLine aliases ThisLine,
	// so each X reads the newly written left neighbor; row order is observable.
	const int32 Width = Definition.Width;
	const int32 Height = Definition.Height;
	TArray<uint8> FirstLine;
	FirstLine.Append(OutputIndices.GetData(), Width);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const uint8* LowerLine = Y == Height - 1
			? FirstLine.GetData()
			: OutputIndices.GetData() + (Y + 1) * Width;
		uint8* ThisLine = OutputIndices.GetData() + Y * Width;
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 Sum = ThisLine[X]
				+ ThisLine[(X - 1) & (Width - 1)]
				+ ThisLine[(X + 1) & (Width - 1)]
				+ LowerLine[X];
			ThisLine[X] = RenderTable[Sum];
		}
	}
}

void FWoTFireFractalSim::PostDrawSparks()
{
	// The original UFireTexture::PostDrawSparks.
	if (!bStarStatus)
	{
		return;
	}
	bool bFoundStar = false;
	for (FWoTFractalSpark& Spark : Sparks)
	{
		if (Spark.Type != SparkStars)
		{
			continue;
		}
		bFoundStar = true;
		const int32 Pixel = Spark.Y * Definition.Width + Spark.X;
		Spark.ByteB = OutputIndices[Pixel];
		if (Spark.ByteB < StarCutoff)
		{
			OutputIndices[Pixel] = Spark.ByteA;
		}
	}
	bStarStatus = bFoundStar;
}

bool FWoTIceFractalSim::Initialize(
	const FWoTFractalTextureRecord& InDefinition)
{
	bValid = false;
	if (InDefinition.FractalClass != EWoTFractalClass::Ice
		|| !IsPowerOfTwoAtLeast8(InDefinition.Width)
		|| !IsPowerOfTwoAtLeast8(InDefinition.Height)
		|| InDefinition.InitialBitmap.Num()
			!= InDefinition.Width * InDefinition.Height
		|| InDefinition.SourceWidth != InDefinition.Width
		|| InDefinition.SourceHeight != InDefinition.Height
		|| InDefinition.GlassWidth != InDefinition.Width
		|| InDefinition.GlassHeight != InDefinition.Height
		|| InDefinition.SourceIndices.Num()
			!= InDefinition.Width * InDefinition.Height
		|| InDefinition.GlassIndices.Num()
			!= InDefinition.Width * InDefinition.Height
		|| InDefinition.PanningStyle > 4
		|| InDefinition.TimeMethod > 1)
	{
		return false;
	}
	Definition = InDefinition;
	bValid = true;
	Reset();
	return true;
}

bool FWoTIceFractalSim::UpdateSourceIndices(
	const TConstArrayView<uint8> InSourceIndices, const int32 SourceSlot)
{
	if (!bValid || InSourceIndices.Num() != Definition.Width * Definition.Height)
	{
		return false;
	}
	if (SourceSlot == 0)
	{
		SourceIndices.Reset(InSourceIndices.Num());
		SourceIndices.Append(InSourceIndices.GetData(), InSourceIndices.Num());
		return true;
	}
	if (SourceSlot == 1)
	{
		GlassIndices.Reset(InSourceIndices.Num());
		GlassIndices.Append(InSourceIndices.GetData(), InSourceIndices.Num());
		return true;
	}
	return false;
}

void FWoTIceFractalSim::Reset()
{
	if (!bValid)
	{
		return;
	}
	SourceIndices = Definition.SourceIndices;
	GlassIndices = Definition.GlassIndices;
	OutputIndices = Definition.InitialBitmap;
	MasterCount = Definition.MasterCount;
	UDisplace = Definition.UDisplace;
	VDisplace = Definition.VDisplace;
	UPosition = Definition.UPosition;
	VPosition = Definition.VPosition;
	Accumulator = 0.0f;
	OldUDisplace = -1;
	OldVDisplace = -1;
	PrimeCurrent = 0;
	StepCount = 0;
	bForceRefresh = true;
}

int32 FWoTIceFractalSim::RetailRound(const float Value)
{
	// Retail Win32 appRound is x87 FISTP:
	// round-to-nearest-even under the default control word.
	const int32 Lower = FMath::FloorToInt(Value);
	const float Fraction = Value - static_cast<float>(Lower);
	if (Fraction < 0.5f)
	{
		return Lower;
	}
	if (Fraction > 0.5f)
	{
		return Lower + 1;
	}
	return (Lower & 1) == 0 ? Lower : Lower + 1;
}

void FWoTIceFractalSim::MoveIcePosition(const float DeltaSeconds)
{
	// UIceTexture::MoveIcePosition. These assignment
	// boundaries stay FLOAT; trig alone promotes its already-rounded argument.
	MasterCount += DeltaSeconds * 120.0f;
	UDisplace += 2.0f * DeltaSeconds
		* -SignedByte(static_cast<uint8>(Definition.HorizPanSpeed - 128));
	VDisplace += 2.0f * DeltaSeconds
		* SignedByte(static_cast<uint8>(Definition.VertPanSpeed - 128));
	const float Phase = MasterCount * (Definition.Frequency + 1) * 0.0012f;
	const float MotionAmplitude = static_cast<float>(Definition.Amplitude + 1);
	switch (Definition.PanningStyle)
	{
	case 0:
		UPosition = UDisplace;
		VPosition = VDisplace;
		break;
	case 1:
		UPosition = UDisplace + RetailRound(static_cast<float>(
			MotionAmplitude * FMath::Sin(static_cast<double>(Phase))));
		VPosition = VDisplace + RetailRound(static_cast<float>(
			MotionAmplitude * FMath::Cos(static_cast<double>(Phase))));
		break;
	case 2:
		UPosition = UDisplace + RetailRound(static_cast<float>(
			MotionAmplitude * FMath::Sin(static_cast<double>(Phase))));
		VPosition = VDisplace + RetailRound(static_cast<float>(MotionAmplitude
			* FMath::Cos(static_cast<double>(
				MasterCount * (Definition.Frequency + 1) * 0.0011f))));
		break;
	case 3:
		UPosition = UDisplace + RetailRound(static_cast<float>(0.5
			* MotionAmplitude * FMath::Sin(static_cast<double>(Phase))));
		VPosition = VDisplace;
		break;
	case 4:
		UPosition = UDisplace;
		VPosition = VDisplace + RetailRound(static_cast<float>(0.5
			* MotionAmplitude * FMath::Cos(static_cast<double>(Phase))));
		break;
	default:
		checkNoEntry();
		break;
	}
}

void FWoTIceFractalSim::BlitTexIce()
{
	// The original C BlitTexIce: the panned glass byte displaces
	// U while the source row remains static.
	const int32 UMask = Definition.Width - 1;
	const int32 VMask = Definition.Height - 1;
	const int32 UDisp = RetailRound(UPosition) & UMask;
	const int32 VDisp = RetailRound(VPosition) & VMask;
	for (int32 V = 0; V < Definition.Height; ++V)
	{
		for (int32 U = 0; U < Definition.Width; ++U)
		{
			const uint8 Glass = GlassIndices[
				((V + VDisp) & VMask) * Definition.Width + ((U + UDisp) & UMask)];
			OutputIndices[V * Definition.Width + U] =
				SourceIndices[V * Definition.Width + ((U + Glass) & UMask)];
		}
	}
}

void FWoTIceFractalSim::BlitIceTex()
{
	// The original C BlitIceTex: glass stays fixed while source
	// U and V pan beneath it.
	const int32 UMask = Definition.Width - 1;
	const int32 VMask = Definition.Height - 1;
	const int32 UDisp = RetailRound(UPosition) & UMask;
	const int32 VDisp = RetailRound(VPosition) & VMask;
	for (int32 V = 0; V < Definition.Height; ++V)
	{
		for (int32 U = 0; U < Definition.Width; ++U)
		{
			const uint8 Glass = GlassIndices[V * Definition.Width + U];
			OutputIndices[V * Definition.Width + U] = SourceIndices[
				((V + VDisp) & VMask) * Definition.Width
				+ ((U + UDisp + Glass) & UMask)];
		}
	}
}

void FWoTIceFractalSim::RenderIce(const float DeltaSeconds)
{
	MoveIcePosition(DeltaSeconds);
	const int32 NewU = RetailRound(UPosition);
	const int32 NewV = RetailRound(VPosition);
	if (NewU == OldUDisplace && NewV == OldVDisplace && !bForceRefresh)
	{
		return;
	}
	OldUDisplace = NewU;
	OldVDisplace = NewV;
	if (Definition.bMoveIce)
	{
		BlitTexIce();
	}
	else
	{
		BlitIceTex();
	}
	bForceRefresh = false;
}

void FWoTIceFractalSim::ConstantTimeTick(const FBeforeStep& BeforeStep)
{
	if (!bValid)
	{
		return;
	}
	if (BeforeStep)
	{
		BeforeStep();
	}
	// UIceTexture::ConstantTimeTick.
	RenderIce(1.0f / 120.0f);
	++StepCount;
}

void FWoTIceFractalSim::Prime(const FBeforeStep& BeforeStep)
{
	while (PrimeCurrent < FMath::Max(Definition.PrimeCount, 0))
	{
		++PrimeCurrent;
		ConstantTimeTick(BeforeStep);
	}
}

int32 FWoTIceFractalSim::TickTexture(
	const float DeltaSeconds, const FBeforeStep& BeforeStep)
{
	if (!bValid)
	{
		return 0;
	}
	const int32 Before = StepCount;
	if (Definition.TimeMethod != 0)
	{
		// UIceTexture::Tick: real-time scroll is a
		// full raw-tick override and does not enter UTexture's rate accumulator.
		if (BeforeStep)
		{
			BeforeStep();
		}
		RenderIce(DeltaSeconds);
		++StepCount;
		return 1;
	}

	Prime(BeforeStep);
	if (Definition.MaxFrameRate == 0.0f)
	{
		ConstantTimeTick(BeforeStep);
	}
	else
	{
		if (!WoTAdvanceRateLimitedTexture(Accumulator, Definition.MinFrameRate,
			Definition.MaxFrameRate, DeltaSeconds,
			[this, &BeforeStep]() { ConstantTimeTick(BeforeStep); }))
		{
			return StepCount - Before;
		}
	}
	return StepCount - Before;
}
