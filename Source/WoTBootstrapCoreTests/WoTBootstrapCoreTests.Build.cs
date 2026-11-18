using UnrealBuildTool;

public class WoTBootstrapCoreTests : ModuleRules
{
	public WoTBootstrapCoreTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoSharedPCHs;
		PrivatePCHHeaderFile = "Private/WoTBootstrapCoreTestsPCH.h";

		// RequiredProgramMainCPPInclude.h reaches into Launch's private sources to
		// supply the program entry point; this is the standard Program pattern.
		PrivateIncludePathModuleNames.Add("Launch");

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Projects",
			"WoTBootstrapCore",
			"WoTBootstrapCoreNoCheckTests"
		});
	}
}
