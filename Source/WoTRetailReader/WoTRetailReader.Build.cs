using System.IO;
using UnrealBuildTool;

public class WoTRetailReader : ModuleRules
{
	public WoTRetailReader(ReadOnlyTargetRules Target) : base(Target)
	{
		bool bGuardedBootstrap = System.Linq.Enumerable.Contains(
			Target.GlobalDefinitions, "WOT_GUARDED_BOOTSTRAP=1");
		if (!bGuardedBootstrap)
		{
			PublicDefinitions.Add("WOT_GUARDED_BOOTSTRAP=0");
		}
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		FPSemantics = FPSemanticsMode.Precise;
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"WoTBootstrapCore"
		});
		PrivateDependencyModuleNames.Add("WoTInnoExtract");
		PrivateIncludePaths.AddRange(new string[]
		{
			// FrontEndWorld shares only the module's bounded, pure decoding helpers.
			Path.Combine(ModuleDirectory, "Private"),
			Path.Combine(ModuleDirectory, "../WoTBootstrapCore/Private/Tests")
		});
		// The cache builder refuses to run without the checked-in production
		// descriptor (CacheBuilder.cpp WoTCacheLoadProductionDescriptor reads
		// ProjectDir()/Build/WoTRelease/...). A packaged game must carry it: stage it
		// beside the project as a loose file. Editor and program targets read the
		// source tree's copy and have no project to stage into.
		if (Target.ProjectFile != null
			&& Target.Type != TargetType.Editor && Target.Type != TargetType.Program)
		{
			RuntimeDependencies.Add(
				"$(ProjectDir)/Build/WoTRelease/cache-v3-production-descriptor.json",
				StagedFileType.NonUFS);
		}
	}
}
