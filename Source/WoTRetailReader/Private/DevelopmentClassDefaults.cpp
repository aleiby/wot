#include "WoTRetailReader/DevelopmentClassDefaults.h"

#if !WOT_GUARDED_BOOTSTRAP

#include "RemainingPackage.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/Hash.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoTDevelopmentClassDefaults, Log, All);

TWoTBootstrapResult<TArray<uint8>> WoTRemainingBuildPackageAggregateObject(
	TConstArrayView<uint8> SourceBytes, FStringView SourcePath,
	const FWoTRequiredFact& Fact, FWoTRemainingPackageResolver& Resolver);

namespace
{
template <typename T>
TWoTBootstrapResult<T> WoTDevelopmentClassDefaultsError(
	EWoTBootstrapError Code, const TCHAR* Message, FString Path = FString())
{
	return TWoTBootstrapResult<T>::Error(
		MakeWoTBootstrapError(Code, Message, MoveTemp(Path)));
}

void WoTDevelopmentClassDefaultsHashUnsigned(
	FWoTSha256Hasher& Hasher, uint64 Value)
{
	uint8 Encoded[8];
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Encoded[Index] = static_cast<uint8>((Value >> (Index * 8)) & 0xffu);
	}
	Hasher.Update(MakeArrayView(Encoded));
}

void WoTDevelopmentClassDefaultsHashString(
	FWoTSha256Hasher& Hasher, FStringView Value)
{
	const FTCHARToUTF8 Utf8(Value.GetData(), Value.Len());
	WoTDevelopmentClassDefaultsHashUnsigned(
		Hasher, static_cast<uint64>(Utf8.Length()));
	Hasher.Update(MakeArrayView(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

FWoTSha256 WoTDevelopmentClassDefaultsInputSetDigest(
	const TMap<FString, TArray<uint8>>& Inputs)
{
	TArray<FString> Paths;
	Inputs.GetKeys(Paths);
	Paths.Sort([](const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive) < 0;
	});
	FWoTSha256Hasher Hasher;
	WoTDevelopmentClassDefaultsHashString(
		Hasher, TEXT("wot-development-class-default-input-set-v1"));
	WoTDevelopmentClassDefaultsHashUnsigned(
		Hasher, static_cast<uint64>(Paths.Num()));
	for (const FString& Path : Paths)
	{
		WoTDevelopmentClassDefaultsHashString(Hasher, Path);
		const FWoTSha256 SourceSha = Sha256(Inputs.FindChecked(Path));
		Hasher.Update(MakeArrayView(SourceSha.GetData(), SourceSha.Num()));
	}
	return Hasher.Finish();
}

bool WoTDevelopmentClassDefaultsValidateCache(
	TConstArrayView<uint8> Bytes, const FWoTRequiredFact& Fact,
	const FWoTSha256& SourceSha256)
{
	if (Bytes.Num() < 12
		|| FMemory::Memcmp(Bytes.GetData(), "WOTOBJ3", 8) != 0)
	{
		return false;
	}
	const uint32 HeaderBytes = static_cast<uint32>(Bytes[8])
		| (static_cast<uint32>(Bytes[9]) << 8)
		| (static_cast<uint32>(Bytes[10]) << 16)
		| (static_cast<uint32>(Bytes[11]) << 24);
	if (HeaderBytes > static_cast<uint32>(Bytes.Num() - 12)
		|| HeaderBytes > 4u * 1024u * 1024u)
	{
		return false;
	}
	TWoTBootstrapResult<FWoTCanonicalJsonValue> Parsed =
		ParseCanonicalJson(TConstArrayView<uint8>(
			Bytes.GetData() + 12, static_cast<int32>(HeaderBytes)));
	if (Parsed.IsError())
	{
		return false;
	}
	const FWoTCanonicalJsonValue& Header = Parsed.GetValue();
	FString Encoding;
	FString Kind;
	FString FactId;
	FString Importer;
	uint64 PayloadBytes = 0;
	const FWoTCanonicalJsonValue* Source = nullptr;
	TOptional<FWoTBootstrapError> Error = ReadStringField(
		Header, TEXT("encoding"), TEXT("cached class-default object"),
		Encoding);
	if (!Error.IsSet()) Error = ReadStringField(
		Header, TEXT("kind"), TEXT("cached class-default object"), Kind);
	if (!Error.IsSet()) Error = ReadStringField(
		Header, TEXT("fact_id"), TEXT("cached class-default object"), FactId);
	if (!Error.IsSet()) Error = ReadStringField(
		Header, TEXT("importer"), TEXT("cached class-default object"), Importer);
	if (!Error.IsSet()) Error = ReadUnsignedField(
		Header, TEXT("payload_byte_length"),
		TEXT("cached class-default object"), PayloadBytes);
	if (!Error.IsSet()) Error = ReadObjectField(
		Header, TEXT("source"), TEXT("cached class-default object"), Source);
	FString SourceExport;
	FString SourcePackage;
	FString SourceSha;
	if (!Error.IsSet()) Error = ReadStringField(
		*Source, TEXT("export"), TEXT("cached class-default source"),
		SourceExport);
	if (!Error.IsSet()) Error = ReadStringField(
		*Source, TEXT("package"), TEXT("cached class-default source"),
		SourcePackage);
	if (!Error.IsSet()) Error = ReadStringField(
		*Source, TEXT("sha256"), TEXT("cached class-default source"),
		SourceSha);
	return !Error.IsSet()
		&& Encoding == TEXT("ue1-class-defaults-v2")
		&& Kind == TEXT("class_default")
		&& FactId == Fact.FactId
		&& Importer == Fact.Importer
		&& SourceExport == Fact.SourceExport
		&& SourcePackage == Fact.SourcePath
		&& SourceSha == SourceSha256.ToHex()
		&& PayloadBytes
			== static_cast<uint64>(Bytes.Num() - 12 - HeaderBytes);
}

bool WoTDevelopmentClassDefaultsReadCache(
	const FString& Filename, const FWoTRequiredFact& Fact,
	const FWoTSha256& SourceSha256, TArray<uint8>& Out)
{
	const int64 Size = IFileManager::Get().FileSize(*Filename);
	if (Size < 12 || Size > MAX_int32
		|| Fact.Outputs.IsEmpty()
		|| static_cast<uint64>(Size) > Fact.Outputs[0].MaxDecodedSize
		|| !FFileHelper::LoadFileToArray(Out, *Filename)
		|| !WoTDevelopmentClassDefaultsValidateCache(
			Out, Fact, SourceSha256))
	{
		Out.Reset();
		return false;
	}
	FString ExpectedObjectSha;
	if (!FFileHelper::LoadFileToString(
		ExpectedObjectSha, *(Filename + TEXT(".sha256")))
		|| ExpectedObjectSha.Len() != 64
		|| Sha256(Out).ToHex() != ExpectedObjectSha)
	{
		Out.Reset();
		return false;
	}
	return true;
}

bool WoTDevelopmentClassDefaultsWriteCache(
	const FString& Filename, const TArray<uint8>& Bytes)
{
	if (!IFileManager::Get().MakeDirectory(
		*FPaths::GetPath(Filename), true))
	{
		return false;
	}
	const FString Temporary = FString::Printf(
		TEXT("%s.tmp.%u.%llu"), *Filename,
		FPlatformProcess::GetCurrentProcessId(),
		static_cast<unsigned long long>(FPlatformTime::Cycles64()));
	const FString DigestFilename = Filename + TEXT(".sha256");
	const FString DigestTemporary = Temporary + TEXT(".sha256");
	if (!FFileHelper::SaveArrayToFile(Bytes, *Temporary)
		|| !FFileHelper::SaveStringToFile(
			Sha256(Bytes).ToHex(), *DigestTemporary)
		|| !IFileManager::Get().Move(
			*Filename, *Temporary, true, false, false, true)
		|| !IFileManager::Get().Move(
			*DigestFilename, *DigestTemporary, true, false, false, true))
	{
		IFileManager::Get().Delete(*Temporary, false, true, true);
		IFileManager::Get().Delete(*DigestTemporary, false, true, true);
		return false;
	}
	return true;
}
}

FWoTDevelopmentPackageCache::FWoTDevelopmentPackageCache(
	FStringView ReferenceOriginalRoot)
	: Root(FPaths::ConvertRelativePathToFull(FString(ReferenceOriginalRoot)))
{
	FPaths::NormalizeDirectoryName(Root);
}

TWoTBootstrapResult<TSharedPtr<const TArray<uint8>>>
FWoTDevelopmentPackageCache::Load(FStringView SourcePath)
{
	using FResult =
		TWoTBootstrapResult<TSharedPtr<const TArray<uint8>>>;
	const FString Relative(SourcePath);
	TArray<FString> Components;
	Relative.ParseIntoArray(Components, TEXT("/"), false);
	if (Root.IsEmpty() || Relative.IsEmpty() || !FPaths::IsRelative(Relative)
		|| Relative.Contains(TEXT("\\")) || Components.IsEmpty()
		|| Components.ContainsByPredicate(
			[](const FString& Component)
			{
				return Component.IsEmpty() || Component == TEXT(".")
					|| Component == TEXT("..");
			}))
	{
		return WoTDevelopmentClassDefaultsError<
			TSharedPtr<const TArray<uint8>>>(
				EWoTBootstrapError::UnknownField,
				TEXT("the development source path is unsafe"), Relative);
	}
	if (const TSharedPtr<const TArray<uint8>>* Existing =
		Packages.Find(Relative))
	{
		return FResult::Success(*Existing);
	}
	FString Filename = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(Root, Relative));
	FPaths::NormalizeFilename(Filename);
	FString RootPrefix = Root;
	FPaths::NormalizeFilename(RootPrefix);
	RootPrefix += TEXT("/");
	if (!Filename.StartsWith(RootPrefix, ESearchCase::IgnoreCase))
	{
		return WoTDevelopmentClassDefaultsError<
			TSharedPtr<const TArray<uint8>>>(
				EWoTBootstrapError::UnknownField,
				TEXT("the development source escapes the reference root"),
				Relative);
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
	{
		return WoTDevelopmentClassDefaultsError<
			TSharedPtr<const TArray<uint8>>>(
				EWoTBootstrapError::UnknownField,
				TEXT("the development source cannot be read"), Relative);
	}
	if (Bytes.IsEmpty()
		|| static_cast<uint64>(Bytes.Num()) > WoTRemainingMaxSourceBytes)
	{
		return WoTDevelopmentClassDefaultsError<
			TSharedPtr<const TArray<uint8>>>(
				EWoTBootstrapError::SizeLimit,
				TEXT("the development source exceeds its bound"), Relative);
	}
	TSharedPtr<const TArray<uint8>> Stored =
		MakeShared<TArray<uint8>>(MoveTemp(Bytes));
	Packages.Add(Relative, Stored);
	return FResult::Success(MoveTemp(Stored));
}

TWoTBootstrapResult<TArray<FWoTDevelopmentClassDefaultObject>>
WoTBuildClassDefaultObjectsForDevelopment(
	const FWoTRetailContract& Contract,
	FStringView ReferenceSystemRoot,
	FStringView CacheRoot,
	bool bForceRebuild)
{
	const FString SystemRoot = FPaths::ConvertRelativePathToFull(
		FString(ReferenceSystemRoot));
	if (!IFileManager::Get().DirectoryExists(*SystemRoot))
	{
		return WoTDevelopmentClassDefaultsError<
			TArray<FWoTDevelopmentClassDefaultObject>>(
			EWoTBootstrapError::UnknownField,
			TEXT("the development reference System root is absent"),
			SystemRoot);
	}
	FWoTDevelopmentPackageCache SourceCache(FPaths::GetPath(SystemRoot));
	return WoTBuildClassDefaultObjectsForDevelopment(
		Contract, SourceCache, CacheRoot, bForceRebuild);
}

TWoTBootstrapResult<TArray<FWoTDevelopmentClassDefaultObject>>
WoTBuildClassDefaultObjectsForDevelopment(
	const FWoTRetailContract& Contract,
	FWoTDevelopmentPackageCache& SourceCache,
	FStringView CacheRoot,
	bool bForceRebuild)
{
	using FResult =
		TWoTBootstrapResult<TArray<FWoTDevelopmentClassDefaultObject>>;

	TArray<const FWoTRequiredFact*> ClassDefaultFacts;
	TMap<FString, TArray<uint8>> Inputs;
	for (const FWoTRequiredFact& Fact : Contract.Facts)
	{
		if (Fact.Domain == EWoTFactDomain::ClassDefault)
		{
			ClassDefaultFacts.Add(&Fact);
		}
		if (!Fact.SourcePath.StartsWith(
			TEXT("System/"), ESearchCase::CaseSensitive))
		{
			continue;
		}
		if (!Inputs.Contains(Fact.SourcePath))
		{
			TWoTBootstrapResult<TSharedPtr<const TArray<uint8>>> Loaded =
				SourceCache.Load(Fact.SourcePath);
			if (Loaded.IsError())
			{
				return FResult::Error(MoveTemp(Loaded).GetError());
			}
			Inputs.Add(Fact.SourcePath, *Loaded.GetValue());
		}
	}
	const FString ObjectCacheRoot = FPaths::ConvertRelativePathToFull(
		FString(CacheRoot) / TEXT("class-default-v2")
		/ WoTDevelopmentClassDefaultsInputSetDigest(Inputs).ToHex());
	FWoTRemainingSourceLoader Loader =
		[&Inputs](FStringView SourcePath)
			-> TWoTBootstrapResult<TArray<uint8>>
	{
		const TArray<uint8>* Bytes = Inputs.Find(FString(SourcePath));
		if (Bytes == nullptr)
		{
			return WoTDevelopmentClassDefaultsError<TArray<uint8>>(
				EWoTBootstrapError::UnknownField,
				TEXT("the development class-default dependency is outside the input set"),
				FString(SourcePath));
		}
		return TWoTBootstrapResult<TArray<uint8>>::Success(*Bytes);
	};

	FWoTRemainingPackageResolver Resolver(Contract, Loader);
	TArray<FWoTDevelopmentClassDefaultObject> Objects;
	for (const FWoTRequiredFact* FactPointer : ClassDefaultFacts)
	{
		const FWoTRequiredFact& Fact = *FactPointer;
		if (Fact.SourcePath.IsEmpty()
			|| Fact.Importer != TEXT("class-default-v2")
			|| Fact.Outputs.Num() != 1
			|| Fact.Outputs[0].LogicalId.IsEmpty()
			|| Fact.Outputs[0].Schema != TEXT("wot-class-default-v3"))
		{
			return WoTDevelopmentClassDefaultsError<
				TArray<FWoTDevelopmentClassDefaultObject>>(
				EWoTBootstrapError::UnknownField,
				TEXT("a development class-default fact has a closed-schema mismatch"),
				Fact.FactId);
		}
		const TArray<uint8>& Source = Inputs.FindChecked(Fact.SourcePath);
		const FWoTSha256 SourceSha256 = Sha256(Source);
		const FString CacheFile = ObjectCacheRoot
			/ (SourceSha256.ToHex() + TEXT(".wotobj3"));
		TArray<uint8> ObjectBytes;
		const bool bCacheHit = !bForceRebuild
			&& WoTDevelopmentClassDefaultsReadCache(
			CacheFile, Fact, SourceSha256, ObjectBytes);
		if (!bCacheHit)
		{
			TWoTBootstrapResult<TArray<uint8>> Object =
				WoTRemainingBuildPackageAggregateObject(
					Source, Fact.SourcePath, Fact, Resolver);
			if (Object.IsError())
			{
				return FResult::Error(MoveTemp(Object).GetError());
			}
			ObjectBytes = MoveTemp(Object).GetValue();
			if (!WoTDevelopmentClassDefaultsWriteCache(
				CacheFile, ObjectBytes))
			{
				UE_LOG(LogWoTDevelopmentClassDefaults, Warning,
					TEXT("Could not cache development class-default object %s"),
					*Fact.FactId);
			}
		}
		FWoTDevelopmentClassDefaultObject& Built =
			Objects.AddDefaulted_GetRef();
		Built.LogicalId = Fact.Outputs[0].LogicalId;
		Built.ObjectBytes = MoveTemp(ObjectBytes);
		Built.bLoadedFromCache = bCacheHit;
	}
	return FResult::Success(MoveTemp(Objects));
}

#if WITH_AUTOMATION_TESTS
FString WoTDevelopmentClassDefaultInputSetDigestForTesting(
	const TMap<FString, TArray<uint8>>& Inputs)
{
	return WoTDevelopmentClassDefaultsInputSetDigest(Inputs).ToHex();
}
#endif

#endif // !WOT_GUARDED_BOOTSTRAP
