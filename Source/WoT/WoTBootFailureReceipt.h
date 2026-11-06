#pragma once

#include "CoreMinimal.h"
#include "WoTBootstrapViewModel.h"
#include "WoTBootstrapCore/Error.h"

inline constexpr TCHAR WoTBootFailureReceiptLeaf[] =
	TEXT("boot-failure-v1.json");

/** Publish the canonical guarded-boot failure receipt below the per-user state
 * root. The receipt contains no source paths or free-form error text. */
FWoTBootstrapStatus WoTWriteBootFailureReceipt(
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase);

#if WITH_AUTOMATION_TESTS
FWoTBootstrapStatus WoTWriteBootFailureReceiptForTesting(
	FStringView InstallRoot,
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase,
	TArray<FString>* DirectoryFlushOrder = nullptr);
TArray<uint8> WoTBootFailureReceiptCanonicalBytesForTesting(
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase,
	FStringView TimestampUtc);
void SetWoTBootFailureReceiptRootForTesting(FString InstallRoot);
#if !UE_BUILD_SHIPPING
[[noreturn]] WOT_API void WoTWriteBootFailureReceiptAndExitForTesting(
	FStringView InstallRoot,
	EWoTBootstrapDiagnosticCode DiagnosticCode,
	EWoTBootstrapPhase Phase);
#endif
#endif
