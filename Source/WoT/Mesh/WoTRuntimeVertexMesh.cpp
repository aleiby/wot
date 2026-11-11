#include "WoTRuntimeVertexMesh.h"

#include "../Payload/WoTNeutralResources.h"
#include "../Payload/WoTPayloadActivation.h"
#include "../Payload/WoTRuntimeRegistry.h"

#include "Misc/ScopeLock.h"
#include "WoTBootstrapCore/CanonicalJson.h"

namespace
{
constexpr uint32 PolyInvisible = 0x00000001;
constexpr uint32 PolyMasked = 0x00000002;
constexpr uint32 PolyTranslucent = 0x00000004;
constexpr uint32 PolyEnvironment = 0x00000010;
constexpr uint32 PolyModulated = 0x00000040;
constexpr uint32 PolyTwoSided = 0x00000100;
constexpr uint32 PolyNoSmooth = 0x00000800;
constexpr uint32 PolyFlat = 0x00004000;
constexpr uint32 PolyUnlit = 0x00400000;
constexpr uint32 SupportedPolyFlags = PolyInvisible | PolyMasked
	| PolyTranslucent | PolyEnvironment | PolyModulated | PolyTwoSided
	| PolyNoSmooth | PolyFlat | PolyUnlit;
constexpr uint32 MaximumTriangles = 1520;
constexpr uint32 MaximumFrames = 65536;
constexpr uint32 MaximumVertices = 1024 * 1024;

TMap<FString, FWoTRuntimeVertexMeshDataPtr> RuntimeMeshCache;
TSet<FString> RejectedRuntimeMeshes;

FWoTBootstrapError RuntimeMeshError(const TCHAR* Message)
{
	return MakeWoTBootstrapError(
		EWoTBootstrapError::CorruptInstallation, Message);
}

template <typename T>
TWoTBootstrapResult<T> RuntimeMeshFailure(const TCHAR* Message)
{
	return TWoTBootstrapResult<T>::Error(RuntimeMeshError(Message));
}

bool ReadString(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, FString& Out)
{
	return !ReadStringField(Object, Field, TEXT("vertex mesh header"), Out).IsSet();
}

bool ReadU32(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, uint32& Out)
{
	return !ReadUnsigned32Field(
		Object, Field, TEXT("vertex mesh header"), Out).IsSet();
}

bool BitsToFloat(uint32 Bits, float& Out)
{
	FMemory::Memcpy(&Out, &Bits, sizeof(Out));
	return FMath::IsFinite(Out);
}

bool ReadFloatBits3(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, FVector3f& Out)
{
	const FWoTCanonicalJsonValue* Array = Object.FindMember(Field);
	if (!Array || !Array->IsArray() || Array->Num() != 3)
	{
		return false;
	}
	float Values[3] = {};
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FWoTCanonicalJsonValue& Value = Array->GetElement(Index);
		if (Value.GetKind() != EWoTCanonicalJsonKind::Integer
			|| Value.GetInteger() < 0 || Value.GetInteger() > MAX_uint32
			|| !BitsToFloat(static_cast<uint32>(Value.GetInteger()), Values[Index]))
		{
			return false;
		}
	}
	Out = FVector3f(Values[0], Values[1], Values[2]);
	return true;
}

bool ReadRotation3(const FWoTCanonicalJsonValue& Object, int32 Out[3])
{
	const FWoTCanonicalJsonValue* Array = Object.FindMember(TEXT("rotation_origin"));
	if (!Array || !Array->IsArray() || Array->Num() != 3)
	{
		return false;
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FWoTCanonicalJsonValue& Value = Array->GetElement(Index);
		if (Value.GetKind() != EWoTCanonicalJsonKind::Integer
			|| Value.GetInteger() < MIN_int32 || Value.GetInteger() > MAX_int32)
		{
			return false;
		}
		Out[Index] = static_cast<int32>(Value.GetInteger());
	}
	return true;
}

bool ReadUnsignedArray3(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, uint32 Out[3])
{
	const FWoTCanonicalJsonValue* Array = Object.FindMember(Field);
	if (!Array || !Array->IsArray() || Array->Num() != 3)
	{
		return false;
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FWoTCanonicalJsonValue& Value = Array->GetElement(Index);
		if (Value.GetKind() != EWoTCanonicalJsonKind::Integer
			|| Value.GetInteger() < 0 || Value.GetInteger() > MAX_uint32)
		{
			return false;
		}
		Out[Index] = static_cast<uint32>(Value.GetInteger());
	}
	return true;
}

bool ReadOptionalU32(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, uint32& Out, bool& bOutPresent)
{
	const FWoTCanonicalJsonValue* Value = Object.FindMember(Field);
	if (!Value) return false;
	if (Value->IsNull())
	{
		bOutPresent = false;
		Out = 0;
		return true;
	}
	bOutPresent = true;
	return Value->GetKind() == EWoTCanonicalJsonKind::Integer
		&& Value->GetInteger() >= 0 && Value->GetInteger() <= MAX_uint32
		&& (Out = static_cast<uint32>(Value->GetInteger()), true);
}

bool ReadOptionalString(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, FString& Out, bool& bOutPresent)
{
	const FWoTCanonicalJsonValue* Value = Object.FindMember(Field);
	if (!Value) return false;
	if (Value->IsNull())
	{
		bOutPresent = false;
		Out.Reset();
		return true;
	}
	bOutPresent = true;
	return Value->GetKind() == EWoTCanonicalJsonKind::String
		&& (Out = Value->GetString(), true);
}

bool ReadOptionalUnsignedArray3(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, uint32 Out[3], bool& bOutPresent)
{
	const FWoTCanonicalJsonValue* Value = Object.FindMember(Field);
	if (!Value) return false;
	if (Value->IsNull())
	{
		bOutPresent = false;
		return true;
	}
	bOutPresent = true;
	return ReadUnsignedArray3(Object, Field, Out);
}

bool ValidateSource(const FWoTCanonicalJsonValue& Header)
{
	const FWoTCanonicalJsonValue* Source = nullptr;
	static const TCHAR* Fields[] = {
		TEXT("export"), TEXT("package"), TEXT("sha256")
	};
	FString Export;
	FString Package;
	FString Sha256;
	return !ReadObjectField(Header, TEXT("source"), TEXT("vertex mesh header"),
			Source).IsSet()
		&& Source
		&& !RequireExactMembers(*Source, MakeArrayView(Fields),
			TEXT("vertex mesh source")).IsSet()
		&& ReadString(*Source, TEXT("export"), Export)
		&& ReadString(*Source, TEXT("package"), Package)
		&& ReadString(*Source, TEXT("sha256"), Sha256)
		&& !Export.IsEmpty() && !Package.IsEmpty() && Sha256.Len() == 64;
}

class FCursor
{
public:
	explicit FCursor(TConstArrayView<uint8> InBytes) : Bytes(InBytes) {}
	bool Skip(int32 Count)
	{
		if (Count < 0 || Offset < 0 || Offset > Bytes.Num() - Count) return false;
		Offset += Count;
		return true;
	}
	bool U16(uint16& Out)
	{
		if (Offset < 0 || Offset > Bytes.Num() - 2) return false;
		Out = static_cast<uint16>(Bytes[Offset])
			| static_cast<uint16>(Bytes[Offset + 1]) << 8;
		Offset += 2;
		return true;
	}
	bool U32(uint32& Out)
	{
		if (Offset < 0 || Offset > Bytes.Num() - 4) return false;
		Out = static_cast<uint32>(Bytes[Offset])
			| static_cast<uint32>(Bytes[Offset + 1]) << 8
			| static_cast<uint32>(Bytes[Offset + 2]) << 16
			| static_cast<uint32>(Bytes[Offset + 3]) << 24;
		Offset += 4;
		return true;
	}
	bool I32(int32& Out)
	{
		uint32 Bits = 0;
		if (!U32(Bits)) return false;
		FMemory::Memcpy(&Out, &Bits, sizeof(Out));
		return true;
	}
	bool Float(float& Out)
	{
		uint32 Bits = 0;
		return U32(Bits) && BitsToFloat(Bits, Out);
	}
	bool AtEnd() const { return Offset == Bytes.Num(); }
	int32 Position() const { return Offset; }

private:
	TConstArrayView<uint8> Bytes;
	int32 Offset = 0;
};

int32 SignExtend(uint32 Value, uint32 Bits)
{
	const uint32 Sign = 1u << (Bits - 1u);
	const uint32 Mask = (1u << Bits) - 1u;
	Value &= Mask;
	return static_cast<int32>((Value ^ Sign) - Sign);
}

void RotationSinCos(int32 Units, double& OutSin, double& OutCos)
{
	const uint32 Wrapped = static_cast<uint32>(Units) & 0xffffu;
	switch (Wrapped)
	{
	case 0: OutSin = 0.0; OutCos = 1.0; return;
	case 16384: OutSin = 1.0; OutCos = 0.0; return;
	case 32768: OutSin = 0.0; OutCos = -1.0; return;
	case 49152: OutSin = -1.0; OutCos = 0.0; return;
	default: break;
	}
	const double Angle = static_cast<double>(Wrapped)
		* (6.283185307179586476925286766559 / 65536.0);
	OutSin = FMath::Sin(Angle);
	OutCos = FMath::Cos(Angle);
}

FVector3f DecodePosition(uint32 Packed, const FVector3f& Origin,
	const FVector3f& Scale, const int32 RotationOrigin[3])
{
	double X = (static_cast<float>(SignExtend(Packed, 11)) - Origin.X) * Scale.X;
	double Y = (static_cast<float>(SignExtend(Packed >> 11, 11)) - Origin.Y) * Scale.Y;
	double Z = (static_cast<float>(SignExtend(Packed >> 22, 10)) - Origin.Z) * Scale.Z;
	double Sin = 0.0;
	double Cos = 1.0;
	if (RotationOrigin[2] != 0)
	{
		RotationSinCos(RotationOrigin[2], Sin, Cos);
		const double NewY = Y * Cos + Z * Sin;
		const double NewZ = -Y * Sin + Z * Cos;
		Y = NewY;
		Z = NewZ;
	}
	if (RotationOrigin[0] != 0)
	{
		RotationSinCos(RotationOrigin[0], Sin, Cos);
		const double NewX = X * Cos - Z * Sin;
		const double NewZ = X * Sin + Z * Cos;
		X = NewX;
		Z = NewZ;
	}
	if (RotationOrigin[1] != 0)
	{
		RotationSinCos(RotationOrigin[1], Sin, Cos);
		const double NewX = X * Cos - Y * Sin;
		const double NewY = X * Sin + Y * Cos;
		X = NewX;
		Y = NewY;
	}
	return FVector3f(static_cast<float>(X), static_cast<float>(Y),
		static_cast<float>(Z));
}

struct FSkin
{
	FString Package;
	FString Name;
	FString LogicalId;
};

struct FUV
{
	uint8 U = 0;
	uint8 V = 0;
};

struct FSection
{
	uint32 FirstTriangle = 0;
	uint32 TriangleCount = 0;
	uint32 PolyFlags = 0;
	int32 SkinIndex = INDEX_NONE;
	bool bHasFlags = false;
	bool bHasSkin = false;
	bool bAttachment = false;
};

int32 JamesBaseType(uint32 Flags)
{
	if ((Flags & PolyInvisible) != 0) return 8;
	if ((Flags & PolyModulated) != 0) return 4;
	if ((Flags & PolyTranslucent) != 0) return 2;
	if ((Flags & PolyMasked) != 0) return 3;
	if ((Flags & PolyTwoSided) != 0) return 1;
	return 0;
}

EWoTRuntimeVertexMeshStyle StyleForBaseType(int32 BaseType)
{
	switch (BaseType)
	{
	case 1: return EWoTRuntimeVertexMeshStyle::TwoSided;
	case 2: return EWoTRuntimeVertexMeshStyle::Translucent;
	case 3: return EWoTRuntimeVertexMeshStyle::Masked;
	case 4: return EWoTRuntimeVertexMeshStyle::Modulated;
	default: return EWoTRuntimeVertexMeshStyle::Normal;
	}
}

const TCHAR* SuffixForStyle(EWoTRuntimeVertexMeshStyle Style)
{
	switch (Style)
	{
	case EWoTRuntimeVertexMeshStyle::TwoSided: return TEXT("_2S");
	case EWoTRuntimeVertexMeshStyle::Masked: return TEXT("_M");
	case EWoTRuntimeVertexMeshStyle::Translucent: return TEXT("_T");
	case EWoTRuntimeVertexMeshStyle::Modulated: return TEXT("_Mod");
	default: return TEXT("");
	}
}

bool SplitMeshLogicalId(FStringView LogicalId, FString& OutPackage,
	FString& OutName)
{
	constexpr FStringView Prefix(TEXTVIEW("cache/mesh/"));
	if (!LogicalId.StartsWith(Prefix, ESearchCase::IgnoreCase)) return false;
	const FString Tail(LogicalId.RightChop(Prefix.Len()));
	const int32 Separator = Tail.Find(TEXT("__"), ESearchCase::CaseSensitive);
	if (Separator <= 0 || Separator + 2 >= Tail.Len()) return false;
	OutPackage = Tail.Left(Separator);
	OutName = Tail.Mid(Separator + 2);
	return true;
}

FString RuntimeMeshCacheKey(FStringView MeshLogicalId,
	FStringView AnimationLogicalId)
{
	return FString(MeshLogicalId).ToLower()
		+ TEXT("|") + FString(AnimationLogicalId).ToLower();
}

struct FCurvyWorkingTriangle
{
	uint32 Positions[3] = {};
	FVector2f UVs[3] = {};
	int32 SlotIndex = INDEX_NONE;
	uint32 PolyFlags = 0;
};

void EvaluateCurvyMidpoint(const FVector3d& PositionA,
	const FVector3d& PositionB, const FVector3d& NormalA,
	const FVector3d& NormalB, FVector3d& OutPosition,
	FVector3d& OutNormal)
{
	OutNormal = NormalA + NormalB;
	const double NormalLength = OutNormal.Length();
	if (NormalLength > 0.0)
	{
		OutNormal /= NormalLength;
	}
	else
	{
		OutNormal = FVector3d::ZeroVector;
	}
	const double Push = 0.15 * (PositionA - PositionB).Length()
		* FVector3d::CrossProduct(NormalA, NormalB).Length();
	OutPosition = (PositionA + PositionB) * 0.5 + OutNormal * Push;
}
}

const FWoTRuntimeVertexMeshPose* FWoTRuntimeVertexMeshData::GetFramePose(
	int32 Frame) const
{
	if (Frame == FrameCount) Frame = 0;
	if (Frame < 0 || Frame >= FrameCount || SourceVertexCount <= 0
		|| PackedFrames.Num() != SourceVertexCount * FrameCount)
	{
		return nullptr;
	}
	FScopeLock Lock(&PoseGuard);
	if (!FramePoses.IsValidIndex(Frame)) FramePoses.SetNum(FrameCount);
	if (FramePoses[Frame]) return FramePoses[Frame].Get();

	TUniquePtr<FWoTRuntimeVertexMeshPose> Pose =
		MakeUnique<FWoTRuntimeVertexMeshPose>();
	Pose->SourcePositions.SetNumUninitialized(SourceVertexCount);
	const int32 FrameBase = Frame * SourceVertexCount;
	for (int32 Vertex = 0; Vertex < SourceVertexCount; ++Vertex)
	{
		Pose->SourcePositions[Vertex] = DecodePosition(
			PackedFrames[FrameBase + Vertex], Origin, Scale, RotationOrigin);
	}

	TArray<FVector3d> SourceNormals;
	SourceNormals.SetNumZeroed(SourceVertexCount);
	for (const FWoTRuntimeVertexMeshTriangle& Triangle : Triangles)
	{
		const FVector3d A(Pose->SourcePositions[Triangle.SourceVertices[0]]);
		const FVector3d B(Pose->SourcePositions[Triangle.SourceVertices[1]]);
		const FVector3d C(Pose->SourcePositions[Triangle.SourceVertices[2]]);
		FVector3d Face = FVector3d::CrossProduct(B - A, C - A);
		Face /= FMath::Sqrt(Face.SizeSquared() + 0.001);
		for (const uint32 SourceVertex : Triangle.SourceVertices)
		{
			SourceNormals[SourceVertex] += Face;
		}
	}
	for (FVector3d& Normal : SourceNormals)
	{
		const double Length = Normal.Length();
		if (Length > 0.0) Normal /= Length;
	}

	TArray<FVector3d> ExtendedPositions;
	ExtendedPositions.Reserve(SourceVertexCount + DerivedParents.Num());
	for (const FVector3f& Position : Pose->SourcePositions)
	{
		ExtendedPositions.Add(FVector3d(Position));
	}
	TArray<FVector3d> ExtendedNormals = SourceNormals;
	ExtendedNormals.Reserve(SourceVertexCount + DerivedParents.Num());
	for (const FUintVector2& ParentPair : DerivedParents)
	{
		if (!ExtendedPositions.IsValidIndex(static_cast<int32>(ParentPair.X))
			|| !ExtendedPositions.IsValidIndex(static_cast<int32>(ParentPair.Y)))
		{
			return nullptr;
		}
		const FVector3d& ParentA = ExtendedPositions[ParentPair.X];
		const FVector3d& ParentB = ExtendedPositions[ParentPair.Y];
		const FVector3d& NormalA = ExtendedNormals[ParentPair.X];
		const FVector3d& NormalB = ExtendedNormals[ParentPair.Y];
		FVector3d MidpointPosition;
		FVector3d MidpointNormal;
		EvaluateCurvyMidpoint(ParentA, ParentB, NormalA, NormalB,
			MidpointPosition, MidpointNormal);
		ExtendedPositions.Add(MidpointPosition);
		ExtendedNormals.Add(MidpointNormal);
	}

	Pose->RenderPositions.SetNumUninitialized(RenderVertices.Num());
	Pose->RenderNormals.SetNumUninitialized(RenderVertices.Num());
	Pose->RenderTangents.SetNumUninitialized(RenderVertices.Num());
	for (int32 RenderIndex = 0; RenderIndex < RenderVertices.Num(); ++RenderIndex)
	{
		const int32 SourceVertex = static_cast<int32>(
			RenderVertices[RenderIndex].SourceVertex);
		if (!ExtendedPositions.IsValidIndex(SourceVertex)) return nullptr;
		Pose->RenderPositions[RenderIndex] = FVector3f(
			static_cast<float>(ExtendedPositions[SourceVertex].X),
			static_cast<float>(ExtendedPositions[SourceVertex].Y),
			static_cast<float>(ExtendedPositions[SourceVertex].Z));
		const FVector3f Normal(
			static_cast<float>(ExtendedNormals[SourceVertex].X),
			static_cast<float>(ExtendedNormals[SourceVertex].Y),
			static_cast<float>(ExtendedNormals[SourceVertex].Z));
		Pose->RenderNormals[RenderIndex] = Normal;
		const FVector3f Axis = FMath::Abs(Normal.X) < 0.9f
			? FVector3f(1, 0, 0) : FVector3f(0, 1, 0);
		Pose->RenderTangents[RenderIndex] = FVector3f::CrossProduct(
			Axis, Normal).GetSafeNormal();
	}
	FramePoses[Frame] = MoveTemp(Pose);
	return FramePoses[Frame].Get();
}

bool FWoTRuntimeVertexMeshData::BlendPose(const TMap<int32, float>& Weights,
	FWoTRuntimeVertexMeshPose& OutPose) const
{
	OutPose = FWoTRuntimeVertexMeshPose();
	if (FrameCount <= 0) return false;
	TMap<int32, float> Effective = Weights;
	if (Effective.IsEmpty()) Effective.Add(0, 1.0f);
	OutPose.SourcePositions.SetNumZeroed(SourceVertexCount);
	OutPose.RenderPositions.SetNumZeroed(RenderVertices.Num());
	OutPose.RenderNormals.SetNumZeroed(RenderVertices.Num());
	OutPose.RenderTangents.SetNumZeroed(RenderVertices.Num());
	for (const TPair<int32, float>& Pair : Effective)
	{
		if (!FMath::IsFinite(Pair.Value) || Pair.Value <= 0.0f) continue;
		const FWoTRuntimeVertexMeshPose* Frame = GetFramePose(Pair.Key);
		if (!Frame) return false;
		for (int32 Index = 0; Index < OutPose.SourcePositions.Num(); ++Index)
			OutPose.SourcePositions[Index] += Frame->SourcePositions[Index] * Pair.Value;
		for (int32 Index = 0; Index < OutPose.RenderPositions.Num(); ++Index)
		{
			OutPose.RenderPositions[Index] += Frame->RenderPositions[Index] * Pair.Value;
			OutPose.RenderNormals[Index] += Frame->RenderNormals[Index] * Pair.Value;
			OutPose.RenderTangents[Index] += Frame->RenderTangents[Index] * Pair.Value;
		}
	}
	for (int32 Index = 0; Index < OutPose.RenderNormals.Num(); ++Index)
	{
		OutPose.RenderNormals[Index].Normalize();
		FVector3f& Tangent = OutPose.RenderTangents[Index];
		Tangent -= OutPose.RenderNormals[Index]
			* FVector3f::DotProduct(OutPose.RenderNormals[Index], Tangent);
		if (!Tangent.Normalize())
		{
			const FVector3f Axis = FMath::Abs(OutPose.RenderNormals[Index].X) < 0.9f
				? FVector3f(1, 0, 0) : FVector3f(0, 1, 0);
			Tangent = FVector3f::CrossProduct(
				Axis, OutPose.RenderNormals[Index]).GetSafeNormal();
		}
	}
	return true;
}

TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr>
FWoTRuntimeVertexMesh::Decode(FWoTRuntimeVerifiedObject&& MeshObject,
	FWoTRuntimeVerifiedObject&& AnimationObject, FStringView MeshLogicalId,
	FStringView AnimationLogicalId)
{
	const FWoTCanonicalJsonValue& Header = MeshObject.Header();
	static const TCHAR* MeshFields[] = {
		TEXT("bounds"), TEXT("class_name"), TEXT("collision"),
		TEXT("encoding"), TEXT("fact_id"), TEXT("frame_count"),
		TEXT("importer"), TEXT("importer_version"), TEXT("kind"),
		TEXT("lod"), TEXT("origin_bits"), TEXT("payload_byte_length"),
		TEXT("render_vertices"), TEXT("rotation_origin"), TEXT("scale_bits"),
		TEXT("sections"), TEXT("skins"), TEXT("source"),
		TEXT("tangent_rule"), TEXT("triangle_count"), TEXT("triangles"),
		TEXT("uv_count"), TEXT("uvs"), TEXT("vertex_count")
	};
	uint32 VertexCount = 0;
	uint32 TriangleCount = 0;
	uint32 UVCount = 0;
	uint32 FrameCount = 0;
	uint32 ImporterVersion = 0;
	uint32 PayloadBytes = 0;
	FString ClassName;
	FString FactId;
	FString Importer;
	FString TangentRule;
	FVector3f Origin;
	FVector3f Scale;
	int32 Rotation[3] = {};
	const FWoTCanonicalJsonValue* Bounds = Header.FindMember(TEXT("bounds"));
	const FWoTCanonicalJsonValue* Collision = Header.FindMember(TEXT("collision"));
	const FWoTCanonicalJsonValue* RenderVertices =
		Header.FindMember(TEXT("render_vertices"));
	const FWoTCanonicalJsonValue* Sections = Header.FindMember(TEXT("sections"));
	const FWoTCanonicalJsonValue* Skins = Header.FindMember(TEXT("skins"));
	const FWoTCanonicalJsonValue* Triangles = Header.FindMember(TEXT("triangles"));
	const FWoTCanonicalJsonValue* UVs = Header.FindMember(TEXT("uvs"));
	const FWoTCanonicalJsonValue* Lod = Header.FindMember(TEXT("lod"));
	static const TCHAR* BoundsFields[] = {TEXT("max_bits"), TEXT("min_bits")};
	static const TCHAR* CollisionFields[] = {
		TEXT("triangle_indices"), TEXT("uses_base_frame")};
	FVector3f BoundsMin;
	FVector3f BoundsMax;
	bool bUsesBaseFrame = false;
	const FWoTCanonicalJsonValue* CollisionIndices = Collision
		? Collision->FindMember(TEXT("triangle_indices")) : nullptr;
	if (RequireExactMembers(Header, MakeArrayView(MeshFields),
			TEXT("vertex mesh header")).IsSet()
		|| !ReadString(Header, TEXT("class_name"), ClassName)
		|| !ReadString(Header, TEXT("fact_id"), FactId)
		|| !ReadString(Header, TEXT("importer"), Importer)
		|| !ReadString(Header, TEXT("tangent_rule"), TangentRule)
		|| !ReadU32(Header, TEXT("vertex_count"), VertexCount)
		|| !ReadU32(Header, TEXT("triangle_count"), TriangleCount)
		|| !ReadU32(Header, TEXT("uv_count"), UVCount)
		|| !ReadU32(Header, TEXT("frame_count"), FrameCount)
		|| !ReadU32(Header, TEXT("importer_version"), ImporterVersion)
		|| !ReadU32(Header, TEXT("payload_byte_length"), PayloadBytes)
		|| (ClassName != TEXT("Mesh") && ClassName != TEXT("LodMesh"))
		|| FactId.IsEmpty()
		|| Importer != TEXT("mesh-v1") || ImporterVersion != 1
		|| TangentRule != TEXT("unweighted-unit-face-all-triangles-corner-uv-v1")
		|| VertexCount == 0 || VertexCount > MaximumVertices
		|| TriangleCount > MaximumTriangles || UVCount > MaximumVertices * 3u
		|| FrameCount == 0 || FrameCount > MaximumFrames
		|| !ReadFloatBits3(Header, TEXT("origin_bits"), Origin)
		|| !ReadFloatBits3(Header, TEXT("scale_bits"), Scale)
		|| !ReadRotation3(Header, Rotation)
		|| !Bounds || RequireExactMembers(*Bounds, MakeArrayView(BoundsFields),
			TEXT("vertex mesh bounds")).IsSet()
		|| !ReadFloatBits3(*Bounds, TEXT("min_bits"), BoundsMin)
		|| !ReadFloatBits3(*Bounds, TEXT("max_bits"), BoundsMax)
		|| BoundsMin.X > BoundsMax.X || BoundsMin.Y > BoundsMax.Y
		|| BoundsMin.Z > BoundsMax.Z
		|| !Collision || RequireExactMembers(*Collision,
			MakeArrayView(CollisionFields), TEXT("vertex mesh collision")).IsSet()
		|| ReadBoolField(*Collision, TEXT("uses_base_frame"),
			TEXT("vertex mesh collision"), bUsesBaseFrame).IsSet()
		|| !bUsesBaseFrame || !CollisionIndices || !CollisionIndices->IsArray()
		|| !RenderVertices || !RenderVertices->IsArray()
		|| !Sections || !Sections->IsArray()
		|| !Skins || !Skins->IsArray()
		|| !Triangles || !Triangles->IsArray()
		|| !UVs || !UVs->IsArray()
		|| Triangles->Num() != static_cast<int32>(TriangleCount)
		|| UVs->Num() != static_cast<int32>(UVCount)
		|| Lod == nullptr || (!Lod->IsNull() && !Lod->IsObject())
		|| !ValidateSource(Header))
	{
		return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
			TEXT("vertex mesh header is invalid"));
	}
	const bool bHasLod = Lod->IsObject();
	if (bHasLod != (ClassName == TEXT("LodMesh")))
	{
		return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
			TEXT("vertex mesh class disagrees with its LOD facts"));
	}
	if (bHasLod)
	{
		static const TCHAR* LodFields[] = {TEXT("model_vertices"),
			TEXT("old_frame_vertices"), TEXT("remap_present"),
			TEXT("special_vertices")};
		uint32 ModelVertices = 0;
		uint32 OldFrameVertices = 0;
		uint32 SpecialVertices = 0;
		bool bRemapPresent = false;
		if (RequireExactMembers(*Lod, MakeArrayView(LodFields),
				TEXT("vertex mesh LOD facts")).IsSet()
			|| !ReadU32(*Lod, TEXT("model_vertices"), ModelVertices)
			|| !ReadU32(*Lod, TEXT("old_frame_vertices"), OldFrameVertices)
			|| !ReadU32(*Lod, TEXT("special_vertices"), SpecialVertices)
			|| ReadBoolField(*Lod, TEXT("remap_present"),
				TEXT("vertex mesh LOD facts"), bRemapPresent).IsSet()
			|| ModelVertices == 0 || OldFrameVertices == 0
			|| SpecialVertices > VertexCount
			|| ModelVertices + SpecialVertices != VertexCount)
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh LOD facts are invalid"));
		}
	}

	TArray<FSkin> ParsedSkins;
	ParsedSkins.Reserve(Skins->Num());
	for (int32 Index = 0; Index < Skins->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Skin = Skins->GetElement(Index);
		static const TCHAR* Fields[] = {
			TEXT("logical_id"), TEXT("name"), TEXT("package")};
		FSkin& Out = ParsedSkins.AddDefaulted_GetRef();
		bool bHasLogicalId = false;
		bool bHasName = false;
		bool bHasPackage = false;
		if (RequireExactMembers(Skin, MakeArrayView(Fields),
				TEXT("vertex mesh skin")).IsSet()
			|| !ReadOptionalString(Skin, TEXT("logical_id"), Out.LogicalId,
				bHasLogicalId)
			|| !ReadOptionalString(Skin, TEXT("name"), Out.Name, bHasName)
			|| !ReadOptionalString(Skin, TEXT("package"), Out.Package,
				bHasPackage)
			|| bHasLogicalId != bHasName || bHasName != bHasPackage
			|| (bHasName && (Out.Name.IsEmpty() || Out.Package.IsEmpty()
				|| Out.LogicalId.IsEmpty())))
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh skin table is invalid"));
		}
	}

	TArray<FUV> ParsedUVs;
	ParsedUVs.Reserve(UVs->Num());
	for (int32 Index = 0; Index < UVs->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& UV = UVs->GetElement(Index);
		static const TCHAR* Fields[] = {TEXT("u"), TEXT("v")};
		uint32 U = 0;
		uint32 V = 0;
		if (RequireExactMembers(UV, MakeArrayView(Fields),
				TEXT("vertex mesh uv")).IsSet()
			|| !ReadU32(UV, TEXT("u"), U) || !ReadU32(UV, TEXT("v"), V)
			|| U > 255 || V > 255)
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh UV table is invalid"));
		}
		ParsedUVs.Add({static_cast<uint8>(U), static_cast<uint8>(V)});
	}

	TArray<FSection> ParsedSections;
	ParsedSections.Reserve(Sections->Num());
	for (int32 Index = 0; Index < Sections->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Section = Sections->GetElement(Index);
		static const TCHAR* Fields[] = {
			TEXT("first_triangle"), TEXT("lod_attachment"), TEXT("poly_flags"),
			TEXT("skin_index"), TEXT("triangle_count")};
		uint32 SkinIndex = 0;
		bool bHasFlags = false;
		bool bHasSkin = false;
		FSection& Out = ParsedSections.AddDefaulted_GetRef();
		if (RequireExactMembers(Section, MakeArrayView(Fields),
				TEXT("vertex mesh section")).IsSet()
			|| !ReadU32(Section, TEXT("first_triangle"), Out.FirstTriangle)
			|| !ReadU32(Section, TEXT("triangle_count"), Out.TriangleCount)
			|| !ReadOptionalU32(Section, TEXT("poly_flags"),
				Out.PolyFlags, bHasFlags)
			|| !ReadOptionalU32(Section, TEXT("skin_index"),
				SkinIndex, bHasSkin)
			|| ReadBoolField(Section, TEXT("lod_attachment"),
				TEXT("vertex mesh section"), Out.bAttachment).IsSet()
			|| (bHasFlags && (Out.PolyFlags & ~SupportedPolyFlags) != 0)
			|| (bHasSkin && SkinIndex >= static_cast<uint32>(ParsedSkins.Num()))
			|| (Out.bAttachment != (!bHasFlags && !bHasSkin))
			|| static_cast<uint64>(Out.FirstTriangle) + Out.TriangleCount
				> TriangleCount)
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh section table is invalid"));
		}
		Out.SkinIndex = bHasSkin ? static_cast<int32>(SkinIndex) : INDEX_NONE;
		Out.bHasFlags = bHasFlags;
		Out.bHasSkin = bHasSkin;
	}

	TArray<FWoTRuntimeVertexMeshRenderVertex> ParsedRenderVertices;
	ParsedRenderVertices.Reserve(RenderVertices->Num());
	for (int32 Index = 0; Index < RenderVertices->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Vertex = RenderVertices->GetElement(Index);
		static const TCHAR* Fields[] = {
			TEXT("skin_index"), TEXT("source_vertex"), TEXT("uv_index")};
		uint32 SourceVertex = 0;
		uint32 UVIndex = 0;
		uint32 SkinIndex = 0;
		bool bHasUV = false;
		bool bHasSkin = false;
		if (RequireExactMembers(Vertex, MakeArrayView(Fields),
				TEXT("vertex mesh render vertex")).IsSet()
			|| !ReadU32(Vertex, TEXT("source_vertex"), SourceVertex)
			|| !ReadOptionalU32(Vertex, TEXT("uv_index"), UVIndex, bHasUV)
			|| !ReadOptionalU32(Vertex, TEXT("skin_index"), SkinIndex, bHasSkin)
			|| SourceVertex >= VertexCount || (bHasUV && UVIndex >= UVCount)
			|| (bHasSkin && SkinIndex >= static_cast<uint32>(ParsedSkins.Num())))
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh render vertex table is invalid"));
		}
		ParsedRenderVertices.Add({SourceVertex,
			bHasUV ? static_cast<int32>(UVIndex) : INDEX_NONE,
			bHasSkin ? static_cast<int32>(SkinIndex) : INDEX_NONE});
	}

	TArray<FWoTRuntimeVertexMeshTriangle> ParsedTriangles;
	ParsedTriangles.Reserve(Triangles->Num());
	for (int32 Index = 0; Index < Triangles->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Triangle = Triangles->GetElement(Index);
		static const TCHAR* Fields[] = {
			TEXT("indices"), TEXT("lod_attachment"), TEXT("poly_flags"),
			TEXT("render_indices"), TEXT("section_index"),
			TEXT("texture_index"), TEXT("uv_indices")};
		uint32 Source[3] = {};
		uint32 Render[3] = {};
		uint32 UVIndex[3] = {};
		uint32 SectionIndex = 0;
		uint32 TextureIndex = 0;
		uint32 PolyFlags = 0;
		bool bAttachment = false;
		bool bHasUV = false;
		bool bHasFlags = false;
		bool bHasTexture = false;
		if (RequireExactMembers(Triangle, MakeArrayView(Fields),
				TEXT("vertex mesh triangle")).IsSet()
			|| !ReadUnsignedArray3(Triangle, TEXT("indices"), Source)
			|| !ReadUnsignedArray3(Triangle, TEXT("render_indices"), Render)
			|| !ReadOptionalUnsignedArray3(Triangle, TEXT("uv_indices"),
				UVIndex, bHasUV)
			|| !ReadU32(Triangle, TEXT("section_index"), SectionIndex)
			|| !ReadOptionalU32(Triangle, TEXT("texture_index"),
				TextureIndex, bHasTexture)
			|| !ReadOptionalU32(Triangle, TEXT("poly_flags"),
				PolyFlags, bHasFlags)
			|| ReadBoolField(Triangle, TEXT("lod_attachment"),
				TEXT("vertex mesh triangle"), bAttachment).IsSet()
			|| SectionIndex >= static_cast<uint32>(ParsedSections.Num())
			|| (bHasTexture && TextureIndex >= static_cast<uint32>(ParsedSkins.Num()))
			|| (bHasFlags && (PolyFlags & ~SupportedPolyFlags) != 0)
			|| (bAttachment != (!bHasUV && !bHasFlags && !bHasTexture)))
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh triangle table is invalid"));
		}
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			if (Source[Corner] >= VertexCount
				|| Render[Corner] >= static_cast<uint32>(ParsedRenderVertices.Num())
				|| (bHasUV && UVIndex[Corner] >= UVCount)
				|| ParsedRenderVertices[Render[Corner]].SourceVertex != Source[Corner]
				|| (bHasUV && (ParsedRenderVertices[Render[Corner]].UVIndex < 0
					|| ParsedUVs[ParsedRenderVertices[Render[Corner]].UVIndex].U
						!= ParsedUVs[UVIndex[Corner]].U
					|| ParsedUVs[ParsedRenderVertices[Render[Corner]].UVIndex].V
						!= ParsedUVs[UVIndex[Corner]].V))
				|| (!bHasUV
					&& ParsedRenderVertices[Render[Corner]].UVIndex != INDEX_NONE))
			{
				return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
					TEXT("vertex mesh triangle references are invalid"));
			}
		}
		const FSection& Section = ParsedSections[SectionIndex];
		if (Index < static_cast<int32>(Section.FirstTriangle)
			|| Index >= static_cast<int32>(Section.FirstTriangle
				+ Section.TriangleCount)
			|| Section.bHasFlags != bHasFlags
			|| (bHasFlags && Section.PolyFlags != PolyFlags)
			|| Section.bHasSkin != bHasTexture
			|| (bHasTexture
				&& Section.SkinIndex != static_cast<int32>(TextureIndex))
			|| Section.bAttachment != bAttachment)
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh triangle disagrees with its section"));
		}
		FWoTRuntimeVertexMeshTriangle& Out = ParsedTriangles.AddDefaulted_GetRef();
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Out.SourceVertices[Corner] = Source[Corner];
			Out.RenderVertices[Corner] = Render[Corner];
			Out.UVIndices[Corner] = bHasUV
				? static_cast<int32>(UVIndex[Corner]) : INDEX_NONE;
		}
		Out.SectionIndex = static_cast<int32>(SectionIndex);
		Out.TextureIndex = bHasTexture ? static_cast<int32>(TextureIndex) : INDEX_NONE;
		Out.PolyFlags = PolyFlags;
		Out.bHasMaterial = bHasTexture && bHasFlags && bHasUV;
		Out.bLodAttachment = bAttachment;
	}

	for (int32 Index = 0; Index < CollisionIndices->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Value = CollisionIndices->GetElement(Index);
		if (Value.GetKind() != EWoTCanonicalJsonKind::Integer
			|| Value.GetInteger() < 0
			|| Value.GetInteger() >= static_cast<int64>(TriangleCount))
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh collision table is invalid"));
		}
	}

	FCursor MeshCursor(MeshObject.Payload());
	uint16 MeshVersion = 0;
	uint16 MeshFlags = 0;
	uint32 PayloadCounts[8] = {};
	FVector3f PayloadOrigin;
	FVector3f PayloadScale;
	int32 PayloadRotation[3] = {};
	FVector3f PayloadMin;
	FVector3f PayloadMax;
	if (MeshObject.Payload().Num() < 104
		|| FMemory::Memcmp(MeshObject.Payload().GetData(), "WOTMSH3", 8) != 0
		|| !MeshCursor.Skip(8) || !MeshCursor.U16(MeshVersion)
		|| !MeshCursor.U16(MeshFlags))
	{
		return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
			TEXT("WOTMSH3 preamble is invalid"));
	}
	for (uint32& Count : PayloadCounts)
	{
		if (!MeshCursor.U32(Count))
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("WOTMSH3 count table is truncated"));
	}
	if (!MeshCursor.Float(PayloadOrigin.X) || !MeshCursor.Float(PayloadOrigin.Y)
		|| !MeshCursor.Float(PayloadOrigin.Z) || !MeshCursor.Float(PayloadScale.X)
		|| !MeshCursor.Float(PayloadScale.Y) || !MeshCursor.Float(PayloadScale.Z)
		|| !MeshCursor.I32(PayloadRotation[0])
		|| !MeshCursor.I32(PayloadRotation[1])
		|| !MeshCursor.I32(PayloadRotation[2])
		|| !MeshCursor.Float(PayloadMin.X) || !MeshCursor.Float(PayloadMin.Y)
		|| !MeshCursor.Float(PayloadMin.Z) || !MeshCursor.Float(PayloadMax.X)
		|| !MeshCursor.Float(PayloadMax.Y) || !MeshCursor.Float(PayloadMax.Z)
		|| MeshVersion != 2 || (MeshFlags & ~1u) != 0
		|| ((MeshFlags & 1u) != 0) != bHasLod
		|| PayloadCounts[0] != VertexCount || PayloadCounts[1] != TriangleCount
		|| PayloadCounts[2] != UVCount
		|| PayloadCounts[3] != static_cast<uint32>(ParsedSections.Num())
		|| PayloadCounts[4] != static_cast<uint32>(ParsedSkins.Num())
		|| PayloadCounts[5] != static_cast<uint32>(ParsedRenderVertices.Num())
		|| PayloadCounts[6] != static_cast<uint32>(CollisionIndices->Num())
		|| PayloadCounts[7] != FrameCount || PayloadOrigin != Origin
		|| PayloadScale != Scale || FMemory::Memcmp(PayloadRotation, Rotation,
			sizeof(Rotation)) != 0 || PayloadMin != BoundsMin
		|| PayloadMax != BoundsMax || MeshObject.Payload().Num() != PayloadBytes)
	{
		return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
			TEXT("WOTMSH3 header disagrees with the signed header"));
	}

	TArray<uint32> BaseFrame;
	BaseFrame.SetNumUninitialized(VertexCount);
	for (uint32& Packed : BaseFrame)
	{
		if (!MeshCursor.U32(Packed)) return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
			TEXT("WOTMSH3 base frame is truncated"));
	}
	for (const FUV& UV : ParsedUVs)
	{
		uint16 PackedUV = 0;
		if (!MeshCursor.U16(PackedUV)
			|| static_cast<uint8>(PackedUV) != UV.U
			|| static_cast<uint8>(PackedUV >> 8) != UV.V)
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("WOTMSH3 UV table disagrees with the signed header"));
	}
	for (const FWoTRuntimeVertexMeshTriangle& Triangle : ParsedTriangles)
	{
		uint32 Source[3] = {};
		int32 UVIndex[3] = {};
		uint32 Render[3] = {};
		uint32 SectionIndex = 0;
		uint32 Available = 0;
		uint32 Flags = 0;
		uint32 Reserved = 0;
		int32 TextureIndex = 0;
		for (uint32& Value : Source) if (!MeshCursor.U32(Value)) goto MeshPayloadBad;
		for (int32& Value : UVIndex) if (!MeshCursor.I32(Value)) goto MeshPayloadBad;
		for (uint32& Value : Render) if (!MeshCursor.U32(Value)) goto MeshPayloadBad;
		const uint32 ExpectedAvailable = (Triangle.bHasMaterial ? 7u : 0u)
			| (Triangle.bLodAttachment ? 8u : 0u);
		if (!MeshCursor.U32(SectionIndex) || !MeshCursor.U32(Available)
			|| !MeshCursor.U32(Flags) || !MeshCursor.U32(Reserved)
			|| !MeshCursor.I32(TextureIndex)
			|| SectionIndex != static_cast<uint32>(Triangle.SectionIndex)
			|| Available != ExpectedAvailable
			|| Reserved != 0
			|| ((Available & 2u) != 0 && Flags != Triangle.PolyFlags)
			|| ((Available & 4u) != 0 && TextureIndex != Triangle.TextureIndex))
			goto MeshPayloadBad;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			if (Source[Corner] != Triangle.SourceVertices[Corner]
				|| Render[Corner] != Triangle.RenderVertices[Corner]
				|| UVIndex[Corner] != Triangle.UVIndices[Corner])
				goto MeshPayloadBad;
		}
	}
	for (const FSection& Section : ParsedSections)
	{
		uint32 First = 0;
		uint32 Count = 0;
		uint32 Available = 0;
		uint32 Flags = 0;
		int32 Skin = 0;
		const uint32 ExpectedAvailable = (Section.bHasFlags ? 1u : 0u)
			| (Section.bHasSkin ? 2u : 0u)
			| (Section.bAttachment ? 4u : 0u);
		if (!MeshCursor.U32(First) || !MeshCursor.U32(Count)
			|| !MeshCursor.U32(Available) || !MeshCursor.U32(Flags)
			|| !MeshCursor.I32(Skin) || First != Section.FirstTriangle
			|| Count != Section.TriangleCount
			|| Available != ExpectedAvailable
			|| (Section.bHasFlags && Flags != Section.PolyFlags)
			|| (Section.bHasSkin && Skin != Section.SkinIndex))
			goto MeshPayloadBad;
	}
	for (const FWoTRuntimeVertexMeshRenderVertex& Vertex : ParsedRenderVertices)
	{
		uint32 Source = 0;
		int32 UVIndex = 0;
		int32 SkinIndex = 0;
		if (!MeshCursor.U32(Source) || !MeshCursor.I32(UVIndex)
			|| !MeshCursor.I32(SkinIndex) || Source != Vertex.SourceVertex
			|| UVIndex != Vertex.UVIndex || SkinIndex != Vertex.SkinIndex)
			goto MeshPayloadBad;
	}
	for (int32 Index = 0; Index < CollisionIndices->Num(); ++Index)
	{
		uint32 TriangleIndex = 0;
		if (!MeshCursor.U32(TriangleIndex)
			|| TriangleIndex != static_cast<uint32>(
				CollisionIndices->GetElement(Index).GetInteger()))
			goto MeshPayloadBad;
	}
	if (!MeshCursor.AtEnd()) goto MeshPayloadBad;

	{
		const FWoTCanonicalJsonValue& AnimHeader = AnimationObject.Header();
		static const TCHAR* AnimFields[] = {
			TEXT("encoding"), TEXT("fact_id"), TEXT("frame_byte_length"),
			TEXT("frame_count"), TEXT("importer"), TEXT("importer_version"),
			TEXT("kind"), TEXT("payload_byte_length"), TEXT("sequence_count"),
			TEXT("sequences"), TEXT("source"), TEXT("vertex_count")};
		uint32 AnimVertices = 0;
		uint32 AnimFrames = 0;
		uint32 FrameByteLength = 0;
		uint32 SequenceCount = 0;
		uint32 AnimImporterVersion = 0;
		uint32 AnimPayloadBytes = 0;
		FString AnimFact;
		FString AnimImporter;
		const FWoTCanonicalJsonValue* SequenceArray =
			AnimHeader.FindMember(TEXT("sequences"));
		if (RequireExactMembers(AnimHeader, MakeArrayView(AnimFields),
				TEXT("animation header")).IsSet()
			|| !ReadString(AnimHeader, TEXT("fact_id"), AnimFact)
			|| !ReadString(AnimHeader, TEXT("importer"), AnimImporter)
			|| !ReadU32(AnimHeader, TEXT("importer_version"), AnimImporterVersion)
			|| !ReadU32(AnimHeader, TEXT("vertex_count"), AnimVertices)
			|| !ReadU32(AnimHeader, TEXT("frame_count"), AnimFrames)
			|| !ReadU32(AnimHeader, TEXT("frame_byte_length"), FrameByteLength)
			|| !ReadU32(AnimHeader, TEXT("sequence_count"), SequenceCount)
			|| !ReadU32(AnimHeader, TEXT("payload_byte_length"), AnimPayloadBytes)
			|| AnimFact.IsEmpty() || AnimImporter != TEXT("animation-v1")
			|| AnimImporterVersion != 1 || AnimVertices != VertexCount
			|| AnimFrames != FrameCount || FrameByteLength != VertexCount * 4u
			|| SequenceCount > 4096 || !SequenceArray || !SequenceArray->IsArray()
			|| SequenceArray->Num() != static_cast<int32>(SequenceCount)
			|| !ValidateSource(AnimHeader))
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("animation header is invalid for the vertex mesh"));
		}

		TArray<FWoTAnimSequence> ParsedSequences;
		ParsedSequences.Reserve(SequenceArray->Num());
		TSet<FName> SequenceNames;
		uint64 NotifyCount = 0;
		for (int32 Index = 0; Index < SequenceArray->Num(); ++Index)
		{
			const FWoTCanonicalJsonValue& Sequence = SequenceArray->GetElement(Index);
			static const TCHAR* Fields[] = {TEXT("first_frame"), TEXT("group"),
				TEXT("name"), TEXT("notifies"), TEXT("num_frames"), TEXT("rate_bits")};
			FString Name;
			FString Group;
			uint32 First = 0;
			uint32 Count = 0;
			uint32 RateBits = 0;
			const FWoTCanonicalJsonValue* Notifies = Sequence.FindMember(TEXT("notifies"));
			if (RequireExactMembers(Sequence, MakeArrayView(Fields),
					TEXT("animation sequence")).IsSet()
				|| !ReadString(Sequence, TEXT("name"), Name)
				|| !ReadString(Sequence, TEXT("group"), Group)
				|| !ReadU32(Sequence, TEXT("first_frame"), First)
				|| !ReadU32(Sequence, TEXT("num_frames"), Count)
				|| !ReadU32(Sequence, TEXT("rate_bits"), RateBits)
				|| Name.IsEmpty() || Count == 0 || First > MAX_int32
				|| Count > MAX_int32
				|| static_cast<uint64>(First) + Count
					> static_cast<uint64>(FrameCount) + 1u
				|| !Notifies || !Notifies->IsArray()
				|| NotifyCount + Notifies->Num() > 65536)
			{
				return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
					TEXT("animation sequence table is invalid"));
			}
			FWoTAnimSequence& Out = ParsedSequences.AddDefaulted_GetRef();
			Out.Name = FName(*Name);
			Out.Group = FName(*Group);
			Out.StartFrame = static_cast<int32>(First);
			Out.NumFrames = static_cast<int32>(Count);
			if (SequenceNames.Contains(Out.Name)
				|| !BitsToFloat(RateBits, Out.Rate) || Out.Rate <= 0.0f)
			{
				return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
					TEXT("animation sequence names or rates are invalid"));
			}
			SequenceNames.Add(Out.Name);
			NotifyCount += Notifies->Num();
			for (int32 NotifyIndex = 0; NotifyIndex < Notifies->Num(); ++NotifyIndex)
			{
				const FWoTCanonicalJsonValue& Notify = Notifies->GetElement(NotifyIndex);
				static const TCHAR* NotifyFields[] = {TEXT("function"), TEXT("time_bits")};
				FString Function;
				uint32 TimeBits = 0;
				FWoTAnimNotify& OutNotify = Out.Notifies.AddDefaulted_GetRef();
				if (RequireExactMembers(Notify, MakeArrayView(NotifyFields),
						TEXT("animation notify")).IsSet()
					|| !ReadString(Notify, TEXT("function"), Function)
					|| !ReadU32(Notify, TEXT("time_bits"), TimeBits)
					|| Function.IsEmpty()
					|| !BitsToFloat(TimeBits, OutNotify.Fraction)
					|| OutNotify.Fraction < 0.0f || OutNotify.Fraction > 1.0f)
				{
					return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
						TEXT("animation notify table is invalid"));
				}
				OutNotify.Function = FName(*Function);
			}
		}

		FCursor AnimCursor(AnimationObject.Payload());
		uint16 AnimVersion = 0;
		uint16 AnimFlags = 0;
		uint32 PayloadVertices = 0;
		uint32 PayloadFrames = 0;
		uint32 PackedCount = 0;
		uint32 PayloadFrameBytes = 0;
		const uint64 ExpectedPacked = static_cast<uint64>(VertexCount) * FrameCount;
		if (AnimationObject.Payload().Num() < 28
			|| FMemory::Memcmp(AnimationObject.Payload().GetData(), "WOTANI3", 8) != 0
			|| !AnimCursor.Skip(8) || !AnimCursor.U16(AnimVersion)
			|| !AnimCursor.U16(AnimFlags) || !AnimCursor.U32(PayloadVertices)
			|| !AnimCursor.U32(PayloadFrames) || !AnimCursor.U32(PackedCount)
			|| !AnimCursor.U32(PayloadFrameBytes) || AnimVersion != 1
			|| AnimFlags != 0 || PayloadVertices != VertexCount
			|| PayloadFrames != FrameCount || PackedCount != ExpectedPacked
			|| PayloadFrameBytes != FrameByteLength
			|| AnimationObject.Payload().Num() != AnimPayloadBytes
			|| static_cast<uint64>(AnimationObject.Payload().Num())
				!= 28ull + ExpectedPacked * 4ull)
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("WOTANI3 header is invalid"));
		}

		TSharedPtr<FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe> Data =
			MakeShared<FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe>();
		Data->PackedFrames.SetNumUninitialized(static_cast<int32>(ExpectedPacked));
		for (uint32& Packed : Data->PackedFrames)
		{
			if (!AnimCursor.U32(Packed))
				return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
					TEXT("WOTANI3 frame data is truncated"));
		}
		if (!AnimCursor.AtEnd()
			|| !Data->PackedFrames.IsValidIndex(static_cast<int32>(VertexCount) - 1)
			|| FMemory::Memcmp(BaseFrame.GetData(), Data->PackedFrames.GetData(),
				static_cast<SIZE_T>(VertexCount) * sizeof(uint32)) != 0)
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("mesh base frame does not match animation frame zero"));
		}

		FString MeshPackage;
		FString MeshName;
		if (!SplitMeshLogicalId(MeshLogicalId, MeshPackage, MeshName))
		{
			return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
				TEXT("vertex mesh logical id is invalid"));
		}
		Data->MeshLogicalId = FString(MeshLogicalId);
		Data->AnimationLogicalId = FString(AnimationLogicalId);
		Data->MeshPackage = FName(*MeshPackage);
		Data->MeshName = FName(*MeshName);
		Data->SourceVertexCount = static_cast<int32>(VertexCount);
		Data->FrameCount = static_cast<int32>(FrameCount);
		Data->bLodMesh = ClassName == TEXT("LodMesh");
		Data->Origin = Origin;
		Data->Scale = Scale;
		FMemory::Memcpy(Data->RotationOrigin, Rotation, sizeof(Rotation));
		Data->Bounds = FBox(FVector(BoundsMin), FVector(BoundsMax));
		Data->Triangles = MoveTemp(ParsedTriangles);
		Data->RenderVertices = MoveTemp(ParsedRenderVertices);
		Data->RenderUVs.SetNumUninitialized(Data->RenderVertices.Num());
		for (int32 Index = 0; Index < Data->RenderVertices.Num(); ++Index)
		{
			const int32 UVIndex = Data->RenderVertices[Index].UVIndex;
			if (ParsedUVs.IsValidIndex(UVIndex))
			{
				const FUV& UV = ParsedUVs[UVIndex];
				Data->RenderUVs[Index] = FVector2f(UV.U / 256.0f, UV.V / 256.0f);
			}
			else
			{
				Data->RenderUVs[Index] = FVector2f::ZeroVector;
			}
		}
		Data->Sequences = MoveTemp(ParsedSequences);

		struct FSlotKey
		{
			int32 Texture = INDEX_NONE;
			int32 BaseType = 0;
			bool bUnlit = false;
			bool operator==(const FSlotKey& Other) const
			{
				return Texture == Other.Texture && BaseType == Other.BaseType
					&& bUnlit == Other.bUnlit;
			}
		};
		TArray<FSlotKey> Keys;
		for (const FWoTRuntimeVertexMeshTriangle& Triangle : Data->Triangles)
		{
			if (!Triangle.bHasMaterial || Triangle.bLodAttachment
				|| (Triangle.PolyFlags & PolyInvisible) != 0)
				continue;
			FSlotKey Key{Triangle.TextureIndex, JamesBaseType(Triangle.PolyFlags),
				(Triangle.PolyFlags & PolyUnlit) != 0};
			Keys.AddUnique(Key);
		}
		Keys.Sort([](const FSlotKey& A, const FSlotKey& B)
		{
			if (A.Texture != B.Texture) return A.Texture < B.Texture;
			if (A.BaseType != B.BaseType) return A.BaseType < B.BaseType;
			return A.bUnlit < B.bUnlit;
		});
		for (const FSlotKey& Key : Keys)
		{
			FWoTRuntimeVertexMeshSlot& Slot = Data->Slots.AddDefaulted_GetRef();
			Slot.TextureIndex = Key.Texture;
			Slot.Style = StyleForBaseType(Key.BaseType);
			Slot.bUnlit = Key.bUnlit;
			if (ParsedSkins.IsValidIndex(Key.Texture))
			{
				const FSkin& Skin = ParsedSkins[Key.Texture];
				Slot.TexturePackage = Skin.Package;
				Slot.TextureName = Skin.Name;
				Slot.TextureLogicalId = Skin.LogicalId;
			}
			const FString BaseName = Slot.TextureName.IsEmpty()
				? FString::Printf(TEXT("tex%d"), Key.Texture)
				: Slot.TextureName;
			Slot.Name = FName(*(BaseName + SuffixForStyle(Slot.Style)));
		}
		for (const FWoTRuntimeVertexMeshTriangle& Triangle : Data->Triangles)
		{
			if (!Triangle.bHasMaterial || Triangle.bLodAttachment
				|| (Triangle.PolyFlags & PolyInvisible) != 0)
				continue;
			const FSlotKey Key{Triangle.TextureIndex, JamesBaseType(Triangle.PolyFlags),
				(Triangle.PolyFlags & PolyUnlit) != 0};
			const int32 SlotIndex = Keys.IndexOfByKey(Key);
			check(Data->Slots.IsValidIndex(SlotIndex));
			Data->Slots[SlotIndex].Indices.Append({Triangle.RenderVertices[2],
				Triangle.RenderVertices[1], Triangle.RenderVertices[0]});
		}
		FWoTRuntimeVertexMeshDataPtr Result = Data;
		return TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr>::Success(
			MoveTemp(Result));
	}

MeshPayloadBad:
	return RuntimeMeshFailure<FWoTRuntimeVertexMeshDataPtr>(
		TEXT("WOTMSH3 tables disagree with the signed header"));
}

#if WITH_AUTOMATION_TESTS
FWoTRuntimeVertexMeshDataPtr FWoTRuntimeVertexMesh::MakeSyntheticForTesting(
	TConstArrayView<FIntVector> SourcePositions,
	TConstArrayView<FIntVector> SourceTriangles,
	TConstArrayView<uint32> RenderSourceVertices)
{
	if (SourcePositions.IsEmpty() || SourceTriangles.IsEmpty()
		|| RenderSourceVertices.Num() != SourceTriangles.Num() * 3)
	{
		return nullptr;
	}
	TSharedPtr<FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe> Data =
		MakeShared<FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe>();
	Data->MeshLogicalId = TEXT("test/mesh/ClosedSynthetic");
	Data->AnimationLogicalId = TEXT("test/animation/ClosedSynthetic");
	Data->MeshName = TEXT("ClosedSynthetic");
	Data->MeshPackage = TEXT("test");
	Data->SourceVertexCount = SourcePositions.Num();
	Data->FrameCount = 1;
	Data->Origin = FVector3f::ZeroVector;
	Data->Scale = FVector3f(1.0f);
	Data->Bounds = FBox(ForceInit);
	Data->PackedFrames.Reserve(SourcePositions.Num());
	for (const FIntVector& Position : SourcePositions)
	{
		if (Position.X < -1024 || Position.X > 1023
			|| Position.Y < -1024 || Position.Y > 1023
			|| Position.Z < -512 || Position.Z > 511)
		{
			return nullptr;
		}
		Data->PackedFrames.Add(
			(static_cast<uint32>(Position.X) & 0x7ffu)
			| ((static_cast<uint32>(Position.Y) & 0x7ffu) << 11u)
			| ((static_cast<uint32>(Position.Z) & 0x3ffu) << 22u));
		Data->Bounds += FVector(Position);
	}
	Data->RenderVertices.Reserve(RenderSourceVertices.Num());
	Data->RenderUVs.SetNumZeroed(RenderSourceVertices.Num());
	for (const uint32 SourceVertex : RenderSourceVertices)
	{
		if (SourceVertex >= static_cast<uint32>(SourcePositions.Num()))
		{
			return nullptr;
		}
		Data->RenderVertices.Add({SourceVertex, INDEX_NONE, 0});
	}
	FWoTRuntimeVertexMeshSlot& Slot = Data->Slots.AddDefaulted_GetRef();
	Slot.Name = TEXT("Synthetic");
	Slot.TextureIndex = 0;
	Slot.Indices.Reserve(RenderSourceVertices.Num());
	for (int32 TriangleIndex = 0;
		TriangleIndex < SourceTriangles.Num(); ++TriangleIndex)
	{
		const FIntVector& Source = SourceTriangles[TriangleIndex];
		if (Source.X < 0 || Source.Y < 0 || Source.Z < 0
			|| Source.X >= SourcePositions.Num()
			|| Source.Y >= SourcePositions.Num()
			|| Source.Z >= SourcePositions.Num())
		{
			return nullptr;
		}
		FWoTRuntimeVertexMeshTriangle& Triangle =
			Data->Triangles.AddDefaulted_GetRef();
		Triangle.SourceVertices[0] = static_cast<uint32>(Source.X);
		Triangle.SourceVertices[1] = static_cast<uint32>(Source.Y);
		Triangle.SourceVertices[2] = static_cast<uint32>(Source.Z);
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const uint32 RenderIndex = TriangleIndex * 3 + Corner;
			Triangle.RenderVertices[Corner] = RenderIndex;
			Slot.Indices.Add(RenderIndex);
		}
		Triangle.SectionIndex = 0;
		Triangle.TextureIndex = 0;
		Triangle.bHasMaterial = true;
	}
	return Data;
}
#endif

bool FWoTRuntimeVertexMesh::ResolveLogicalIds(FStringView MeshName,
	FName PackageHint, TConstArrayView<FWoTNeutralResourceId> Resources,
	FString& OutMeshLogicalId, FString& OutAnimationLogicalId,
	FString* OutFailure)
{
	OutMeshLogicalId.Empty();
	OutAnimationLogicalId.Empty();
	FString Requested(MeshName);
	Requested.ReplaceInline(TEXT("\\"), TEXT("/"));
	int32 Slash = INDEX_NONE;
	if (Requested.FindLastChar(TEXT('/'), Slash)) Requested.RightChopInline(Slash + 1);
	int32 Dot = INDEX_NONE;
	if (Requested.FindLastChar(TEXT('.'), Dot)) Requested.RightChopInline(Dot + 1);
	TArray<const FWoTNeutralResourceId*> Matches;
	for (const FWoTNeutralResourceId& Resource : Resources)
	{
		if (Resource.Kind != EWoTNeutralResourceKind::VertexMesh) continue;
		FString Package;
		FString Name;
		if (!SplitMeshLogicalId(Resource.LogicalId, Package, Name)
			|| !Name.Equals(Requested, ESearchCase::IgnoreCase)
			|| (!PackageHint.IsNone()
				&& !Package.Equals(PackageHint.ToString(), ESearchCase::IgnoreCase)))
			continue;
		Matches.Add(&Resource);
	}
	if (Matches.Num() != 1)
	{
		if (OutFailure)
		{
			*OutFailure = Matches.IsEmpty()
				? TEXT("vertex mesh name was not found")
				: TEXT("vertex mesh name is ambiguous; supply a package hint");
		}
		return false;
	}
	OutMeshLogicalId = Matches[0]->LogicalId;
	OutAnimationLogicalId = OutMeshLogicalId;
	OutAnimationLogicalId.RemoveFromStart(TEXT("cache/mesh/"));
	OutAnimationLogicalId = TEXT("cache/animation/") + OutAnimationLogicalId;
	return true;
}

FWoTRuntimeVertexMeshDataPtr FWoTRuntimeVertexMesh::MakeCurvy(
	const FWoTRuntimeVertexMeshDataPtr& Source, int32 Levels)
{
	if (!Source.IsValid() || Levels < 0 || Levels > 3) return nullptr;
	if (Levels == 0) return Source;
	if (Source->CurvyLevels != 0)
	{
		return Source->CurvyLevels == Levels ? Source : nullptr;
	}

	const FString BaseCacheKey = RuntimeMeshCacheKey(
		Source->MeshLogicalId, Source->AnimationLogicalId);
	const FString CurvyCacheKey = FString::Printf(
		TEXT("%s|curvy%d"), *BaseCacheKey, Levels);
	if (const FWoTRuntimeVertexMeshDataPtr* Existing =
		RuntimeMeshCache.Find(CurvyCacheKey))
	{
		return *Existing;
	}

	TSharedPtr<FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe> Curvy =
		MakeShared<FWoTRuntimeVertexMeshData, ESPMode::ThreadSafe>();
	Curvy->MeshLogicalId = Source->MeshLogicalId;
	Curvy->AnimationLogicalId = Source->AnimationLogicalId;
	Curvy->MeshName = Source->MeshName;
	Curvy->MeshPackage = Source->MeshPackage;
	Curvy->SourceVertexCount = Source->SourceVertexCount;
	Curvy->FrameCount = Source->FrameCount;
	Curvy->CurvyLevels = Levels;
	Curvy->bLodMesh = Source->bLodMesh;
	Curvy->Origin = Source->Origin;
	Curvy->Scale = Source->Scale;
	FMemory::Memcpy(Curvy->RotationOrigin, Source->RotationOrigin,
		sizeof(Curvy->RotationOrigin));
	Curvy->Triangles = Source->Triangles;
	Curvy->Slots = Source->Slots;
	Curvy->Sequences = Source->Sequences;
	Curvy->PackedFrames = Source->PackedFrames;
	for (FWoTRuntimeVertexMeshSlot& Slot : Curvy->Slots)
	{
		Slot.Indices.Reset();
	}

	TArray<FCurvyWorkingTriangle> WorkingTriangles;
	WorkingTriangles.Reserve(Source->Triangles.Num());
	for (const FWoTRuntimeVertexMeshTriangle& Triangle : Source->Triangles)
	{
		if (!Triangle.bHasMaterial || Triangle.bLodAttachment
			|| (Triangle.PolyFlags & PolyInvisible) != 0)
		{
			continue;
		}
		const int32 BaseType = JamesBaseType(Triangle.PolyFlags);
		const int32 SlotIndex = Source->Slots.IndexOfByPredicate(
			[&Triangle, BaseType](const FWoTRuntimeVertexMeshSlot& Slot)
			{
				return Slot.TextureIndex == Triangle.TextureIndex
					&& Slot.Style == StyleForBaseType(BaseType)
					&& Slot.bUnlit == ((Triangle.PolyFlags & PolyUnlit) != 0);
			});
		if (!Source->Slots.IsValidIndex(SlotIndex)) return nullptr;
		FCurvyWorkingTriangle& Working = WorkingTriangles.AddDefaulted_GetRef();
		Working.SlotIndex = SlotIndex;
		Working.PolyFlags = Triangle.PolyFlags;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Working.Positions[Corner] = Triangle.SourceVertices[Corner];
			const int32 RenderIndex = static_cast<int32>(
				Triangle.RenderVertices[Corner]);
			if (!Source->RenderUVs.IsValidIndex(RenderIndex)) return nullptr;
			Working.UVs[Corner] = Source->RenderUVs[RenderIndex];
		}
	}

	uint32 NextPosition = static_cast<uint32>(Source->SourceVertexCount);
	for (int32 Level = 0; Level < Levels; ++Level)
	{
		TMap<uint64, uint32> EdgePositions;
		TArray<FCurvyWorkingTriangle> Children;
		Children.Reserve(WorkingTriangles.Num() * 4);
		for (const FCurvyWorkingTriangle& Triangle : WorkingTriangles)
		{
			if ((Triangle.PolyFlags & PolyFlat) != 0)
			{
				Children.Add(Triangle);
				continue;
			}
			uint32 MidpointIndices[3] = {};
			FVector2f MidpointUVs[3] = {};
			static constexpr int32 EdgeCorners[3][2] = {
				{0, 1}, {1, 2}, {2, 0}};
			for (int32 EdgeIndex = 0; EdgeIndex < 3; ++EdgeIndex)
			{
				const int32 FirstCorner = EdgeCorners[EdgeIndex][0];
				const int32 SecondCorner = EdgeCorners[EdgeIndex][1];
				const uint32 First = FMath::Min(
					Triangle.Positions[FirstCorner],
					Triangle.Positions[SecondCorner]);
				const uint32 Second = FMath::Max(
					Triangle.Positions[FirstCorner],
					Triangle.Positions[SecondCorner]);
				const uint64 EdgeKey = (static_cast<uint64>(First) << 32)
					| static_cast<uint64>(Second);
				uint32* Existing = EdgePositions.Find(EdgeKey);
				if (Existing)
				{
					MidpointIndices[EdgeIndex] = *Existing;
				}
				else
				{
					MidpointIndices[EdgeIndex] = NextPosition++;
					EdgePositions.Add(EdgeKey, MidpointIndices[EdgeIndex]);
					Curvy->DerivedParents.Add(FUintVector2(First, Second));
				}
				MidpointUVs[EdgeIndex] =
					(Triangle.UVs[FirstCorner] + Triangle.UVs[SecondCorner])
					* 0.5f;
			}

			auto AddChild = [&Children, &Triangle](uint32 A, uint32 B, uint32 C,
				const FVector2f& UVA, const FVector2f& UVB,
				const FVector2f& UVC)
			{
				FCurvyWorkingTriangle Child = Triangle;
				Child.Positions[0] = A;
				Child.Positions[1] = B;
				Child.Positions[2] = C;
				Child.UVs[0] = UVA;
				Child.UVs[1] = UVB;
				Child.UVs[2] = UVC;
				Children.Add(MoveTemp(Child));
			};
			AddChild(Triangle.Positions[0], MidpointIndices[0], MidpointIndices[2],
				Triangle.UVs[0], MidpointUVs[0], MidpointUVs[2]);
			AddChild(MidpointIndices[0], Triangle.Positions[1], MidpointIndices[1],
				MidpointUVs[0], Triangle.UVs[1], MidpointUVs[1]);
			AddChild(MidpointIndices[2], MidpointIndices[1], Triangle.Positions[2],
				MidpointUVs[2], MidpointUVs[1], Triangle.UVs[2]);
			AddChild(MidpointIndices[0], MidpointIndices[1], MidpointIndices[2],
				MidpointUVs[0], MidpointUVs[1], MidpointUVs[2]);
		}
		WorkingTriangles = MoveTemp(Children);
	}

	Curvy->RenderVertices.Reserve(WorkingTriangles.Num() * 3);
	Curvy->RenderUVs.Reserve(WorkingTriangles.Num() * 3);
	for (const FCurvyWorkingTriangle& Triangle : WorkingTriangles)
	{
		if (!Curvy->Slots.IsValidIndex(Triangle.SlotIndex)) return nullptr;
		const uint32 FirstRenderVertex = static_cast<uint32>(
			Curvy->RenderVertices.Num());
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Curvy->RenderVertices.Add({Triangle.Positions[Corner], INDEX_NONE,
				Curvy->Slots[Triangle.SlotIndex].TextureIndex});
			Curvy->RenderUVs.Add(Triangle.UVs[Corner]);
		}
		Curvy->Slots[Triangle.SlotIndex].Indices.Append({
			FirstRenderVertex + 2, FirstRenderVertex + 1, FirstRenderVertex});
	}

	FBox ComputedBounds(ForceInit);
	for (int32 FrameIndex = 0; FrameIndex < Curvy->FrameCount; ++FrameIndex)
	{
		const FWoTRuntimeVertexMeshPose* FramePose = Curvy->GetFramePose(FrameIndex);
		if (!FramePose) return nullptr;
		for (const FVector3f& Position : FramePose->RenderPositions)
		{
			ComputedBounds += FVector(Position);
		}
	}
	if (!ComputedBounds.IsValid) return nullptr;
	Curvy->Bounds = ComputedBounds;
	FWoTRuntimeVertexMeshDataPtr Result = Curvy;
	RuntimeMeshCache.Add(CurvyCacheKey, Result);
	return Result;
}

FWoTRuntimeVertexMeshDataPtr FWoTRuntimeVertexMesh::Load(
	FStringView MeshLogicalId, FStringView AnimationLogicalId)
{
	if (!IsWoTPayloadBootstrapActive()) return nullptr;
	const FString CacheKey = RuntimeMeshCacheKey(
		MeshLogicalId, AnimationLogicalId);
	if (const FWoTRuntimeVertexMeshDataPtr* Existing = RuntimeMeshCache.Find(CacheKey))
		return *Existing;
	if (RejectedRuntimeMeshes.Contains(CacheKey)) return nullptr;
	auto MeshRead = FWoTRuntimeRegistry::Get().ReadObject(
		{FString(MeshLogicalId), EWoTNeutralResourceKind::VertexMesh},
		512ull * 1024ull * 1024ull, TEXT("ue1-vertex-mesh-le-v2"), TEXT("mesh"));
	auto AnimRead = FWoTRuntimeRegistry::Get().ReadObject(
		{FString(AnimationLogicalId), EWoTNeutralResourceKind::Animation},
		512ull * 1024ull * 1024ull,
		TEXT("ue1-packed-vertex-frames-le-v1"), TEXT("animation"));
	if (MeshRead.IsError() || AnimRead.IsError())
	{
		RejectedRuntimeMeshes.Add(CacheKey);
		return nullptr;
	}
	auto Decoded = Decode(MoveTemp(MeshRead).GetValue(),
		MoveTemp(AnimRead).GetValue(), MeshLogicalId, AnimationLogicalId);
	if (Decoded.IsError())
	{
		RejectedRuntimeMeshes.Add(CacheKey);
		return nullptr;
	}
	RuntimeMeshCache.Add(CacheKey, Decoded.GetValue());
	return Decoded.GetValue();
}

FWoTRuntimeVertexMeshDataPtr FWoTRuntimeVertexMesh::ResolveAndLoad(
	FStringView MeshName, FName PackageHint)
{
#if WOT_GUARDED_BOOTSTRAP
	return ResolveAndLoadInternal(MeshName, PackageHint);
#else
	return nullptr;
#endif
}

FWoTRuntimeVertexMeshDataPtr FWoTRuntimeVertexMesh::ResolveAndLoadInternal(
	FStringView MeshName, FName PackageHint)
{
	FString MeshId;
	FString AnimationId;
	const TArray<FWoTNeutralResourceId> Resources =
		FWoTRuntimeRegistry::Get().ListResources(EWoTNeutralResourceKind::VertexMesh);
	return ResolveLogicalIds(MeshName, PackageHint, Resources, MeshId, AnimationId)
		? Load(MeshId, AnimationId) : nullptr;
}

void FWoTRuntimeVertexMesh::ResetRuntimeCache()
{
	RuntimeMeshCache.Empty();
	RejectedRuntimeMeshes.Empty();
}

#if WITH_AUTOMATION_TESTS
FWoTRuntimeVertexMeshDataPtr FWoTRuntimeVertexMesh::ResolveAndLoadForTesting(
	FStringView MeshName, FName PackageHint)
{
	return ResolveAndLoadInternal(MeshName, PackageHint);
}

void FWoTRuntimeVertexMesh::EvaluateCurvyMidpointForTesting(
	const FVector3d& PositionA, const FVector3d& PositionB,
	const FVector3d& NormalA, const FVector3d& NormalB,
	FVector3d& OutPosition, FVector3d& OutNormal)
{
	EvaluateCurvyMidpoint(PositionA, PositionB, NormalA, NormalB,
		OutPosition, OutNormal);
}

TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr>
FWoTRuntimeVertexMesh::DecodeObjectBytesForTesting(TArray<uint8> MeshObject,
	TArray<uint8> AnimationObject, FStringView MeshLogicalId,
	FStringView AnimationLogicalId)
{
	auto MeshRead = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
		MoveTemp(MeshObject), TEXT("ue1-vertex-mesh-le-v2"), TEXT("mesh"));
	if (MeshRead.IsError())
		return TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr>::Error(
			MoveTemp(MeshRead).GetError());
	auto AnimRead = FWoTRuntimeRegistry::DecodeObjectBytesForTesting(
		MoveTemp(AnimationObject), TEXT("ue1-packed-vertex-frames-le-v1"),
		TEXT("animation"));
	if (AnimRead.IsError())
		return TWoTBootstrapResult<FWoTRuntimeVertexMeshDataPtr>::Error(
			MoveTemp(AnimRead).GetError());
	return Decode(MoveTemp(MeshRead).GetValue(), MoveTemp(AnimRead).GetValue(),
		MeshLogicalId, AnimationLogicalId);
}
#endif
