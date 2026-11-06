// The map name every UI surface shows, routed through the adopted identity
// and the retail LevelInfo.Title of guarded (materialized) worlds.
#pragma once

#include "CoreMinimal.h"

class UWorld;

/** The retail map identity of a game world ("Battle3_01"). A guarded world is
 * a neutral shell (/WoTNeutralShells/WoTMapShellNN): its materialized root
 * carries the authenticated identity (the adopted shell route is the
 * fallback). A development umap answers with its PIE-stripped leaf name,
 * which IS the identity there. Empty only for a null world. */
WOT_API FString WoTWorldMapIdentity(const UWorld* World);

/** Retail Level.Title exactly as ArenaScoreBoard.DrawHeader prints it
 * (uc:315-318; BattleScoreBoard.uc:70; CitadelTeamOverview inherits it). A
 * materialized world answers with its world record's LevelInfo Title, which
 * may legitimately be empty (retail then draws no map line). A development
 * umap carries no Title and answers with WoTWorldMapIdentity. */
WOT_API FString WoTWorldDisplayTitle(const UWorld* World);

/** True when the world was materialized from an authenticated world record
 * (it then has a retail LevelInfo Title, possibly empty). */
WOT_API bool WoTWorldHasRetailLevelTitle(const UWorld* World);

/** A travel URL's map segment as the player reads it (PlayerPawn's
 * "Connecting" progress line, which formats FURL.Map): a guarded
 * neutral-shell route ("/WoTNeutralShells/WoTMapShell12" or its leaf) becomes
 * the adopted retail identity ("Battle2_01"); anything else is unchanged. */
WOT_API FString WoTDisplayMapForTravelURLMap(const FString& UrlMap);
