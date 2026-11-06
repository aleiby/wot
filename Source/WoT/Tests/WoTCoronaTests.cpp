// Specs for the corona leaf gate: the UE1 PointRegion walk (UModel::
// PointRegion — front child while PlaneDot >= 0, leaf = last node's
// iLeaf[side]) over importer-shaped flat arrays, the zero-extent BSP
// LineCheck (corona LOS: only csg nodes block), and the viewer-override
// path the shot harness uses.
#include "Misc/AutomationTest.h"
#include "WoTTestWorld.h"

#if WITH_AUTOMATION_TESTS

#include "../WoTLeafMap.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

namespace
{
	// Two-level tree: node 0 splits on z=0 (back -> leaf 0), its front child
	// splits on x=100 (back -> leaf 1, front -> leaf 2).
	void FillTestTree(AWoTLeafMap* Map)
	{
		Map->NodePlanes = {0, 0, 1, 0,   1, 0, 0, 100};
		Map->NodeChildren = {INDEX_NONE, 1,   INDEX_NONE, INDEX_NONE};
		Map->NodeLeaves = {0, INDEX_NONE,   1, 2};
		Map->NumLeaves = 3;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLeafPointWalkTest,
	"WoT.Corona.LeafPointWalk",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLeafPointWalkTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTLeafMap* Map = World->SpawnActor<AWoTLeafMap>();
	FillTestTree(Map);

	TestEqual(TEXT("below the z=0 split"),
		Map->PointLeaf(FVector(0, 0, -5)), 0);
	TestEqual(TEXT("above, x < 100"),
		Map->PointLeaf(FVector(0, 0, 5)), 1);
	TestEqual(TEXT("above, x >= 100"),
		Map->PointLeaf(FVector(200, 0, 5)), 2);
	// UE1 PlaneDot >= 0 takes the FRONT side: a point exactly on both planes
	// lands front/front -> leaf 2.
	TestEqual(TEXT("on-plane point goes front"),
		Map->PointLeaf(FVector(100, 0, 0)), 2);

	// A walk ending on a side without a leaf is INDEX_NONE (retail then
	// draws no coronas at all).
	Map->NodeLeaves = {INDEX_NONE, INDEX_NONE, 1, 2};
	TestEqual(TEXT("leafless side resolves INDEX_NONE"),
		Map->PointLeaf(FVector(0, 0, -5)), (int32)INDEX_NONE);

	// Out-of-range leaf indices (truncated import) fail closed.
	Map->NodeLeaves = {99, INDEX_NONE, 1, 2};
	TestEqual(TEXT("out-of-range leaf fails closed"),
		Map->PointLeaf(FVector(0, 0, -5)), (int32)INDEX_NONE);

	// Malformed arrays fail closed.
	Map->NodePlanes.Pop();
	TestEqual(TEXT("mismatched arrays fail closed"),
		Map->PointLeaf(FVector(0, 0, 5)), (int32)INDEX_NONE);

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLeafLineCheckTest,
	"WoT.Corona.LineCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLeafLineCheckTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTLeafMap* Map = World->SpawnActor<AWoTLeafMap>();

	// Empty universe (RootOutside=1) holding a solid floor below z=0 and a
	// non-csg glass sheet at z=100: node 0 = floor plane (normal away from
	// the solid), front -> node 1, back -> solid; node 1 = the sheet.
	Map->NodePlanes = {0, 0, 1, 0,   0, 0, 1, 100};
	Map->NodeChildren = {INDEX_NONE, 1,   INDEX_NONE, INDEX_NONE};
	Map->NodeCsg = {1, 0};
	Map->bRootOutside = true;

	// The port's reason to exist: a translucent/masked/portal sheet is a
	// non-csg node — the trace crosses it unblocked (an engine collision
	// trace against the imported mesh would stop here).
	TestTrue(TEXT("non-csg sheet does not block"),
		Map->LineCheck(FVector(0, 0, 10), FVector(0, 0, 200)));
	TestTrue(TEXT("clear air above the sheet"),
		Map->LineCheck(FVector(0, 0, 150), FVector(0, 0, 300)));
	TestFalse(TEXT("csg floor blocks"),
		Map->LineCheck(FVector(0, 0, 10), FVector(0, 0, -50)));
	TestFalse(TEXT("segment inside solid blocks"),
		Map->LineCheck(FVector(0, 0, -10), FVector(0, 0, -50)));

	// Same geometry with a csg sheet: now it blocks — the csg bit is the
	// entire lever.
	Map->NodeCsg = {1, 1};
	TestFalse(TEXT("csg sheet blocks"),
		Map->LineCheck(FVector(0, 0, 10), FVector(0, 0, 200)));

	// Shipped-map shape: subtractive world (RootOutside=0), one carved
	// halfspace — air on the front side, rock everywhere behind.
	Map->NodePlanes = {0, 0, 1, 0};
	Map->NodeChildren = {INDEX_NONE, INDEX_NONE};
	Map->NodeCsg = {1};
	Map->bRootOutside = false;
	TestTrue(TEXT("subtractive: carved air is clear"),
		Map->LineCheck(FVector(0, 0, 10), FVector(0, 0, 50)));
	TestFalse(TEXT("subtractive: uncarved side blocks"),
		Map->LineCheck(FVector(0, 0, 10), FVector(0, 0, -10)));

	// Maps without the csg bake fail open (callers keep their own gate).
	Map->NodeCsg = {};
	TestTrue(TEXT("no csg bake fails open"),
		Map->LineCheck(FVector(0, 0, 10), FVector(0, 0, -50)));

	WoTTest::DestroySpecWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTLeafViewerOverrideTest,
	"WoT.Corona.LeafViewerOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTLeafViewerOverrideTest::RunTest(const FString&)
{
	UWorld* World = WoTTest::CreateSpecWorld();
	AWoTLeafMap* Map = World->SpawnActor<AWoTLeafMap>();
	FillTestTree(Map);

	// The shot harness pins the viewer to the manifest PAWN point (retail
	// keys the corona list on Region.iLeaf of the pawn, not the eye).
	Map->SetViewerOverride(FVector(0, 0, -5));
	TestEqual(TEXT("override resolves the pawn leaf"),
		Map->GetViewerLeaf(), 0);

	// Re-pinning invalidates the per-frame cache even within one frame.
	Map->SetViewerOverride(FVector(200, 0, 5));
	TestEqual(TEXT("new override re-resolves"), Map->GetViewerLeaf(), 2);

	WoTTest::DestroySpecWorld(World);
	return true;
}

#endif
