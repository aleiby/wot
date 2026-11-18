#include "../NativeHandleTestFactory.h"

#if WITH_AUTOMATION_TESTS

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace
{
FString Utf8ToString(const ANSICHAR* Text)
{
	const FUTF8ToTCHAR Converted(Text);
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}
}  // namespace

namespace WoTNativeHandleTestFactory
{
FString CreateTemporaryRoot()
{
	ANSICHAR Template[] = "/tmp/wot-native-XXXXXX";
	if (mkdtemp(Template) == nullptr)
	{
		return FString();
	}
	return Utf8ToString(Template);
}

void RemoveTree(const FString& AbsolutePath)
{
	if (AbsolutePath.IsEmpty())
	{
		return;
	}
	// Depth-first removal via the shell would be simpler but would follow links;
	// nftw with FTW_PHYS is not worth the surface here, so the specs create only
	// shallow trees and remove them explicitly.
	const FTCHARToUTF8 Utf8(*AbsolutePath);
	FString Command = FString::Printf(TEXT("rm -rf '%s'"), *AbsolutePath);
	const FTCHARToUTF8 CommandUtf8(*Command);
	const int Result = system(CommandUtf8.Get());
	(void)Result;
}

bool WriteFile(const FString& AbsolutePath, TConstArrayView<uint8> Bytes)
{
	const FTCHARToUTF8 Utf8(*AbsolutePath);
	const int Descriptor = open(Utf8.Get(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (Descriptor < 0)
	{
		return false;
	}
	bool bOk = true;
	int64 Offset = 0;
	while (Offset < Bytes.Num())
	{
		const ssize_t Written = write(Descriptor, Bytes.GetData() + Offset,
			static_cast<size_t>(Bytes.Num() - Offset));
		if (Written <= 0)
		{
			bOk = false;
			break;
		}
		Offset += Written;
	}
	close(Descriptor);
	return bOk;
}

bool CreateSparseFile(const FString& AbsolutePath, uint64 Size)
{
	const FTCHARToUTF8 Utf8(*AbsolutePath);
	const int Descriptor = open(Utf8.Get(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (Descriptor < 0)
	{
		return false;
	}
	const bool bOk = Size <= static_cast<uint64>(TNumericLimits<int64>::Max())
		&& ftruncate(Descriptor, static_cast<off_t>(Size)) == 0;
	close(Descriptor);
	return bOk;
}

bool CreateDirectory(const FString& AbsolutePath)
{
	const FTCHARToUTF8 Utf8(*AbsolutePath);
	return mkdir(Utf8.Get(), 0755) == 0;
}

bool CreateSymbolicLink(const FString& LinkAbsolutePath, const FString& Target)
{
	const FTCHARToUTF8 LinkUtf8(*LinkAbsolutePath);
	const FTCHARToUTF8 TargetUtf8(*Target);
	return symlink(TargetUtf8.Get(), LinkUtf8.Get()) == 0;
}

bool CreateDirectoryAncestorAlias(
	const FString& LinkAbsolutePath, const FString& Target)
{
	return CreateSymbolicLink(LinkAbsolutePath, Target);
}

bool CreateHardLink(const FString& LinkAbsolutePath, const FString& ExistingAbsolutePath)
{
	const FTCHARToUTF8 LinkUtf8(*LinkAbsolutePath);
	const FTCHARToUTF8 ExistingUtf8(*ExistingAbsolutePath);
	return link(ExistingUtf8.Get(), LinkUtf8.Get()) == 0;
}

bool ReplaceByRename(const FString& AbsolutePath, const FString& ReplacementAbsolutePath)
{
	const FTCHARToUTF8 TargetUtf8(*AbsolutePath);
	const FTCHARToUTF8 ReplacementUtf8(*ReplacementAbsolutePath);
	return rename(ReplacementUtf8.Get(), TargetUtf8.Get()) == 0;
}

bool ReplaceDirectoryByRename(const FString& AbsolutePath,
	const FString& ReplacementAbsolutePath)
{
	const FString Retired = AbsolutePath + TEXT("-retired");
	const FTCHARToUTF8 TargetUtf8(*AbsolutePath);
	const FTCHARToUTF8 ReplacementUtf8(*ReplacementAbsolutePath);
	const FTCHARToUTF8 RetiredUtf8(*Retired);
	if (rename(TargetUtf8.Get(), RetiredUtf8.Get()) != 0)
	{
		return false;
	}
	if (rename(ReplacementUtf8.Get(), TargetUtf8.Get()) == 0)
	{
		return true;
	}
	rename(RetiredUtf8.Get(), TargetUtf8.Get());
	return false;
}

bool RemoveName(const FString& AbsolutePath)
{
	const FTCHARToUTF8 Utf8(*AbsolutePath);
	return unlink(Utf8.Get()) == 0;
}

bool WaitPastChangeStateTick(uint64)
{
	// APFS ctime has nanosecond resolution: the next mutation always differs.
	return true;
}

bool ChangeMetadataOnly(const FString& AbsolutePath)
{
	// An extended attribute, as the launch-time com.apple.provenance/macl ones
	// are. APFS moves ctime but not the content, mtime, size, links or file id.
	static uint32 Generation = 0;
	const FTCHARToUTF8 PathUtf8(*AbsolutePath);
	const FTCHARToUTF8 ValueUtf8(*FString::Printf(TEXT("%u"), ++Generation));
	return setxattr(PathUtf8.Get(), "com.wot.metadata-only", ValueUtf8.Get(),
		static_cast<size_t>(ValueUtf8.Length()), 0, XATTR_NOFOLLOW) == 0;
}
}  // namespace WoTNativeHandleTestFactory

#endif  // WITH_AUTOMATION_TESTS
