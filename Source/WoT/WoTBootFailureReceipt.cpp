#include "WoTBootFailureReceipt.h"

#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Updater/WoTGameBootstrapWorker.h"
#include "WoTBootstrapCore/CanonicalJson.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapHeartbeat.h"

#include <cstdlib>

namespace
{
#if WITH_AUTOMATION_TESTS
FCriticalSection GWoTBootFailureReceiptTestRootGuard;
FString GWoTBootFailureReceiptTestRoot;
#endif

TArray<uint8> WoTBootFailureCanonicalBytes(
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase,
	FStringView TimestampUtc)
{
	const FString Diagnostic =
		FWoTBootstrapViewModel::DiagnosticCodeText(
			DiagnosticCode).ToString();
	const FString Document = FString::Printf(
		TEXT("{\"diagnostic_code\":\"%s\",")
		TEXT("\"format\":\"wot-bootstrap-boot-failure-v1\",")
		TEXT("\"phase\":\"%s\",")
		TEXT("\"timestamp_utc\":\"%s\"}"),
		*Diagnostic, WoTBootstrapPhaseName(Phase), *FString(TimestampUtc));
	const FTCHARToUTF8 Utf8(*Document);
	TArray<uint8> Bytes;
	Bytes.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}



FWoTBootstrapStatus WoTBootFailureError(
	const TCHAR* Message, FString Path = FString())
{
	return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::Io, Message, MoveTemp(Path)));
}

FWoTBootstrapStatus WoTBootFailureFlushFile(
	const FWoTNativeFileHandle& File)
{
	FWoTBootstrapStatus Result = FWoTBootstrapStatus::Success();
	Result = FlushNativeFile(File);
	return Result;
}

FWoTBootstrapStatus WoTBootFailureFlushDirectory(
	const FWoTNativeDirectoryHandle& Directory)
{
	FWoTBootstrapStatus Result = FWoTBootstrapStatus::Success();
	Result = FlushNativeDirectory(Directory);
	return Result;
}

FWoTBootstrapStatus WoTBootFailurePublish(
	const FWoTNativeDirectoryHandle& Directory, FStringView TemporaryLeaf)
{
	FWoTBootstrapStatus Result = FWoTBootstrapStatus::Success();
	Result = RenameNativeEntryAtNoFollow(
			Directory, TemporaryLeaf, Directory,
			WoTBootFailureReceiptLeaf, true);
	return Result;
}

FWoTBootstrapStatus WoTWriteBootFailureReceiptAtRoot(
	FStringView InstallRoot,
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase,
	TArray<FString>* DirectoryFlushOrder = nullptr,
	const FWoTNativeFileIdentity* ExpectedRootIdentity = nullptr,
	const TArray<uint8>* ExactBytes = nullptr)
{
	FString Root = FPaths::ConvertRelativePathToFull(FString(InstallRoot));
	FPaths::NormalizeDirectoryName(Root);
	if (FPaths::IsRelative(Root)
		|| !FPaths::CollapseRelativeDirectories(Root))
	{
		return WoTBootFailureError(
			TEXT("the boot-failure receipt root is unsafe"), Root);
	}
	bool bCreatedRoot = false;
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> OpenedRoot = [&]()
	{
		using FResult = TWoTBootstrapResult<FWoTCanonicalNativeDirectory>;
		if (ExpectedRootIdentity == nullptr)
		{
			return OpenOrCreateCanonicalNativeDirectoryNoFollow(
				Root, bCreatedRoot);
		}
		TWoTBootstrapResult<FWoTNativeDirectoryHandle> Reopened =
			OpenNativeDirectoryNoFollow(Root);
		if (Reopened.IsError())
		{
			return FResult::Error(MoveTemp(Reopened).GetError());
		}
		if (!Reopened.GetValue().GetIdentity().IsSameObject(
				*ExpectedRootIdentity))
		{
			return FResult::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::UnsafePath,
				TEXT("the boot-failure receipt root identity changed"), Root));
		}
		FWoTCanonicalNativeDirectory Result;
		Result.CanonicalPath = Root;
		Result.Directory = MoveTemp(Reopened).GetValue();
		return FResult::Success(MoveTemp(Result));
	}();
	if (OpenedRoot.IsError())
	{
		return FWoTBootstrapStatus::Error(
			MoveTemp(OpenedRoot).GetError());
	}
	bool bCreatedState = false;
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> State =
		OpenOrCreateNativeDirectoryAtNoFollow(
			OpenedRoot.GetValue().Directory, TEXT("UpdaterState"), bCreatedState);
	if (State.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(State).GetError());
	}

	const FString TemporaryLeaf = FString::Printf(
		TEXT(".boot-failure-v1.%u.%llu.tmp"),
		FPlatformProcess::GetCurrentProcessId(),
		static_cast<unsigned long long>(FPlatformTime::Cycles64()));
	TWoTBootstrapResult<FWoTNativeFileHandle> Temporary =
		CreateNativeFileAtWriteNoFollow(State.GetValue(), TemporaryLeaf);
	if (Temporary.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Temporary).GetError());
	}
	FWoTNativeFileHandle TemporaryHandle =
		MoveTemp(Temporary).GetValue();
	const TArray<uint8> Bytes = ExactBytes != nullptr
		? *ExactBytes : WoTBootFailureCanonicalBytes(
			DiagnosticCode, Phase, FDateTime::UtcNow().ToIso8601());
	if (Bytes.IsEmpty())
	{
		TemporaryHandle = FWoTNativeFileHandle();
		(void)RemoveNativeEntryAtNoFollow(
			State.GetValue(), TemporaryLeaf, EWoTTreeEntryType::File);
		return WoTBootFailureError(
			TEXT("the boot-failure receipt could not be encoded"));
	}
	if (FWoTBootstrapStatus Written =
			WriteNativeFile(TemporaryHandle, Bytes);
		Written.IsError())
	{
		TemporaryHandle = FWoTNativeFileHandle();
		(void)RemoveNativeEntryAtNoFollow(
			State.GetValue(), TemporaryLeaf, EWoTTreeEntryType::File);
		return MoveTemp(Written);
	}
	if (FWoTBootstrapStatus Flushed =
			WoTBootFailureFlushFile(TemporaryHandle);
		Flushed.IsError())
	{
		TemporaryHandle = FWoTNativeFileHandle();
		(void)RemoveNativeEntryAtNoFollow(
			State.GetValue(), TemporaryLeaf, EWoTTreeEntryType::File);
		return MoveTemp(Flushed);
	}
	TemporaryHandle = FWoTNativeFileHandle();
	if (FWoTBootstrapStatus Published = WoTBootFailurePublish(
			State.GetValue(), TemporaryLeaf);
		Published.IsError())
	{
		(void)RemoveNativeEntryAtNoFollow(
			State.GetValue(), TemporaryLeaf, EWoTTreeEntryType::File);
		return MoveTemp(Published);
	}
	if (DirectoryFlushOrder != nullptr)
	{
		DirectoryFlushOrder->Add(TEXT("UpdaterState"));
	}
	if (FWoTBootstrapStatus Flushed =
			WoTBootFailureFlushDirectory(State.GetValue());
		Flushed.IsError())
	{
		return MoveTemp(Flushed);
	}
	if (bCreatedState)
	{
		if (DirectoryFlushOrder != nullptr)
		{
			DirectoryFlushOrder->Add(TEXT("InstallRoot"));
		}
		if (FWoTBootstrapStatus Flushed =
				WoTBootFailureFlushDirectory(
					OpenedRoot.GetValue().Directory);
			Flushed.IsError())
		{
			return MoveTemp(Flushed);
		}
	}
	if (bCreatedRoot)
	{
		if (DirectoryFlushOrder != nullptr)
		{
			DirectoryFlushOrder->Add(TEXT("InstallParent"));
		}
		if (FWoTBootstrapStatus Flushed =
				WoTBootFailureFlushDirectory(
					OpenedRoot.GetValue().ParentDirectory);
			Flushed.IsError())
		{
			return MoveTemp(Flushed);
		}
	}
	return FWoTBootstrapStatus::Success();
}
} // namespace

FWoTBootstrapStatus WoTWriteBootFailureReceipt(
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase)
{
	TWoTBootstrapResult<FWoTCanonicalNativeDirectory> Root =
		WoTReopenResolvedUserStateRoot();
	if (Root.IsError())
	{
		return FWoTBootstrapStatus::Error(MoveTemp(Root).GetError());
	}
	const FWoTNativeFileIdentity Identity =
		Root.GetValue().Directory.GetIdentity();
	const FString Path = Root.GetValue().CanonicalPath;
	return WoTWriteBootFailureReceiptAtRoot(
		Path, DiagnosticCode, Phase, nullptr, &Identity);
}



#if WITH_AUTOMATION_TESTS
FWoTBootstrapStatus WoTWriteBootFailureReceiptForTesting(
	FStringView InstallRoot,
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase,
	TArray<FString>* DirectoryFlushOrder)
{
	return WoTWriteBootFailureReceiptAtRoot(
		InstallRoot, DiagnosticCode, Phase, DirectoryFlushOrder);
}

TArray<uint8> WoTBootFailureReceiptCanonicalBytesForTesting(
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase,
	FStringView TimestampUtc)
{
	return WoTBootFailureCanonicalBytes(
		DiagnosticCode, Phase, TimestampUtc);
}

void SetWoTBootFailureReceiptRootForTesting(FString InstallRoot)
{
	FScopeLock Lock(&GWoTBootFailureReceiptTestRootGuard);
	GWoTBootFailureReceiptTestRoot = MoveTemp(InstallRoot);
}





#if !UE_BUILD_SHIPPING
[[noreturn]] void WoTWriteBootFailureReceiptAndExitForTesting(
	FStringView InstallRoot,
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase)
{
	const FWoTBootstrapStatus Receipt = WoTWriteBootFailureReceiptAtRoot(
		InstallRoot, DiagnosticCode, Phase);
	if (Receipt.IsError())
	{
		FPlatformMisc::LowLevelOutputDebugStringf(
			TEXT("WoT test boot-failure receipt publication failed: %s\n"),
			*Receipt.GetError().ToString());
	}
	std::_Exit(EXIT_FAILURE);
}
#endif
#endif
