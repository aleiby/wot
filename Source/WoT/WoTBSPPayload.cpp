#include "WoTBSPPayload.h"

#include "Containers/StringConv.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "WoTSha256.h"

namespace
{
	constexpr uint32 MaxVertices = 2000000;
	constexpr uint32 MaxIndices = 6000000;
	constexpr uint32 MaxSections = 4096;
	constexpr uint32 MaxSlotBytes = 1024;
	constexpr uint32 MaxLightmapDimension = 8192;
	constexpr uint64 MaxLightmapPixels = 4096ull * 8192ull;
	constexpr uint64 MaxEntryLightmapPixels = 4ull * 1024ull * 1024ull;
	constexpr uint32 MaxEntrySurfaces = 65536;
	constexpr uint32 MaxEntryLights = 8;
	constexpr uint32 MaxEntryPairs = 4096;
	constexpr uint32 MaxEntryIdentityBytes = 1024;
	constexpr uint64 MaxEntryPairBytes = 4ull * 1024ull * 1024ull;
	constexpr uint32 MaxAnimationSurfaces = 65536;
	constexpr uint32 MaxAnimationGroups = 4;
	constexpr uint32 MaxAnimationTerms = 6;
	constexpr uint64 MaxAnimationBytes = 64ull * 1024ull * 1024ull;
	constexpr uint32 MaxSkyRestSurfaces = 65536;
	constexpr uint32 MaxSkyRestGroups = 4096;
	constexpr uint32 MaxSkyRestTriangles = 2000000;
	constexpr float MaxEntryPanAbs = 1024.0f;
	constexpr float MaxEntryLevelBrightness = 1024.0f;
	constexpr float MaxCoordinateAbs = 10000000.0f;
	constexpr uint64 MaxPayloadBytes = 256ull * 1024ull * 1024ull;
	constexpr uint32 HeaderBytes = 52;

	bool CheckedAdd(uint64 A, uint64 B, uint64& Out)
	{
		if (B > MAX_uint64 - A) return false;
		Out = A + B;
		return true;
	}

	bool CheckedMultiply(uint64 A, uint64 B, uint64& Out)
	{
		if (A != 0 && B > MAX_uint64 / A) return false;
		Out = A * B;
		return true;
	}

	bool ReadU16(TConstArrayView<uint8> Bytes, uint64& Offset, uint16& Out)
	{
		uint64 End = 0;
		if (!CheckedAdd(Offset, 2, End) || End > static_cast<uint64>(Bytes.Num()))
			return false;
		const int32 At = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(At) || !Bytes.IsValidIndex(At + 1)) return false;
		Out = static_cast<uint16>(Bytes[At])
			| (static_cast<uint16>(Bytes[At + 1]) << 8);
		Offset = End;
		return true;
	}

	bool ReadU32(TConstArrayView<uint8> Bytes, uint64& Offset, uint32& Out)
	{
		uint64 End = 0;
		if (!CheckedAdd(Offset, 4, End) || End > static_cast<uint64>(Bytes.Num()))
			return false;
		const int32 At = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(At) || !Bytes.IsValidIndex(At + 1)
			|| !Bytes.IsValidIndex(At + 2) || !Bytes.IsValidIndex(At + 3)) return false;
		Out = static_cast<uint32>(Bytes[At])
			| (static_cast<uint32>(Bytes[At + 1]) << 8)
			| (static_cast<uint32>(Bytes[At + 2]) << 16)
			| (static_cast<uint32>(Bytes[At + 3]) << 24);
		Offset = End;
		return true;
	}

	bool ReadU8(TConstArrayView<uint8> Bytes, uint64& Offset, uint8& Out)
	{
		uint64 End = 0;
		if (!CheckedAdd(Offset, 1, End) || End > static_cast<uint64>(Bytes.Num()))
			return false;
		const int32 At = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(At)) return false;
		Out = Bytes[At];
		Offset = End;
		return true;
	}

	bool ReadFloat(TConstArrayView<uint8> Bytes, uint64& Offset, float& Out)
	{
		uint32 Bits = 0;
		if (!ReadU32(Bytes, Offset, Bits)) return false;
		FMemory::Memcpy(&Out, &Bits, sizeof(float));
		return FMath::IsFinite(Out);
	}

	bool IsSHA256(const FString& Value)
	{
		if (Value.Len() != 64) return false;
		for (TCHAR Character : Value)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f')))) return false;
		}
		return true;
	}

	void AppendUInt64LE(uint64 Value, TArray<uint8>& Out)
	{
		for (int32 Byte = 0; Byte < 8; ++Byte)
			Out.Add(static_cast<uint8>((Value >> (Byte * 8)) & 0xff));
	}

	bool AppendFramedUTF8(const FString& Value, TArray<uint8>& Out)
	{
		FTCHARToUTF8 Encoded(*Value);
		if (Encoded.Length() < 0 || Encoded.Length() > MAX_int32 - Out.Num() - 8)
			return false;
		AppendUInt64LE(static_cast<uint64>(Encoded.Length()), Out);
		Out.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		return true;
	}

	struct FEntrySurfacePreflight
	{
		uint32 X = 0;
		uint32 Y = 0;
		uint32 W = 0;
		uint32 H = 0;
	};

	struct FEntryRowInterval
	{
		uint32 Begin = 0;
		uint32 End = 0;
	};

	struct FEntryTrailerLayout
	{
		uint32 TriangleCount = 0;
		uint32 SurfaceCount = 0;
		uint32 LightCount = 0;
		uint32 PairCount = 0;
		bool bRootOutside = false;
	};

	// The strict text rule every BSP payload string shares (Tools/convert/
	// bsp_payload.py _decode_bsp_text_at): valid UTF-8 that round-trips byte for
	// byte (no replacement characters, overlong forms or lone surrogates), no
	// embedded NUL and no control characters. Count 0 decodes to "".
	bool DecodeStrictUtf8(TConstArrayView<uint8> Bytes, int32 At, int32 Count,
		FString& Out)
	{
		Out.Reset();
		if (Count == 0)
		{
			return true;
		}
		if (Count < 0 || !Bytes.IsValidIndex(At) || !Bytes.IsValidIndex(At + Count - 1))
		{
			return false;
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Bytes[At + Index] == 0)
			{
				return false;
			}
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + At), Count);
		Out = FString(Converted.Length(), Converted.Get());
		const FTCHARToUTF8 RoundTrip(*Out);
		if (Out.IsEmpty() || RoundTrip.Length() != Count
			|| FMemory::Memcmp(RoundTrip.Get(), Bytes.GetData() + At, Count) != 0)
		{
			Out.Reset();
			return false;
		}
		for (const TCHAR Character : Out)
		{
			if (Character < 32 || Character == 127)
			{
				Out.Reset();
				return false;
			}
		}
		return true;
	}

	bool ReadEntryIdentity(TConstArrayView<uint8> Bytes, uint64& Offset,
		uint16 IdentityBytes, FString& OutIdentity)
	{
		uint64 End = 0;
		if (IdentityBytes == 0 || IdentityBytes > MaxEntryIdentityBytes
			|| !CheckedAdd(Offset, IdentityBytes, End)
			|| End > static_cast<uint64>(Bytes.Num()))
		{
			return false;
		}
		const int32 At = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(At) || !Bytes.IsValidIndex(static_cast<int32>(End - 1)))
			return false;
		for (uint16 Byte = 0; Byte < IdentityBytes; ++Byte)
			if (Bytes[At + Byte] == 0) return false;
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + At), IdentityBytes);
		OutIdentity = FString(Converted.Length(), Converted.Get());
		const FTCHARToUTF8 RoundTrip(*OutIdentity);
		if (OutIdentity.IsEmpty() || Converted.Length() == 0
			|| RoundTrip.Length() != IdentityBytes
			|| FMemory::Memcmp(RoundTrip.Get(), Bytes.GetData() + At,
				IdentityBytes) != 0)
		{
			return false;
		}
		for (TCHAR Character : OutIdentity)
			if (Character < 32 || Character == 127) return false;
		Offset = End;
		return true;
	}

	bool ReadSectionIdentity(TConstArrayView<uint8> Bytes, uint64& Offset,
		uint16 IdentityBytes, FString& OutIdentity, bool bOptional)
	{
		if (IdentityBytes == 0)
		{
			OutIdentity.Empty();
			return bOptional;
		}
		return ReadEntryIdentity(Bytes, Offset, IdentityBytes, OutIdentity);
	}

	bool PreflightEntryTrailer(TConstArrayView<uint8> Bytes, uint64 TrailerStart,
		uint32 IndexCount, uint32 LightmapWidth, uint32 LightmapHeight,
		FEntryTrailerLayout& OutLayout, FString& OutError)
	{
		uint64 Offset = TrailerStart;
		uint32 TrailerByteCount = 0;
		uint32 RootOutside = 0;
		if (!ReadU32(Bytes, Offset, TrailerByteCount)
			|| !ReadU32(Bytes, Offset, OutLayout.TriangleCount)
			|| !ReadU32(Bytes, Offset, OutLayout.SurfaceCount)
			|| !ReadU32(Bytes, Offset, OutLayout.LightCount)
			|| !ReadU32(Bytes, Offset, OutLayout.PairCount)
			|| !ReadU32(Bytes, Offset, RootOutside))
		{
			OutError = TEXT("truncated BSP entry trailer header");
			return false;
		}
		const uint64 PayloadBytes = static_cast<uint64>(Bytes.Num());
		uint64 LightmapPixels = 0;
		if (TrailerStart > PayloadBytes
			|| TrailerByteCount != PayloadBytes - TrailerStart
			|| TrailerByteCount < 24
			|| OutLayout.TriangleCount != IndexCount / 3
			|| OutLayout.SurfaceCount == 0
			|| OutLayout.SurfaceCount > MaxEntrySurfaces
			|| OutLayout.LightCount > MaxEntryLights
			|| OutLayout.PairCount > MaxEntryPairs
			|| RootOutside > 1
			|| !CheckedMultiply(LightmapWidth, LightmapHeight, LightmapPixels)
			|| LightmapPixels > MaxEntryLightmapPixels)
		{
			OutError = TEXT("invalid BSP entry trailer counts or length");
			return false;
		}
		OutLayout.bRootOutside = RootOutside != 0;
		for (uint32 Triangle = 0; Triangle < OutLayout.TriangleCount; ++Triangle)
		{
			uint32 SurfaceIndex = 0;
			if (!ReadU32(Bytes, Offset, SurfaceIndex)
				|| SurfaceIndex >= OutLayout.SurfaceCount)
			{
				OutError = TEXT("invalid BSP entry triangle surface index");
				return false;
			}
		}

		TArray<FEntrySurfacePreflight> Surfaces;
		Surfaces.SetNumUninitialized(static_cast<int32>(OutLayout.SurfaceCount));
		TArray<TArray<FEntryRowInterval>> RowIntervals;
		RowIntervals.SetNum(static_cast<int32>(LightmapHeight));
		uint64 TotalPaddedArea = 0;
		for (uint32 Surface = 0; Surface < OutLayout.SurfaceCount; ++Surface)
		{
			uint32 SurfaceIndex = 0;
			FEntrySurfacePreflight& Record = Surfaces[static_cast<int32>(Surface)];
			float PanU = 0.0f, PanV = 0.0f;
			if (!ReadU32(Bytes, Offset, SurfaceIndex)
				|| !ReadU32(Bytes, Offset, Record.X)
				|| !ReadU32(Bytes, Offset, Record.Y)
				|| !ReadU32(Bytes, Offset, Record.W)
				|| !ReadU32(Bytes, Offset, Record.H)
				|| !ReadFloat(Bytes, Offset, PanU)
				|| !ReadFloat(Bytes, Offset, PanV)
				|| SurfaceIndex != Surface
				|| FMath::Abs(PanU) > MaxEntryPanAbs
				|| FMath::Abs(PanV) > MaxEntryPanAbs)
			{
				OutError = TEXT("invalid BSP entry surface row");
				return false;
			}
			const bool bEmpty = Record.X == 0 && Record.Y == 0
				&& Record.W == 0 && Record.H == 0;
			if (bEmpty) continue;
			uint64 InteriorRight = 0, InteriorBottom = 0;
			uint64 PaddedWidth = 0, PaddedHeight = 0, PaddedArea = 0;
			if (Record.X == 0 || Record.Y == 0 || Record.W == 0 || Record.H == 0
				|| !CheckedAdd(Record.X, Record.W, InteriorRight)
				|| !CheckedAdd(Record.Y, Record.H, InteriorBottom)
				|| InteriorRight >= LightmapWidth || InteriorBottom >= LightmapHeight
				|| !CheckedAdd(Record.W, 2, PaddedWidth)
				|| !CheckedAdd(Record.H, 2, PaddedHeight)
				|| !CheckedMultiply(PaddedWidth, PaddedHeight, PaddedArea)
				|| !CheckedAdd(TotalPaddedArea, PaddedArea, TotalPaddedArea)
				|| TotalPaddedArea > LightmapPixels)
			{
				OutError = TEXT("invalid BSP entry surface rectangle or gutter");
				return false;
			}
			const FEntryRowInterval Interval = {
				Record.X - 1, static_cast<uint32>(InteriorRight + 1)};
			for (uint64 Row = Record.Y - 1; Row < InteriorBottom + 1; ++Row)
				RowIntervals[static_cast<int32>(Row)].Add(Interval);
		}
		for (TArray<FEntryRowInterval>& Intervals : RowIntervals)
		{
			Intervals.Sort([](const FEntryRowInterval& Left,
				const FEntryRowInterval& Right)
			{
				return Left.Begin == Right.Begin
					? Left.End < Right.End : Left.Begin < Right.Begin;
			});
			for (int32 Index = 1; Index < Intervals.Num(); ++Index)
			{
				if (Intervals[Index].Begin < Intervals[Index - 1].End)
				{
					OutError = TEXT("overlapping BSP entry atlas allocations");
					return false;
				}
			}
		}

		TArray<uint8> PreviousIdentityBytes;
		TSet<FString> FoldedIdentities;
		for (uint32 Light = 0; Light < OutLayout.LightCount; ++Light)
		{
			uint16 IdentityBytes = 0, HeaderReserved = 0;
			FString Identity;
			uint8 Brightness = 0, Hue = 0, Saturation = 0, ColorReserved = 0;
			float LevelBrightness = 0.0f;
			if (!ReadU16(Bytes, Offset, IdentityBytes)
				|| !ReadU16(Bytes, Offset, HeaderReserved)
				|| HeaderReserved != 0)
			{
				OutError = TEXT("invalid BSP entry light row");
				return false;
			}
			const uint64 IdentityOffset = Offset;
			if (!ReadEntryIdentity(Bytes, Offset, IdentityBytes, Identity)
				|| !ReadU8(Bytes, Offset, Brightness)
				|| !ReadU8(Bytes, Offset, Hue)
				|| !ReadU8(Bytes, Offset, Saturation)
				|| !ReadU8(Bytes, Offset, ColorReserved)
				|| !ReadFloat(Bytes, Offset, LevelBrightness)
				|| ColorReserved != 0 || LevelBrightness < 0.0f
				|| LevelBrightness > MaxEntryLevelBrightness)
			{
				OutError = TEXT("invalid BSP entry light row");
				return false;
			}
			const FString Folded = Identity.ToLower();
			int32 Utf8Order = -1;
			if (!PreviousIdentityBytes.IsEmpty())
			{
				const int32 SharedBytes = FMath::Min(
					PreviousIdentityBytes.Num(), static_cast<int32>(IdentityBytes));
				Utf8Order = FMemory::Memcmp(PreviousIdentityBytes.GetData(),
					Bytes.GetData() + static_cast<int32>(IdentityOffset), SharedBytes);
				if (Utf8Order == 0)
					Utf8Order = PreviousIdentityBytes.Num()
						- static_cast<int32>(IdentityBytes);
			}
			if (Utf8Order >= 0 || FoldedIdentities.Contains(Folded))
			{
				OutError = TEXT("duplicate or out-of-order BSP entry light identity");
				return false;
			}
			FoldedIdentities.Add(Folded);
			PreviousIdentityBytes.Reset(static_cast<int32>(IdentityBytes));
			PreviousIdentityBytes.Append(
				Bytes.GetData() + static_cast<int32>(IdentityOffset), IdentityBytes);
		}

		uint64 TotalPairBytes = 0;
		uint32 PreviousLight = 0, PreviousSurface = 0;
		for (uint32 Pair = 0; Pair < OutLayout.PairCount; ++Pair)
		{
			uint32 LightIndex = 0, SurfaceIndex = 0, ByteCount = 0;
			if (!ReadU32(Bytes, Offset, LightIndex)
				|| !ReadU32(Bytes, Offset, SurfaceIndex)
				|| !ReadU32(Bytes, Offset, ByteCount)
				|| LightIndex >= OutLayout.LightCount
				|| SurfaceIndex >= OutLayout.SurfaceCount)
			{
				OutError = TEXT("invalid BSP entry illumination reference");
				return false;
			}
			if (Pair > 0 && (LightIndex < PreviousLight
				|| (LightIndex == PreviousLight && SurfaceIndex <= PreviousSurface)))
			{
				OutError = TEXT("duplicate or out-of-order BSP entry illumination pair");
				return false;
			}
			const FEntrySurfacePreflight& Surface =
				Surfaces[static_cast<int32>(SurfaceIndex)];
			uint64 ExpectedBytes = 0;
			uint64 PairEnd = 0;
			if (Surface.W == 0 || Surface.H == 0
				|| !CheckedMultiply(Surface.W, Surface.H, ExpectedBytes)
				|| ExpectedBytes != ByteCount
				|| !CheckedAdd(TotalPairBytes, ByteCount, TotalPairBytes)
				|| TotalPairBytes > MaxEntryPairBytes
				|| !CheckedAdd(Offset, ByteCount, PairEnd)
				|| PairEnd > PayloadBytes)
			{
				OutError = TEXT("invalid BSP entry illumination byte count");
				return false;
			}
			Offset = PairEnd;
			PreviousLight = LightIndex;
			PreviousSurface = SurfaceIndex;
		}
		if (Offset != PayloadBytes)
		{
			OutError = TEXT("BSP entry trailer contains extra bytes");
			return false;
		}
		return true;
	}

	bool DecodeEntryTrailer(TConstArrayView<uint8> Bytes, uint64 TrailerStart,
		const FEntryTrailerLayout& Layout, FWoTBSPPayloadData& Out,
		FString& OutError)
	{
		uint64 Offset = TrailerStart;
		uint32 Ignored = 0;
		for (int32 HeaderField = 0; HeaderField < 6; ++HeaderField)
			if (!ReadU32(Bytes, Offset, Ignored)) return false;
		Out.bRootOutside = Layout.bRootOutside;
		Out.TriangleSurfaces.SetNumUninitialized(
			static_cast<int32>(Layout.TriangleCount));
		for (uint32& SurfaceIndex : Out.TriangleSurfaces)
			if (!ReadU32(Bytes, Offset, SurfaceIndex)) return false;
		Out.EntrySurfaces.SetNumUninitialized(static_cast<int32>(Layout.SurfaceCount));
		for (FWoTEntrySurfaceRecord& Surface : Out.EntrySurfaces)
		{
			if (!ReadU32(Bytes, Offset, Surface.SurfaceIndex)
				|| !ReadU32(Bytes, Offset, Surface.X)
				|| !ReadU32(Bytes, Offset, Surface.Y)
				|| !ReadU32(Bytes, Offset, Surface.W)
				|| !ReadU32(Bytes, Offset, Surface.H)
				|| !ReadFloat(Bytes, Offset, Surface.PanUV.X)
				|| !ReadFloat(Bytes, Offset, Surface.PanUV.Y)) return false;
		}
		Out.EntryLights.Reserve(static_cast<int32>(Layout.LightCount));
		for (uint32 Light = 0; Light < Layout.LightCount; ++Light)
		{
			uint16 IdentityBytes = 0, Reserved16 = 0;
			uint8 Reserved8 = 0;
			FWoTEntryLightRecord& Record = Out.EntryLights.AddDefaulted_GetRef();
			if (!ReadU16(Bytes, Offset, IdentityBytes)
				|| !ReadU16(Bytes, Offset, Reserved16)
				|| !ReadEntryIdentity(Bytes, Offset, IdentityBytes, Record.Identity)
				|| !ReadU8(Bytes, Offset, Record.Brightness)
				|| !ReadU8(Bytes, Offset, Record.Hue)
				|| !ReadU8(Bytes, Offset, Record.Saturation)
				|| !ReadU8(Bytes, Offset, Reserved8)
				|| !ReadFloat(Bytes, Offset, Record.LevelBrightness)) return false;
		}
		Out.EntryIllumination.Reserve(static_cast<int32>(Layout.PairCount));
		for (uint32 Pair = 0; Pair < Layout.PairCount; ++Pair)
		{
			uint32 ByteCount = 0;
			FWoTEntryIlluminationRecord& Record =
				Out.EntryIllumination.AddDefaulted_GetRef();
			if (!ReadU32(Bytes, Offset, Record.LightIndex)
				|| !ReadU32(Bytes, Offset, Record.SurfaceIndex)
				|| !ReadU32(Bytes, Offset, ByteCount)) return false;
			Record.Illumination.Append(Bytes.GetData() + static_cast<int32>(Offset),
				static_cast<int32>(ByteCount));
			Offset += ByteCount;
		}
		if (Offset != static_cast<uint64>(Bytes.Num()))
		{
			OutError = TEXT("BSP entry trailer changed after preflight");
			return false;
		}
		return true;
	}
}

namespace
{
	/** Version-6 sky-rest trailer (flag 32): the skybake.py bake_rest census
	    (sky zone, ray and covered-ray counts, ascending per-surface counts) and
	    the skygeo.py rest groups. Every count is bounded by the remaining bytes
	    before anything is allocated. */
	bool DecodeSkyRestTrailer(TConstArrayView<uint8> Bytes, uint64 TrailerStart,
		uint64 TrailerEnd, FWoTBSPPayloadData& Out, FString& OutError)
	{
		uint64 Offset = TrailerStart;
		uint32 TrailerByteCount = 0, SurfaceCount = 0, GroupCount = 0;
		FWoTBSPSkyRest SkyRest;
		SkyRest.bPresent = true;
		if (TrailerEnd > static_cast<uint64>(Bytes.Num()) || TrailerStart > TrailerEnd
			|| !ReadU32(Bytes, Offset, TrailerByteCount)
			|| TrailerByteCount != TrailerEnd - TrailerStart
			|| !ReadU32(Bytes, Offset, SkyRest.SkyZone)
			|| !ReadU32(Bytes, Offset, SkyRest.RayCount)
			|| !ReadU32(Bytes, Offset, SkyRest.CoveredRays)
			|| !ReadU32(Bytes, Offset, SurfaceCount)
			|| SkyRest.SkyZone == 0 || SkyRest.SkyZone > MAX_uint8
			|| SkyRest.RayCount == 0 || SkyRest.CoveredRays > SkyRest.RayCount
			|| SurfaceCount > MaxSkyRestSurfaces
			|| static_cast<uint64>(SurfaceCount) * 16 > TrailerEnd - Offset)
		{
			OutError = TEXT("invalid BSP sky-rest trailer header");
			return false;
		}
		SkyRest.Surfaces.SetNumUninitialized(static_cast<int32>(SurfaceCount));
		for (int32 Index = 0; Index < SkyRest.Surfaces.Num(); ++Index)
		{
			FWoTBSPSkyRestSurface& Row = SkyRest.Surfaces[Index];
			if (!ReadU32(Bytes, Offset, Row.Surface)
				|| !ReadU32(Bytes, Offset, Row.VisibleRays)
				|| !ReadU32(Bytes, Offset, Row.BlockerRays)
				|| !ReadU32(Bytes, Offset, Row.FakeBackdropRays)
				|| (Index > 0 && Row.Surface <= SkyRest.Surfaces[Index - 1].Surface)
				|| Row.VisibleRays + static_cast<uint64>(Row.BlockerRays)
					+ Row.FakeBackdropRays == 0)
			{
				OutError = TEXT("invalid BSP sky-rest surface row");
				return false;
			}
		}
		uint64 TotalTriangles = 0;
		if (!ReadU32(Bytes, Offset, GroupCount) || GroupCount > MaxSkyRestGroups
			|| static_cast<uint64>(GroupCount) * 8 > TrailerEnd - Offset)
		{
			OutError = TEXT("invalid BSP sky-rest group count");
			return false;
		}
		TSet<FString> Seen;
		for (uint32 GroupIndex = 0; GroupIndex < GroupCount; ++GroupIndex)
		{
			uint16 RoleAndPad = 0, SlotBytes = 0;
			if (!ReadU16(Bytes, Offset, RoleAndPad) || !ReadU16(Bytes, Offset, SlotBytes)
				|| (RoleAndPad >> 8) != 0 || (RoleAndPad & 0xff) < 1
				|| (RoleAndPad & 0xff) > 3 || SlotBytes > MaxSlotBytes
				|| SlotBytes > TrailerEnd - Offset)
			{
				OutError = TEXT("invalid BSP sky-rest group header");
				return false;
			}
			FWoTBSPSkyRestGroup& Group = SkyRest.Groups.AddDefaulted_GetRef();
			Group.Role = static_cast<EWoTBSPSkyRestRole>(RoleAndPad & 0xff);
			if (SlotBytes > 0)
			{
				if (!DecodeStrictUtf8(Bytes, static_cast<int32>(Offset), SlotBytes,
						Group.Slot))
				{
					OutError = TEXT("unsafe BSP sky-rest slot");
					return false;
				}
				Offset += SlotBytes;
			}
			const FString Key = FString::Printf(TEXT("%d:%s"),
				static_cast<int32>(Group.Role), *Group.Slot);
			uint32 TriangleCount = 0;
			if ((Group.Role == EWoTBSPSkyRestRole::Visible) != !Group.Slot.IsEmpty()
				|| Seen.Contains(Key) || !ReadU32(Bytes, Offset, TriangleCount)
				|| TriangleCount == 0
				|| (TotalTriangles += TriangleCount) > MaxSkyRestTriangles
				|| static_cast<uint64>(TriangleCount) * 96 > TrailerEnd - Offset)
			{
				OutError = TEXT("invalid BSP sky-rest group");
				return false;
			}
			Seen.Add(Key);
			const int32 VertexCount = static_cast<int32>(TriangleCount) * 3;
			Group.Positions.SetNumUninitialized(VertexCount);
			Group.Normals.SetNumUninitialized(VertexCount);
			Group.UV0.SetNumUninitialized(VertexCount);
			Group.UV1.SetNumUninitialized(VertexCount);
			for (uint32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
			{
				FVector3f Normal;
				if (!ReadFloat(Bytes, Offset, Normal.X) || !ReadFloat(Bytes, Offset, Normal.Y)
					|| !ReadFloat(Bytes, Offset, Normal.Z))
				{
					OutError = TEXT("truncated BSP sky-rest normal");
					return false;
				}
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 Vertex = static_cast<int32>(Triangle) * 3 + Corner;
					FVector3f& Position = Group.Positions[Vertex];
					FVector2f& UV0 = Group.UV0[Vertex];
					FVector2f& UV1 = Group.UV1[Vertex];
					if (!ReadFloat(Bytes, Offset, Position.X)
						|| !ReadFloat(Bytes, Offset, Position.Y)
						|| !ReadFloat(Bytes, Offset, Position.Z)
						|| !ReadFloat(Bytes, Offset, UV0.X) || !ReadFloat(Bytes, Offset, UV0.Y)
						|| !ReadFloat(Bytes, Offset, UV1.X) || !ReadFloat(Bytes, Offset, UV1.Y)
						|| Position.GetAbsMax() > MaxCoordinateAbs)
					{
						OutError = TEXT("invalid BSP sky-rest vertex");
						return false;
					}
					Group.Normals[Vertex] = Normal;
				}
			}
		}
		if (Offset != TrailerEnd)
		{
			OutError = TEXT("BSP sky-rest trailer contains extra bytes");
			return false;
		}
		Out.SkyRest = MoveTemp(SkyRest);
		return true;
	}

	/** Version-5 animation trailer: placements, waveform groups, response terms
	    with sparse per-surface unsigned RGB rects. Every bound is checked
	    before any allocation; rects must sit inside the atlas with their
	    one-pixel gutters and must not overlap. */
	bool DecodeAnimationTrailer(TConstArrayView<uint8> Bytes, uint64 TrailerStart,
		uint64 TrailerEnd, uint32 LightmapWidth, uint32 LightmapHeight,
		FWoTBSPPayloadData& Out, FString& OutError)
	{
		uint64 Offset = TrailerStart;
		uint32 TrailerByteCount = 0, SurfaceCount = 0;
		if (TrailerEnd > static_cast<uint64>(Bytes.Num()) || TrailerStart > TrailerEnd
			|| !ReadU32(Bytes, Offset, TrailerByteCount)
			|| TrailerByteCount != TrailerEnd - TrailerStart
			|| !ReadU32(Bytes, Offset, SurfaceCount)
			|| SurfaceCount == 0 || SurfaceCount > MaxAnimationSurfaces
			|| static_cast<uint64>(SurfaceCount) * 16 > TrailerEnd - Offset)
		{
			OutError = TEXT("invalid BSP animation trailer header");
			return false;
		}
		TArray<FWoTBSPAnimationSurface> Surfaces;
		Surfaces.SetNumUninitialized(static_cast<int32>(SurfaceCount));
		TArray<TArray<FEntryRowInterval>> RowIntervals;
		RowIntervals.SetNum(static_cast<int32>(LightmapHeight));
		uint64 TotalPaddedArea = 0;
		const uint64 LightmapPixels = static_cast<uint64>(LightmapWidth) * LightmapHeight;
		for (uint32 Surface = 0; Surface < SurfaceCount; ++Surface)
		{
			FWoTBSPAnimationSurface& Record = Surfaces[static_cast<int32>(Surface)];
			if (!ReadU32(Bytes, Offset, Record.X) || !ReadU32(Bytes, Offset, Record.Y)
				|| !ReadU32(Bytes, Offset, Record.W) || !ReadU32(Bytes, Offset, Record.H))
			{
				OutError = TEXT("truncated BSP animation surface row");
				return false;
			}
			const bool bEmpty = Record.X == 0 && Record.Y == 0
				&& Record.W == 0 && Record.H == 0;
			if (bEmpty) continue;
			uint64 InteriorRight = 0, InteriorBottom = 0;
			uint64 PaddedWidth = 0, PaddedHeight = 0, PaddedArea = 0;
			if (Record.X == 0 || Record.Y == 0 || Record.W == 0 || Record.H == 0
				|| !CheckedAdd(Record.X, Record.W, InteriorRight)
				|| !CheckedAdd(Record.Y, Record.H, InteriorBottom)
				|| InteriorRight >= LightmapWidth || InteriorBottom >= LightmapHeight
				|| !CheckedAdd(Record.W, 2, PaddedWidth)
				|| !CheckedAdd(Record.H, 2, PaddedHeight)
				|| !CheckedMultiply(PaddedWidth, PaddedHeight, PaddedArea)
				|| !CheckedAdd(TotalPaddedArea, PaddedArea, TotalPaddedArea)
				|| TotalPaddedArea > LightmapPixels)
			{
				OutError = TEXT("invalid BSP animation surface rectangle or gutter");
				return false;
			}
			const FEntryRowInterval Interval = {
				Record.X - 1, static_cast<uint32>(InteriorRight + 1)};
			for (uint64 Row = Record.Y - 1; Row < InteriorBottom + 1; ++Row)
				RowIntervals[static_cast<int32>(Row)].Add(Interval);
		}
		for (TArray<FEntryRowInterval>& Intervals : RowIntervals)
		{
			Intervals.Sort([](const FEntryRowInterval& Left, const FEntryRowInterval& Right)
			{
				return Left.Begin == Right.Begin
					? Left.End < Right.End : Left.Begin < Right.Begin;
			});
			for (int32 Index = 1; Index < Intervals.Num(); ++Index)
			{
				if (Intervals[Index].Begin < Intervals[Index - 1].End)
				{
					OutError = TEXT("overlapping BSP animation atlas allocations");
					return false;
				}
			}
		}
		uint32 GroupCount = 0;
		if (!ReadU32(Bytes, Offset, GroupCount) || GroupCount > MaxAnimationGroups)
		{
			OutError = TEXT("invalid BSP animation group table");
			return false;
		}
		TArray<FWoTBSPAnimationGroup> Groups;
		Groups.Reserve(static_cast<int32>(GroupCount));
		for (uint32 Group = 0; Group < GroupCount; ++Group)
		{
			FWoTBSPAnimationGroup& Record = Groups.AddDefaulted_GetRef();
			uint8 Reserved = 0;
			if (!ReadU8(Bytes, Offset, Record.Type) || !ReadU8(Bytes, Offset, Record.Period)
				|| !ReadU8(Bytes, Offset, Record.Phase) || !ReadU8(Bytes, Offset, Reserved)
				|| !ReadU32(Bytes, Offset, Record.LightCount)
				|| Reserved != 0
				|| (Record.Type != 2 && Record.Type != 3 && Record.Type != 7)
				|| Record.LightCount == 0)
			{
				OutError = TEXT("invalid BSP animation group row");
				return false;
			}
			if (Group > 0)
			{
				const FWoTBSPAnimationGroup& Previous = Groups[static_cast<int32>(Group) - 1];
				const bool bAscending = Previous.Type < Record.Type
					|| (Previous.Type == Record.Type
						&& (Previous.Period < Record.Period
							|| (Previous.Period == Record.Period
								&& Previous.Phase < Record.Phase)));
				if (!bAscending)
				{
					OutError = TEXT("BSP animation groups are not sorted and unique");
					return false;
				}
			}
		}
		uint32 TermCount = 0;
		if (!ReadU32(Bytes, Offset, TermCount) || TermCount == 0
			|| TermCount > MaxAnimationTerms)
		{
			OutError = TEXT("invalid BSP animation term table");
			return false;
		}
		TArray<FWoTBSPAnimationTerm> Terms;
		Terms.Reserve(static_cast<int32>(TermCount));
		uint64 ResponseBytes = 0;
		uint32 WaveformTerms = 0;
		for (uint32 TermIndex = 0; TermIndex < TermCount; ++TermIndex)
		{
			FWoTBSPAnimationTerm& Term = Terms.AddDefaulted_GetRef();
			uint8 Reserved = 0;
			uint32 RectCount = 0;
			if (!ReadU8(Bytes, Offset, Term.Kind) || !ReadU8(Bytes, Offset, Term.Group)
				|| !ReadU8(Bytes, Offset, Term.Effect) || !ReadU8(Bytes, Offset, Reserved)
				|| !ReadU32(Bytes, Offset, Term.LightCount)
				|| !ReadU32(Bytes, Offset, RectCount)
				|| Reserved != 0 || Term.Kind > 2 || Term.LightCount == 0
				|| RectCount > SurfaceCount
				|| (Term.Kind == 0 && (Term.Group >= GroupCount || Term.Effect != 0))
				|| (Term.Kind == 1 && (Term.Group != 255
					|| Term.Effect < 1 || Term.Effect > 3))
				|| (Term.Kind == 2 && (Term.Group >= GroupCount
					|| Term.Effect < 1 || Term.Effect > 3)))
			{
				OutError = TEXT("invalid BSP animation term row");
				return false;
			}
			// Material slot order: waveform terms in group order, then effects
			// by class, then intersections by (group, effect).
			if (TermIndex > 0)
			{
				const FWoTBSPAnimationTerm& Previous = Terms[static_cast<int32>(TermIndex) - 1];
				const bool bOrdered = Previous.Kind < Term.Kind
					|| (Previous.Kind == Term.Kind
						&& (Term.Kind == 1 ? Previous.Effect < Term.Effect
							: (Previous.Group < Term.Group
								|| (Previous.Group == Term.Group
									&& Previous.Effect < Term.Effect))));
				if (!bOrdered)
				{
					OutError = TEXT("BSP animation terms are not in material slot order");
					return false;
				}
			}
			if (Term.Kind == 0)
			{
				if (Term.Group != WaveformTerms)
				{
					OutError = TEXT("BSP animation waveform slots do not match the group table");
					return false;
				}
				++WaveformTerms;
			}
			Term.Rects.Reserve(static_cast<int32>(RectCount));
			uint32 PreviousSurface = 0;
			for (uint32 RectIndex = 0; RectIndex < RectCount; ++RectIndex)
			{
				FWoTBSPAnimationRect& Rect = Term.Rects.AddDefaulted_GetRef();
				if (!ReadU32(Bytes, Offset, Rect.SurfaceIndex)
					|| Rect.SurfaceIndex >= SurfaceCount
					|| (RectIndex > 0 && Rect.SurfaceIndex <= PreviousSurface))
				{
					OutError = TEXT("invalid BSP animation rect surface");
					return false;
				}
				const FWoTBSPAnimationSurface& Placement =
					Surfaces[static_cast<int32>(Rect.SurfaceIndex)];
				uint64 ByteCount = 0;
				if (Placement.W == 0 || Placement.H == 0
					|| !CheckedMultiply(Placement.W, Placement.H, ByteCount)
					|| !CheckedMultiply(ByteCount, 3, ByteCount)
					|| ByteCount > TrailerEnd - Offset
					|| !CheckedAdd(ResponseBytes, ByteCount, ResponseBytes)
					|| ResponseBytes > MaxAnimationBytes)
				{
					OutError = TEXT("invalid BSP animation rect bytes");
					return false;
				}
				Rect.RGB.Append(Bytes.GetData() + static_cast<int32>(Offset),
					static_cast<int32>(ByteCount));
				Offset += ByteCount;
				PreviousSurface = Rect.SurfaceIndex;
			}
		}
		if (WaveformTerms != GroupCount)
		{
			OutError = TEXT("BSP animation group table has no waveform term");
			return false;
		}
		if (Offset != TrailerEnd)
		{
			OutError = TEXT("BSP animation trailer contains extra bytes");
			return false;
		}
		Out.AnimationSurfaces = MoveTemp(Surfaces);
		Out.AnimationGroups = MoveTemp(Groups);
		Out.AnimationTerms = MoveTemp(Terms);
		return true;
	}

	bool DecodeTriangleSurfaceTrailer(TConstArrayView<uint8> Bytes,
		uint64 TrailerStart, uint64 TrailerEnd, uint32 ExpectedTriangleCount,
		FWoTBSPPayloadData& Out, FString& OutError)
	{
		uint64 Offset = TrailerStart;
		uint32 ByteCount = 0, TriangleCount = 0, SurfaceCount = 0;
		uint64 ExpectedBytes = 0;
		if (TrailerStart > TrailerEnd || TrailerEnd > static_cast<uint64>(Bytes.Num())
			|| !ReadU32(Bytes, Offset, ByteCount)
			|| !ReadU32(Bytes, Offset, TriangleCount)
			|| !ReadU32(Bytes, Offset, SurfaceCount)
			|| TriangleCount != ExpectedTriangleCount
			|| SurfaceCount == 0 || SurfaceCount > MaxEntrySurfaces
			|| !CheckedMultiply(TriangleCount, 4, ExpectedBytes)
			|| !CheckedAdd(ExpectedBytes, 12, ExpectedBytes)
			|| ByteCount != ExpectedBytes
			|| ByteCount != TrailerEnd - TrailerStart)
		{
			OutError = TEXT("invalid BSP triangle-surface trailer header");
			return false;
		}
		Out.TriangleSurfaces.SetNumUninitialized(static_cast<int32>(TriangleCount));
		for (uint32& SurfaceIndex : Out.TriangleSurfaces)
		{
			if (!ReadU32(Bytes, Offset, SurfaceIndex) || SurfaceIndex >= SurfaceCount)
			{
				OutError = TEXT("invalid BSP triangle surface index");
				return false;
			}
		}
		if (Offset != TrailerEnd)
		{
			OutError = TEXT("BSP triangle-surface trailer contains extra bytes");
			return false;
		}
		return true;
	}
}

bool UWoTBSPPayloadAssetUserData::ValidateDescriptor(
	const UStaticMesh* Mesh, FString& OutError) const
{
	const int32 SectionCount = MaterialSlots.Num();
	if (!Mesh || PayloadKey.IsEmpty()
		|| (Role != TEXT("main") && Role != TEXT("portal"))
		|| PayloadFormatVersion != 1
		|| TangentAlgorithm != TEXT("ue-mikk-preserve-normals-v1")
		|| VertexCount <= 0 || VertexCount > static_cast<int32>(MaxVertices)
		|| IndexCount < 3 || IndexCount > static_cast<int32>(MaxIndices)
		|| IndexCount % 3 != 0 || SectionCount <= 0
		|| SectionCount > static_cast<int32>(MaxSections)
		|| Mesh->GetStaticMaterials().Num() != SectionCount
		|| !bSyntheticCarrier || BoundsMin.ContainsNaN() || BoundsMax.ContainsNaN()
		|| BoundsMin.X > BoundsMax.X || BoundsMin.Y > BoundsMax.Y
		|| BoundsMin.Z > BoundsMax.Z || BoundsMin.GetAbsMax() > MaxCoordinateAbs
		|| BoundsMax.GetAbsMax() > MaxCoordinateAbs)
	{
		OutError = TEXT("invalid BSP carrier descriptor header/counts");
		return false;
	}
	if (SectionFirstIndices.Num() != SectionCount
		|| SectionIndexCounts.Num() != SectionCount
		|| SectionMinVertices.Num() != SectionCount
		|| SectionMaxVertices.Num() != SectionCount
		|| SectionCollisionEnabled.Num() != SectionCount)
	{
		OutError = TEXT("BSP carrier section arrays are not equally dense");
		return false;
	}
	int32 ExpectedFirst = 0;
	TSet<FString> SeenSlots;
	for (int32 Section = 0; Section < SectionCount; ++Section)
	{
		if (!MaterialSlots.IsValidIndex(Section)
			|| !Mesh->GetStaticMaterials().IsValidIndex(Section)
			|| !SectionFirstIndices.IsValidIndex(Section)
			|| !SectionIndexCounts.IsValidIndex(Section)
			|| !SectionMinVertices.IsValidIndex(Section)
			|| !SectionMaxVertices.IsValidIndex(Section)
			|| !SectionCollisionEnabled.IsValidIndex(Section))
		{
			OutError = FString::Printf(
				TEXT("BSP carrier section %d is missing parallel metadata"), Section);
			return false;
		}
		const FString Slot = MaterialSlots[Section];
		if (Slot.IsEmpty() || SeenSlots.Contains(Slot)
			|| Mesh->GetStaticMaterials()[Section].MaterialSlotName.ToString() != Slot
			|| SectionFirstIndices[Section] != ExpectedFirst
			|| SectionIndexCounts[Section] <= 0 || SectionIndexCounts[Section] % 3
			|| ExpectedFirst > IndexCount
			|| SectionIndexCounts[Section] > IndexCount - ExpectedFirst
			|| SectionMinVertices[Section] < 0
			|| SectionMaxVertices[Section] < SectionMinVertices[Section]
			|| SectionMaxVertices[Section] >= VertexCount
			|| SectionCollisionEnabled[Section] != (Role == TEXT("main")))
		{
			OutError = FString::Printf(TEXT("invalid BSP carrier section %d"), Section);
			return false;
		}
		SeenSlots.Add(Slot);
		ExpectedFirst += SectionIndexCounts[Section];
	}
	if (ExpectedFirst != IndexCount)
	{
		OutError = TEXT("BSP carrier sections do not cover the payload indices");
		return false;
	}
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() != 1)
	{
		OutError = TEXT("carrier is not the expected tiny one-LOD synthetic mesh");
		return false;
	}
	if (!RenderData->LODResources.IsValidIndex(0))
	{
		OutError = TEXT("carrier LOD0 is absent");
		return false;
	}
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	if (LOD.GetNumVertices() != static_cast<uint32>(SectionCount * 3)
		|| LOD.IndexBuffer.GetNumIndices() != static_cast<uint32>(SectionCount * 3)
		|| LOD.Sections.Num() != SectionCount
		|| RenderData->Bounds.BoxExtent.GetMax() > 1.0
		|| RenderData->Bounds.Origin.GetAbsMax() > 1.0)
	{
		OutError = TEXT("carrier contains geometry outside the tiny proof contract");
		return false;
	}
	for (int32 Section = 0; Section < SectionCount; ++Section)
	{
		if (!LOD.Sections.IsValidIndex(Section))
		{
			OutError = FString::Printf(TEXT("carrier section %d is absent"), Section);
			return false;
		}
		if (LOD.Sections[Section].MaterialIndex != Section
			|| LOD.Sections[Section].NumTriangles != 1)
		{
			OutError = TEXT("carrier sections are not one synthetic triangle per slot");
			return false;
		}
	}
	OutError.Reset();
	return true;
}

FString UWoTBSPPayloadLibrary::StampCarrier(
	UStaticMesh* Mesh, const FString& PayloadKey, const FString& Role,
	int32 VertexCount, int32 IndexCount, FVector BoundsMin, FVector BoundsMax,
	const FString& TangentAlgorithm, const TArray<FString>& MaterialSlots,
	const TArray<int32>& SectionFirstIndices,
	const TArray<int32>& SectionIndexCounts,
	const TArray<int32>& SectionMinVertices,
	const TArray<int32>& SectionMaxVertices,
	const TArray<bool>& SectionCollisionEnabled)
{
#if WITH_EDITOR
	if (!Mesh) return TEXT("null static mesh carrier");
	UWoTBSPPayloadAssetUserData* Binding =
		NewObject<UWoTBSPPayloadAssetUserData>(Mesh);
	Binding->PayloadKey = PayloadKey;
	Binding->Role = Role;
	Binding->VertexCount = VertexCount;
	Binding->IndexCount = IndexCount;
	Binding->BoundsMin = BoundsMin;
	Binding->BoundsMax = BoundsMax;
	Binding->TangentAlgorithm = TangentAlgorithm;
	Binding->MaterialSlots = MaterialSlots;
	Binding->SectionFirstIndices = SectionFirstIndices;
	Binding->SectionIndexCounts = SectionIndexCounts;
	Binding->SectionMinVertices = SectionMinVertices;
	Binding->SectionMaxVertices = SectionMaxVertices;
	Binding->SectionCollisionEnabled = SectionCollisionEnabled;
	FString Error;
	if (!Binding->ValidateDescriptor(Mesh, Error)) return Error;
	Mesh->RemoveUserDataOfClass(UWoTBSPPayloadAssetUserData::StaticClass());
	Mesh->AddAssetUserData(Binding);
	Mesh->bAllowCPUAccess = true;
	Mesh->NeverStream = true;
	Mesh->MarkPackageDirty();
	return FString();
#else
	return TEXT("BSP carrier stamping is editor-only");
#endif
}

FString UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
	const FString& PayloadSHA256, const FString& EngineBuildIdentity,
	const FString& PlatformName, int32 DerivedFormatVersion,
	const FString& TangentAlgorithm)
{
	if (!IsSHA256(PayloadSHA256) || EngineBuildIdentity.IsEmpty()
		|| PlatformName.IsEmpty() || DerivedFormatVersion != 1
		|| TangentAlgorithm != TEXT("ue-mikk-preserve-normals-v1")) return FString();
	TArray<uint8> Input;
	static constexpr ANSICHAR Domain[] = "WOT-BSP-DERIVED-KEY";
	Input.Append(reinterpret_cast<const uint8*>(Domain), sizeof(Domain));
	if (!AppendFramedUTF8(LexToString(DerivedFormatVersion), Input)
		|| !AppendFramedUTF8(PayloadSHA256, Input)
		|| !AppendFramedUTF8(EngineBuildIdentity, Input)
		|| !AppendFramedUTF8(PlatformName, Input)
		|| !AppendFramedUTF8(TangentAlgorithm, Input)
		|| static_cast<uint64>(Input.Num()) > MAX_uint32) return FString();
	return WoTSha256::HashBuffer(Input.GetData(), Input.Num());
}

bool UWoTBSPPayloadLibrary::DecodePayload(
	TConstArrayView<uint8> Bytes, FWoTBSPPayloadData& OutData, FString& OutError)
{
	OutData = FWoTBSPPayloadData();
	if (Bytes.Num() < static_cast<int32>(HeaderBytes)
		|| static_cast<uint64>(Bytes.Num()) > MaxPayloadBytes
		|| !Bytes.GetData())
	{
		OutError = TEXT("truncated or oversized BSP payload");
		return false;
	}
	static constexpr uint8 Magic[8] = {'W','O','T','P','B','S','P',0};
	if (FMemory::Memcmp(Bytes.GetData(), Magic, 8) != 0)
	{
		OutError = TEXT("invalid BSP payload magic");
		return false;
	}
	uint64 Offset = 8;
	uint16 Version = 0, Flags = 0, UVChannels = 0, Tangent = 0;
	uint32 VertexCount = 0, IndexCount = 0, SectionCount = 0;
	float SerializedBounds[6] = {};
	if (!ReadU16(Bytes, Offset, Version) || !ReadU16(Bytes, Offset, Flags)
		|| !ReadU32(Bytes, Offset, VertexCount) || !ReadU32(Bytes, Offset, IndexCount)
		|| !ReadU32(Bytes, Offset, SectionCount) || !ReadU16(Bytes, Offset, UVChannels)
		|| !ReadU16(Bytes, Offset, Tangent))
	{
		OutError = TEXT("truncated BSP payload header");
		return false;
	}
	for (float& Value : SerializedBounds)
	{
		if (!ReadFloat(Bytes, Offset, Value))
		{
			OutError = TEXT("invalid BSP bounds");
			return false;
		}
	}
	// Version 10 is the sole current multiplayer format. It retains v9 exact
	// triangle provenance and extends macro records with PanU/PanV terms.
	// Superseded production versions 7-9 are deliberately rejected.
	const bool bVersion10 = Version == 10;
	const bool bVersion4 = (Version >= 4 && Version <= 6) || bVersion10;
	const bool bVersion5 = Version == 5 || Version == 6 || bVersion10;
	const bool bVersion6 = Version == 6 || bVersion10;
	const bool bHasMacroRecord = bVersion10;
	if ((Version != 1 && Version != 2 && Version != 3 && !bVersion4)
		|| (Flags & ~(bVersion10 ? 127u : (bVersion6 ? 63u
			: (bVersion5 ? 31u : (bVersion4 ? 15u : 3u))))) != 0
		|| (Flags & 2) == 0
		|| (bVersion10 && (Flags & 64) == 0)
		|| (bVersion4 && (Flags & 8) != 0 && (Flags & 4) == 0)
		|| (bVersion5 && (Flags & 16) != 0 && (Flags & 4) == 0)
		|| (bVersion6 && (Flags & 32) != 0 && (Flags & 4) == 0)
		|| VertexCount == 0 || VertexCount > MaxVertices
		|| IndexCount < 3 || IndexCount > MaxIndices || IndexCount % 3
		|| SectionCount == 0 || SectionCount > MaxSections
		|| UVChannels != 2 || Tangent != 1)
	{
		OutError = TEXT("invalid BSP payload header contract");
		return false;
	}
	FWoTBSPPayloadData Decoded;
	Decoded.FormatVersion = Version;
	Decoded.bPortal = (Flags & 1) != 0;
	Decoded.BoundsMin = FVector3f(
		SerializedBounds[0], SerializedBounds[1], SerializedBounds[2]);
	Decoded.BoundsMax = FVector3f(
		SerializedBounds[3], SerializedBounds[4], SerializedBounds[5]);
	if (Decoded.BoundsMin.X > Decoded.BoundsMax.X
		|| Decoded.BoundsMin.Y > Decoded.BoundsMax.Y
		|| Decoded.BoundsMin.Z > Decoded.BoundsMax.Z
		|| Decoded.BoundsMin.GetAbsMax() > MaxCoordinateAbs
		|| Decoded.BoundsMax.GetAbsMax() > MaxCoordinateAbs)
	{
		OutError = TEXT("invalid BSP bounds ordering");
		return false;
	}
	uint32 ExpectedFirst = 0;
	TSet<FString> SeenSlots;
	for (uint32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
	{
		uint16 NameBytes = 0, Reserved = 0;
		uint32 First = 0, Count = 0, Minimum = 0, Maximum = 0;
		if (!ReadU16(Bytes, Offset, NameBytes) || !ReadU16(Bytes, Offset, Reserved)
			|| !ReadU32(Bytes, Offset, First) || !ReadU32(Bytes, Offset, Count)
			|| !ReadU32(Bytes, Offset, Minimum) || !ReadU32(Bytes, Offset, Maximum))
		{
			OutError = TEXT("truncated BSP section header");
			return false;
		}
		uint64 CollisionEnd = 0;
		if (!CheckedAdd(Offset, 4, CollisionEnd)
			|| CollisionEnd > static_cast<uint64>(Bytes.Num()))
		{
			OutError = TEXT("truncated BSP section flags");
			return false;
		}
		const int32 FlagsOffset = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(FlagsOffset)
			|| !Bytes.IsValidIndex(FlagsOffset + 1)
			|| !Bytes.IsValidIndex(FlagsOffset + 2)
			|| !Bytes.IsValidIndex(FlagsOffset + 3))
		{
			OutError = TEXT("truncated BSP section flags");
			return false;
		}
		const uint8 Collision = Bytes[FlagsOffset];
		const bool bReservedBytesZero = Bytes[FlagsOffset + 1] == 0
			&& Bytes[FlagsOffset + 2] == 0 && Bytes[FlagsOffset + 3] == 0;
		Offset = CollisionEnd;
		uint64 NameEnd = 0;
		if (Reserved != 0 || NameBytes == 0 || NameBytes > MaxSlotBytes
			|| First != ExpectedFirst || First > IndexCount || Count == 0 || Count % 3
			|| Count > IndexCount - First || Minimum > Maximum || Maximum >= VertexCount
			|| Collision > 1 || static_cast<bool>(Collision) == Decoded.bPortal
			|| !bReservedBytesZero || !CheckedAdd(Offset, NameBytes, NameEnd)
			|| NameEnd > static_cast<uint64>(Bytes.Num()))
		{
			OutError = TEXT("invalid BSP section range/flags");
			return false;
		}
		const int32 NameOffset = static_cast<int32>(Offset);
		if (!Bytes.IsValidIndex(NameOffset)
			|| !Bytes.IsValidIndex(static_cast<int32>(NameEnd - 1)))
		{
			OutError = TEXT("truncated BSP material slot bytes");
			return false;
		}
		bool bEmbeddedNull = false;
		for (uint16 NameByte = 0; NameByte < NameBytes; ++NameByte)
		{
			if (Bytes[NameOffset + NameByte] == 0)
			{
				bEmbeddedNull = true;
				break;
			}
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + NameOffset), NameBytes);
		FString Slot(Converted.Length(), Converted.Get());
		const FTCHARToUTF8 RoundTrip(*Slot);
		if (Slot.IsEmpty() || SeenSlots.Contains(Slot)
			|| bEmbeddedNull
			|| Converted.Length() == 0 || RoundTrip.Length() != NameBytes
			|| FMemory::Memcmp(RoundTrip.Get(), Bytes.GetData() + NameOffset,
				NameBytes) != 0)
		{
			OutError = TEXT("invalid BSP material slot UTF-8");
			return false;
		}
		SeenSlots.Add(Slot);
		for (TCHAR Character : Slot)
		{
			if (Character < 32 || Character == 127)
			{
				OutError = TEXT("unsafe BSP material slot");
				return false;
			}
		}
		FWoTBSPPayloadSection& Section = Decoded.Sections.AddDefaulted_GetRef();
		Section.MaterialSlot = MoveTemp(Slot);
		Section.FirstIndex = First;
		Section.IndexCount = Count;
		Section.MinVertex = Minimum;
		Section.MaxVertex = Maximum;
		Section.bCollisionEnabled = Collision != 0;
		ExpectedFirst += Count;
		Offset = NameEnd;
		if (bVersion4)
		{
			uint16 PackageBytes = 0, TextureBytes = 0;
			uint16 SourceBytes = 0, PanZoneBytes = 0;
			if (!ReadU16(Bytes, Offset, PackageBytes)
				|| !ReadU16(Bytes, Offset, TextureBytes)
				|| !ReadU16(Bytes, Offset, SourceBytes)
				|| !ReadU16(Bytes, Offset, PanZoneBytes)
				|| !ReadU32(Bytes, Offset, Section.SurfaceFlags)
				|| !ReadSectionIdentity(Bytes, Offset, PackageBytes,
					Section.CanonicalPackage, true)
				|| !ReadSectionIdentity(Bytes, Offset, TextureBytes,
					Section.CanonicalName, true)
				|| !ReadSectionIdentity(Bytes, Offset, SourceBytes,
					Section.SourceTextureIdentity, true)
				|| !ReadSectionIdentity(Bytes, Offset, PanZoneBytes,
					Section.PanZoneActorIdentity, true)
				|| (Section.MaterialSlot != TEXT("__None")
					&& (Section.CanonicalPackage.IsEmpty()
						|| Section.CanonicalName.IsEmpty()
						|| Section.SourceTextureIdentity.IsEmpty())))
			{
				OutError = TEXT("invalid BSP section identity record");
				return false;
			}
		}
		if (bHasMacroRecord)
		{
			uint16 PackageBytes = 0, TextureBytes = 0;
			uint16 SourceBytes = 0, MacroReserved = 0;
			if (!ReadU16(Bytes, Offset, PackageBytes)
				|| !ReadU16(Bytes, Offset, TextureBytes)
				|| !ReadU16(Bytes, Offset, SourceBytes)
				|| !ReadU16(Bytes, Offset, MacroReserved)
				|| !ReadFloat(Bytes, Offset, Section.MacroRatio)
				|| !ReadFloat(Bytes, Offset, Section.MacroOffsetU)
				|| !ReadFloat(Bytes, Offset, Section.MacroOffsetV)
				|| MacroReserved != 0
				|| !ReadSectionIdentity(Bytes, Offset, PackageBytes,
					Section.MacroCanonicalPackage, true)
				|| !ReadSectionIdentity(Bytes, Offset, TextureBytes,
					Section.MacroCanonicalName, true)
				|| !ReadSectionIdentity(Bytes, Offset, SourceBytes,
					Section.MacroSourceTextureIdentity, true))
			{
				OutError = TEXT("invalid BSP section macro identity record");
				return false;
			}
			const bool bHasMacro = !Section.MacroCanonicalPackage.IsEmpty()
				|| !Section.MacroCanonicalName.IsEmpty()
				|| !Section.MacroSourceTextureIdentity.IsEmpty()
				|| Section.MacroRatio != 0.0f
				|| Section.MacroOffsetU != 0.0f
				|| Section.MacroOffsetV != 0.0f;
			if (bHasMacro && (Section.MacroCanonicalPackage.IsEmpty()
				|| Section.MacroCanonicalName.IsEmpty()
				|| Section.MacroSourceTextureIdentity.IsEmpty()
				|| !FMath::IsFinite(Section.MacroRatio)
				|| !FMath::IsFinite(Section.MacroOffsetU)
				|| !FMath::IsFinite(Section.MacroOffsetV)
				|| Section.MacroRatio <= 0.0f))
			{
				OutError = TEXT("incomplete BSP section macro identity");
				return false;
			}
		}
	}
	uint64 FloatCount = 0, FloatBytes = 0, IndexBytes = 0, StreamsEnd = 0;
	if (ExpectedFirst != IndexCount
		|| !CheckedMultiply(VertexCount, 10, FloatCount)
		|| !CheckedMultiply(FloatCount, 4, FloatBytes)
		|| !CheckedMultiply(IndexCount, 4, IndexBytes)
		|| !CheckedAdd(Offset, FloatBytes, StreamsEnd)
		|| !CheckedAdd(StreamsEnd, IndexBytes, StreamsEnd)
		|| StreamsEnd > static_cast<uint64>(Bytes.Num()))
	{
		OutError = TEXT("BSP stream sizes disagree with header");
		return false;
	}
	uint32 LightmapWidth = 0, LightmapHeight = 0, LightmapByteCount = 0;
	uint64 LightmapDataOffset = 0;
	uint64 LightmapEnd = StreamsEnd;
	FEntryTrailerLayout EntryLayout;
	const bool bHasLightmap = Version == 2 || Version == 3
		|| (bVersion4 && (Flags & 4) != 0);
	const bool bHasEntryTrailer = Version == 3
		|| (bVersion4 && (Flags & 8) != 0);
	const bool bHasAnimationTrailer = bVersion5 && (Flags & 16) != 0;
	const bool bHasSkyRestTrailer = bVersion6 && (Flags & 32) != 0;
	const bool bHasTriangleSurfaceTrailer = bVersion10 && (Flags & 64) != 0;
	const bool bHasMapTrailer = bHasAnimationTrailer || bHasSkyRestTrailer
		|| bHasTriangleSurfaceTrailer;
	uint64 AnimationStart = 0;
	uint64 AnimationEnd = 0;
	uint64 SkyRestStart = 0;
	uint64 SkyRestEnd = 0;
	uint64 TriangleSurfaceStart = 0;
	uint64 TriangleSurfaceEnd = 0;
	uint64 EntryEnd = static_cast<uint64>(Bytes.Num());
	if (!bHasLightmap)
	{
		if (!bHasTriangleSurfaceTrailer
			&& StreamsEnd != static_cast<uint64>(Bytes.Num()))
		{
			OutError = TEXT("version-1 BSP payload contains trailing bytes");
			return false;
		}
		if (bHasTriangleSurfaceTrailer)
		{
			uint32 TrailerByteCount = 0;
			uint64 Probe = StreamsEnd;
			if (!ReadU32(Bytes, Probe, TrailerByteCount)
				|| TrailerByteCount < 12
				|| TrailerByteCount != static_cast<uint64>(Bytes.Num()) - StreamsEnd)
			{
				OutError = TEXT("invalid BSP triangle-surface trailer byte count");
				return false;
			}
			TriangleSurfaceStart = StreamsEnd;
			TriangleSurfaceEnd = static_cast<uint64>(Bytes.Num());
		}
	}
	else
	{
		uint64 LightmapHeaderOffset = StreamsEnd;
		if (!ReadU32(Bytes, LightmapHeaderOffset, LightmapWidth)
			|| !ReadU32(Bytes, LightmapHeaderOffset, LightmapHeight)
			|| !ReadU32(Bytes, LightmapHeaderOffset, LightmapByteCount))
		{
			OutError = TEXT("truncated BSP lightmap header");
			return false;
		}
		uint64 LightmapPixels = 0, ExpectedLightmapBytes = 0;
		if (LightmapWidth == 0 || LightmapWidth > MaxLightmapDimension
			|| LightmapHeight == 0 || LightmapHeight > MaxLightmapDimension
			|| !CheckedMultiply(LightmapWidth, LightmapHeight, LightmapPixels)
			|| LightmapPixels > MaxLightmapPixels
			|| (bHasEntryTrailer && LightmapPixels > MaxEntryLightmapPixels)
			|| !CheckedMultiply(LightmapPixels, 4, ExpectedLightmapBytes)
			|| ExpectedLightmapBytes != LightmapByteCount
			|| !CheckedAdd(LightmapHeaderOffset, LightmapByteCount, LightmapEnd)
			|| (!bHasEntryTrailer && !bHasMapTrailer
				&& LightmapEnd != static_cast<uint64>(Bytes.Num()))
			|| ((bHasEntryTrailer || bHasMapTrailer)
				&& LightmapEnd >= static_cast<uint64>(Bytes.Num())))
		{
			OutError = TEXT("invalid BSP lightmap dimensions or byte count");
			return false;
		}
		if (bHasMapTrailer)
		{
			// Trailers follow the atlas in flag order (entry 8, animation 16,
			// sky rest 32, triangle provenance 64), each prefixed by its own byte count; the last one
			// ends the payload. The builder never emits the entry trailer
			// together with a map trailer, but the walk admits it.
			uint64 Next = LightmapEnd;
			if (bHasEntryTrailer)
			{
				uint32 EntryByteCount = 0;
				uint64 Probe = Next;
				if (!ReadU32(Bytes, Probe, EntryByteCount)
					|| EntryByteCount < 24
					|| EntryByteCount > static_cast<uint64>(Bytes.Num()) - Next)
				{
					OutError = TEXT("invalid BSP entry trailer before a map trailer");
					return false;
				}
				Next += EntryByteCount;
				EntryEnd = Next;
			}
			if (bHasAnimationTrailer)
			{
				uint32 AnimationByteCount = 0;
				uint64 Probe = Next;
				if (!ReadU32(Bytes, Probe, AnimationByteCount)
					|| AnimationByteCount < 16
					|| AnimationByteCount > static_cast<uint64>(Bytes.Num()) - Next)
				{
					OutError = TEXT("invalid BSP animation trailer byte count");
					return false;
				}
				AnimationStart = Next;
				Next += AnimationByteCount;
				AnimationEnd = Next;
			}
			if (bHasSkyRestTrailer)
			{
				uint32 SkyRestByteCount = 0;
				uint64 Probe = Next;
				if (!ReadU32(Bytes, Probe, SkyRestByteCount)
					|| SkyRestByteCount < 24
					|| SkyRestByteCount > static_cast<uint64>(Bytes.Num()) - Next)
				{
					OutError = TEXT("invalid BSP sky-rest trailer byte count");
					return false;
				}
				SkyRestStart = Next;
				Next += SkyRestByteCount;
				SkyRestEnd = Next;
			}
			if (bHasTriangleSurfaceTrailer)
			{
				uint32 TrailerByteCount = 0;
				uint64 Probe = Next;
				if (!ReadU32(Bytes, Probe, TrailerByteCount)
					|| TrailerByteCount < 12
					|| TrailerByteCount > static_cast<uint64>(Bytes.Num()) - Next)
				{
					OutError = TEXT("invalid BSP triangle-surface trailer byte count");
					return false;
				}
				TriangleSurfaceStart = Next;
				Next += TrailerByteCount;
				TriangleSurfaceEnd = Next;
			}
			if (Next != static_cast<uint64>(Bytes.Num()))
			{
				OutError = TEXT("BSP trailers do not account for the payload");
				return false;
			}
		}
		LightmapDataOffset = LightmapHeaderOffset;
		for (uint64 Alpha = LightmapDataOffset + 3;
			Alpha < LightmapEnd; Alpha += 4)
		{
			if (Bytes[static_cast<int32>(Alpha)] != 255)
			{
				OutError = TEXT("BSP lightmap contains non-opaque texels");
				return false;
			}
		}
		if (bHasEntryTrailer && !PreflightEntryTrailer(
			bHasMapTrailer ? Bytes.Left(static_cast<int32>(EntryEnd)) : Bytes,
			LightmapEnd, IndexCount, LightmapWidth, LightmapHeight, EntryLayout, OutError))
		{
			return false;
		}
	}
	Decoded.Positions.SetNumUninitialized(VertexCount);
	Decoded.Normals.SetNumUninitialized(VertexCount);
	Decoded.UV0.SetNumUninitialized(VertexCount);
	Decoded.UV1.SetNumUninitialized(VertexCount);
	for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		const int32 VertexIndex = static_cast<int32>(Vertex);
		if (!Decoded.Positions.IsValidIndex(VertexIndex))
		{
			OutError = TEXT("BSP position allocation is shorter than declared"); return false;
		}
		if (!ReadFloat(Bytes, Offset, Decoded.Positions[VertexIndex].X)
			|| !ReadFloat(Bytes, Offset, Decoded.Positions[VertexIndex].Y)
			|| !ReadFloat(Bytes, Offset, Decoded.Positions[VertexIndex].Z)
			|| Decoded.Positions[VertexIndex].GetAbsMax() > MaxCoordinateAbs)
		{
			OutError = TEXT("invalid BSP position stream"); return false;
		}
	}
	for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		const int32 VertexIndex = static_cast<int32>(Vertex);
		if (!Decoded.Normals.IsValidIndex(VertexIndex))
		{
			OutError = TEXT("BSP normal allocation is shorter than declared"); return false;
		}
		if (!ReadFloat(Bytes, Offset, Decoded.Normals[VertexIndex].X)
			|| !ReadFloat(Bytes, Offset, Decoded.Normals[VertexIndex].Y)
			|| !ReadFloat(Bytes, Offset, Decoded.Normals[VertexIndex].Z)
			|| !FMath::IsNearlyEqual(Decoded.Normals[VertexIndex].SquaredLength(), 1.0f, 0.02f))
		{
			OutError = TEXT("invalid BSP normal stream"); return false;
		}
	}
	for (TArray<FVector2f>* UVs : {&Decoded.UV0, &Decoded.UV1})
	{
		for (uint32 Vertex = 0; Vertex < VertexCount; ++Vertex)
		{
			const int32 VertexIndex = static_cast<int32>(Vertex);
			if (!UVs || !UVs->IsValidIndex(VertexIndex))
			{
				OutError = TEXT("BSP UV allocation is shorter than declared"); return false;
			}
			if (!ReadFloat(Bytes, Offset, (*UVs)[VertexIndex].X)
				|| !ReadFloat(Bytes, Offset, (*UVs)[VertexIndex].Y))
			{
				OutError = TEXT("invalid BSP UV stream"); return false;
			}
		}
	}
	Decoded.Indices.SetNumUninitialized(IndexCount);
	for (uint32& Index : Decoded.Indices)
	{
		if (!ReadU32(Bytes, Offset, Index) || Index >= VertexCount)
		{
			OutError = TEXT("invalid BSP index stream"); return false;
		}
	}
	for (int32 SectionIndex = 0; SectionIndex < Decoded.Sections.Num(); ++SectionIndex)
	{
		if (!Decoded.Sections.IsValidIndex(SectionIndex))
		{
			OutError = TEXT("BSP section array is shorter than declared"); return false;
		}
		const FWoTBSPPayloadSection& Section = Decoded.Sections[SectionIndex];
		uint64 SectionEnd = 0;
		if (!CheckedAdd(Section.FirstIndex, Section.IndexCount, SectionEnd)
			|| SectionEnd > static_cast<uint64>(Decoded.Indices.Num()))
		{
			OutError = FString::Printf(
				TEXT("BSP section %d index range is outside the decoded stream"), SectionIndex);
			return false;
		}
		uint32 Minimum = MAX_uint32, Maximum = 0;
		for (uint64 At = Section.FirstIndex; At < SectionEnd; ++At)
		{
			const int32 IndexAt = static_cast<int32>(At);
			if (!Decoded.Indices.IsValidIndex(IndexAt))
			{
				OutError = FString::Printf(
					TEXT("BSP section %d references missing index %llu"), SectionIndex,
					static_cast<unsigned long long>(At));
				return false;
			}
			Minimum = FMath::Min(Minimum, Decoded.Indices[IndexAt]);
			Maximum = FMath::Max(Maximum, Decoded.Indices[IndexAt]);
		}
		if (Minimum != Section.MinVertex || Maximum != Section.MaxVertex)
		{
			OutError = TEXT("BSP section min/max disagrees with indices");
			return false;
		}
	}
	for (uint32 Triangle = 0; Triangle < IndexCount; Triangle += 3)
	{
		const int32 TriangleIndex = static_cast<int32>(Triangle);
		if (!Decoded.Indices.IsValidIndex(TriangleIndex)
			|| !Decoded.Indices.IsValidIndex(TriangleIndex + 1)
			|| !Decoded.Indices.IsValidIndex(TriangleIndex + 2))
		{
			OutError = FString::Printf(
				TEXT("BSP triangle %u is truncated in the decoded index stream"), Triangle / 3);
			return false;
		}
		const uint32 A = Decoded.Indices[TriangleIndex];
		const uint32 B = Decoded.Indices[TriangleIndex + 1];
		const uint32 C = Decoded.Indices[TriangleIndex + 2];
		if (!Decoded.Positions.IsValidIndex(static_cast<int32>(A))
			|| !Decoded.Positions.IsValidIndex(static_cast<int32>(B))
			|| !Decoded.Positions.IsValidIndex(static_cast<int32>(C)))
		{
			OutError = FString::Printf(
				TEXT("BSP triangle %u references a missing position"), Triangle / 3);
			return false;
		}
		// Repeated INDICES stay fail-closed; zero-area (collinear) triangles
		// with distinct indices are LEGITIMATE retail geometry — bsp2obj's
		// sliver removal misses float-rounding-collinear ones (Battle2_02's
		// main payload carries one), the cooked reference ships them, the
		// rebuild path sanitizes their basis, and Chaos skips them. The
		// python builder applies the same rule (bsp_payload.py).
		if (A == B || A == C || B == C)
		{
			OutError = TEXT("BSP payload contains a degenerate triangle");
			return false;
		}
	}
	FBox3f ComputedBounds(EForceInit::ForceInit);
	for (const FVector3f& Position : Decoded.Positions) ComputedBounds += Position;
	if (!ComputedBounds.Min.Equals(Decoded.BoundsMin, 0.0f)
		|| !ComputedBounds.Max.Equals(Decoded.BoundsMax, 0.0f))
	{
		OutError = TEXT("BSP declared bounds disagree with positions");
		return false;
	}
	if (bHasLightmap)
	{
		Decoded.LightmapWidth = LightmapWidth;
		Decoded.LightmapHeight = LightmapHeight;
		Decoded.LightmapBGRA.Append(
			Bytes.GetData() + static_cast<int32>(LightmapDataOffset),
			static_cast<int32>(LightmapByteCount));
	}
	if (bHasEntryTrailer
		&& !DecodeEntryTrailer(
			bHasMapTrailer ? Bytes.Left(static_cast<int32>(EntryEnd)) : Bytes,
			LightmapEnd, EntryLayout, Decoded, OutError))
	{
		if (OutError.IsEmpty())
			OutError = TEXT("failed to copy preflighted BSP entry trailer");
		return false;
	}
	if (bHasAnimationTrailer
		&& !DecodeAnimationTrailer(Bytes, AnimationStart,
			AnimationEnd, LightmapWidth, LightmapHeight,
			Decoded, OutError))
	{
		if (OutError.IsEmpty())
			OutError = TEXT("failed to decode the BSP animation trailer");
		return false;
	}
	if (bHasSkyRestTrailer
		&& !DecodeSkyRestTrailer(Bytes, SkyRestStart,
			SkyRestEnd, Decoded, OutError))
	{
		if (OutError.IsEmpty())
			OutError = TEXT("failed to decode the BSP sky-rest trailer");
		return false;
	}
	if (bHasTriangleSurfaceTrailer
		&& !DecodeTriangleSurfaceTrailer(Bytes, TriangleSurfaceStart,
			TriangleSurfaceEnd, IndexCount / 3, Decoded, OutError))
	{
		if (OutError.IsEmpty())
			OutError = TEXT("failed to decode the BSP triangle-surface trailer");
		return false;
	}
	OutData = MoveTemp(Decoded);
	OutError.Reset();
	return true;
}
