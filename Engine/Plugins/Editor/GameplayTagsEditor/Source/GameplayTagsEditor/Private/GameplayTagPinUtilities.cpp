// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "GameplayTagPinUtilities.h"
#include "GameplayTagsManager.h"
#include "SGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionTerminator.h"

FString GameplayTagPinUtilities::ExtractTagFilterStringFromGraphPin(UEdGraphPin* InTagPin)
{
	FString FilterString;

	if (ensure(InTagPin))
	{
		const UGameplayTagsManager& TagManager = UGameplayTagsManager::Get();
		if (UScriptStruct* PinStructType = Cast<UScriptStruct>(InTagPin->PinType.PinSubCategoryObject.Get()))
		{
			FilterString = TagManager.GetCategoriesMetaFromField(PinStructType);
		}

		if (FilterString.IsEmpty())
		{
			const UEdGraphNode* OwningNode = InTagPin->GetOwningNode();
			if (const UK2Node_CallFunction* CallFuncNode = Cast<UK2Node_CallFunction>(OwningNode))
			{
				if (const UFunction* TargetFunction = CallFuncNode->GetTargetFunction())
				{
					FilterString = TagManager.GetCategoriesMetaFromFunction(TargetFunction, InTagPin->PinName);
				}
			}
			else if (const UK2Node_VariableSet* VariableSetNode = Cast<UK2Node_VariableSet>(OwningNode))
			{
				FilterString = TagManager.GetCategoriesMetaFromField(VariableSetNode->GetPropertyForVariable());
			}
			else if (const UK2Node_FunctionTerminator* FuncTermNode = Cast<UK2Node_FunctionTerminator>(OwningNode))
			{
				if (const UFunction* SignatureFunction = FuncTermNode->FindSignatureFunction())
				{
					FilterString = TagManager.GetCategoriesMetaFromFunction(SignatureFunction, InTagPin->PinName);
				}
			}
		}
	}

	return FilterString;
}