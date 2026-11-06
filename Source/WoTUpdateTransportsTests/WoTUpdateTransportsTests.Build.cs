using UnrealBuildTool;

public class WoTUpdateTransportsTests : ModuleRules
{
	public WoTUpdateTransportsTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateIncludePathModuleNames.Add("Launch");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"Projects",
			"WoTBootstrapCore",
			"WoTUpdateTransports"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// The loopback spec server (winsock); the executor's winhttp.lib
			// arrives via the WoTUpdateTransports public dependency.
			PublicSystemLibraries.Add("ws2_32.lib");
		}
	}
}
