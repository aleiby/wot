#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS && WITH_EDITOR && PLATFORM_WINDOWS

#include "../FrontEnd/WoTFrontEndSceneMaterializer.h"
#include "../Payload/WoTFrontEndFontOwner.h"
#include "../WoTPayloadSubsystem.h"
#include "WoTFrontEndTestData.h"
#include "Engine/FontFace.h"
#include "Windows/WindowsHWrapper.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_SYSTEM_H

namespace
{
// Catch the production BDF parser reading beyond its 1024-byte line buffer.
// Track every allocation so an intentionally caught parser fault can be cleaned
// up without asking FreeType to traverse its partially initialized structures.
struct FGuardedFontMemory
{
	TMap<void*, void*> Allocations;
	int32 GuardedBuffers = 0;
	FT_MemoryRec_ Memory = {};

	FGuardedFontMemory()
	{
		Memory.user = this;
		Memory.alloc = Allocate;
		Memory.free = Free;
		Memory.realloc = Reallocate;
	}
	~FGuardedFontMemory()
	{
		while (!Allocations.IsEmpty()) Free(&Memory, Allocations.CreateConstIterator().Key());
	}
	static void* Allocate(FT_Memory Memory, long Size)
	{
		auto& State = *static_cast<FGuardedFontMemory*>(Memory->user);
		void* Allocation = nullptr;
		void* Base = nullptr;
		if (Size == 1024)
		{
			SYSTEM_INFO Info;
			GetSystemInfo(&Info);
			Base = VirtualAlloc(nullptr, Info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (!Base) return nullptr;
			DWORD OldProtection;
			if (!VirtualProtect(static_cast<uint8*>(Base) + Info.dwPageSize,
				Info.dwPageSize, PAGE_NOACCESS, &OldProtection))
			{
				VirtualFree(Base, 0, MEM_RELEASE);
				return nullptr;
			}
			Allocation = static_cast<uint8*>(Base) + Info.dwPageSize - Size;
			++State.GuardedBuffers;
		}
		else Allocation = FMemory::Malloc(Size);
		State.Allocations.Add(Allocation, Base);
		return Allocation;
	}
	static void Free(FT_Memory Memory, void* Block)
	{
		if (!Block) return;
		auto& State = *static_cast<FGuardedFontMemory*>(Memory->user);
		void* Base = State.Allocations.FindChecked(Block);
		State.Allocations.Remove(Block);
		if (Base) VirtualFree(Base, 0, MEM_RELEASE);
		else FMemory::Free(Block);
	}
	static void* Reallocate(FT_Memory Memory, long OldSize, long NewSize, void* Block)
	{
		void* Result = Allocate(Memory, NewSize);
		if (Result)
		{
			if (Block) FMemory::Memcpy(Result, Block, FMath::Min(OldSize, NewSize));
			Free(Memory, Block);
		}
		return Result;
	}
};

// SEH is intentionally confined to a leaf without C++ objects to unwind.
bool LoadGuardedFace(FT_Library Library, const uint8* Bytes, int32 Count,
	FT_Face* Face, FT_Error* Error)
{
	__try
	{
		*Error = FT_New_Memory_Face(Library, Bytes, Count, 0, Face);
		return true;
	}
	__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
		? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
		return false;
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWoTBdfReadBoundarySpec,
	"WoT.Menu.Multiplayer.BdfReadBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWoTBdfReadBoundarySpec::RunTest(const FString&)
{
	AddExpectedError(TEXT("Cannot read payload manifest"), EAutomationExpectedErrorFlags::Contains, 1);
	FWoTFrontEndStageProductionFixture Fixture;
	Fixture.bPatternedFontGlyphs = true;
	Fixture.bZeroWidthFontGlyph = true;
	FString Error;
	if (!TestTrue(TEXT("authenticated fixture activates"),
		WoTFrontEndTest_BuildProductionStageFixture(Fixture, false, Error))) return false;
	auto* Fonts = Fixture.Payload->FrontEndFontOwnerForScene();
	if (!TestNotNull(TEXT("font owner exists"), Fonts)) return false;
	FWoTFrontEndSceneMaterializer Scene;
	auto Staged = Scene.Stage(*Fixture.World, Fixture.Descriptor, *Fixture.WorldLease,
		*Fixture.Payload, *Fonts, {}, []() { return false; });
	if (!TestFalse(TEXT("scene stages"), Staged.IsError())) return false;
	if (!TestFalse(TEXT("scene commits"), MoveTemp(Staged).GetValue().Commit().IsError())) return false;
	const auto* Resource = Fonts->ResolveResource(FName(TEXT("F_WOTReg14")));
	if (!TestNotNull(TEXT("generated font exists"), Resource)) return false;
	const auto& Bdf = Resource->SlateFaceObject()->GetFontFaceData()->GetData();
	int32 HeaderEnd = 0;
	while (HeaderEnd < Bdf.Num() && Bdf[HeaderEnd++] != '\n') {}
	// An ignored comment shifts real generated bitmap rows through every parser
	// buffer alignment. Removing the row-read safety padding must fail this test.
	for (int32 Shift = 0; Shift < 1024; ++Shift)
	{
		TArray<uint8> Bytes;
		Bytes.Append(Bdf.GetData(), HeaderEnd);
		Bytes.Add('#');
		for (int32 Index = 0; Index < Shift; ++Index) Bytes.Add('x');
		Bytes.Add('\n');
		Bytes.Append(Bdf.GetData() + HeaderEnd, Bdf.Num() - HeaderEnd);
		FGuardedFontMemory Guard;
		FT_Library Library = nullptr;
		if (!TestEqual(TEXT("test FreeType library initializes"),
			FT_New_Library(&Guard.Memory, &Library), 0)) return false;
		FT_Add_Default_Modules(Library);
		FT_Face Face = nullptr;
		FT_Error LoadError = 0;
		if (!TestTrue(FString::Printf(TEXT("BDF load stays in bounds at alignment %d"), Shift),
			LoadGuardedFace(Library, Bytes.GetData(), Bytes.Num(), &Face, &LoadError))) return false;
		const bool bLoaded = TestEqual(TEXT("generated BDF is accepted"), LoadError, 0);
		if (Face) FT_Done_Face(Face);
		FT_Done_Library(Library);
		if (!TestTrue(TEXT("parser used guarded buffer"), Guard.GuardedBuffers > 0) || !bLoaded) return false;
	}
	Scene.Shutdown();
	return !HasAnyErrors();
}

#endif
