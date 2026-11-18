#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"

class FSlateApplication;
class SWidget;
struct FKeyEvent;

/**
 * Keeps one back-stack entry registered. Dropping the handle removes the
 * entry; an entry whose owner widget dies is also pruned at the next press.
 */
class WOT_API FWoTBackStackEntryHandle : public FNoncopyable
{
public:
	~FWoTBackStackEntryHandle();
	uint64 GetId() const { return Id; }

private:
	friend class FWoTBackStack;
	explicit FWoTBackStackEntryHandle(uint64 InId) : Id(InId) {}
	uint64 Id = 0;
};

/**
 * B and View on a controller (spec r3 §1, "Back stack"). Each window, page or
 * overlay registers a weak, predicate-based entry when it is built: its owner
 * widget, IsActive() and OnBack(). A press scans the entries from the most
 * recently registered down; the first whose owner is alive, shown, active and
 * in the focused user's window family handles it, and its answer is final:
 * a handled press is consumed, so closing an overlay never also backs the
 * screen. Dead entries are pruned at press time, which covers a widget
 * destroyed without any close callback.
 *
 * Held B or View backs exactly one level: native key repeats are swallowed
 * while a back-stack surface has focus. Escape is not routed here; each
 * window keeps its own Escape path.
 *
 * Exported so plugin windows can register their own entries.
 */
class WOT_API FWoTBackStack
{
public:
	static FWoTBackStack& Get();

	/** True for the controller back buttons the stack routes: B and View. */
	static bool IsBackKey(const FKey& Key);

	/**
	 * Registers an entry above every existing one. IsActive gates it (an
	 * overlay passes its visibility predicate); OnBack runs the action and
	 * returns whether the press was handled. Either callback may be empty:
	 * no IsActive means always active, no OnBack means consume and do nothing.
	 */
	[[nodiscard]] TSharedRef<FWoTBackStackEntryHandle> Register(
		const TSharedRef<SWidget>& Owner, TFunction<bool()> IsActive,
		TFunction<bool()> OnBack);

	/**
	 * The press path (run by the stack's own input preprocessor). Returns true
	 * when the press was consumed: handled by an entry, or a repeat swallowed
	 * while an entry has focus.
	 */
	bool HandleKeyDown(FSlateApplication& SlateApp, const FKeyEvent& KeyEvent);

	/**
	 * The entry a press from this user would reach now, or 0 when no back-stack
	 * surface has focus. Prunes dead entries.
	 */
	uint64 FindHandlerId(FSlateApplication& SlateApp, int32 UserIndex);

	/** Live entries, after pruning dead owners. */
	int32 NumEntries();

private:
	friend class FWoTBackStackEntryHandle;

	struct FEntry
	{
		uint64 Id = 0;
		TWeakPtr<SWidget> Owner;
		TFunction<bool()> IsActive;
		TFunction<bool()> OnBack;
	};

	void Unregister(uint64 Id);
	void Prune();
	void SyncInputProcessor();
	int32 FindHandlerIndex(FSlateApplication& SlateApp, int32 UserIndex);

	TArray<FEntry> Entries;
	uint64 NextId = 1;
	TSharedPtr<class IInputProcessor> InputProcessor;
};
