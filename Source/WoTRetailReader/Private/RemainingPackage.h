#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "WoTBootstrapCore/Error.h"
#include "WoTBootstrapCore/Hash.h"
#include "WoTRetailReader/Contract.h"

extern const uint64 WoTRemainingMaxSourceBytes;
extern const uint32 WoTRemainingMaxItems;

FWoTBootstrapError WoTRemainingError(EWoTBootstrapError Code,
	const TCHAR* Message, FString Path = FString());

class FWoTRemainingReader
{
public:
	explicit FWoTRemainingReader(TConstArrayView<uint8> InBytes);
	FWoTRemainingReader(TConstArrayView<uint8> InBytes, uint64 InOffset, uint64 InEnd);

	bool ReadU8(uint8& Out);
	bool ReadU16(uint16& Out);
	bool ReadU32(uint32& Out);
	bool ReadI32(int32& Out);
	bool ReadCompact(int32& Out);
	bool ReadBytes(uint64 Count, TArray<uint8>& Out);
	bool Skip(uint64 Count);
	bool SetOffset(uint64 Value);
	bool Require(uint64 Count);
	uint64 Tell() const;
	uint64 GetEnd() const;
	uint64 Remaining() const;
	bool IsValid() const;

private:
	TConstArrayView<uint8> Bytes;
	uint64 Offset = 0;
	uint64 End = 0;
	bool bValid = true;
};

struct FWoTRemainingImport
{
	FString ClassPackage;
	FString ClassName;
	int32 Outer = 0;
	FString Name;
};

struct FWoTRemainingExport
{
	int32 ClassReference = 0;
	int32 SuperReference = 0;
	int32 Outer = 0;
	FString Name;
	uint32 Flags = 0;
	uint64 Offset = 0;
	uint64 Size = 0;
};

struct FWoTRemainingSource
{
	FString Path;
	FString PackageName;
	TArray<uint8> Bytes;
	uint16 Version = 0;
	TArray<FString> Names;
	TArray<FWoTRemainingImport> Imports;
	TArray<FWoTRemainingExport> Exports;
	TMap<int32, FString> CanonicalIdentityByReference;
	TMap<FString, int32> CanonicalReferenceByIdentity;
	TMap<int32, FString> CanonicalClassByReference;
	FWoTSha256 Sha256;

	const FString* NameAt(int32 Index) const;
	const FString* ObjectName(int32 Reference) const;
};

FString WoTRemainingFold(FStringView Value);
bool WoTRemainingReferenceValid(const FWoTRemainingSource& Source, int32 Reference);
TWoTBootstrapResult<FString> WoTRemainingObjectPath(
	const FWoTRemainingSource& Source, int32 Reference, bool bOptional = false);
TWoTBootstrapResult<FString> WoTRemainingCanonicalIdentity(
	const FWoTRemainingSource& Source, int32 Reference, bool bOptional = false);
TWoTBootstrapResult<FString> WoTRemainingObjectClass(
	const FWoTRemainingSource& Source, int32 Reference);
FWoTBootstrapStatus WoTRemainingRequireSemanticReference(
	const FWoTRemainingSource& Source, FStringView Identity,
	FStringView ExpectedClass, FStringView Context);
TWoTBootstrapResult<FWoTRemainingSource> WoTRemainingParsePackage(
	TConstArrayView<uint8> SourceBytes, FStringView Path);
bool WoTRemainingOpenExport(const FWoTRemainingSource& Source,
	const FWoTRemainingExport& Export, FWoTRemainingReader& Out);
bool WoTRemainingReadName(FWoTRemainingReader& Reader,
	const FWoTRemainingSource& Source, FString& Out);

using FWoTRemainingSourceLoader =
	TFunction<TWoTBootstrapResult<TArray<uint8>>(FStringView)>;

/** Contract-closed package resolver for foreign object references. Each
    package is loaded through the installer-backed loader and parsed once. */
class FWoTRemainingPackageResolver
{
public:
	FWoTRemainingPackageResolver(const FWoTRetailContract& Contract,
		const FWoTRemainingSourceLoader& InLoader);

	TWoTBootstrapResult<const FWoTRemainingSource*> Resolve(
		FStringView PackageName, const FWoTRemainingSource& Current);

private:
	const FWoTRemainingSourceLoader& Loader;
	TMap<FString, FString> SourcePathByPackage;
	TMap<FString, TUniquePtr<FWoTRemainingSource>> Parsed;
};
