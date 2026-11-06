# Public export manifest

`public-files.txt` is the reviewed, exact source-path manifest for the public
export. The exporter fails closed when the manifest is missing, malformed,
admits a denied path or LFS object, or names a path that is absent from the
selected Git tree.

To prepare a candidate update for review:

```sh
python3 Tools/public/export_public_tree.py propose --repo .
```

The proposal comes from a broad path policy. It never changes the manifest
implicitly. Review its diff, update `public-files.txt`, and run the
exporter and leak guard before accepting a new path.

Every proposal candidate must be classified. `private-files.txt` is the
maintainer's reviewed list of candidates that stay private (it is not
exported), in the same format as the manifest (sorted, one path per line,
naming itself). Export fails while a
candidate is in neither list, is in both, or the private list names a path that
is missing or that the deny policy already excludes. A new file therefore stops
the export until someone decides which side it belongs on. To list the paths
export would refuse:

```sh
python3 Tools/public/export_public_tree.py propose --repo . --unclassified
```

## Generated identity registries

The production cache contract and these generated registries carry identities,
types, sizes and counts only, never game data. They are published:

- `Build/WoTRelease/cache-contract-v3-production.json`
- `Source/WoTRetailReader/Public/WoTRetailReader/Generated/CacheContractProduction.generated.h`
- `Source/WoTRetailReader/Public/WoTRetailReader/Generated/CacheContractV3.generated.h`
- `Source/WoTRetailReader/Public/WoTRetailReader/Generated/RequiredWorlds.generated.h`
- `Source/WoTRetailReader/Public/WoTRetailReader/Generated/WorldProperties.generated.h`
- `Source/WoTRetailReader/Public/WoTRetailReader/Generated/WorldTextures.generated.h`

## Tables built from the player's data

These files keep native behavior and generic resolvers. Their rows are built
during payload activation from the class defaults in the player's own content
cache; no compiled table or fallback remains:

- `Source/WoT/WoTAnimTable.cpp`
- `Source/WoT/WoTSoundTable.cpp`
- `Source/WoT/WoTCreatureChunk.cpp`
- `Source/WoT/WoTWeaveFX.cpp`
- `Source/WoT/WoTFootsteps.h`
- `Source/WoT/WoTFootsteps.cpp`
- `Source/WoT/AI/WoTNPCWeaponData.cpp`
- `Source/WoT/AI/WoTCreatureData.cpp`
- `Source/WoT/WoTBloodFX.cpp`
- `Source/WoT/WoTDecal.cpp`
- `Source/WoT/Traps/WoTFireWall.cpp`
- `Source/WoT/WoTUE1LightRegistry.cpp`
- `Source/WoT/WoTMeshFX.cpp`

The same kinds of fact (names, scalars, counts, sizes, hashes) also appear in:

- `Tools/import/import_pickups.py`, `Tools/import/import_seal_skins.py`
  (pickup mesh names, DrawScale, palette RGB)
- `Source/WoT/Tests/WoTRuntimeMapProjectionTests.cpp`,
  `Source/WoTRetailReader/Private/Tests/FrontEndWorldTests.cpp` (census literals)

## Third-party files

`ThirdParty/vcpkg/vcpkg.json` and the vcpkg overlay triplets are published. For
innoextract, the upstream source archive, patches, recipe and README are
published; the prebuilt libraries are not, and a checkout fetches or builds them
(see [docs/BUILD-PATHS.md](../../docs/BUILD-PATHS.md)).

`binary-allowlist.sha256` is a separate reviewed list for non-text content. Each
entry is an exact SHA-256 and path; changing any listed file's bytes requires an
explicit hash review.
