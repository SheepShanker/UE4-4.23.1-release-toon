// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "CameraShakeSourceActor.h"
#include "CameraShakeSourceComponent.h"


ACameraShakeSourceActor::ACameraShakeSourceActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	CameraShakeSourceComponent = CreateDefaultSubobject<UCameraShakeSourceComponent>(TEXT("CameraShakeSourceComponent"));
	RootComponent = CameraShakeSourceComponent;
}
