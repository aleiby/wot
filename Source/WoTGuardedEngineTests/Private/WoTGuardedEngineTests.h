// Shared by the guarded engine test program's entry point and its specs.
#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

/** Every spec in this program runs in its Program context. */
constexpr EAutomationTestFlags WoTGuardedEngineSpecFlags =
	EAutomationTestFlags::ProgramContext | EAutomationTestFlags::EditorContext
	| EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;

/** The cache-decode inputs main parsed from the command line: --cache-root (a
 * directory holding current/manifest.json) and --decode-receipt (the JSON
 * receipt to write). Empty when absent. */
struct FWoTGuardedCacheDecodeInputs
{
	FString CacheRoot;
	FString DecodeReceipt;
};

const FWoTGuardedCacheDecodeInputs& WoTGuardedCacheDecodeInputs();
