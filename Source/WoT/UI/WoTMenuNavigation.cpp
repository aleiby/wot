#include "UI/WoTMenuNavigation.h"

#include "Input/NavigationReply.h"
#include "Layout/Geometry.h"
#include "Types/NavigationMetaData.h"
#include "Widgets/SWidget.h"

namespace
{
using FWoTWeakRow = TArray<TWeakPtr<SWidget>>;

struct FWoTNavLocation
{
	int32 Column = INDEX_NONE;
	int32 Row = INDEX_NONE;
	int32 Index = INDEX_NONE;
	bool bFooter = false;
};

bool WoTMenuNavigation_HasGeometry(const TSharedPtr<SWidget>& Widget)
{
	return Widget.IsValid()
		&& Widget->GetCachedGeometry().GetLocalSize().SizeSquared() > 0.0f;
}

FVector2D WoTMenuNavigation_Center(const TSharedPtr<SWidget>& Widget)
{
	return FVector2D(Widget->GetCachedGeometry()
		.GetAbsolutePositionAtCoordinates(FVector2f(0.5f, 0.5f)));
}

/**
 * One navigation graph shared by the delegates of every widget in it. It
 * holds its widgets weakly; the widgets hold the graph through their
 * navigation metadata, so there is no ownership cycle.
 */
class FWoTNavGraph final
{
public:
	TArray<TArray<FWoTWeakRow>> Columns;
	FWoTWeakRow Footer;
	bool bSwitchColumns = false;
	bool bHoldVertical = false;

	TOptional<FWoTNavLocation> Locate(const SWidget* Widget) const
	{
		for (int32 Column = 0; Column < Columns.Num(); ++Column)
		{
			for (int32 Row = 0; Row < Columns[Column].Num(); ++Row)
			{
				const FWoTWeakRow& Cells = Columns[Column][Row];
				for (int32 Index = 0; Index < Cells.Num(); ++Index)
				{
					if (Cells[Index].Pin().Get() == Widget)
					{
						return FWoTNavLocation{Column, Row, Index, false};
					}
				}
			}
		}
		for (int32 Index = 0; Index < Footer.Num(); ++Index)
		{
			if (Footer[Index].Pin().Get() == Widget)
			{
				return FWoTNavLocation{0, INDEX_NONE, Index, true};
			}
		}
		return {};
	}

	TSharedPtr<SWidget> Navigate(const TSharedPtr<SWidget>& From,
		EUINavigation Direction) const
	{
		if (!From.IsValid())
		{
			return nullptr;
		}
		const TOptional<FWoTNavLocation> Location = Locate(From.Get());
		if (!Location.IsSet())
		{
			return nullptr;
		}
		TSharedPtr<SWidget> Result;
		switch (Direction)
		{
		case EUINavigation::Up:
		case EUINavigation::Down:
			if (!bHoldVertical)
			{
				Result = Vertical(From, Location.GetValue(),
					Direction == EUINavigation::Down ? 1 : -1);
			}
			break;
		case EUINavigation::Left:
		case EUINavigation::Right:
			Result = Horizontal(From, Location.GetValue(),
				Direction == EUINavigation::Right ? 1 : -1);
			break;
		default:
			break;
		}
		// A link back to the focused control is no move at all.
		return Result == From ? nullptr : Result;
	}

private:
	static TArray<TSharedPtr<SWidget>> Navigable(const FWoTWeakRow& Row)
	{
		TArray<TSharedPtr<SWidget>> Result;
		for (const TWeakPtr<SWidget>& Weak : Row)
		{
			const TSharedPtr<SWidget> Widget = Weak.Pin();
			if (WoTIsNavigable(Widget))
			{
				Result.Add(Widget);
			}
		}
		return Result;
	}

	/** The control in Candidates nearest to From along X (or by position). */
	static TSharedPtr<SWidget> NearestHorizontally(const TSharedPtr<SWidget>& From,
		int32 FromIndex, const TArray<TSharedPtr<SWidget>>& Candidates)
	{
		if (Candidates.IsEmpty())
		{
			return nullptr;
		}
		bool bGeometry = WoTMenuNavigation_HasGeometry(From);
		for (const TSharedPtr<SWidget>& Candidate : Candidates)
		{
			bGeometry = bGeometry && WoTMenuNavigation_HasGeometry(Candidate);
		}
		if (!bGeometry)
		{
			return Candidates[FMath::Clamp(FromIndex, 0, Candidates.Num() - 1)];
		}
		const double FromX = WoTMenuNavigation_Center(From).X;
		TSharedPtr<SWidget> Best;
		double BestDistance = TNumericLimits<double>::Max();
		for (const TSharedPtr<SWidget>& Candidate : Candidates)
		{
			const double Distance = FMath::Abs(WoTMenuNavigation_Center(Candidate).X - FromX);
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Candidate;
			}
		}
		return Best;
	}

	/** Column rows followed by the footer row: one vertical cycle. */
	TArray<const FWoTWeakRow*> Sequence(int32 Column) const
	{
		TArray<const FWoTWeakRow*> Rows;
		if (Columns.IsValidIndex(Column))
		{
			for (const FWoTWeakRow& Row : Columns[Column])
			{
				Rows.Add(&Row);
			}
		}
		if (!Footer.IsEmpty())
		{
			Rows.Add(&Footer);
		}
		return Rows;
	}

	TSharedPtr<SWidget> Vertical(const TSharedPtr<SWidget>& From,
		const FWoTNavLocation& Location, int32 Step) const
	{
		// The footer belongs to the left column's cycle.
		const int32 Column = Location.bFooter ? 0 : Location.Column;
		const TArray<const FWoTWeakRow*> Rows = Sequence(Column);
		if (Rows.IsEmpty())
		{
			return nullptr;
		}
		int32 Position = Location.bFooter ? Rows.Num() - 1 : Location.Row;
		for (int32 Visited = 0; Visited < Rows.Num(); ++Visited)
		{
			Position = (Position + Step + Rows.Num()) % Rows.Num();
			const TArray<TSharedPtr<SWidget>> Candidates = Navigable(*Rows[Position]);
			if (!Candidates.IsEmpty())
			{
				return NearestHorizontally(From, Location.Index, Candidates);
			}
		}
		return nullptr;
	}

	TSharedPtr<SWidget> Horizontal(const TSharedPtr<SWidget>& From,
		const FWoTNavLocation& Location, int32 Step) const
	{
		const FWoTWeakRow& Row = Location.bFooter
			? Footer : Columns[Location.Column][Location.Row];
		for (int32 Index = Location.Index + Step;
			Index >= 0 && Index < Row.Num(); Index += Step)
		{
			const TSharedPtr<SWidget> Candidate = Row[Index].Pin();
			if (WoTIsNavigable(Candidate))
			{
				return Candidate;
			}
		}
		if (bSwitchColumns && !Location.bFooter && Columns.Num() > 1)
		{
			const int32 Other = (Location.Column + 1) % Columns.Num();
			if (const TSharedPtr<SWidget> Switched = EnterColumn(From, Location, Other, Step))
			{
				return Switched;
			}
		}
		// Wrap within the row.
		const TArray<TSharedPtr<SWidget>> Candidates = Navigable(Row);
		if (Candidates.IsEmpty())
		{
			return nullptr;
		}
		return Step > 0 ? Candidates[0] : Candidates.Last();
	}

	TSharedPtr<SWidget> EnterColumn(const TSharedPtr<SWidget>& From,
		const FWoTNavLocation& Location, int32 Column, int32 Step) const
	{
		const TArray<FWoTWeakRow>& Rows = Columns[Column];
		const bool bGeometry = WoTMenuNavigation_HasGeometry(From);
		const double FromY = bGeometry ? WoTMenuNavigation_Center(From).Y : 0.0;
		const int32 SourceRows = FMath::Max(1, Columns[Location.Column].Num() - 1);
		const double FromFraction = static_cast<double>(Location.Row) / SourceRows;
		TArray<TSharedPtr<SWidget>> BestRow;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 RowIndex = 0; RowIndex < Rows.Num(); ++RowIndex)
		{
			const TArray<TSharedPtr<SWidget>> Candidates = Navigable(Rows[RowIndex]);
			if (Candidates.IsEmpty())
			{
				continue;
			}
			double Distance;
			if (bGeometry && WoTMenuNavigation_HasGeometry(Candidates[0]))
			{
				Distance = FMath::Abs(WoTMenuNavigation_Center(Candidates[0]).Y - FromY);
			}
			else
			{
				const double Fraction = static_cast<double>(RowIndex)
					/ FMath::Max(1, Rows.Num() - 1);
				Distance = FMath::Abs(Fraction - FromFraction);
			}
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				BestRow = Candidates;
			}
		}
		if (BestRow.IsEmpty())
		{
			return nullptr;
		}
		return Step > 0 ? BestRow[0] : BestRow.Last();
	}
};

void WoTMenuNavigation_Bind(const TSharedRef<FWoTNavGraph>& Graph,
	const TSharedPtr<SWidget>& Widget, TConstArrayView<EUINavigation> Directions)
{
	if (!Widget.IsValid())
	{
		return;
	}
	const TSharedRef<FNavigationMetaData> MetaData =
		Widget->FindOrAddMetaData<FNavigationMetaData>();
	const TWeakPtr<SWidget> WeakWidget = Widget;
	for (const EUINavigation Direction : Directions)
	{
		MetaData->SetNavigationCustom(Direction, EUINavigationRule::Custom,
			FNavigationDelegate::CreateLambda(
				[Graph, WeakWidget](EUINavigation Requested) -> TSharedPtr<SWidget>
				{
					return Graph->Navigate(WeakWidget.Pin(), Requested);
				}));
	}
}

FWoTWeakRow WoTMenuNavigation_Weak(TConstArrayView<TSharedPtr<SWidget>> Widgets)
{
	FWoTWeakRow Row;
	for (const TSharedPtr<SWidget>& Widget : Widgets)
	{
		if (Widget.IsValid())
		{
			Row.Add(Widget);
		}
	}
	return Row;
}

constexpr EUINavigation WoTMenuNavigation_AllDirections[] = {
	EUINavigation::Up, EUINavigation::Down, EUINavigation::Left, EUINavigation::Right};
constexpr EUINavigation WoTMenuNavigation_VerticalDirections[] = {
	EUINavigation::Up, EUINavigation::Down};
constexpr EUINavigation WoTMenuNavigation_HorizontalDirections[] = {
	EUINavigation::Left, EUINavigation::Right};

void WoTMenuNavigation_BindAll(const TSharedRef<FWoTNavGraph>& Graph,
	TConstArrayView<EUINavigation> Directions)
{
	for (const TArray<FWoTWeakRow>& Column : Graph->Columns)
	{
		for (const FWoTWeakRow& Row : Column)
		{
			for (const TWeakPtr<SWidget>& Widget : Row)
			{
				WoTMenuNavigation_Bind(Graph, Widget.Pin(), Directions);
			}
		}
	}
	for (const TWeakPtr<SWidget>& Widget : Graph->Footer)
	{
		WoTMenuNavigation_Bind(Graph, Widget.Pin(), Directions);
	}
}

TArray<FWoTWeakRow> WoTMenuNavigation_WeakRows(TConstArrayView<FWoTNavRow> Rows)
{
	TArray<FWoTWeakRow> Result;
	for (const FWoTNavRow& Row : Rows)
	{
		FWoTWeakRow Weak = WoTMenuNavigation_Weak(Row);
		if (!Weak.IsEmpty())
		{
			Result.Add(MoveTemp(Weak));
		}
	}
	return Result;
}
}

bool WoTIsNavigable(const TSharedPtr<SWidget>& Widget)
{
	if (!Widget.IsValid() || !Widget->SupportsKeyboardFocus())
	{
		return false;
	}
	for (TSharedPtr<SWidget> Current = Widget; Current.IsValid();
		Current = Current->GetParentWidget())
	{
		if (!Current->IsEnabled() || !Current->GetVisibility().IsVisible())
		{
			return false;
		}
		if (Current->Advanced_IsWindow())
		{
			return true;
		}
	}
	// Not in any window: nothing can focus it.
	return false;
}

void WoTWrapVerticalNavigation(TConstArrayView<TSharedPtr<SWidget>> Entries)
{
	const TSharedRef<FWoTNavGraph> Graph = MakeShared<FWoTNavGraph>();
	TArray<FWoTWeakRow>& Rows = Graph->Columns.AddDefaulted_GetRef();
	for (const TSharedPtr<SWidget>& Entry : Entries)
	{
		if (Entry.IsValid())
		{
			Rows.Add(FWoTWeakRow{TWeakPtr<SWidget>(Entry)});
		}
	}
	WoTMenuNavigation_BindAll(Graph, WoTMenuNavigation_VerticalDirections);
}

void WoTWrapHorizontalNavigation(TConstArrayView<TSharedPtr<SWidget>> Entries,
	bool bHoldVertical)
{
	const TSharedRef<FWoTNavGraph> Graph = MakeShared<FWoTNavGraph>();
	Graph->bHoldVertical = bHoldVertical;
	Graph->Columns.AddDefaulted_GetRef().Add(WoTMenuNavigation_Weak(Entries));
	WoTMenuNavigation_BindAll(Graph, bHoldVertical
		? TConstArrayView<EUINavigation>(WoTMenuNavigation_AllDirections)
		: TConstArrayView<EUINavigation>(WoTMenuNavigation_HorizontalDirections));
}

void WoTWrapGridNavigation(TConstArrayView<FWoTNavRow> Rows)
{
	const TSharedRef<FWoTNavGraph> Graph = MakeShared<FWoTNavGraph>();
	Graph->Columns.Add(WoTMenuNavigation_WeakRows(Rows));
	WoTMenuNavigation_BindAll(Graph, WoTMenuNavigation_AllDirections);
}

void WoTTwoColumnNavigation(TConstArrayView<FWoTNavRow> LeftColumn,
	TConstArrayView<FWoTNavRow> RightColumn, const FWoTNavRow& Footer)
{
	const TSharedRef<FWoTNavGraph> Graph = MakeShared<FWoTNavGraph>();
	Graph->bSwitchColumns = true;
	Graph->Columns.Add(WoTMenuNavigation_WeakRows(LeftColumn));
	Graph->Columns.Add(WoTMenuNavigation_WeakRows(RightColumn));
	Graph->Footer = WoTMenuNavigation_Weak(Footer);
	WoTMenuNavigation_BindAll(Graph, WoTMenuNavigation_AllDirections);
}
