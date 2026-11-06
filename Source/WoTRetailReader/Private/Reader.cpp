#include "WoTRetailReader/Reader.h"

#include "WoTRetailReader/Package.h"

TWoTBootstrapResult<FWoTCacheBuildResult> FWoTRetailReader::Build(
	FWoTRetailPackage& Package, const FWoTRetailContract& Contract,
	IWoTObjectWriter& Writer,
	IWoTProgressSink& Progress)
{
	return WoT::Retail::Detail::BuildRetailCache(
		Package, Contract, Writer, Progress);
}
