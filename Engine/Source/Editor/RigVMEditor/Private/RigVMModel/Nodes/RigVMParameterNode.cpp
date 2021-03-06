// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "RigVMModel/Nodes/RigVMParameterNode.h"

const FString URigVMParameterNode::ValueName = TEXT("Value");

URigVMParameterNode::URigVMParameterNode()
{
}

FString URigVMParameterNode::GetNodeTitle() const
{
	return GetParameterName().ToString();
}

FName URigVMParameterNode::GetParameterName() const
{
	return ParameterName;
}

bool URigVMParameterNode::IsInput() const
{
	URigVMPin* ValuePin = FindPin(ValueName);
	if (ValuePin == nullptr)
	{
		return false;
	}
	return ValuePin->GetDirection() == ERigVMPinDirection::Output;
}

FString URigVMParameterNode::GetCPPType() const
{
	URigVMPin* ValuePin = FindPin(ValueName);
	if (ValuePin == nullptr)
	{
		return FString();
	}
	return ValuePin->GetCPPType();
}

UScriptStruct* URigVMParameterNode::GetScriptStruct() const
{
	URigVMPin* ValuePin = FindPin(ValueName);
	if (ValuePin == nullptr)
	{
		return nullptr;
	}
	return ValuePin->GetScriptStruct();
}

FString URigVMParameterNode::GetDefaultValue() const
{

	URigVMPin* ValuePin = FindPin(ValueName);
	if (ValuePin == nullptr)
	{
		return FString();
	}
	return ValuePin->GetDefaultValue();
}

FRigVMGraphParameterDescription URigVMParameterNode::GetParameterDescription() const
{
	FRigVMGraphParameterDescription Parameter;
	Parameter.Name = GetParameterName();
	Parameter.bIsInput = IsInput();
	Parameter.CPPType = GetCPPType();
	Parameter.ScriptStruct = GetScriptStruct();
	Parameter.DefaultValue = GetDefaultValue();
	return Parameter;
}
