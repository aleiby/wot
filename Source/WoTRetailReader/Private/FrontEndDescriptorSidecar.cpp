#include "WoTRetailReader/FrontEndDescriptorSidecar.h"

#include "CanonicalObjectBuilder.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

namespace
{
constexpr TCHAR WoTFrontEndSidecarFormat[] =
	TEXT("wot-front-end-descriptor-sidecar-v2");
constexpr uint32 WoTFrontEndSidecarMaxBytes = 1024 * 1024;

#if WITH_AUTOMATION_TESTS
// Intentional process-lifetime storage: a static or cross-TU token may drain
// during teardown, after ordinary namespace statics would have been destroyed.
FCriticalSection& GWoTFrontEndDescriptorParseObserverGuard =
	*new FCriticalSection();
TArray<TSharedPtr<TFunction<void()>, ESPMode::ThreadSafe>>
	& GWoTFrontEndDescriptorParseObservers =
		*new TArray<TSharedPtr<TFunction<void()>, ESPMode::ThreadSafe>>();
TSet<const void*>& GWoTActiveFrontEndDescriptorParseObservers =
	*new TSet<const void*>();
TMap<const void*, int32>& GWoTFrontEndDescriptorParseObserverInFlight =
	*new TMap<const void*, int32>();
TMap<const void*, TMap<uint32, int32>>
	& GWoTFrontEndDescriptorParseObserverThreads =
		*new TMap<const void*, TMap<uint32, int32>>();
TMap<const void*, FEvent*>& GWoTFrontEndDescriptorParseObserverDrainedEvents =
	*new TMap<const void*, FEvent*>();
#endif

template <typename T>
TWoTBootstrapResult<T> WoTFrontEndSidecarError(
	EWoTBootstrapError Code, const TCHAR* Message)
{
	return TWoTBootstrapResult<T>::Error(MakeWoTBootstrapError(Code, Message));
}

int64 WoTFrontEndDoubleBits(double Value)
{
	uint64 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return static_cast<int64>(Bits);
}

int64 WoTFrontEndFloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return static_cast<int64>(Bits);
}

const TCHAR* WoTFrontEndSidecarDomainName(EWoTFactDomain Domain)
{
	switch (Domain)
	{
	case EWoTFactDomain::Texture: return TEXT("texture");
	case EWoTFactDomain::Sound: return TEXT("sound");
	case EWoTFactDomain::Music: return TEXT("music");
	case EWoTFactDomain::Mesh: return TEXT("mesh");
	case EWoTFactDomain::Animation: return TEXT("animation");
	case EWoTFactDomain::BSP: return TEXT("bsp");
	case EWoTFactDomain::World: return TEXT("world");
	case EWoTFactDomain::Font: return TEXT("font");
	case EWoTFactDomain::Procedural: return TEXT("procedural");
	case EWoTFactDomain::Registry: return TEXT("registry");
	case EWoTFactDomain::Localization: return TEXT("localization");
	case EWoTFactDomain::ClassDefault: return TEXT("class_default");
	default: return TEXT("invalid");
	}
}

TOptional<EWoTFactDomain> WoTFrontEndSidecarParseDomain(FStringView Name)
{
	for (uint8 Index = 0;
		Index <= static_cast<uint8>(EWoTFactDomain::ClassDefault); ++Index)
	{
		const EWoTFactDomain Domain = static_cast<EWoTFactDomain>(Index);
		if (Name == WoTFrontEndSidecarDomainName(Domain)) return Domain;
	}
	return TOptional<EWoTFactDomain>();
}

TWoTBootstrapResult<FWoTCanonicalJsonValue> WoTFrontEndSidecarDescriptorJson(
	const FWoTFrontEndDescriptor& Descriptor)
{
	using FResult = TWoTBootstrapResult<FWoTCanonicalJsonValue>;
	FWoTCanonicalJsonValue Location = FWoTCanonicalJsonValue::MakeArray();
	Location.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		WoTFrontEndDoubleBits(Descriptor.CameraLocation.X)));
	Location.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		WoTFrontEndDoubleBits(Descriptor.CameraLocation.Y)));
	Location.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		WoTFrontEndDoubleBits(Descriptor.CameraLocation.Z)));
	FWoTCanonicalJsonValue Rotation = FWoTCanonicalJsonValue::MakeArray();
	Rotation.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		WoTFrontEndDoubleBits(Descriptor.CameraRotation.Pitch)));
	Rotation.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		WoTFrontEndDoubleBits(Descriptor.CameraRotation.Yaw)));
	Rotation.AddElement(FWoTCanonicalJsonValue::MakeInteger(
		WoTFrontEndDoubleBits(Descriptor.CameraRotation.Roll)));
	FWoTCanonicalObjectBuilder Camera;
	Camera.Add(TEXT("horizontal_fov_bits"),
		FWoTCanonicalJsonValue::MakeInteger(
			WoTFrontEndFloatBits(Descriptor.HorizontalFov)));
	Camera.Add(TEXT("location_bits"), MoveTemp(Location));
	Camera.Add(TEXT("rotation_bits"), MoveTemp(Rotation));
	FResult CameraJson = MoveTemp(Camera).Build(TEXT("front-end camera"));
	if (CameraJson.IsError()) return CameraJson;
	FWoTCanonicalObjectBuilder Census;
	Census.Add(TEXT("ambient_sounds"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.AmbientSounds));
	Census.Add(TEXT("lightning_actors"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.LightningActors));
	Census.Add(TEXT("lights"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.Lights));
	Census.Add(TEXT("rain_cylinders"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.RainCylinders));
	Census.Add(TEXT("random_lightning_controllers"),
		FWoTCanonicalJsonValue::MakeInteger(
			Descriptor.Census.RandomLightningControllers));
	Census.Add(TEXT("splash_emitters"), FWoTCanonicalJsonValue::MakeInteger(
		Descriptor.Census.SplashEmitters));
	FResult CensusJson = MoveTemp(Census).Build(TEXT("front-end census"));
	if (CensusJson.IsError()) return CensusJson;
	FWoTCanonicalJsonValue Required = FWoTCanonicalJsonValue::MakeArray();
	for (const FString& LogicalId : Descriptor.RequiredLogicalIds)
		Required.AddElement(FWoTCanonicalJsonValue::MakeString(LogicalId));
	FWoTCanonicalJsonValue Thunder = FWoTCanonicalJsonValue::MakeArray();
	for (const FString& SourceExport : Descriptor.ThunderSoundSourceExports)
		Thunder.AddElement(FWoTCanonicalJsonValue::MakeString(SourceExport));
	FWoTCanonicalJsonValue Resources = FWoTCanonicalJsonValue::MakeArray();
	for (const FWoTFrontEndResourceRequirement& Requirement
		: Descriptor.RequiredResources)
	{
		FWoTCanonicalObjectBuilder Resource;
		Resource.Add(TEXT("domain"), FWoTCanonicalJsonValue::MakeString(
			WoTFrontEndSidecarDomainName(Requirement.Domain)));
		Resource.Add(TEXT("logical_id"), FWoTCanonicalJsonValue::MakeString(
			Requirement.LogicalId));
		Resource.Add(TEXT("source_export"), FWoTCanonicalJsonValue::MakeString(
			Requirement.SourceExport));
		Resource.Add(TEXT("source_path"), FWoTCanonicalJsonValue::MakeString(
			Requirement.SourcePath));
		FResult ResourceJson = MoveTemp(Resource).Build(
			TEXT("front-end resource requirement"));
		if (ResourceJson.IsError()) return ResourceJson;
		Resources.AddElement(MoveTemp(ResourceJson).GetValue());
	}
	FWoTCanonicalObjectBuilder Root;
	Root.Add(TEXT("bsp_logical_id"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.BspLogicalId));
	Root.Add(TEXT("camera"), MoveTemp(CameraJson).GetValue());
	Root.Add(TEXT("census"), MoveTemp(CensusJson).GetValue());
	Root.Add(TEXT("descriptor_root"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.DescriptorRoot.ToHex()));
	Root.Add(TEXT("map_name"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.MapName));
	Root.Add(TEXT("required_logical_ids"), MoveTemp(Required));
	Root.Add(TEXT("required_resources"), MoveTemp(Resources));
	Root.Add(TEXT("source_root"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.SourceRoot.ToHex()));
	Root.Add(TEXT("thunder_sound_source_exports"), MoveTemp(Thunder));
	Root.Add(TEXT("world_logical_id"), FWoTCanonicalJsonValue::MakeString(
		Descriptor.WorldLogicalId));
	return MoveTemp(Root).Build(TEXT("front-end descriptor"));
}

TWoTBootstrapResult<TArray<uint8>> WoTFrontEndUnsignedSidecar(
	const FWoTFrontEndDescriptor& Descriptor,
	const FWoTSha256& InstallerSha256,
	const FWoTSha256& ContractSha256,
	const FWoTSha256& ProductionDescriptorSha256)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> DescriptorJson =
		WoTFrontEndSidecarDescriptorJson(Descriptor);
	if (DescriptorJson.IsError())
		return FResult::Error(MoveTemp(DescriptorJson).GetError());
	FWoTCanonicalObjectBuilder Root;
	Root.Add(TEXT("contract_sha256"), FWoTCanonicalJsonValue::MakeString(
		ContractSha256.ToHex()));
	Root.Add(TEXT("descriptor"), MoveTemp(DescriptorJson).GetValue());
	Root.Add(TEXT("format"), FWoTCanonicalJsonValue::MakeString(
		WoTFrontEndSidecarFormat));
	Root.Add(TEXT("installer_sha256"), FWoTCanonicalJsonValue::MakeString(
		InstallerSha256.ToHex()));
	Root.Add(TEXT("production_descriptor_sha256"),
		FWoTCanonicalJsonValue::MakeString(ProductionDescriptorSha256.ToHex()));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Json =
		MoveTemp(Root).Build(TEXT("unsigned front-end sidecar"));
	if (Json.IsError()) return FResult::Error(MoveTemp(Json).GetError());
	return WriteCanonicalJson(Json.GetValue());
}

FWoTSha256 WoTFrontEndSidecarSeal(TConstArrayView<uint8> UnsignedBytes)
{
	FWoTSha256Hasher Hasher;
	static const ANSICHAR Domain[] = "WOT-FRONT-END-DESCRIPTOR-SIDECAR-V2";
	Hasher.Update(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Domain),
		UE_ARRAY_COUNT(Domain)));
	Hasher.Update(UnsignedBytes);
	return Hasher.Finish();
}

bool WoTFrontEndReadSha(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, FWoTSha256& Out)
{
	FString Hex;
	if (ReadStringField(Object, Field, TEXT("front-end sidecar"), Hex).IsSet())
		return false;
	TWoTBootstrapResult<FWoTSha256> Parsed = FWoTSha256::FromHex(Hex);
	if (Parsed.IsError()) return false;
	Out = MoveTemp(Parsed).GetValue();
	return true;
}

bool WoTFrontEndReadU32(const FWoTCanonicalJsonValue& Object,
	const TCHAR* Field, uint32& Out)
{
	return !ReadUnsigned32Field(Object, Field, TEXT("front-end descriptor"), Out).IsSet();
}

bool WoTFrontEndReadDoubleBits(const FWoTCanonicalJsonValue& Value,
	double& Out)
{
	if (Value.GetKind() != EWoTCanonicalJsonKind::Integer) return false;
	const uint64 Bits = static_cast<uint64>(Value.GetInteger());
	FMemory::Memcpy(&Out, &Bits, sizeof(Bits));
	return FMath::IsFinite(Out);
}

TWoTBootstrapResult<FWoTFrontEndDescriptor> WoTFrontEndParseDescriptor(
	const FWoTCanonicalJsonValue& Value)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	static const TCHAR* const Fields[] = {
		TEXT("bsp_logical_id"), TEXT("camera"), TEXT("census"),
		TEXT("descriptor_root"), TEXT("map_name"),
		TEXT("required_logical_ids"), TEXT("required_resources"),
		TEXT("source_root"), TEXT("thunder_sound_source_exports"),
		TEXT("world_logical_id")};
	if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
		Value, Fields, TEXT("front-end descriptor")); Error.IsSet())
		return FResult::Error(MoveTemp(Error.GetValue()));
	FWoTFrontEndDescriptor Descriptor;
	if (ReadStringField(Value, TEXT("bsp_logical_id"),
			TEXT("front-end descriptor"), Descriptor.BspLogicalId).IsSet()
		|| ReadStringField(Value, TEXT("map_name"),
			TEXT("front-end descriptor"), Descriptor.MapName).IsSet()
		|| ReadStringField(Value, TEXT("world_logical_id"),
			TEXT("front-end descriptor"), Descriptor.WorldLogicalId).IsSet()
		|| !WoTFrontEndReadSha(Value, TEXT("descriptor_root"),
			Descriptor.DescriptorRoot)
		|| !WoTFrontEndReadSha(Value, TEXT("source_root"), Descriptor.SourceRoot))
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end descriptor scalar fields are malformed"));
	const FWoTCanonicalJsonValue* Camera = Value.FindMember(TEXT("camera"));
	const FWoTCanonicalJsonValue* Census = Value.FindMember(TEXT("census"));
	const FWoTCanonicalJsonValue* Required =
		Value.FindMember(TEXT("required_logical_ids"));
	const FWoTCanonicalJsonValue* Resources =
		Value.FindMember(TEXT("required_resources"));
	const FWoTCanonicalJsonValue* Thunder =
		Value.FindMember(TEXT("thunder_sound_source_exports"));
	if (Camera == nullptr || !Camera->IsObject()
		|| Census == nullptr || !Census->IsObject()
		|| Required == nullptr || !Required->IsArray()
		|| Resources == nullptr || !Resources->IsArray()
		|| Thunder == nullptr || !Thunder->IsArray() || Thunder->Num() != 10)
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end descriptor collections are malformed"));
	for (int32 Index = 0; Index < Thunder->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& SourceExport = Thunder->GetElement(Index);
		if (SourceExport.GetKind() != EWoTCanonicalJsonKind::String
			|| SourceExport.GetString().IsEmpty())
			return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("the front-end ordered thunder route is malformed"));
		Descriptor.ThunderSoundSourceExports.Add(SourceExport.GetString());
	}
	static const TCHAR* const CameraFields[] = {
		TEXT("horizontal_fov_bits"), TEXT("location_bits"),
		TEXT("rotation_bits")};
	if (RequireExactMembers(*Camera, CameraFields,
		TEXT("front-end camera")).IsSet())
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end camera has unknown fields"));
	uint32 FovBits = 0;
	if (!WoTFrontEndReadU32(*Camera, TEXT("horizontal_fov_bits"), FovBits))
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::InvalidNumber,
			TEXT("the front-end FOV bits are malformed"));
	FMemory::Memcpy(&Descriptor.HorizontalFov, &FovBits, sizeof(FovBits));
	const FWoTCanonicalJsonValue* Location = Camera->FindMember(TEXT("location_bits"));
	const FWoTCanonicalJsonValue* Rotation = Camera->FindMember(TEXT("rotation_bits"));
	double LocationValues[3];
	double RotationValues[3];
	if (Location == nullptr || !Location->IsArray() || Location->Num() != 3
		|| Rotation == nullptr || !Rotation->IsArray() || Rotation->Num() != 3)
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end camera vectors are malformed"));
	for (int32 Index = 0; Index < 3; ++Index)
		if (!WoTFrontEndReadDoubleBits(Location->GetElement(Index), LocationValues[Index])
			|| !WoTFrontEndReadDoubleBits(Rotation->GetElement(Index),
				RotationValues[Index]))
			return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::InvalidNumber,
				TEXT("the front-end camera contains non-finite bits"));
	Descriptor.CameraLocation = FVector(
		LocationValues[0], LocationValues[1], LocationValues[2]);
	Descriptor.CameraRotation = FRotator(
		RotationValues[0], RotationValues[1], RotationValues[2]);
	static const TCHAR* const CensusFields[] = {
		TEXT("ambient_sounds"), TEXT("lightning_actors"), TEXT("lights"),
		TEXT("rain_cylinders"), TEXT("random_lightning_controllers"),
		TEXT("splash_emitters")};
	if (RequireExactMembers(*Census, CensusFields,
			TEXT("front-end census")).IsSet()
		|| !WoTFrontEndReadU32(*Census, TEXT("ambient_sounds"),
			Descriptor.Census.AmbientSounds)
		|| !WoTFrontEndReadU32(*Census, TEXT("lightning_actors"),
			Descriptor.Census.LightningActors)
		|| !WoTFrontEndReadU32(*Census, TEXT("lights"),
			Descriptor.Census.Lights)
		|| !WoTFrontEndReadU32(*Census, TEXT("rain_cylinders"),
			Descriptor.Census.RainCylinders)
		|| !WoTFrontEndReadU32(*Census, TEXT("random_lightning_controllers"),
			Descriptor.Census.RandomLightningControllers)
		|| !WoTFrontEndReadU32(*Census, TEXT("splash_emitters"),
			Descriptor.Census.SplashEmitters))
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end census is malformed"));
	for (int32 Index = 0; Index < Required->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& LogicalId = Required->GetElement(Index);
		if (LogicalId.GetKind() != EWoTCanonicalJsonKind::String)
			return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("the front-end closure contains a non-string ID"));
		Descriptor.RequiredLogicalIds.Add(LogicalId.GetString());
	}
	for (int32 Index = 0; Index < Resources->Num(); ++Index)
	{
		const FWoTCanonicalJsonValue& Resource = Resources->GetElement(Index);
		static const TCHAR* const ResourceFields[] = {
			TEXT("domain"), TEXT("logical_id"), TEXT("source_export"),
			TEXT("source_path")};
		FString DomainName;
		FWoTFrontEndResourceRequirement Requirement;
		if (RequireExactMembers(Resource, ResourceFields,
				TEXT("front-end resource requirement")).IsSet()
			|| ReadStringField(Resource, TEXT("domain"),
				TEXT("front-end resource requirement"), DomainName).IsSet()
			|| ReadStringField(Resource, TEXT("logical_id"),
				TEXT("front-end resource requirement"), Requirement.LogicalId).IsSet()
			|| ReadStringField(Resource, TEXT("source_export"),
				TEXT("front-end resource requirement"), Requirement.SourceExport).IsSet()
			|| ReadStringField(Resource, TEXT("source_path"),
				TEXT("front-end resource requirement"), Requirement.SourcePath).IsSet())
			return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("a front-end resource requirement is malformed"));
		const TOptional<EWoTFactDomain> Domain =
			WoTFrontEndSidecarParseDomain(DomainName);
		if (!Domain.IsSet())
			return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
				EWoTBootstrapError::UnknownField,
				TEXT("a front-end resource requirement has an unknown domain"));
		Requirement.Domain = Domain.GetValue();
		Descriptor.RequiredResources.Add(MoveTemp(Requirement));
	}
	FWoTBootstrapStatus Validated = ValidateFrontEndDescriptor(Descriptor);
	if (Validated.IsError())
		return FResult::Error(MoveTemp(Validated).GetError());
	return FResult::Success(MoveTemp(Descriptor));
}
}

#if WITH_AUTOMATION_TESTS
TSharedPtr<uint8, ESPMode::ThreadSafe>
WoTObserveFrontEndDescriptorParsesForTesting(
	TFunction<void()> Observer)
{
	return WoTRegisterAutomationObserverForTesting(
		GWoTFrontEndDescriptorParseObserverGuard,
		GWoTFrontEndDescriptorParseObservers,
		GWoTActiveFrontEndDescriptorParseObservers,
		GWoTFrontEndDescriptorParseObserverInFlight,
		GWoTFrontEndDescriptorParseObserverThreads,
		GWoTFrontEndDescriptorParseObserverDrainedEvents, MoveTemp(Observer));
}
#endif

TWoTBootstrapResult<TArray<uint8>> WriteSealedFrontEndDescriptorSidecar(
	const FWoTFrontEndDescriptor& Descriptor,
	const FWoTSha256& InstallerSha256,
	const FWoTSha256& ContractSha256,
	const FWoTSha256& ProductionDescriptorSha256)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	FWoTBootstrapStatus Validated = ValidateFrontEndDescriptor(Descriptor);
	if (Validated.IsError()) return FResult::Error(MoveTemp(Validated).GetError());
	FResult Unsigned = WoTFrontEndUnsignedSidecar(Descriptor, InstallerSha256,
		ContractSha256, ProductionDescriptorSha256);
	if (Unsigned.IsError()) return Unsigned;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> DescriptorJson =
		WoTFrontEndSidecarDescriptorJson(Descriptor);
	if (DescriptorJson.IsError())
		return FResult::Error(MoveTemp(DescriptorJson).GetError());
	FWoTCanonicalObjectBuilder Root;
	Root.Add(TEXT("contract_sha256"), FWoTCanonicalJsonValue::MakeString(
		ContractSha256.ToHex()));
	Root.Add(TEXT("descriptor"), MoveTemp(DescriptorJson).GetValue());
	Root.Add(TEXT("format"), FWoTCanonicalJsonValue::MakeString(
		WoTFrontEndSidecarFormat));
	Root.Add(TEXT("installer_sha256"), FWoTCanonicalJsonValue::MakeString(
		InstallerSha256.ToHex()));
	Root.Add(TEXT("production_descriptor_sha256"),
		FWoTCanonicalJsonValue::MakeString(ProductionDescriptorSha256.ToHex()));
	Root.Add(TEXT("record_sha256"), FWoTCanonicalJsonValue::MakeString(
		WoTFrontEndSidecarSeal(Unsigned.GetValue()).ToHex()));
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Json =
		MoveTemp(Root).Build(TEXT("front-end sidecar"));
	if (Json.IsError()) return FResult::Error(MoveTemp(Json).GetError());
	FResult Encoded = WriteCanonicalJson(Json.GetValue());
	if (!Encoded.IsError()
		&& Encoded.GetValue().Num() > static_cast<int32>(WoTFrontEndSidecarMaxBytes))
		return WoTFrontEndSidecarError<TArray<uint8>>(
			EWoTBootstrapError::SizeLimit,
			TEXT("the front-end sidecar exceeds its byte bound"));
	return Encoded;
}

TWoTBootstrapResult<FWoTFrontEndDescriptor> ParseSealedFrontEndDescriptorSidecar(
	TConstArrayView<uint8> Bytes,
	const FWoTSha256& ExpectedInstallerSha256,
	const FWoTSha256& ExpectedContractSha256,
	const FWoTSha256& ExpectedProductionDescriptorSha256,
	const FWoTSha256& ExpectedDescriptorRoot)
{
	using FResult = TWoTBootstrapResult<FWoTFrontEndDescriptor>;
	#if WITH_AUTOMATION_TESTS
	WoTBroadcastAutomationObserverForTesting(
		GWoTFrontEndDescriptorParseObserverGuard,
		GWoTFrontEndDescriptorParseObservers,
		GWoTActiveFrontEndDescriptorParseObservers,
		GWoTFrontEndDescriptorParseObserverInFlight,
		GWoTFrontEndDescriptorParseObserverThreads,
		GWoTFrontEndDescriptorParseObserverDrainedEvents);
	#endif
	FWoTCanonicalJsonLimits Limits;
	Limits.MaxBytes = WoTFrontEndSidecarMaxBytes;
	Limits.MaxMembers = 65536;
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(Bytes, Limits);
	if (Parsed.IsError()) return FResult::Error(MoveTemp(Parsed).GetError());
	const FWoTCanonicalJsonValue& Root = Parsed.GetValue();
	static const TCHAR* const Fields[] = {
		TEXT("contract_sha256"), TEXT("descriptor"), TEXT("format"),
		TEXT("installer_sha256"), TEXT("production_descriptor_sha256"),
		TEXT("record_sha256")};
	if (TOptional<FWoTBootstrapError> Error = RequireExactMembers(
		Root, Fields, TEXT("front-end sidecar")); Error.IsSet())
		return FResult::Error(MoveTemp(Error.GetValue()));
	FString Format;
	FWoTSha256 InstallerSha256;
	FWoTSha256 ContractSha256;
	FWoTSha256 ProductionDescriptorSha256;
	FWoTSha256 RecordSha256;
	if (ReadStringField(Root, TEXT("format"), TEXT("front-end sidecar"),
			Format).IsSet()
		|| !WoTFrontEndReadSha(Root, TEXT("installer_sha256"), InstallerSha256)
		|| !WoTFrontEndReadSha(Root, TEXT("contract_sha256"), ContractSha256)
		|| !WoTFrontEndReadSha(Root, TEXT("production_descriptor_sha256"),
			ProductionDescriptorSha256)
		|| !WoTFrontEndReadSha(Root, TEXT("record_sha256"), RecordSha256)
		|| Format != WoTFrontEndSidecarFormat
		|| InstallerSha256 != ExpectedInstallerSha256
		|| ContractSha256 != ExpectedContractSha256
		|| ProductionDescriptorSha256 != ExpectedProductionDescriptorSha256)
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the front-end sidecar protocol or authenticated identity is wrong"));
	const FWoTCanonicalJsonValue* DescriptorJson = Root.FindMember(TEXT("descriptor"));
	if (DescriptorJson == nullptr)
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::UnknownField,
			TEXT("the front-end sidecar has no descriptor"));
	FResult Descriptor = WoTFrontEndParseDescriptor(*DescriptorJson);
	if (Descriptor.IsError()) return Descriptor;
	if (Descriptor.GetValue().DescriptorRoot != ExpectedDescriptorRoot)
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the front-end descriptor root is not the authenticated snapshot"));
	TWoTBootstrapResult<TArray<uint8>> Unsigned = WoTFrontEndUnsignedSidecar(
		Descriptor.GetValue(), InstallerSha256, ContractSha256,
		ProductionDescriptorSha256);
	if (Unsigned.IsError())
		return FResult::Error(MoveTemp(Unsigned).GetError());
	if (RecordSha256 != WoTFrontEndSidecarSeal(Unsigned.GetValue()))
		return WoTFrontEndSidecarError<FWoTFrontEndDescriptor>(
			EWoTBootstrapError::CorruptInstallation,
			TEXT("the front-end sidecar record seal is invalid"));
	return Descriptor;
}
