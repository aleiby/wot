#include "WoTFirstPersonGrip.h"

#include "Algo/Sort.h"

namespace
{
#include "Generated/WoTHandPoses.inl"
}

namespace WoTFirstPerson
{
const FWoTHandPoseRow* FindHandPose(FName Name)
{
	for (const FWoTHandPoseRow& Row : ::HandPoseRows)
		if (Name == FName(Row.Name)) return &Row;
	return nullptr;
}

TConstArrayView<FWoTHandPoseRow> HandPoses()
{
	return MakeArrayView(::HandPoseRows);
}

const FWoTHeldItemGrip* FindHeldItemGrip(FName Class)
{
	for (const FWoTHeldItemGrip& Row : ::HeldItemGrips)
		if (Row.Class && Class == FName(Row.Class)) return &Row;
	return nullptr;
}

TConstArrayView<FWoTHeldItemGrip> HeldItemGrips()
{
	// With nothing listed the generator writes one empty row.
	return MakeArrayView(::HeldItemGrips, ::HeldItemGrips[0].Class ? static_cast<int32>(UE_ARRAY_COUNT(::HeldItemGrips)) : 0);
}

FName GripForItem(FName Class, const FBox& Bounds)
{
	if (const FWoTHeldItemGrip* Item = FindHeldItemGrip(Class)) return Item->Grip;
	const FVector Extent = Bounds.GetExtent();
	double Sorted[3] = {Extent.X, Extent.Y, Extent.Z};
	Algo::Sort(Sorted, [](double A, double B) { return A > B; });
	return Sorted[0] >= 2.0 * Sorted[1] ? FName(TEXT("rod")) : FName(TEXT("orb"));
}

FQuat AlignAxes(const FVector& FromAxis, const FVector& FromFront, const FVector& ToAxis, const FVector& ToFront)
{
	const FVector A = FromAxis.GetSafeNormal();
	const FVector B = ToAxis.GetSafeNormal();
	const FVector F = (FromFront - A * (FromFront | A)).GetSafeNormal();
	const FVector G = (ToFront - B * (ToFront | B)).GetSafeNormal();
	if (F.IsNearlyZero() || G.IsNearlyZero()) return FQuat::FindBetweenNormals(A, B);
	const FMatrix From(A, F, A ^ F, FVector::ZeroVector);
	const FMatrix To(B, G, B ^ G, FVector::ZeroVector);
	return FQuat(From.GetTransposed() * To);
}

FTransform HeldItemTransform(const FWoTHandPoseRow& Row, FName Class, const FBox& Bounds,
	double ScaleMultiplier, const FHeldItemNudge& Nudge)
{
	// Unlisted items lead with their longest axis at the grip's own size.
	const FVector Extent = Bounds.GetExtent();
	FVector Axis = Extent.X >= Extent.Y && Extent.X >= Extent.Z ? FVector::XAxisVector
		: Extent.Y >= Extent.Z ? FVector::YAxisVector : FVector::ZAxisVector;
	FVector Front = Axis.X != 0 ? FVector::ZAxisVector : FVector::XAxisVector;
	double Size = Row.ItemHalfLength;
	double Slide = 0;
	FVector Anchor = Bounds.GetCenter();
	if (const FWoTHeldItemGrip* Item = FindHeldItemGrip(Class))
	{
		Axis = Item->Axis;
		Front = Item->Front;
		Size = Item->Size;
		Slide = Item->Slide;
		Anchor += Item->Offset;
	}
	Size = FMath::Max(Size + Nudge.SizeDelta, 0.1);
	// Scaled so its largest half-extent is Size, the leading axis lies on the
	// grip line and the front turns to the grip's front; the anchor sits on the
	// grip point, moved along the line as the item outgrows the grip's own size
	// (ItemLead per cm) and by the slides.
	const double Scale = Size / FMath::Max<double>(Extent.GetMax(), UE_SMALL_NUMBER) * ScaleMultiplier;
	const FQuat Turn(FVector::ZAxisVector, FMath::DegreesToRadians(Nudge.GripAngle));
	const FVector Line = Turn.RotateVector(Row.ItemAxis);
	const FQuat Align = AlignAxes(Nudge.bFlip ? -Axis : Axis, Front, Line, Turn.RotateVector(Row.ItemFront));
	const FQuat Rotation = FQuat(Line, FMath::DegreesToRadians(Row.ItemRoll + Nudge.Roll)) * Align;
	const double Along = Row.ItemLead * (Size - Row.ItemHalfLength) + Slide + Nudge.Slide;
	return FTransform(Rotation, Row.ItemPoint + Line * Along - Rotation.RotateVector(Anchor * Scale), FVector(Scale));
}
}
