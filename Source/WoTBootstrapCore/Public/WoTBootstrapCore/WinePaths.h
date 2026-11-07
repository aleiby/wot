// Unix-side paths for a Windows build running under Wine (Proton on Linux and
// Steam Deck), where the player's real folders live outside the Wine prefix.
#pragma once

#include "CoreMinimal.h"

/**
 * The Linux user's Downloads folder for a Unix HOME, e.g. "/home/player" ->
 * "/home/player/Downloads". Empty when HOME is empty, relative or "/".
 */
WOTBOOTSTRAPCORE_API FString WoTWineUnixDownloadsPath(FStringView UnixHome);

/**
 * Wine's DOS path for a Unix path, e.g. "/home/player/Downloads" ->
 * "Z:\home\player\Downloads". Empty outside Wine, for an empty path, or when
 * Wine cannot map it.
 */
WOTBOOTSTRAPCORE_API FString WoTWineDosPathForUnixPath(FStringView UnixPath);
