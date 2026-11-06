// NullRHI-safe specs for BSP binary, derived-key, and per-map barrier contracts.
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTBSPPayload.h"
#include "../WoTPayloadSubsystem.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"

namespace
{
	void AppendU16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(Value & 0xff); Bytes.Add((Value >> 8) & 0xff);
	}

	void AppendU32(TArray<uint8>& Bytes, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
			Bytes.Add(static_cast<uint8>((Value >> (Index * 8)) & 0xff));
	}

	void AppendFloat(TArray<uint8>& Bytes, float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, 4);
		AppendU32(Bytes, Bits);
	}

	bool OverwriteU32(TArray<uint8>& Bytes, int32 Offset, uint32 Value)
	{
		if (Offset < 0 || Offset > Bytes.Num() - 4 || !Bytes.GetData()) return false;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			if (!Bytes.IsValidIndex(Offset + Index)) return false;
			Bytes[Offset + Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xff);
		}
		return true;
	}

	bool OverwriteU16(TArray<uint8>& Bytes, int32 Offset, uint16 Value)
	{
		if (Offset < 0 || Offset > Bytes.Num() - 2 || !Bytes.GetData()) return false;
		Bytes[Offset] = static_cast<uint8>(Value & 0xff);
		Bytes[Offset + 1] = static_cast<uint8>((Value >> 8) & 0xff);
		return true;
	}

	TArray<uint8> FixtureWithSlot(const TArray<uint8>& Slot, bool bPortal = false)
	{
		TArray<uint8> Bytes;
		const uint8 Magic[8] = {'W','O','T','P','B','S','P',0};
		Bytes.Append(Magic, 8);
		AppendU16(Bytes, 1);
		AppendU16(Bytes, (bPortal ? 1 : 0) | 2);
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 1);
		AppendU16(Bytes, 2);
		AppendU16(Bytes, 1);
		for (float Bound : {0.f, 0.f, 0.f, 100.f, 100.f, 0.f}) AppendFloat(Bytes, Bound);
		AppendU16(Bytes, static_cast<uint16>(Slot.Num()));
		AppendU16(Bytes, 0);
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 2);
		Bytes.Add(bPortal ? 0 : 1);
		Bytes.Append({0, 0, 0});
		Bytes.Append(Slot);
		for (float Value : {0.f,0.f,0.f, 100.f,0.f,0.f, 0.f,100.f,0.f})
			AppendFloat(Bytes, Value);
		for (int32 Vertex = 0; Vertex < 3; ++Vertex)
			for (float Value : {0.f, 0.f, 1.f}) AppendFloat(Bytes, Value);
		for (float Value : {0.f,0.f, 1.f,0.f, 0.f,1.f}) AppendFloat(Bytes, Value);
		for (float Value : {0.f,0.f, .5f,0.f, 0.f,.5f}) AppendFloat(Bytes, Value);
		AppendU32(Bytes, 0); AppendU32(Bytes, 1); AppendU32(Bytes, 2);
		return Bytes;
	}

	TArray<uint8> Fixture(bool bPortal = false)
	{
		return FixtureWithSlot({
			'J', 'C', 'y', 'l', 'i', 'n', 'd', 'e', 'r', '0'
		}, bPortal);
	}

	TArray<uint8> Version4IdentityFixture()
	{
		TArray<uint8> Bytes;
		const uint8 Magic[8] = {'W','O','T','P','B','S','P',0};
		Bytes.Append(Magic, 8);
		AppendU16(Bytes, 4);
		AppendU16(Bytes, 2);
		AppendU32(Bytes, 3); AppendU32(Bytes, 3); AppendU32(Bytes, 1);
		AppendU16(Bytes, 2); AppendU16(Bytes, 1);
		for (float Bound : {0.f, 0.f, 0.f, 100.f, 100.f, 0.f})
			AppendFloat(Bytes, Bound);
		const ANSICHAR Slot[] = "WorldT__Stone";
		const ANSICHAR Package[] = "WorldT";
		const ANSICHAR Name[] = "Stone";
		const ANSICHAR Source[] = "WorldT.Detail.Stone";
		const ANSICHAR PanZone[] = "Arena_01.ZoneInfo2";
		AppendU16(Bytes, sizeof(Slot) - 1); AppendU16(Bytes, 0);
		AppendU32(Bytes, 0); AppendU32(Bytes, 3);
		AppendU32(Bytes, 0); AppendU32(Bytes, 2);
		Bytes.Append({1, 0, 0, 0});
		Bytes.Append(reinterpret_cast<const uint8*>(Slot), sizeof(Slot) - 1);
		AppendU16(Bytes, sizeof(Package) - 1);
		AppendU16(Bytes, sizeof(Name) - 1);
		AppendU16(Bytes, sizeof(Source) - 1);
		AppendU16(Bytes, sizeof(PanZone) - 1);
		AppendU32(Bytes, 0x00000602u);
		Bytes.Append(reinterpret_cast<const uint8*>(Package), sizeof(Package) - 1);
		Bytes.Append(reinterpret_cast<const uint8*>(Name), sizeof(Name) - 1);
		Bytes.Append(reinterpret_cast<const uint8*>(Source), sizeof(Source) - 1);
		Bytes.Append(reinterpret_cast<const uint8*>(PanZone), sizeof(PanZone) - 1);
		for (float Value : {0.f,0.f,0.f, 100.f,0.f,0.f, 0.f,100.f,0.f})
			AppendFloat(Bytes, Value);
		for (int32 Vertex = 0; Vertex < 3; ++Vertex)
			for (float Value : {0.f, 0.f, 1.f}) AppendFloat(Bytes, Value);
		for (float Value : {0.f,0.f, 1.f,0.f, 0.f,1.f}) AppendFloat(Bytes, Value);
		for (float Value : {0.f,0.f, .5f,0.f, 0.f,.5f}) AppendFloat(Bytes, Value);
		AppendU32(Bytes, 0); AppendU32(Bytes, 1); AppendU32(Bytes, 2);
		return Bytes;
	}

	TArray<uint8> Version10MacroFixture()
	{
		TArray<uint8> Bytes = Version4IdentityFixture();
		if (!OverwriteU16(Bytes, 8, 10) || !OverwriteU16(Bytes, 10, 66)) return {};
		const ANSICHAR Slot[] = "WorldT__Stone";
		const ANSICHAR Package[] = "WorldT";
		const ANSICHAR Name[] = "Stone";
		const ANSICHAR Source[] = "WorldT.Detail.Stone";
		const ANSICHAR PanZone[] = "Arena_01.ZoneInfo2";
		const int32 MacroOffset = 52 + 24 + sizeof(Slot) - 1 + 12
			+ sizeof(Package) - 1 + sizeof(Name) - 1 + sizeof(Source) - 1
			+ sizeof(PanZone) - 1;
		const ANSICHAR MacroPackage[] = "WOTDetail";
		const ANSICHAR MacroName[] = "MacroRock_003";
		const ANSICHAR MacroSource[] = "WOTDetail.Macro.MacroRock_003";
		TArray<uint8> Macro;
		AppendU16(Macro, sizeof(MacroPackage) - 1);
		AppendU16(Macro, sizeof(MacroName) - 1);
		AppendU16(Macro, sizeof(MacroSource) - 1);
		AppendU16(Macro, 0);
		AppendFloat(Macro, 1.0f / 6.0f);
		AppendFloat(Macro, 0.125f);
		AppendFloat(Macro, -0.25f);
		Macro.Append(reinterpret_cast<const uint8*>(MacroPackage),
			sizeof(MacroPackage) - 1);
		Macro.Append(reinterpret_cast<const uint8*>(MacroName),
			sizeof(MacroName) - 1);
		Macro.Append(reinterpret_cast<const uint8*>(MacroSource),
			sizeof(MacroSource) - 1);
		Bytes.Insert(Macro.GetData(), Macro.Num(), MacroOffset);
		AppendU32(Bytes, 16);
		AppendU32(Bytes, 1);
		AppendU32(Bytes, 1);
		AppendU32(Bytes, 0);
		return Bytes;
	}

	TArray<uint8> Version2Fixture()
	{
		TArray<uint8> Bytes = Fixture();
		if (!OverwriteU16(Bytes, 8, 2)) return {};
		AppendU32(Bytes, 2);
		AppendU32(Bytes, 2);
		AppendU32(Bytes, 16);
		Bytes.Append({
			1, 2, 3, 255,
			4, 5, 6, 255,
			7, 8, 9, 255,
			10, 11, 12, 255
		});
		return Bytes;
	}

	TArray<uint8> Version3Fixture()
	{
		TArray<uint8> Bytes = Fixture();
		if (!OverwriteU16(Bytes, 8, 3)) return {};
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 3);
		AppendU32(Bytes, 36);
		for (int32 Index = 0; Index < 9; ++Index)
			Bytes.Append({20, 40, 60, 255});
		AppendU32(Bytes, 56);
		AppendU32(Bytes, 1);
		AppendU32(Bytes, 1);
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 1);
		AppendU32(Bytes, 0);
		for (uint32 Value : {0u, 1u, 1u, 1u, 1u})
			AppendU32(Bytes, Value);
		AppendFloat(Bytes, 0);
		AppendFloat(Bytes, 0);
		return Bytes;
	}

	void AppendVersion3Surface(TArray<uint8>& Bytes, uint32 SurfaceIndex,
		uint32 X, uint32 Y, uint32 W, uint32 H)
	{
		for (uint32 Value : {SurfaceIndex, X, Y, W, H}) AppendU32(Bytes, Value);
		AppendFloat(Bytes, 0);
		AppendFloat(Bytes, 0);
	}

	void AppendVersion3LightBytes(TArray<uint8>& Bytes,
		std::initializer_list<uint8> Identity)
	{
		AppendU16(Bytes, static_cast<uint16>(Identity.size()));
		AppendU16(Bytes, 0);
		Bytes.Append(Identity);
		Bytes.Append({64, 0, 255, 0});
		AppendFloat(Bytes, 1);
	}

	void AppendVersion3Light(TArray<uint8>& Bytes, uint8 Identity)
	{
		AppendVersion3LightBytes(Bytes, {Identity});
	}

	void AppendVersion3Pair(TArray<uint8>& Bytes, uint32 LightIndex,
		uint32 SurfaceIndex, uint32 ByteCount, uint8 Value)
	{
		AppendU32(Bytes, LightIndex);
		AppendU32(Bytes, SurfaceIndex);
		AppendU32(Bytes, ByteCount);
		Bytes.AddZeroed(ByteCount);
		for (int32 Index = Bytes.Num() - static_cast<int32>(ByteCount);
			Index < Bytes.Num(); ++Index)
		{
			Bytes[Index] = Value;
		}
	}

	TArray<uint8> Version3TwoLightFixture()
	{
		TArray<uint8> Bytes = Fixture();
		if (!OverwriteU16(Bytes, 8, 3)) return {};
		AppendU32(Bytes, 3); AppendU32(Bytes, 3); AppendU32(Bytes, 36);
		for (int32 Index = 0; Index < 9; ++Index) Bytes.Append({20, 40, 60, 255});
		const int32 TrailerOffset = Bytes.Num();
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 1); AppendU32(Bytes, 1); AppendU32(Bytes, 2);
		AppendU32(Bytes, 2); AppendU32(Bytes, 1);
		AppendU32(Bytes, 0);
		AppendVersion3Surface(Bytes, 0, 1, 1, 1, 1);
		AppendVersion3Light(Bytes, 'A');
		AppendVersion3Light(Bytes, 'B');
		AppendVersion3Pair(Bytes, 0, 0, 1, 7);
		AppendVersion3Pair(Bytes, 1, 0, 1, 8);
		if (!OverwriteU32(Bytes, TrailerOffset,
			static_cast<uint32>(Bytes.Num() - TrailerOffset))) return {};
		return Bytes;
	}

	TArray<uint8> Version3Utf8OrderedLightFixture()
	{
		TArray<uint8> Bytes = Fixture();
		if (!OverwriteU16(Bytes, 8, 3)) return {};
		AppendU32(Bytes, 3); AppendU32(Bytes, 3); AppendU32(Bytes, 36);
		for (int32 Index = 0; Index < 9; ++Index) Bytes.Append({20, 40, 60, 255});
		const int32 TrailerOffset = Bytes.Num();
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 1); AppendU32(Bytes, 1); AppendU32(Bytes, 2);
		AppendU32(Bytes, 0); AppendU32(Bytes, 1);
		AppendU32(Bytes, 0);
		AppendVersion3Surface(Bytes, 0, 1, 1, 1, 1);
		// Exact UTF-8 order is U+E000 (EE 80 80), then U+10000
		// (F0 90 80 80); UTF-16 code-unit ordering reverses this pair.
		AppendVersion3LightBytes(Bytes, {0xee, 0x80, 0x80});
		AppendVersion3LightBytes(Bytes, {0xf0, 0x90, 0x80, 0x80});
		if (!OverwriteU32(Bytes, TrailerOffset,
			static_cast<uint32>(Bytes.Num() - TrailerOffset))) return {};
		return Bytes;
	}

	TArray<uint8> Version3OverlappingSurfaceFixture()
	{
		TArray<uint8> Bytes = Fixture();
		if (!OverwriteU16(Bytes, 8, 3)) return {};
		AppendU32(Bytes, 6); AppendU32(Bytes, 3); AppendU32(Bytes, 72);
		for (int32 Index = 0; Index < 18; ++Index)
			Bytes.Append({20, 40, 60, 255});
		const int32 TrailerOffset = Bytes.Num();
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 1); AppendU32(Bytes, 2); AppendU32(Bytes, 0);
		AppendU32(Bytes, 0); AppendU32(Bytes, 1);
		AppendU32(Bytes, 0);
		AppendVersion3Surface(Bytes, 0, 1, 1, 1, 1);
		AppendVersion3Surface(Bytes, 1, 1, 1, 1, 1);
		if (!OverwriteU32(Bytes, TrailerOffset,
			static_cast<uint32>(Bytes.Num() - TrailerOffset))) return {};
		return Bytes;
	}

	TArray<uint8> Version3ExcessPairWorkFixture()
	{
		constexpr uint32 AtlasWidth = 1025;
		constexpr uint32 AtlasHeight = 515;
		constexpr uint32 SurfaceWidth = 1023;
		constexpr uint32 SurfaceHeight = 513;
		constexpr uint32 PairBytes = SurfaceWidth * SurfaceHeight;
		TArray<uint8> Bytes = Fixture();
		if (!OverwriteU16(Bytes, 8, 3)) return {};
		AppendU32(Bytes, AtlasWidth);
		AppendU32(Bytes, AtlasHeight);
		AppendU32(Bytes, AtlasWidth * AtlasHeight * 4);
		for (uint32 Pixel = 0; Pixel < AtlasWidth * AtlasHeight; ++Pixel)
			Bytes.Append({0, 0, 0, 255});
		const int32 TrailerOffset = Bytes.Num();
		AppendU32(Bytes, 0);
		AppendU32(Bytes, 1); AppendU32(Bytes, 1); AppendU32(Bytes, 8);
		AppendU32(Bytes, 8); AppendU32(Bytes, 1);
		AppendU32(Bytes, 0);
		AppendVersion3Surface(Bytes, 0, 1, 1, SurfaceWidth, SurfaceHeight);
		for (uint8 Light = 0; Light < 8; ++Light)
			AppendVersion3Light(Bytes, static_cast<uint8>('A' + Light));
		for (uint32 Light = 0; Light < 8; ++Light)
			AppendVersion3Pair(Bytes, Light, 0, PairBytes, 1);
		if (!OverwriteU32(Bytes, TrailerOffset,
			static_cast<uint32>(Bytes.Num() - TrailerOffset))) return {};
		return Bytes;
	}

	bool IsCleared(const FWoTBSPPayloadData& Payload)
	{
		return Payload.FormatVersion == 0 && !Payload.bPortal && !Payload.bRootOutside
			&& Payload.BoundsMin == FVector3f::ZeroVector
			&& Payload.BoundsMax == FVector3f::ZeroVector
			&& Payload.Positions.IsEmpty() && Payload.Normals.IsEmpty()
			&& Payload.UV0.IsEmpty() && Payload.UV1.IsEmpty()
			&& Payload.Indices.IsEmpty() && Payload.Sections.IsEmpty()
			&& Payload.LightmapWidth == 0 && Payload.LightmapHeight == 0
			&& Payload.LightmapBGRA.IsEmpty()
			&& Payload.TriangleSurfaces.IsEmpty()
			&& Payload.EntrySurfaces.IsEmpty()
			&& Payload.EntryLights.IsEmpty()
			&& Payload.EntryIllumination.IsEmpty();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPRenderMaterialSlotsTest,
	"WoT.Payload.BSP.RenderMaterialSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPRenderMaterialSlotsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWoTBSPPayloadData Payload;
	FString Error;
	if (!UWoTBSPPayloadLibrary::DecodePayload(Fixture(), Payload, Error))
	{
		AddError(Error);
		return false;
	}
	// Two differently named sections catch both absent slots and all sections
	// accidentally resolving to slot zero. Names deliberately sort backwards.
	Payload.Sections[0].MaterialSlot = TEXT("Wall__Stone");
	// Arena_02's dynamic-light ribbon used this exact kind of skinny BSP
	// triangle. Half UVs merge the first two lightmap coordinates even though
	// their world positions remain distinct.
	Payload.UV1[0] = FVector2f(0.289611757f, 0.164587036f);
	Payload.UV1[1] = FVector2f(0.289546162f, 0.164558545f);
	Payload.UV1[2] = FVector2f(0.284671724f, 0.175289348f);
	Payload.Indices.Append({0, 2, 1});
	FWoTBSPPayloadSection Second = Payload.Sections[0];
	Second.MaterialSlot = TEXT("Floor__Tile");
	Second.FirstIndex = 3;
	Payload.Sections.Add(Second);
	UStaticMesh* Mesh = NewObject<UStaticMesh>();
	if (!TestTrue(TEXT("render mesh builds"),
		WoTBuildBSPRenderMeshOnly(*Mesh, Payload, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("wall material can be bound by its payload slot"),
		Mesh->GetMaterialIndex(TEXT("Wall__Stone")), 0);
	TestEqual(TEXT("floor material can be bound by its payload slot"),
		Mesh->GetMaterialIndex(TEXT("Floor__Tile")), 1);
	const FStaticMeshRenderData* Render = Mesh->GetRenderData();
	if (!TestTrue(TEXT("render LOD exists"),
		Render != nullptr && Render->LODResources.Num() == 1)) return false;
	const auto& Sections = Render->LODResources[0].Sections;
	if (!TestEqual(TEXT("both sections reach the renderer"), Sections.Num(), 2))
		return false;
	TestEqual(TEXT("wall triangles use material zero"), Sections[0].MaterialIndex, 0);
	TestEqual(TEXT("floor triangles use material one"), Sections[1].MaterialIndex, 1);
	const FStaticMeshVertexBuffer& Vertices =
		Render->LODResources[0].VertexBuffers.StaticMeshVertexBuffer;
	TestTrue(TEXT("BSP lightmap UVs reach the renderer at full precision"),
		Vertices.GetUseFullPrecisionUVs());
	if (TestEqual(TEXT("three BSP render vertices stay dense"),
		Vertices.GetNumVertices(), 3u))
	{
		TestEqual(TEXT("first skinny-edge UV1 is exact"),
			Vertices.GetVertexUV(0, 1), Payload.UV1[0]);
		TestEqual(TEXT("second skinny-edge UV1 remains distinct"),
			Vertices.GetVertexUV(1, 1), Payload.UV1[1]);
		TestTrue(TEXT("skinny-edge UV1 delta survives render packing"),
			Vertices.GetVertexUV(0, 1) != Vertices.GetVertexUV(1, 1));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPPayloadVersion2AtlasTest,
	"WoT.Payload.BSP.DecodeVersion2Atlas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPPayloadVersion2AtlasTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWoTBSPPayloadData Payload;
	FString Error;
	const TArray<uint8> ExpectedPixels = {
		1, 2, 3, 255,
		4, 5, 6, 255,
		7, 8, 9, 255,
		10, 11, 12, 255
	};
	const bool bValidV2 = UWoTBSPPayloadLibrary::DecodePayload(
		Version2Fixture(), Payload, Error);
	TestTrue(TEXT("literal version-2 payload decodes"), bValidV2);
	if (!bValidV2)
	{
		AddError(FString::Printf(TEXT("valid version-2 fixture failed: %s"), *Error));
		return false;
	}
	TestEqual(TEXT("version-2 format is retained"), Payload.FormatVersion,
		static_cast<uint16>(2));
	TestFalse(TEXT("version-2 has no entry root state"), Payload.bRootOutside);
	TestTrue(TEXT("version-2 has no triangle provenance"),
		Payload.TriangleSurfaces.IsEmpty());
	TestTrue(TEXT("version-2 has no entry surface records"),
		Payload.EntrySurfaces.IsEmpty());
	TestTrue(TEXT("version-2 has no entry light records"),
		Payload.EntryLights.IsEmpty());
	TestTrue(TEXT("version-2 has no entry illumination records"),
		Payload.EntryIllumination.IsEmpty());
	TestEqual(TEXT("atlas width"), Payload.LightmapWidth, 2u);
	TestEqual(TEXT("atlas height"), Payload.LightmapHeight, 2u);
	TestTrue(TEXT("atlas bytes are copied exactly"),
		Payload.LightmapBGRA == ExpectedPixels);

	const bool bValidV1 = UWoTBSPPayloadLibrary::DecodePayload(
		Fixture(), Payload, Error);
	TestTrue(TEXT("version-1 payload remains compatible"), bValidV1);
	if (bValidV1)
	{
		TestEqual(TEXT("version-1 format is retained"), Payload.FormatVersion,
			static_cast<uint16>(1));
		TestFalse(TEXT("version-1 has no entry root state"), Payload.bRootOutside);
		TestEqual(TEXT("version-1 atlas width stays zero"), Payload.LightmapWidth, 0u);
		TestEqual(TEXT("version-1 atlas height stays zero"), Payload.LightmapHeight, 0u);
		TestTrue(TEXT("version-1 atlas bytes stay empty"),
			Payload.LightmapBGRA.IsEmpty());
	}

	const int32 ExtensionOffset = Fixture().Num();
	TArray<uint8> TruncatedHeader = Fixture();
	TestTrue(TEXT("truncated-header fixture becomes version 2"),
		OverwriteU16(TruncatedHeader, 8, 2));
	AppendU32(TruncatedHeader, 2);
	AppendU32(TruncatedHeader, 2);
	TestFalse(TEXT("truncated atlas header fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(TruncatedHeader, Payload, Error));
	TestTrue(TEXT("truncated atlas header clears output"), IsCleared(Payload));

	TArray<uint8> TruncatedPixels = Version2Fixture();
	TruncatedPixels.Pop();
	TestFalse(TEXT("truncated atlas pixels fail closed"),
		UWoTBSPPayloadLibrary::DecodePayload(TruncatedPixels, Payload, Error));
	TestTrue(TEXT("truncated atlas pixels clear output"), IsCleared(Payload));

	TArray<uint8> ZeroDimension = Version2Fixture();
	TestTrue(TEXT("zero-dimension mutation is in range"),
		OverwriteU32(ZeroDimension, ExtensionOffset, 0));
	TestFalse(TEXT("zero atlas dimension fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(ZeroDimension, Payload, Error));

	TArray<uint8> OversizedDimension = Version2Fixture();
	TestTrue(TEXT("oversized-dimension mutation is in range"),
		OverwriteU32(OversizedDimension, ExtensionOffset, 8193));
	TestFalse(TEXT("atlas dimension above 8192 fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(OversizedDimension, Payload, Error));

	TArray<uint8> OversizedArea = Version2Fixture();
	TestTrue(TEXT("oversized-area width mutation is in range"),
		OverwriteU32(OversizedArea, ExtensionOffset, 8192));
	TestTrue(TEXT("oversized-area height mutation is in range"),
		OverwriteU32(OversizedArea, ExtensionOffset + 4, 8192));
	TestTrue(TEXT("oversized-area count mutation is in range"),
		OverwriteU32(OversizedArea, ExtensionOffset + 8, 8192u * 8192u * 4u));
	TestFalse(TEXT("atlas area above 4096-by-8192 fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(OversizedArea, Payload, Error));

	TArray<uint8> OverflowingDimensions = Version2Fixture();
	TestTrue(TEXT("overflow width mutation is in range"),
		OverwriteU32(OverflowingDimensions, ExtensionOffset, MAX_uint32));
	TestTrue(TEXT("overflow height mutation is in range"),
		OverwriteU32(OverflowingDimensions, ExtensionOffset + 4, MAX_uint32));
	TestFalse(TEXT("overflowing atlas dimensions fail closed"),
		UWoTBSPPayloadLibrary::DecodePayload(OverflowingDimensions, Payload, Error));

	TArray<uint8> InconsistentCount = Version2Fixture();
	TestTrue(TEXT("inconsistent-count mutation is in range"),
		OverwriteU32(InconsistentCount, ExtensionOffset + 8, 12));
	TestFalse(TEXT("atlas byte count must equal width times height times four"),
		UWoTBSPPayloadLibrary::DecodePayload(InconsistentCount, Payload, Error));

	TArray<uint8> TransparentPixel = Version2Fixture();
	TransparentPixel[TransparentPixel.Num() - 1] = 254;
	TestFalse(TEXT("atlas alpha must be opaque"),
		UWoTBSPPayloadLibrary::DecodePayload(TransparentPixel, Payload, Error));

	TArray<uint8> Version2Trailing = Version2Fixture();
	Version2Trailing.Add(0);
	TestFalse(TEXT("version-2 trailing bytes fail closed"),
		UWoTBSPPayloadLibrary::DecodePayload(Version2Trailing, Payload, Error));

	TArray<uint8> Version1Trailing = Fixture();
	Version1Trailing.Add(0);
	TestFalse(TEXT("version-1 trailing bytes remain forbidden"),
		UWoTBSPPayloadLibrary::DecodePayload(Version1Trailing, Payload, Error));

	Payload.LightmapWidth = 77;
	Payload.LightmapHeight = 88;
	Payload.LightmapBGRA = {99};
	TestFalse(TEXT("rejection after prior data still fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(InconsistentCount, Payload, Error));
	TestTrue(TEXT("every payload field is cleared on rejection"), IsCleared(Payload));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPPayloadVersion3WeatherTest,
	"WoT.Payload.BSP.DecodeVersion3Weather",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPPayloadVersion3WeatherTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWoTBSPPayloadData Payload;
	FString Error;
	if (!TestTrue(TEXT("v3 literal decodes"),
		UWoTBSPPayloadLibrary::DecodePayload(Version3Fixture(), Payload, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("v3 format is retained"), Payload.FormatVersion,
		static_cast<uint16>(3));
	TestTrue(TEXT("v3 root-outside state is retained"), Payload.bRootOutside);
	TestTrue(TEXT("v3 triangle surface is exact"),
		Payload.TriangleSurfaces == TArray<uint32>({0}));
	if (TestEqual(TEXT("v3 has one dense surface record"),
		Payload.EntrySurfaces.Num(), 1))
	{
		const FWoTEntrySurfaceRecord& Surface = Payload.EntrySurfaces[0];
		TestEqual(TEXT("v3 surface index"), Surface.SurfaceIndex, 0u);
		TestEqual(TEXT("v3 surface X"), Surface.X, 1u);
		TestEqual(TEXT("v3 surface Y"), Surface.Y, 1u);
		TestEqual(TEXT("v3 surface W"), Surface.W, 1u);
		TestEqual(TEXT("v3 surface H"), Surface.H, 1u);
		TestTrue(TEXT("v3 surface pan is exact zero"),
			Surface.PanUV == FVector2f::ZeroVector);
	}
	TArray<uint8> ExpectedAtlas;
	for (int32 Index = 0; Index < 9; ++Index)
		ExpectedAtlas.Append({20, 40, 60, 255});
	TestTrue(TEXT("v3 steady atlas bytes are exact"),
		Payload.LightmapBGRA == ExpectedAtlas);
	TestTrue(TEXT("minimal v3 has no light records"), Payload.EntryLights.IsEmpty());
	TestTrue(TEXT("minimal v3 has no illumination records"),
		Payload.EntryIllumination.IsEmpty());

	FWoTBSPPayloadData TwoLightPayload;
	if (TestTrue(TEXT("two lights may affect the same surface rectangle"),
		UWoTBSPPayloadLibrary::DecodePayload(
			Version3TwoLightFixture(), TwoLightPayload, Error)))
	{
		TestEqual(TEXT("two-light fixture retains two lights"),
			TwoLightPayload.EntryLights.Num(), 2);
		TestEqual(TEXT("two-light fixture retains two pairs"),
			TwoLightPayload.EntryIllumination.Num(), 2);
		if (TwoLightPayload.EntryIllumination.Num() == 2)
		{
			TestTrue(TEXT("first illumination byte is exact"),
				TwoLightPayload.EntryIllumination[0].Illumination
					== TArray<uint8>({7}));
			TestTrue(TEXT("second illumination byte is exact"),
				TwoLightPayload.EntryIllumination[1].Illumination
					== TArray<uint8>({8}));
		}
	}
	FWoTBSPPayloadData Utf8OrderedPayload;
	TestTrue(TEXT("light identities use exact UTF-8 byte ordering"),
		UWoTBSPPayloadLibrary::DecodePayload(
			Version3Utf8OrderedLightFixture(), Utf8OrderedPayload, Error));

	const int32 TrailerOffset = Fixture().Num() + 12 + 36;
	const int32 LightOffset = TrailerOffset + 56;
	const int32 LightRowBytes = 13;
	const int32 PairOffset = LightOffset + LightRowBytes * 2;
	auto Reject = [this, &Payload, &Error](const TCHAR* Label, TArray<uint8>&& Bytes)
	{
		TestFalse(Label, UWoTBSPPayloadLibrary::DecodePayload(Bytes, Payload, Error));
	};
	auto MutateU32 = [this, &Reject](const TCHAR* Label, int32 Offset, uint32 Value)
	{
		TArray<uint8> Bytes = Version3Fixture();
		TestTrue(FString(Label) + TEXT(" mutation is in range"),
			OverwriteU32(Bytes, Offset, Value));
		Reject(Label, MoveTemp(Bytes));
	};

	MutateU32(TEXT("v3 trailer length mismatch rejects"), TrailerOffset, 55);
	MutateU32(TEXT("v3 triangle count mismatch rejects"), TrailerOffset + 4, 2);
	MutateU32(TEXT("v3 zero surface count rejects"), TrailerOffset + 8, 0);
	MutateU32(TEXT("v3 invalid root-outside rejects"), TrailerOffset + 20, 2);
	MutateU32(TEXT("v3 triangle surface out of range rejects"), TrailerOffset + 24, 1);
	MutateU32(TEXT("v3 surface records must be dense"), TrailerOffset + 28, 1);
	MutateU32(TEXT("v3 rectangle must retain its left gutter"), TrailerOffset + 32, 0);
	MutateU32(TEXT("v3 rectangle area overflow rejects"), TrailerOffset + 40, MAX_uint32);
	MutateU32(TEXT("v3 NaN pan rejects"), TrailerOffset + 48, 0x7fc00000u);
	MutateU32(TEXT("v3 infinite pan rejects"), TrailerOffset + 52, 0x7f800000u);
	Reject(TEXT("v3 padded surface allocations may not overlap"),
		Version3OverlappingSurfaceFixture());

	TArray<uint8> TooManyLights = Version3TwoLightFixture();
	TestTrue(TEXT("light-count mutation is in range"),
		OverwriteU32(TooManyLights, TrailerOffset + 12, 9));
	Reject(TEXT("v3 rejects more than eight lights"), MoveTemp(TooManyLights));
	TArray<uint8> TooManyPairs = Version3TwoLightFixture();
	TestTrue(TEXT("pair-count mutation is in range"),
		OverwriteU32(TooManyPairs, TrailerOffset + 16, 4097));
	Reject(TEXT("v3 rejects more than 4096 pairs"), MoveTemp(TooManyPairs));

	TArray<uint8> LightHeaderReserved = Version3TwoLightFixture();
	TestTrue(TEXT("light-header reserved mutation is in range"),
		OverwriteU16(LightHeaderReserved, LightOffset + 2, 1));
	Reject(TEXT("v3 light header reserved bytes must be zero"),
		MoveTemp(LightHeaderReserved));
	TArray<uint8> LightColorReserved = Version3TwoLightFixture();
	LightColorReserved[LightOffset + 8] = 1;
	Reject(TEXT("v3 light color reserved byte must be zero"),
		MoveTemp(LightColorReserved));
	TArray<uint8> InvalidUtf8 = Version3TwoLightFixture();
	InvalidUtf8[LightOffset + 4] = 0x80;
	Reject(TEXT("v3 invalid light identity UTF-8 rejects"), MoveTemp(InvalidUtf8));
	TArray<uint8> DuplicateLights = Version3TwoLightFixture();
	DuplicateLights[LightOffset + LightRowBytes + 4] = 'a';
	Reject(TEXT("v3 case-insensitive duplicate light identities reject"),
		MoveTemp(DuplicateLights));
	TArray<uint8> OutOfOrderLights = Version3TwoLightFixture();
	OutOfOrderLights[LightOffset + 4] = 'B';
	OutOfOrderLights[LightOffset + LightRowBytes + 4] = 'A';
	Reject(TEXT("v3 out-of-order light identities reject"),
		MoveTemp(OutOfOrderLights));
	TArray<uint8> NanColor = Version3TwoLightFixture();
	TestTrue(TEXT("NaN color mutation is in range"),
		OverwriteU32(NanColor, LightOffset + 9, 0x7fc00000u));
	Reject(TEXT("v3 NaN level brightness rejects"), MoveTemp(NanColor));
	TArray<uint8> InfiniteColor = Version3TwoLightFixture();
	TestTrue(TEXT("infinite color mutation is in range"),
		OverwriteU32(InfiniteColor, LightOffset + 9, 0x7f800000u));
	Reject(TEXT("v3 infinite level brightness rejects"), MoveTemp(InfiniteColor));

	TArray<uint8> PairLightOutOfRange = Version3TwoLightFixture();
	TestTrue(TEXT("pair-light mutation is in range"),
		OverwriteU32(PairLightOutOfRange, PairOffset, 2));
	Reject(TEXT("v3 pair light index must be in range"),
		MoveTemp(PairLightOutOfRange));
	TArray<uint8> PairSurfaceOutOfRange = Version3TwoLightFixture();
	TestTrue(TEXT("pair-surface mutation is in range"),
		OverwriteU32(PairSurfaceOutOfRange, PairOffset + 4, 1));
	Reject(TEXT("v3 pair surface index must be in range"),
		MoveTemp(PairSurfaceOutOfRange));
	TArray<uint8> PairByteCountMismatch = Version3TwoLightFixture();
	TestTrue(TEXT("pair-byte-count mutation is in range"),
		OverwriteU32(PairByteCountMismatch, PairOffset + 8, 2));
	Reject(TEXT("v3 pair byte count must equal rectangle area"),
		MoveTemp(PairByteCountMismatch));
	TArray<uint8> DuplicatePairs = Version3TwoLightFixture();
	TestTrue(TEXT("duplicate-pair mutation is in range"),
		OverwriteU32(DuplicatePairs, PairOffset + 13, 0));
	Reject(TEXT("v3 duplicate light-surface pairs reject"), MoveTemp(DuplicatePairs));
	TArray<uint8> OutOfOrderPairs = Version3TwoLightFixture();
	TestTrue(TEXT("first out-of-order pair mutation is in range"),
		OverwriteU32(OutOfOrderPairs, PairOffset, 1));
	TestTrue(TEXT("second out-of-order pair mutation is in range"),
		OverwriteU32(OutOfOrderPairs, PairOffset + 13, 0));
	Reject(TEXT("v3 out-of-order light-surface pairs reject"),
		MoveTemp(OutOfOrderPairs));
	TArray<uint8> TruncatedPair = Version3TwoLightFixture();
	TruncatedPair.Pop();
	TestTrue(TEXT("truncated-pair trailer length refresh is in range"),
		OverwriteU32(TruncatedPair, TrailerOffset,
			static_cast<uint32>(TruncatedPair.Num() - TrailerOffset)));
	Reject(TEXT("v3 truncated pair data rejects"), MoveTemp(TruncatedPair));
	TArray<uint8> ExtraBytes = Version3Fixture();
	ExtraBytes.Add(0);
	TestTrue(TEXT("extra-byte trailer length refresh is in range"),
		OverwriteU32(ExtraBytes, TrailerOffset,
			static_cast<uint32>(ExtraBytes.Num() - TrailerOffset)));
	Reject(TEXT("v3 extra bytes reject"), MoveTemp(ExtraBytes));
	Reject(TEXT("v3 aggregate pair work above four MiB rejects"),
		Version3ExcessPairWorkFixture());

	Payload.FormatVersion = 3;
	Payload.bRootOutside = true;
	Payload.TriangleSurfaces = {1};
	Payload.EntrySurfaces.AddDefaulted();
	Payload.EntryLights.AddDefaulted();
	Payload.EntryIllumination.AddDefaulted();
	TArray<uint8> RejectedAfterData = Version3Fixture();
	RejectedAfterData.Add(0);
	TestFalse(TEXT("v3 rejection after prior data still fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(RejectedAfterData, Payload, Error));
	TestTrue(TEXT("v3 rejection clears every old and new field"), IsCleared(Payload));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPPayloadDecodeTest,
	"WoT.Payload.BSP.DecodeAndRejectHostileGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPPayloadDecodeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWoTBSPPayloadData Payload;
	FString Error;
	const bool bValidMain =
		UWoTBSPPayloadLibrary::DecodePayload(Fixture(), Payload, Error);
	TestTrue(TEXT("valid main payload decodes"), bValidMain);
	if (!bValidMain)
	{
		AddError(FString::Printf(TEXT("valid fixture failed: %s"), *Error));
		return false;
	}
	TestEqual(TEXT("three vertices"), Payload.Positions.Num(), 3);
	TestEqual(TEXT("two UV channels stay dense"), Payload.UV1.Num(), 3);
	TestEqual(TEXT("one section"), Payload.Sections.Num(), 1);
	if (!Payload.Sections.IsValidIndex(0))
	{
		AddError(TEXT("decoded payload omitted section zero"));
		return false;
	}
	TestEqual(TEXT("exact ASCII slot"), Payload.Sections[0].MaterialSlot,
		FString(TEXT("JCylinder0")));
	TestTrue(TEXT("main collision bit"), Payload.Sections[0].bCollisionEnabled);
	TestEqual(TEXT("reference max bound"), Payload.BoundsMax,
		FVector3f(100.f, 100.f, 0.f));

	const TArray<uint8> Utf8Slot = {
		'T', 'r', 'a', 'p', 0xe5, 0xa2, 0x99, '1'
	};
	const bool bValidUtf8 = UWoTBSPPayloadLibrary::DecodePayload(
		FixtureWithSlot(Utf8Slot), Payload, Error);
	TestTrue(TEXT("valid non-ASCII UTF-8 slot decodes"), bValidUtf8);
	if (bValidUtf8 && Payload.Sections.IsValidIndex(0))
	{
		FString ExpectedUtf8Slot(TEXT("Trap"));
		ExpectedUtf8Slot.AppendChar(static_cast<TCHAR>(0x5899));
		ExpectedUtf8Slot.AppendChar(TEXT('1'));
		TestEqual(TEXT("exact non-ASCII UTF-8 slot"),
			Payload.Sections[0].MaterialSlot, ExpectedUtf8Slot);
	}

	const TArray<uint8> MalformedUtf8Slot = {
		'T', 'r', 'a', 'p', 0xc0, 0xaf, '1'
	};
	TestFalse(TEXT("malformed UTF-8 slot fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(
			FixtureWithSlot(MalformedUtf8Slot), Payload, Error));
	TestTrue(TEXT("malformed UTF-8 rejection is contextual"),
		Error.Contains(TEXT("UTF-8")));
	TestTrue(TEXT("malformed UTF-8 exposes no partial geometry"),
		Payload.Positions.IsEmpty() && Payload.Normals.IsEmpty()
		&& Payload.UV0.IsEmpty() && Payload.UV1.IsEmpty()
		&& Payload.Indices.IsEmpty() && Payload.Sections.IsEmpty());

	TArray<uint8> Truncated = Fixture();
	if (!Truncated.IsEmpty()) Truncated.Pop();
	TestFalse(TEXT("truncation fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(Truncated, Payload, Error));
	TestTrue(TEXT("truncation leaves no partial geometry"),
		Payload.Positions.IsEmpty() && Payload.Normals.IsEmpty()
		&& Payload.UV0.IsEmpty() && Payload.UV1.IsEmpty()
		&& Payload.Indices.IsEmpty() && Payload.Sections.IsEmpty());
	TArray<uint8> BadIndex = Fixture();
	TestTrue(TEXT("bad-index fixture mutation is in range"),
		OverwriteU32(BadIndex, BadIndex.Num() - 4, 99));
	TestFalse(TEXT("out-of-range index fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(BadIndex, Payload, Error));
	TestTrue(TEXT("bad index leaves no partial geometry"),
		Payload.Positions.IsEmpty() && Payload.Normals.IsEmpty()
		&& Payload.UV0.IsEmpty() && Payload.UV1.IsEmpty()
		&& Payload.Indices.IsEmpty() && Payload.Sections.IsEmpty());

	TArray<uint8> EmptyVertices = Fixture();
	TestTrue(TEXT("empty-vertex fixture mutation is in range"),
		OverwriteU32(EmptyVertices, 12, 0));
	TestFalse(TEXT("declared empty vertex array fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(EmptyVertices, Payload, Error));
	TestTrue(TEXT("empty vertex rejection returns contextual error"), !Error.IsEmpty());
	TestTrue(TEXT("empty vertex rejection exposes no index-zero arrays"),
		Payload.Positions.IsEmpty() && Payload.Normals.IsEmpty()
		&& Payload.UV0.IsEmpty() && Payload.UV1.IsEmpty()
		&& Payload.Indices.IsEmpty() && Payload.Sections.IsEmpty());

	TArray<uint8> EmptySections = Fixture();
	TestTrue(TEXT("empty-section fixture mutation is in range"),
		OverwriteU32(EmptySections, 20, 0));
	TestFalse(TEXT("declared empty section array fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(EmptySections, Payload, Error));
	TestTrue(TEXT("empty section rejection exposes no index-zero arrays"),
		Payload.Positions.IsEmpty() && Payload.Indices.IsEmpty()
		&& Payload.Sections.IsEmpty());

	TArray<uint8> SectionPastIndices = Fixture();
	TestTrue(TEXT("past-index fixture mutation is in range"),
		OverwriteU32(SectionPastIndices, 56, 3));
	TestFalse(TEXT("section first index past expected range fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(SectionPastIndices, Payload, Error));
	TestTrue(TEXT("bad section offset exposes no partial arrays"),
		Payload.Positions.IsEmpty() && Payload.Indices.IsEmpty()
		&& Payload.Sections.IsEmpty());

	TArray<uint8> ZeroSectionIndices = Fixture();
	TestTrue(TEXT("zero-section-range fixture mutation is in range"),
		OverwriteU32(ZeroSectionIndices, 60, 0));
	TestFalse(TEXT("zero-length section range fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(ZeroSectionIndices, Payload, Error));

	TArray<uint8> Portal = Fixture(true);
	const bool bValidPortal =
		UWoTBSPPayloadLibrary::DecodePayload(Portal, Payload, Error);
	TestTrue(TEXT("portal role decodes"), bValidPortal);
	if (!bValidPortal || !Payload.Sections.IsValidIndex(0))
	{
		AddError(FString::Printf(TEXT("valid portal fixture failed: %s"), *Error));
		return false;
	}
	TestTrue(TEXT("portal role"), Payload.bPortal);
	TestFalse(TEXT("portal section has no collision"),
		Payload.Sections[0].bCollisionEnabled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPDerivedKeyTest,
	"WoT.Payload.BSP.DerivedKeyInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPDerivedKeyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString PayloadSHA = FString::ChrN(64, TEXT('1'));
	const FString Key = UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
		PayloadSHA, TEXT("5.8.0-123+main"), TEXT("Mac"));
	TestEqual(TEXT("Python/C++ framed-key fixture"), Key,
		FString(TEXT("2a9bb04e254e4ae0b97288ff3c8e9733d9c53c4d0fe282811c7f43839e44c0b9")));
	TestNotEqual(TEXT("payload invalidates"), Key,
		UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
			FString::ChrN(64, TEXT('2')), TEXT("5.8.0-123+main"), TEXT("Mac")));
	TestNotEqual(TEXT("engine invalidates"), Key,
		UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
			PayloadSHA, TEXT("5.8.0-124+main"), TEXT("Mac")));
	TestNotEqual(TEXT("platform invalidates"), Key,
		UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
			PayloadSHA, TEXT("5.8.0-123+main"), TEXT("Win64")));
	TestTrue(TEXT("unknown derived format fails closed"),
		UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
			PayloadSHA, TEXT("5.8.0-123+main"), TEXT("Mac"), 2).IsEmpty());
	TestTrue(TEXT("unknown tangent recipe fails closed"),
		UWoTBSPPayloadLibrary::BuildDerivedCacheKeyForInputs(
			PayloadSHA, TEXT("5.8.0-123+main"), TEXT("Mac"), 1,
			TEXT("unknown-tangent-v2")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPPerMapBarrierSpec,
	"WoT.Payload.BSP.ManifestDrivenPerMapBarrier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPPerMapBarrierSpec::RunTest(const FString& Parameters)
{
	(void)Parameters;
	// ClassWithin is GameInstance: an outer-less NewObject trips the
	// StaticAllocateObject ensure and auto-fails the spec.
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	UWoTPayloadSubsystem* Subsystem =
		NewObject<UWoTPayloadSubsystem>(GameInstance);
	auto AddManifestEntry = [Subsystem](
		const TCHAR* Key, const TCHAR* AssetPath)
	{
		FWoTBSPPayloadEntry Entry;
		Entry.CacheKey = Key;
		Entry.AssetPath = AssetPath;
		Subsystem->BSPEntries.Add(Key, MoveTemp(Entry));
	};
	AddManifestEntry(TEXT("bsp:Arena_07__Arena_07"),
		TEXT("/Game/WoT/Maps/Geo/Arena_07/Arena_07.Arena_07"));
	AddManifestEntry(TEXT("bsp:Arena_07__Arena_07_Fractal_000"),
		TEXT("/Game/WoT/Maps/Geo/Arena_07/Arena_07_Fractal_000.Arena_07_Fractal_000"));
	AddManifestEntry(TEXT("bsp:Arena_07__Arena_07_portals_Fractal_000"),
		TEXT("/Game/WoT/Maps/Geo/Arena_07_portals/Arena_07_portals_Fractal_000.Arena_07_portals_Fractal_000"));
	AddManifestEntry(TEXT("bsp:Battle2_02__Battle2_02"),
		TEXT("/Game/WoT/Maps/Geo/Battle2_02/Battle2_02.Battle2_02"));
	AddManifestEntry(TEXT("bsp:Foreign__Foreign"),
		TEXT("/Game/WoT/Maps/Geo/Foreign/Foreign.Foreign"));
	AddManifestEntry(TEXT("bsp:Ignored__Ignored"),
		TEXT("/Game/WoT/Meshes/Ignored.Ignored"));
	Subsystem->RebuildRequiredBSPKeysByMap();

	const TArray<FString>* ArenaKeys =
		Subsystem->RequiredBSPKeysByMap.Find(TEXT("Arena_07"));
	TestTrue(TEXT("map with portal and fractal parts is indexed"),
		ArenaKeys != nullptr);
	if (!ArenaKeys)
	{
		return false;
	}
	TestEqual(TEXT("full Arena set has three parts"), ArenaKeys->Num(), 3);
	const TArray<FString>* BattleKeys =
		Subsystem->RequiredBSPKeysByMap.Find(TEXT("Battle2_02"));
	TestTrue(TEXT("main-only map is indexed"), BattleKeys != nullptr);
	if (BattleKeys)
	{
		TestEqual(TEXT("main-only map has one part"), BattleKeys->Num(), 1);
	}
	TestFalse(TEXT("unrelated asset path is ignored"),
		Subsystem->RequiredBSPKeysByMap.Contains(TEXT("Ignored")));

	FString MapName;
	TestTrue(TEXT("game map package yields map identity"),
		UWoTPayloadSubsystem::TryGetBSPMapFromWorldPackage(
			TEXT("/Game/WoT/Maps/Arena_07"), MapName));
	TestEqual(TEXT("world package map identity"), MapName,
		FString(TEXT("Arena_07")));
	TestFalse(TEXT("geo package is not a ticking map package"),
		UWoTPayloadSubsystem::TryGetBSPMapFromWorldPackage(
			TEXT("/Game/WoT/Maps/Geo/Arena_07"), MapName));

	TestFalse(TEXT("barrier starts held with no Arena part ready"),
		Subsystem->IsBSPBarrierReadyForMap(TEXT("Arena_07")));
	Subsystem->ReadyBSPKeys.Add(TEXT("bsp:Arena_07__Arena_07"));
	Subsystem->ReadyBSPKeys.Add(
		TEXT("bsp:Arena_07__Arena_07_portals_Fractal_000"));
	Subsystem->ReadyBSPKeys.Add(TEXT("bsp:Foreign__Foreign"));
	TestFalse(TEXT("foreign-map readiness cannot release missing Arena fractal"),
		Subsystem->IsBSPBarrierReadyForMap(TEXT("Arena_07")));
	Subsystem->ReadyBSPKeys.Add(
		TEXT("bsp:Arena_07__Arena_07_Fractal_000"));
	TestTrue(TEXT("barrier releases only after full per-map set is ready"),
		Subsystem->IsBSPBarrierReadyForMap(TEXT("Arena_07")));
	Subsystem->FailedRequiredBSPKeys.Add(
		TEXT("bsp:Arena_07__Arena_07_portals_Fractal_000"));
	TestFalse(TEXT("failed required key remains fail-closed even when ready"),
		Subsystem->IsBSPBarrierReadyForMap(TEXT("Arena_07")));
	Subsystem->FailedRequiredBSPKeys.Empty();
	TestFalse(TEXT("Arena readiness never releases a main-only foreign map"),
		Subsystem->IsBSPBarrierReadyForMap(TEXT("Battle2_02")));
	Subsystem->ReadyBSPKeys.Add(TEXT("bsp:Battle2_02__Battle2_02"));
	TestTrue(TEXT("main-only map releases on its complete local set"),
		Subsystem->IsBSPBarrierReadyForMap(TEXT("Battle2_02")));
	TestTrue(TEXT("front end with no required set has no barrier"),
		Subsystem->IsBSPBarrierReadyForMap(TEXT("WoTFrontEnd")));
	GameInstance->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPPayloadVersion4IdentitySpec,
	"WoT.Payload.BSP.DecodeVersion4SectionIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPPayloadVersion4IdentitySpec::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWoTBSPPayloadData Payload;
	FString Error;
	TArray<uint8> Bytes = Version4IdentityFixture();
	if (!TestTrue(TEXT("version-4 identity payload decodes"),
		UWoTBSPPayloadLibrary::DecodePayload(Bytes, Payload, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("version is retained"), Payload.FormatVersion,
		static_cast<uint16>(4));
	if (!TestEqual(TEXT("one identity-bearing section"),
		Payload.Sections.Num(), 1)) return false;
	const FWoTBSPPayloadSection& Section = Payload.Sections[0];
	TestEqual(TEXT("canonical package"), Section.CanonicalPackage,
		FString(TEXT("WorldT")));
	TestEqual(TEXT("canonical name"), Section.CanonicalName,
		FString(TEXT("Stone")));
	TestEqual(TEXT("source export identity"), Section.SourceTextureIdentity,
		FString(TEXT("WorldT.Detail.Stone")));
	TestEqual(TEXT("pan-zone actor identity"), Section.PanZoneActorIdentity,
		FString(TEXT("Arena_01.ZoneInfo2")));
	TestEqual(TEXT("last-writer surface flags"), Section.SurfaceFlags,
		0x00000602u);
	TestTrue(TEXT("legacy section has no macro identity"),
		Section.MacroCanonicalPackage.IsEmpty()
			&& Section.MacroCanonicalName.IsEmpty()
			&& Section.MacroSourceTextureIdentity.IsEmpty());
	TestEqual(TEXT("legacy section has neutral macro ratio"),
		Section.MacroRatio, 0.0f);

	TArray<uint8> Truncated = Bytes;
	Truncated.RemoveAt(92, 1);
	TestFalse(TEXT("truncated identity fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(Truncated, Payload, Error));
	TestTrue(TEXT("failed decode clears output"), IsCleared(Payload));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBSPPayloadVersion10MacroSpec,
	"WoT.Payload.BSP.DecodeVersion10MacroAndSurfaceProvenance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBSPPayloadVersion10MacroSpec::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FWoTBSPPayloadData Payload;
	FString Error;
	TArray<uint8> Bytes = Version10MacroFixture();
	if (!TestTrue(TEXT("version-10 macro payload decodes"),
		UWoTBSPPayloadLibrary::DecodePayload(Bytes, Payload, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("version is retained"), Payload.FormatVersion,
		static_cast<uint16>(10));
	if (TestEqual(TEXT("one triangle surface is retained"),
		Payload.TriangleSurfaces.Num(), 1))
	{
		TestEqual(TEXT("triangle retains source surface zero"),
			Payload.TriangleSurfaces[0], 0u);
	}
	if (!TestEqual(TEXT("one macro-bearing section"),
		Payload.Sections.Num(), 1)) return false;
	const FWoTBSPPayloadSection& Section = Payload.Sections[0];
	TestEqual(TEXT("macro canonical package"), Section.MacroCanonicalPackage,
		FString(TEXT("WOTDetail")));
	TestEqual(TEXT("macro canonical name"), Section.MacroCanonicalName,
		FString(TEXT("MacroRock_003")));
	TestEqual(TEXT("macro source identity"), Section.MacroSourceTextureIdentity,
		FString(TEXT("WOTDetail.Macro.MacroRock_003")));
	TestTrue(TEXT("macro ratio is the authored base/macro scale"),
		FMath::IsNearlyEqual(Section.MacroRatio, 1.0f / 6.0f));
	TestEqual(TEXT("macro U offset is decoded exactly"),
		Section.MacroOffsetU, 0.125f);
	TestEqual(TEXT("macro V offset is decoded exactly"),
		Section.MacroOffsetV, -0.25f);

	TArray<uint8> Incomplete = Bytes;
	const int32 MacroHeader = 52 + 24 + FString(TEXT("WorldT__Stone")).Len()
		+ 12 + FString(TEXT("WorldT")).Len() + FString(TEXT("Stone")).Len()
		+ FString(TEXT("WorldT.Detail.Stone")).Len()
		+ FString(TEXT("Arena_01.ZoneInfo2")).Len();
	OverwriteU16(Incomplete, MacroHeader + 2, 0);
	TestFalse(TEXT("partial macro identity fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(Incomplete, Payload, Error));
	TestTrue(TEXT("failed macro decode clears output"), IsCleared(Payload));

	TArray<uint8> Superseded = Bytes;
	OverwriteU16(Superseded, 8, 9);
	TestFalse(TEXT("superseded version 8 fails closed"),
		UWoTBSPPayloadLibrary::DecodePayload(Superseded, Payload, Error));
	TestTrue(TEXT("failed old-version decode clears output"), IsCleared(Payload));
	return true;
}

#endif
