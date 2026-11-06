using UnrealBuildTool;

public class WoTBootstrapCore : ModuleRules
{
	public WoTBootstrapCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// This module is shared verbatim by the game, the standalone updater, and
		// the byte-frozen commit service, so its dependency list is a contract:
		// Core plus the engine's statically linked OpenSSL, and nothing else. No
		// CoreUObject, no Engine, no Slate, no HTTP, no process launch, no
		// privilege escalation, and no product policy may enter this module. The
		// payload-activation authority is plain C++ here for the same reason: both
		// launch/session producers and runtime consumers bind to this lower seam.
		PublicDependencyModuleNames.Add("Core");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

		// The native handle layer talks to the OS directly rather than through an
		// engine file abstraction, because no engine API promises to refuse to
		// follow a symlink. These are system libraries, not new module edges.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Advapi32.lib");
		}
	}
}
