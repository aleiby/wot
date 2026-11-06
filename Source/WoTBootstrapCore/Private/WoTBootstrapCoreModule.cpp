#include "Modules/ModuleManager.h"

// No module class: this module holds pure functions and value types only. It has
// no startup order, no singletons, and nothing to tick — which is what lets the
// game, the updater, and the byte-frozen commit service link the same code.
IMPLEMENT_MODULE(FDefaultModuleImpl, WoTBootstrapCore);
