#include "WoTAnimData.h"

#include "Mesh/WoTRuntimeVertexMesh.h"
#include "Payload/WoTPayloadActivation.h"
#include "Payload/WoTRuntimeRegistry.h"

namespace
{
#if !WOT_GUARDED_BOOTSTRAP
	UDataTable* LoadAnimTable()
	{
		static const FSoftObjectPath TablePath(
			TEXT("/Game/WoT/Anim/DT_WoTAnimSequences.DT_WoTAnimSequences"));
		return Cast<UDataTable>(TablePath.TryLoad());
	}
#else
	TMap<FString, TArray<FWoTAnimSequence>> WoTAnimDataRuntimeRows;
	TSet<FString> WoTAnimDataRejectedRows;

	bool WoTAnimDataReadString(const FWoTCanonicalJsonValue& Object,
		const TCHAR* Field, FString& Out)
	{
		return !ReadStringField(
			Object, Field, TEXT("animation header"), Out).IsSet();
	}

	bool WoTAnimDataReadU32(const FWoTCanonicalJsonValue& Object,
		const TCHAR* Field, uint32& Out)
	{
		return !ReadUnsigned32Field(
			Object, Field, TEXT("animation header"), Out).IsSet();
	}

	bool WoTAnimDataBitsToFloat(uint32 Bits, float& Out)
	{
		FMemory::Memcpy(&Out, &Bits, sizeof(Out));
		return FMath::IsFinite(Out);
	}

	bool WoTAnimDataReadPayloadU16(
		TConstArrayView<uint8> Bytes, int32& Offset, uint16& Out)
	{
		if (Offset < 0 || Offset > Bytes.Num() - 2)
		{
			return false;
		}
		Out = static_cast<uint16>(Bytes[Offset])
			| (static_cast<uint16>(Bytes[Offset + 1]) << 8);
		Offset += 2;
		return true;
	}

	bool WoTAnimDataReadPayloadU32(
		TConstArrayView<uint8> Bytes, int32& Offset, uint32& Out)
	{
		if (Offset < 0 || Offset > Bytes.Num() - 4)
		{
			return false;
		}
		Out = static_cast<uint32>(Bytes[Offset])
			| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
			| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
			| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
		Offset += 4;
		return true;
	}

	bool WoTAnimDataValidateSource(const FWoTCanonicalJsonValue& Header)
	{
		const FWoTCanonicalJsonValue* Source = nullptr;
		static const TCHAR* SourceFields[] = {
			TEXT("export"), TEXT("package"), TEXT("sha256")
		};
		FString Export;
		FString Package;
		FString Sha256;
		return !ReadObjectField(
				Header, TEXT("source"), TEXT("animation header"), Source).IsSet()
			&& Source != nullptr
			&& !RequireExactMembers(
				*Source, MakeArrayView(SourceFields),
				TEXT("animation source")).IsSet()
			&& WoTAnimDataReadString(*Source, TEXT("export"), Export)
			&& WoTAnimDataReadString(*Source, TEXT("package"), Package)
			&& WoTAnimDataReadString(*Source, TEXT("sha256"), Sha256)
			&& !Package.IsEmpty() && Sha256.Len() == 64;
	}

	const TArray<FWoTAnimSequence>* WoTAnimDataLoadRuntime(
		FStringView AnimationLogicalId)
	{
		const FString LogicalId(AnimationLogicalId);
		if (!IsWoTPayloadBootstrapActive()
			|| !LogicalId.StartsWith(
				TEXT("cache/animation/"), ESearchCase::CaseSensitive)
			|| WoTAnimDataRejectedRows.Contains(LogicalId))
		{
			return nullptr;
		}
		const FWoTNeutralResourceId Resource{
			LogicalId, EWoTNeutralResourceKind::Animation};
		TWoTBootstrapResult<FWoTRuntimeVerifiedObject> Read =
			FWoTRuntimeRegistry::Get().ReadObject(
				Resource, 512ull * 1024ull * 1024ull,
				TEXT("ue1-packed-vertex-frames-le-v1"), TEXT("animation"));
		if (Read.IsError())
		{
			WoTAnimDataRejectedRows.Add(LogicalId);
			return nullptr;
		}
		FWoTRuntimeVerifiedObject Object =
			MoveTemp(Read).GetValue();
		const FWoTCanonicalJsonValue& Header = Object.Header();
		static const TCHAR* HeaderFields[] = {
			TEXT("encoding"), TEXT("fact_id"), TEXT("frame_byte_length"),
			TEXT("frame_count"), TEXT("importer"), TEXT("importer_version"),
			TEXT("kind"), TEXT("payload_byte_length"), TEXT("sequence_count"),
			TEXT("sequences"), TEXT("source"), TEXT("vertex_count")
		};
		uint32 FrameByteLength = 0;
		uint32 FrameCount = 0;
		uint32 ImporterVersion = 0;
		uint32 SequenceCount = 0;
		uint32 VertexCount = 0;
		FString FactId;
		FString Importer;
		const FWoTCanonicalJsonValue* Sequences =
			Header.FindMember(TEXT("sequences"));
		if (RequireExactMembers(
				Header, MakeArrayView(HeaderFields),
				TEXT("animation header")).IsSet()
			|| !WoTAnimDataReadString(Header, TEXT("fact_id"), FactId)
			|| !WoTAnimDataReadString(Header, TEXT("importer"), Importer)
			|| !WoTAnimDataReadU32(
				Header, TEXT("importer_version"), ImporterVersion)
			|| !WoTAnimDataReadU32(
				Header, TEXT("frame_byte_length"), FrameByteLength)
			|| !WoTAnimDataReadU32(
				Header, TEXT("frame_count"), FrameCount)
			|| !WoTAnimDataReadU32(
				Header, TEXT("sequence_count"), SequenceCount)
			|| !WoTAnimDataReadU32(
				Header, TEXT("vertex_count"), VertexCount)
			|| FactId.IsEmpty() || Importer != TEXT("animation-v1")
			|| ImporterVersion != 1 || FrameCount == 0 || VertexCount == 0
			|| VertexCount > MAX_uint32 / 4
			|| FrameByteLength != VertexCount * 4
			|| SequenceCount > 4096 || Sequences == nullptr
			|| !Sequences->IsArray()
			|| Sequences->Num() != static_cast<int32>(SequenceCount)
			|| !WoTAnimDataValidateSource(Header))
		{
			WoTAnimDataRejectedRows.Add(LogicalId);
			return nullptr;
		}

		const TConstArrayView<uint8> Payload = Object.Payload();
		uint16 PayloadVersion = 0;
		uint16 PayloadFlags = 0;
		uint32 PayloadVertexCount = 0;
		uint32 PayloadFrameCount = 0;
		uint32 PackedCount = 0;
		uint32 PayloadFrameByteLength = 0;
		int32 PayloadOffset = 8;
		const uint64 ExpectedPackedCount =
			static_cast<uint64>(VertexCount) * FrameCount;
		const uint64 ExpectedPayloadBytes =
			28ull + ExpectedPackedCount * 4ull;
		if (Payload.Num() < 28
			|| FMemory::Memcmp(Payload.GetData(), "WOTANI3", 8) != 0
			|| !WoTAnimDataReadPayloadU16(
				Payload, PayloadOffset, PayloadVersion)
			|| !WoTAnimDataReadPayloadU16(
				Payload, PayloadOffset, PayloadFlags)
			|| !WoTAnimDataReadPayloadU32(
				Payload, PayloadOffset, PayloadVertexCount)
			|| !WoTAnimDataReadPayloadU32(
				Payload, PayloadOffset, PayloadFrameCount)
			|| !WoTAnimDataReadPayloadU32(
				Payload, PayloadOffset, PackedCount)
			|| !WoTAnimDataReadPayloadU32(
				Payload, PayloadOffset, PayloadFrameByteLength)
			|| PayloadVersion != 1 || PayloadFlags != 0
			|| PayloadVertexCount != VertexCount
			|| PayloadFrameCount != FrameCount
			|| static_cast<uint64>(PackedCount) != ExpectedPackedCount
			|| PayloadFrameByteLength != FrameByteLength
			|| static_cast<uint64>(Payload.Num()) != ExpectedPayloadBytes)
		{
			WoTAnimDataRejectedRows.Add(LogicalId);
			return nullptr;
		}

		TArray<FWoTAnimSequence> Parsed;
		Parsed.Reserve(static_cast<int32>(SequenceCount));
		uint64 NotifyCount = 0;
		for (int32 SequenceIndex = 0;
			SequenceIndex < Sequences->Num(); ++SequenceIndex)
		{
			const FWoTCanonicalJsonValue& Sequence =
				Sequences->GetElement(SequenceIndex);
			static const TCHAR* SequenceFields[] = {
				TEXT("first_frame"), TEXT("group"), TEXT("name"),
				TEXT("notifies"), TEXT("num_frames"), TEXT("rate_bits")
			};
			FString Name;
			FString Group;
			uint32 FirstFrame = 0;
			uint32 NumFrames = 0;
			uint32 RateBits = 0;
			const FWoTCanonicalJsonValue* Notifies =
				Sequence.FindMember(TEXT("notifies"));
			if (RequireExactMembers(
					Sequence, MakeArrayView(SequenceFields),
					TEXT("animation sequence")).IsSet()
				|| !WoTAnimDataReadString(Sequence, TEXT("name"), Name)
				|| !WoTAnimDataReadString(Sequence, TEXT("group"), Group)
				|| !WoTAnimDataReadU32(
					Sequence, TEXT("first_frame"), FirstFrame)
				|| !WoTAnimDataReadU32(
					Sequence, TEXT("num_frames"), NumFrames)
				|| !WoTAnimDataReadU32(
					Sequence, TEXT("rate_bits"), RateBits)
				|| Name.IsEmpty() || FirstFrame > MAX_int32
				|| NumFrames == 0 || NumFrames > MAX_int32
				|| static_cast<uint64>(FirstFrame) + NumFrames
					> static_cast<uint64>(FrameCount) + 1u
				|| Notifies == nullptr || !Notifies->IsArray()
				|| NotifyCount + static_cast<uint64>(Notifies->Num()) > 65536)
			{
				WoTAnimDataRejectedRows.Add(LogicalId);
				return nullptr;
			}
			NotifyCount += static_cast<uint64>(Notifies->Num());
			FWoTAnimSequence& Out = Parsed.AddDefaulted_GetRef();
			Out.Name = FName(*Name);
			Out.Group = FName(*Group);
			Out.StartFrame = static_cast<int32>(FirstFrame);
			Out.NumFrames = static_cast<int32>(NumFrames);
			if (!WoTAnimDataBitsToFloat(RateBits, Out.Rate))
			{
				WoTAnimDataRejectedRows.Add(LogicalId);
				return nullptr;
			}
			if (Out.Rate <= 0.0f)
			{
				WoTAnimDataRejectedRows.Add(LogicalId);
				return nullptr;
			}
			Out.Notifies.Reserve(Notifies->Num());
			for (int32 NotifyIndex = 0;
				NotifyIndex < Notifies->Num(); ++NotifyIndex)
			{
				const FWoTCanonicalJsonValue& Notify =
					Notifies->GetElement(NotifyIndex);
				static const TCHAR* NotifyFields[] = {
					TEXT("function"), TEXT("time_bits")
				};
				FString Function;
				uint32 TimeBits = 0;
				FWoTAnimNotify& OutNotify =
					Out.Notifies.AddDefaulted_GetRef();
				if (RequireExactMembers(
						Notify, MakeArrayView(NotifyFields),
						TEXT("animation notify")).IsSet()
					|| !WoTAnimDataReadString(
						Notify, TEXT("function"), Function)
					|| !WoTAnimDataReadU32(
						Notify, TEXT("time_bits"), TimeBits)
					|| Function.IsEmpty()
					|| !WoTAnimDataBitsToFloat(
						TimeBits, OutNotify.Fraction)
					|| OutNotify.Fraction < 0.0f
					|| OutNotify.Fraction > 1.0f)
				{
					WoTAnimDataRejectedRows.Add(LogicalId);
					return nullptr;
				}
				OutNotify.Function = FName(*Function);
			}
		}
		WoTAnimDataRuntimeRows.Add(LogicalId, MoveTemp(Parsed));
		return WoTAnimDataRuntimeRows.Find(LogicalId);
	}
#endif // !WOT_GUARDED_BOOTSTRAP
}

const TArray<FWoTAnimSequence>* WoTAnimData::FindSequences(
	FName MeshName, FStringView AnimationLogicalId)
{
	if (MeshName.IsNone())
	{
		return nullptr;
	}
#if WOT_GUARDED_BOOTSTRAP
	const FString LogicalId(AnimationLogicalId);
	if (const TArray<FWoTAnimSequence>* Existing =
		WoTAnimDataRuntimeRows.Find(LogicalId))
	{
		return Existing;
	}
	return WoTAnimDataLoadRuntime(AnimationLogicalId);
#else
	UDataTable* Table = LoadAnimTable();
	if (!Table)
	{
		return nullptr;
	}
	const FWoTAnimSequenceSetRow* Row =
		Table->FindRow<FWoTAnimSequenceSetRow>(MeshName, TEXT("WoTAnimData"), /*bWarnIfMissing*/false);
	return Row ? &Row->Sequences : nullptr;
#endif
}

bool WoTAnimData::PopulateSequences(UWoTVertexAnimComponent* Component,
	FName MeshName, FStringView AnimationLogicalId)
{
	if (!Component)
	{
		return false;
	}
	if (const TArray<FWoTAnimSequence>* Found =
		FindSequences(MeshName, AnimationLogicalId))
	{
		Component->Sequences = *Found;
		return true;
	}
	return false;
}

void WoTAnimData::ResetRuntimeCache()
{
#if WOT_GUARDED_BOOTSTRAP
	WoTAnimDataRuntimeRows.Empty();
	WoTAnimDataRejectedRows.Empty();
#endif
	FWoTRuntimeVertexMesh::ResetRuntimeCache();
}
