#include "WoTLaunchEngineHooks.h"

#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "WoTBootstrapCore/NativeHandles.h"
#include "WoTBootstrapCore/ProfileSession.h"
#include "WoTBootstrapCore/TestOverrides.h"
#include "WoTUserDataRoot.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>

#if PLATFORM_MAC
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
FString WoTLaunchProfileSessionRoot()
{
#if WOT_BOOTSTRAP_TEST_IDENTITIES
	FString HarnessRoot;
	if (FParse::Value(FCommandLine::Get(), TEXT("wottestinstallroot="),
		HarnessRoot))
	{
		HarnessRoot = FPaths::ConvertRelativePathToFull(MoveTemp(HarnessRoot));
		FPaths::NormalizeDirectoryName(HarnessRoot);
		return !FPaths::IsRelative(HarnessRoot)
			&& FPaths::CollapseRelativeDirectories(HarnessRoot)
				? HarnessRoot : FString();
	}
#endif
	return WoTUserDataRoot();
}

enum class EMaterializerState : uint8
{
	Open,
	Registering,
	Registered,
	DispatchStarted,
};

std::atomic<EMaterializerState> GMaterializerState{EMaterializerState::Open};
std::atomic<FWoTGuardedWorldMaterializer> GWorldMaterializer{nullptr};
std::atomic<FWoTGuardedWorldExposureCommitter> GWorldExposureCommitter{nullptr};

bool Reject(FString* Error, const TCHAR* Message)
{
	if (Error)
	{
		*Error = Message;
	}
	return false;
}

TWoTBootstrapResult<TArray<uint8>> FixedPreferenceReadError(
	const TCHAR* Message)
{
	return TWoTBootstrapResult<TArray<uint8>>::Error(MakeWoTBootstrapError(
		EWoTBootstrapError::Io, Message, TEXT("WoTUserPreferences")));
}

#if PLATFORM_MAC
class FWoTScopedFileDescriptor final
{
public:
	explicit FWoTScopedFileDescriptor(int InValue = -1) : Value(InValue) {}
	~FWoTScopedFileDescriptor() { if (Value >= 0) { close(Value); } }
	FWoTScopedFileDescriptor(const FWoTScopedFileDescriptor&) = delete;
	FWoTScopedFileDescriptor& operator=(const FWoTScopedFileDescriptor&) = delete;
	int Get() const { return Value; }
	void Reset(int InValue)
	{
		if (Value >= 0) { close(Value); }
		Value = InValue;
	}

private:
	int Value;
};

bool WoTMacIdentityMatches(const struct stat& Left, const struct stat& Right)
{
	return Left.st_dev == Right.st_dev && Left.st_ino == Right.st_ino
		&& Left.st_mode == Right.st_mode && Left.st_uid == Right.st_uid
		&& Left.st_size == Right.st_size
		&& Left.st_mtimespec.tv_sec == Right.st_mtimespec.tv_sec
		&& Left.st_mtimespec.tv_nsec == Right.st_mtimespec.tv_nsec;
}

TWoTBootstrapResult<TArray<uint8>> ReadFixedWoTUserPreferences(
	uint32 MaximumBytes)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	const uid_t UserId = getuid();
	const FString RootPath = WoTUserDataRoot();
	const FString RootSuffix =
		TEXT("/Library/Application Support/WoT Reborn");
	if (RootPath.IsEmpty()
		|| !RootPath.EndsWith(RootSuffix, ESearchCase::CaseSensitive)
		|| RootPath.Len() <= RootSuffix.Len())
	{
		return FixedPreferenceReadError(
			TEXT("the shared per-user data root could not be established"));
	}
	const FString HomePath = RootPath.LeftChop(RootSuffix.Len());
	const FTCHARToUTF8 HomeUtf8(*HomePath);

	FWoTScopedFileDescriptor Home(open(HomeUtf8.Get(),
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	if (Home.Get() < 0)
	{
		return FixedPreferenceReadError(
			TEXT("the native per-user home handle could not be pinned"));
	}
	struct stat Identities[5] = {};
	if (fstat(Home.Get(), &Identities[0]) != 0
		|| !S_ISDIR(Identities[0].st_mode) || Identities[0].st_uid != UserId)
	{
		return FixedPreferenceReadError(
			TEXT("the native per-user home handle failed identity validation"));
	}

	FWoTScopedFileDescriptor Directories[4];
	int Parent = Home.Get();
	const ANSICHAR* Components[] = {
		"Library", "Application Support", "WoT Reborn", "Preferences"};
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const int Descriptor = openat(Parent, Components[Index],
			O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (Descriptor < 0)
		{
			if (errno == ENOENT)
			{
				return FResult::Success(TArray<uint8>{});
			}
			return FixedPreferenceReadError(
				TEXT("a fixed per-user preference directory was unsafe or unreadable"));
		}
		Directories[Index].Reset(Descriptor);
		if (fstat(Descriptor, &Identities[Index + 1]) != 0
			|| !S_ISDIR(Identities[Index + 1].st_mode)
			|| Identities[Index + 1].st_uid != UserId)
		{
			return FixedPreferenceReadError(
				TEXT("a fixed per-user preference directory changed identity"));
		}
		Parent = Descriptor;
	}

	FWoTScopedFileDescriptor File(openat(Parent, "WoTUserPreferences",
		O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	if (File.Get() < 0)
	{
		if (errno == ENOENT)
		{
			return FResult::Success(TArray<uint8>{});
		}
		return FixedPreferenceReadError(
			TEXT("the fixed WoTUserPreferences leaf was unsafe or unreadable"));
	}
	struct stat Before = {};
	if (fstat(File.Get(), &Before) != 0 || !S_ISREG(Before.st_mode)
		|| Before.st_uid != UserId || Before.st_nlink != 1 || Before.st_size < 0)
	{
		return FixedPreferenceReadError(
			TEXT("the fixed WoTUserPreferences leaf failed identity validation"));
	}

	const uint64 ReadLimit = static_cast<uint64>(MaximumBytes) + 1;
	const uint64 Requested64 = FMath::Min<uint64>(
		static_cast<uint64>(Before.st_size), ReadLimit);
	if (Requested64 > static_cast<uint64>(MAX_int32))
	{
		return FixedPreferenceReadError(
			TEXT("the fixed WoTUserPreferences bounded read was not representable"));
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(Requested64));
	int32 Offset = 0;
	while (Offset < Bytes.Num())
	{
		const ssize_t Read = pread(File.Get(), Bytes.GetData() + Offset,
			Bytes.Num() - Offset, Offset);
		if (Read < 0 && errno == EINTR)
		{
			continue;
		}
		if (Read <= 0)
		{
			return FixedPreferenceReadError(
				TEXT("the fixed WoTUserPreferences bounded read was torn"));
		}
		Offset += static_cast<int32>(Read);
	}

	struct stat After = {};
	if (fstat(File.Get(), &After) != 0 || !WoTMacIdentityMatches(Before, After))
	{
		return FixedPreferenceReadError(
			TEXT("the fixed WoTUserPreferences leaf changed during its read"));
	}
	struct stat Recheck = {};
	if (fstat(Home.Get(), &Recheck) != 0
		|| !WoTMacIdentityMatches(Identities[0], Recheck))
	{
		return FixedPreferenceReadError(
			TEXT("the per-user root identity changed during preference parsing"));
	}
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (fstat(Directories[Index].Get(), &Recheck) != 0
			|| !WoTMacIdentityMatches(Identities[Index + 1], Recheck))
		{
			return FixedPreferenceReadError(
				TEXT("a preference directory identity changed during parsing"));
		}
	}
	return FResult::Success(MoveTemp(Bytes));
}
#elif PLATFORM_WINDOWS
bool WoTWindowsOpenWasAbsent(const FWoTBootstrapError& Error)
{
	return Error.Code == EWoTBootstrapError::Io
		&& (Error.Message.EndsWith(TEXT("Windows error 2"))
			|| Error.Message.EndsWith(TEXT("Windows error 3")));
}

TWoTBootstrapResult<TArray<uint8>> ReadFixedWoTUserPreferences(
	uint32 MaximumBytes)
{
	using FResult = TWoTBootstrapResult<TArray<uint8>>;
	const FString RootPath = WoTUserDataRoot();
	if (RootPath.IsEmpty())
	{
		return FixedPreferenceReadError(
			TEXT("the shared per-user data root could not be established"));
	}
	TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenRoot =
		OpenNativeDirectoryNoFollow(RootPath);
	if (OpenRoot.IsError())
	{
		return FixedPreferenceReadError(
			TEXT("the native per-user data-directory handle could not be pinned"));
	}
	FWoTNativeDirectoryHandle Root = MoveTemp(OpenRoot).GetValue();
	const FWoTNativeFileIdentity RootBefore = Root.GetIdentity();

	TWoTBootstrapResult<FWoTNativeDirectoryHandle> OpenDirectory =
		OpenNativeDirectoryAtNoFollow(Root, TEXT("Preferences"));
	if (OpenDirectory.IsError())
	{
		if (WoTWindowsOpenWasAbsent(OpenDirectory.GetError()))
		{
			return FResult::Success(TArray<uint8>{});
		}
		return FixedPreferenceReadError(
			TEXT("the fixed per-user Preferences directory was unsafe or unreadable"));
	}
	FWoTNativeDirectoryHandle Directory =
		MoveTemp(OpenDirectory).GetValue();
	const FWoTNativeFileIdentity DirectoryBefore = Directory.GetIdentity();

	TWoTBootstrapResult<FWoTNativeFileHandle> OpenFile =
		OpenNativeFileAtReadNoFollow(Directory, TEXT("WoTUserPreferences"));
	if (OpenFile.IsError())
	{
		if (WoTWindowsOpenWasAbsent(OpenFile.GetError()))
		{
			return FResult::Success(TArray<uint8>{});
		}
		return FixedPreferenceReadError(
			TEXT("the fixed WoTUserPreferences leaf was unsafe or unreadable"));
	}
	FWoTNativeFileHandle File = MoveTemp(OpenFile).GetValue();
	const FWoTNativeFileIdentity Before = File.GetIdentity();
	if (Before.Type != EWoTTreeEntryType::File || Before.LinkCount != 1)
	{
		return FixedPreferenceReadError(
			TEXT("the fixed WoTUserPreferences leaf failed identity validation"));
	}

	TArray<uint8> Bytes;
	if (Before.Size > MaximumBytes)
	{
		// The parser needs only the bounded oversized signal, never attacker-owned
		// bytes beyond the signed maximum.
		Bytes.SetNumZeroed(static_cast<int32>(MaximumBytes) + 1);
	}
	else
	{
		TWoTBootstrapResult<TArray<uint8>> Read = File.ReadAll();
		if (Read.IsError())
		{
			return FixedPreferenceReadError(
				TEXT("the fixed WoTUserPreferences bounded read was torn"));
		}
		Bytes = MoveTemp(Read).GetValue();
	}

	TWoTBootstrapResult<FWoTNativeFileIdentity> After =
		File.QueryCurrentIdentity();
	TWoTBootstrapResult<FWoTNativeFileIdentity> RootAfter =
		Root.QueryCurrentIdentity();
	TWoTBootstrapResult<FWoTNativeFileIdentity> DirectoryAfter =
		Directory.QueryCurrentIdentity();
	if (After.IsError() || RootAfter.IsError() || DirectoryAfter.IsError()
		|| Before != After.GetValue()
		|| RootBefore != RootAfter.GetValue()
		|| DirectoryBefore != DirectoryAfter.GetValue())
	{
		return FixedPreferenceReadError(
			TEXT("the per-user preference handle chain changed during its read"));
	}
	return FResult::Success(MoveTemp(Bytes));
}
#else
TWoTBootstrapResult<TArray<uint8>> ReadFixedWoTUserPreferences(uint32)
{
	return TWoTBootstrapResult<TArray<uint8>>::Success(TArray<uint8>{});
}
#endif

bool PublishFixedEarlyUIForStartup(FString* Error)
{
#if !WITH_EDITOR && !IS_PROGRAM
	const FString DataRoot = WoTLaunchProfileSessionRoot();
	FWoTBootstrapStatus Session =
		WoTAcquireProcessProfileSessionShared(DataRoot);
	if (Session.IsError())
	{
		if (Error)
		{
			*Error = WoTProfileSessionFailureMessage(
				DataRoot, Session.GetError());
		}
		return false;
	}
#endif
	const FWoTEarlyUILimits Limits = FWoTEarlyUILimits::Production();
	const FWoTInventoryLocaleSet Locales = FWoTInventoryLocaleSet::Production();
	TWoTBootstrapResult<TArray<uint8>> Read =
		ReadFixedWoTUserPreferences(Limits.MaxSerializedBytes);
	if (Read.IsError())
	{
		if (Error) { *Error = Read.GetError().ToString(); }
		return false;
	}
	TArray<uint8> Bytes = MoveTemp(Read).GetValue();
	TWoTBootstrapResult<FWoTAuthenticatedEarlyUI> Parsed =
		ParseEarlyUIV1(Bytes, Limits, Locales);
	if (Parsed.IsError())
	{
		if (Error) { *Error = Parsed.GetError().ToString(); }
		return false;
	}
	FWoTBootstrapStatus Published = GetEarlyUIAuthority().PublishOnce(
		MoveTemp(Parsed).GetValue());
	if (Published.IsError())
	{
		if (Error) { *Error = Published.GetError().ToString(); }
		return false;
	}
	return true;
}
}  // namespace

class FWoTLaunchModule final : public FDefaultModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultModuleImpl::StartupModule();
#if !WITH_EDITOR && !IS_PROGRAM
		const FString DataRoot = WoTLaunchProfileSessionRoot();
		FWoTBootstrapStatus Acquired =
			WoTAcquireProcessProfileSessionShared(DataRoot);
		if (Acquired.IsError())
		{
			const FString Message = WoTProfileSessionFailureMessage(
				DataRoot, Acquired.GetError());
			FPlatformMisc::LowLevelOutputDebugStringf(TEXT("%s\n"), *Message);
			if (!FApp::IsUnattended())
			{
				FPlatformMisc::MessageBoxExt(
					EAppMsgType::Ok, *Message, TEXT("WoT Reborn"));
			}
			std::_Exit(EXIT_FAILURE);
		}
#endif
	}
};

IMPLEMENT_MODULE(FWoTLaunchModule, WoTLaunch);

bool WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
	const FWoTEarlyUINativeProjectionSink& Sink, FString* Error)
{
	FWoTBootstrapStatus Applied =
		GetEarlyUIAuthority().ApplyNativeProjectionOnce(Sink);
	if (Applied.IsError())
	{
		if (Error) { *Error = Applied.GetError().ToString(); }
		return false;
	}
	if (Error) { Error->Reset(); }
	return true;
}

extern "C" bool
WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration_EngineBridge(
	FWoTEarlyUIEngineProjection Apply, void* Context, FString* Error)
{
	if (Apply == nullptr)
	{
		return Reject(Error, TEXT("the EarlyUI engine projection sink is null"));
	}
	if (!PublishFixedEarlyUIForStartup(Error))
	{
		return false;
	}
	auto TypedApply = [Apply, Context](uint16 Width, uint16 Height,
		EWoTWindowMode WindowMode) -> FWoTBootstrapStatus
	{
		if (!Apply(Width, Height, static_cast<uint8>(WindowMode), Context))
		{
			return FWoTBootstrapStatus::Error(MakeWoTBootstrapError(
				EWoTBootstrapError::CorruptInstallation,
				TEXT("the real engine window-configuration path rejected EarlyUI"),
				TEXT("EarlyUI-v1")));
		}
		return FWoTBootstrapStatus::Success();
	};
	const FWoTEarlyUINativeProjectionSink Sink{TypedApply};
	return WoTGuardedStartup_ApplyEarlyUINativeProjectionAtWindowConfiguration(
		Sink, Error);
}

bool WoTRegisterGuardedWorldMaterializer(
	FWoTGuardedWorldMaterializer Materializer,
	FWoTGuardedWorldExposureCommitter ExposureCommitter, FString* Error)
{
	if (!Materializer || !ExposureCommitter)
	{
		return Reject(Error,
			TEXT("a null world materializer callback pair cannot be registered"));
	}

	EMaterializerState Expected = EMaterializerState::Open;
	if (!GMaterializerState.compare_exchange_strong(Expected,
		EMaterializerState::Registering, std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		if (Expected == EMaterializerState::DispatchStarted)
		{
			return Reject(Error,
				TEXT("world materializer registration is too late"));
		}
		return Reject(Error,
			TEXT("a world materializer is already registered or registering"));
	}

	// The callback is published before the Registered state. An acquiring
	// dispatcher can therefore never observe Registered with a null slot.
	GWorldMaterializer.store(Materializer, std::memory_order_release);
	GWorldExposureCommitter.store(ExposureCommitter, std::memory_order_release);
	GMaterializerState.store(EMaterializerState::Registered,
		std::memory_order_release);
	if (Error)
	{
		Error->Reset();
	}
	return true;
}

extern "C" bool WoTGuardedStartup_CommitWorldExposure(
	UWorld* World, FString* Error)
{
	if (!World)
	{
		return Reject(Error, TEXT("a null world exposure cannot be committed"));
	}
	if (GMaterializerState.load(std::memory_order_acquire)
		!= EMaterializerState::DispatchStarted)
	{
		return Reject(Error,
			TEXT("world exposure commit did not follow guarded materialization"));
	}
	FWoTGuardedWorldExposureCommitter Committer =
		GWorldExposureCommitter.load(std::memory_order_acquire);
	if (!Committer || !Committer(World, Error))
	{
		if (Error && Error->IsEmpty())
		{
			*Error = TEXT("the registered exposure committer rejected this world");
		}
		return false;
	}
	if (Error)
	{
		Error->Reset();
	}
	return true;
}

extern "C" bool WoTGuardedStartup_MaterializeWorld(
	UWorld* World, EWoTWorldStartPath Path, FString* Error)
{
	if (!World)
	{
		return Reject(Error, TEXT("a null world cannot be materialized"));
	}

	EMaterializerState State = GMaterializerState.load(std::memory_order_acquire);
	if (State == EMaterializerState::Registered)
	{
		GMaterializerState.compare_exchange_strong(State,
			EMaterializerState::DispatchStarted, std::memory_order_acq_rel,
			std::memory_order_acquire);
		State = GMaterializerState.load(std::memory_order_acquire);
	}
	if (State != EMaterializerState::DispatchStarted)
	{
		return Reject(Error,
			TEXT("no world materializer was registered before world start"));
	}

	FWoTGuardedWorldMaterializer Materializer =
		GWorldMaterializer.load(std::memory_order_acquire);
	if (!Materializer)
	{
		return Reject(Error, TEXT("the world materializer slot is empty"));
	}

	if (!Materializer(World, Path, Error))
	{
		if (Error && Error->IsEmpty())
		{
			*Error = TEXT("the registered materializer rejected this world");
		}
		return false;
	}
	if (Error)
	{
		Error->Reset();
	}
	return true;
}
