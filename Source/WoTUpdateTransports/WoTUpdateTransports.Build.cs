using UnrealBuildTool;

public class WoTUpdateTransports : ModuleRules
{
	public WoTUpdateTransports(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Core-only by design: the pure HTTPS route-policy/hop engine plus the
		// two platform executors (WinHttp / NSURLSession). Keeping this module
		// engine-free is what lets its loopback suite run as a monolithic test
		// PROGRAM (WoTUpdateTransportsTests) with WOT_BOOTSTRAP_TEST_ENDPOINTS=1
		// — the standalone Program-test pattern — because forcing
		// automation compilation into a Test-config ENGINE build drags in
		// Epic's own test sources, which do not compile there (measured:
		// AnalyticsPropertyStoreTests C2065 under MSVC/Test).
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"WoTBootstrapCore"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// The WinHttp executor.
			PublicSystemLibraries.Add("winhttp.lib");
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			// The NSURLSession executor.
			PublicFrameworks.AddRange(new string[]
			{
				"CFNetwork",
				"Foundation"
			});
		}
	}
}
