using UnrealBuildTool;

public class WoTBootstrapCoreNoCheckTests : ModuleRules
{
	public WoTBootstrapCoreNoCheckTests(ReadOnlyTargetRules Target) : base(Target)
	{
		// Instantiate the automation-observer header under the check-elided
		// configuration without changing the runner/engine modules' ABI macros.
		PrivateDefinitions.Add("WITH_AUTOMATION_TESTS=1");
		PrivateDefinitions.Add("DO_CHECK=0");
		PCHUsage = PCHUsageMode.NoSharedPCHs;
		PrivatePCHHeaderFile =
			"Private/WoTBootstrapCoreNoCheckTestsPrivatePCH.h";

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"WoTBootstrapCore"
		});
	}
}
