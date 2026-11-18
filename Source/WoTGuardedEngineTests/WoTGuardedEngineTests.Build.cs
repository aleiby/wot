using UnrealBuildTool;

public class WoTGuardedEngineTests : ModuleRules
{
	public WoTGuardedEngineTests(ReadOnlyTargetRules Target) : base(Target)
	{
		// Module-local automation. Core's AutomationTest.h only derives
		// WITH_AUTOMATION_TESTS from the dev/perf macros when it is not already
		// defined, so this wins here without forcing engine-wide automation tests
		// on — which does not compile in Shipping (see WoTGuardedEngineTests.Target.cs).
		PrivateDefinitions.Add("WITH_AUTOMATION_TESTS=1");

		PCHUsage = PCHUsageMode.NoSharedPCHs;
		PrivateIncludePathModuleNames.Add("Launch");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Launch",
			// Compile and link the actual game module under this target's
			// WOT_GUARDED_BOOTSTRAP=1 environment. Editor automation alone uses
			// guard=0 and cannot cover guarded preprocessing failures.
			"WoT",
			"WoTLaunch",
			// The private subsystem header used by this harness exposes retail
			// reader value types through its materializer contract.
			"WoTRetailReader",
		});
	}
}
