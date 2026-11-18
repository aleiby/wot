using System.IO;
using System.Text;
using UnrealBuildTool;

public class WoTInnoExtract : ModuleRules
{
	public WoTInnoExtract(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
		string RepositoryRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../.."));
		string VendorRoot = Path.Combine(RepositoryRoot, "ThirdParty", "innoextract", "1.9");
		PublicSystemIncludePaths.Add(Path.Combine(VendorRoot, "include"));

		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicAdditionalLibraries.Add(RequireVendorLibrary(
				Path.Combine(VendorRoot, "lib", "Mac", "libwot_innoextract.a"), "Mac"));
			// The promoted archive merges wot_innoextract with the static
			// boost filters and liblzma, but the Apple-SDK-provided libraries
			// (iconv for the encoding shim, bz2/z for the boost stream
			// filters) stay dynamic and must be named here — a static archive
			// carries no link dependencies of its own.
			PublicSystemLibraries.Add("iconv");
			PublicSystemLibraries.Add("bz2");
			PublicSystemLibraries.Add("z");
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicAdditionalLibraries.Add(RequireVendorLibrary(
				Path.Combine(VendorRoot, "lib", "Win64", "wot_innoextract.lib"), "Win64"));
		}
		else
		{
			// Retail extraction is a Win64/Mac concern — those are the only
			// platforms the retail installer targets, and the only ones we
			// promote a reviewed library for. Throwing here used to make the
			// GAME MODULE unbuildable everywhere else, because WoT depends on
			// WoTRetailReader which depends on this module: vendoring an
			// installer parser silently locked Linux contributors out of
			// compiling the game at all. Degrade instead, and let
			// InnoExtractor.cpp return a clean unsupported-platform error.
			PublicDefinitions.Add("WOT_HAS_INNOEXTRACT=0");
			return;
		}

		PublicDefinitions.Add("WOT_HAS_INNOEXTRACT=1");
	}

	// The public repository carries no Git LFS, so a fresh public checkout has
	// no library here, and a private clone made without git-lfs has only a
	// pointer file. Either way the link would fail far from the cause; name
	// the fix instead.
	private static string RequireVendorLibrary(string LibraryPath, string PlatformName)
	{
		string Problem = null;
		if (!File.Exists(LibraryPath))
		{
			Problem = "is missing";
		}
		else if (new FileInfo(LibraryPath).Length < 1024)
		{
			string Head = Encoding.ASCII.GetString(File.ReadAllBytes(LibraryPath));
			if (Head.StartsWith("version https://git-lfs"))
			{
				Problem = "is a Git LFS pointer, not the library";
			}
		}
		if (Problem != null)
		{
			throw new BuildException(
				"The vendored innoextract library " + LibraryPath + " " + Problem + ". " +
				"Fetch the reviewed library with: python Tools/build/fetch_innoextract_libs.py " +
				"fetch --platform " + PlatformName + " (or build it from source with " +
				"Tools/build/build_innoextract_native.py).");
		}
		return LibraryPath;
	}
}
