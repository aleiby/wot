# Arena_07 BSP proof tool contract

`make_plan.py` emits plan version 1 for
`/Game/WoT/Maps/Geo/Arena_07/Arena_07`. The gameplay pawn default is
`/Script/WoT.WoTAesSedai`, matching `AWoTGameMode::DefaultPawnClass`. Blocking
shots use `/Script/WoT.WoTDartProjectile`; every non-portal shot carries
`expect_block:true`, every portal-directed shot carries `expect_block:false`,
and the plan pins ten travel captures.

The blocked-route case uses `/Script/WoT.WoTSeekerProjectile`. This is the
shipped `AWoTSeekerProjectile` subclass of `AWoTSeekingProjectile`; the latter's
`CalcNextDirection` calls `UNavigationSystemV1::FindPathToLocationSynchronously`
in `Source/WoT/Projectiles/WoTSeekingProjectile.cpp`. The proof therefore
exercises the same nav-driven steering path as gameplay rather than a harness
substitute.

The render JSON schema is the packaged harness output: one file per `main` or
`portal` role with a `meshes` array. Each mesh contains loaded object identity,
runtime flags, ordered static materials, LOD0 sections, full CPU vertex/index
streams, local bounds, and triangle total. The comparator canonicalizes
triangles only by cyclic corner rotation and groups them by material slot over
the union of meshes in a role. Both reference and injected imports use the
same deterministic fractal split, so split-vs-split comparison is trivial while
still requiring every carrier's canonical part identity and exact ordered slot
list.

`--bsp-proof-carrier` is an alias for the general `--bsp-stripped` path. It runs `split_world_glb` over both the
main and `_portals` source GLBs using the runtime flipbook texture identities,
then emits one payload carrier per returned part. Part names are exactly the
splitter's filename-derived names (`Arena_07`, `Arena_07_portals`, and any
`_Fractal_NNN` suffixes).

## Portal collision policy

Reference A's portal static-mesh sections retain Interchange's default
`bEnableCollision=true`; its no-collision policy is applied at actor level.
That metadata divergence is intentional and reference A must not be mutated.
The comparator gates only proof B's payload contract (all main section bits
true and all portal section bits false), does not require A/B section-bit
equality, and uses portal-directed no-hit shots on both sides as the behavioral
equivalence proof.

Every mesh's sections must independently and exactly tile its index stream,
with derived min/max vertex metadata and triangle totals. Triangle equivalence
uses centroid buckets on a `4e-4` grid and tolerance-checked multiset matching,
so equal/tied centroids cannot be mis-paired by sort order. Collision minimums,
required IDs, the reference hit floor, travel cycle count, nonzero per-cycle nav
tiles, final seeking capture, and used-physical growth are all plan-gated.
Seeking capture first proves the authored LOS is blocked, then samples the
shipped projectile every tick; the selected side comes from the horizontal
cross-product sign against the plan obstruction center, and its final record
includes the last valid endpoint, completion reason, and hit actor class.
