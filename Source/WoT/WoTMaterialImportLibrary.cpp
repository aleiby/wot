#include "WoTMaterialImportLibrary.h"

#if WITH_EDITOR

#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"

int32 UWoTMaterialImportLibrary::SetCustomNodeCode(
	UMaterial* Material, const FString& Description, const FString& Code)
{
	if (!Material)
	{
		return 0;
	}
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression);
		if (!Custom || Custom->Description != Description)
		{
			continue;
		}
		if (Custom->Code.Equals(Code))
		{
			return 1;
		}
		Custom->Modify();
		Custom->Code = Code;
		Custom->PostEditChange();
		Material->MarkPackageDirty();
		return 2;
	}
	return 0;
}

int32 UWoTMaterialImportLibrary::WrapCustomNodeInVertexInterpolator(
	UMaterial* Material, const FString& Description)
{
	if (!Material)
	{
		return 0;
	}
	UMaterialExpressionCustom* Custom = nullptr;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		UMaterialExpressionCustom* Candidate = Cast<UMaterialExpressionCustom>(Expression);
		if (Candidate && Candidate->Description == Description)
		{
			Custom = Candidate;
			break;
		}
	}
	if (!Custom)
	{
		return 0;
	}

	// Collect every input (expression pins + material property roots) that
	// consumes the custom node.
	TArray<FExpressionInput*> Consumers;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (Expression == Custom)
		{
			continue;
		}
		if (Cast<UMaterialExpressionVertexInterpolator>(Expression))
		{
			UMaterialExpressionVertexInterpolator* Existing =
				Cast<UMaterialExpressionVertexInterpolator>(Expression);
			if (Existing->Input.Expression == Custom)
			{
				return 1;   // already wrapped
			}
		}
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			if (It->Expression == Custom)
			{
				Consumers.Add(It.Input);
			}
		}
	}
	for (int32 Property = 0; Property < MP_MAX; ++Property)
	{
		FExpressionInput* Input =
			Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Property));
		if (Input && Input->Expression == Custom)
		{
			Consumers.Add(Input);
		}
	}
	if (Consumers.IsEmpty())
	{
		return 0;
	}

	Material->Modify();
	UMaterialExpressionVertexInterpolator* Interpolator =
		NewObject<UMaterialExpressionVertexInterpolator>(Material, NAME_None, RF_Transactional);
	Interpolator->Material = Material;
	Interpolator->MaterialExpressionEditorX = Custom->MaterialExpressionEditorX + 150;
	Interpolator->MaterialExpressionEditorY = Custom->MaterialExpressionEditorY;
	Material->GetExpressionCollection().AddExpression(Interpolator);
	Interpolator->Input.Connect(0, Custom);
	for (FExpressionInput* Input : Consumers)
	{
		Input->Connect(0, Interpolator);
	}
	Material->MarkPackageDirty();
	return 2;
}

int32 UWoTMaterialImportLibrary::UnwrapCustomNodeVertexInterpolator(
	UMaterial* Material, const FString& Description)
{
	if (!Material)
	{
		return 0;
	}
	UMaterialExpressionCustom* Custom = nullptr;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		UMaterialExpressionCustom* Candidate = Cast<UMaterialExpressionCustom>(Expression);
		if (Candidate && Candidate->Description == Description)
		{
			Custom = Candidate;
			break;
		}
	}
	if (!Custom)
	{
		return 0;
	}
	UMaterialExpressionVertexInterpolator* Interpolator = nullptr;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		UMaterialExpressionVertexInterpolator* Candidate =
			Cast<UMaterialExpressionVertexInterpolator>(Expression);
		if (Candidate && Candidate->Input.Expression == Custom)
		{
			Interpolator = Candidate;
			break;
		}
	}
	if (!Interpolator)
	{
		return 1;
	}

	Material->Modify();
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (Expression == Interpolator)
		{
			continue;
		}
		for (FExpressionInputIterator It{ Expression }; It; ++It)
		{
			if (It->Expression == Interpolator)
			{
				It->Connect(0, Custom);
			}
		}
	}
	for (int32 Property = 0; Property < MP_MAX; ++Property)
	{
		FExpressionInput* Input =
			Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Property));
		if (Input && Input->Expression == Interpolator)
		{
			Input->Connect(0, Custom);
		}
	}
	Material->GetExpressionCollection().RemoveExpression(Interpolator);
	Interpolator->MarkAsGarbage();
	Material->MarkPackageDirty();
	return 2;
}

#endif
