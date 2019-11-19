// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ClothConfigBase.h"
#include "CoreMinimal.h"
#include "ChaosClothConfig.generated.h"

/** Holds initial, asset level config for clothing actors. */
// Hiding categories that will be used in the future
UCLASS(HideCategories = (Collision))
class CHAOSCLOTH_API UChaosClothConfig : public UClothConfigBase
{
	GENERATED_BODY()
public:
	UChaosClothConfig() {};

	/**
	 * How cloth particle mass is determined
	 * -	Uniform Mass: Every particle's mass will be set to the value specified in the UniformMass setting
	 * -	Total Mass: The total mass is distributed equally over all the particles
	 * -	Density: A constant mass density is used	 	 
	 */
	UPROPERTY(EditAnywhere, Category = MassConfig)
	EClothMassMode MassMode = EClothMassMode::Density;

	// The value used when the Mass Mode is set to Uniform Mass
	UPROPERTY(EditAnywhere, Category = MassConfig)
	float UniformMass = 1.0f;

	// The value used when Mass Mode is set to TotalMass
	UPROPERTY(EditAnywhere, Category = MassConfig)
	float TotalMass = 100.0f;

	/**
	 * The value used when Mass Mode is set to Density 
	 * Water: 1.0
	 * Cotton: 0.155
	 * Wool: 0.13
	 * Silk: 0.133
	 */
	UPROPERTY(EditAnywhere, Category = MassConfig)
	float Density = 0.1f;

	// This is a lower bound to cloth particle masses
	UPROPERTY(EditAnywhere, Category = MassConfig)
	float MinPerParticleMass = 0.0001f;	

	// The Stiffness of the Edge constraints
	UPROPERTY(EditAnywhere, Category = Stiffness, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float EdgeStiffness = 1.f;

	// The Stiffness of the bending constraints
	UPROPERTY(EditAnywhere, Category = Stiffness, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float BendingStiffness = 1.f;

	// The stiffness of the area preservation constraints
	UPROPERTY(EditAnywhere, Category = Stiffness, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float AreaStiffness = 1.f;

	// The stiffness of the volume preservation constraints
	UPROPERTY(EditAnywhere, Category = Stiffness, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float VolumeStiffness = 0.f;

	// The stiffness of the strain limiting constraints
	UPROPERTY(EditAnywhere, Category = Stiffness, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float StrainLimitingStiffness = 1.f;
	
	// The stiffness of the shape target constraints
	UPROPERTY(EditAnywhere, Category = Stiffness, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float ShapeTargetStiffness = 0.f;	

	// Friction coefficient for cloth - collider interaction
	UPROPERTY(EditAnywhere, Category = Collision, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "10"))
	float CoefficientOfFriction = 0.0f;	

	// Default spring stiffness for anim drive if an anim drive is in use
	UPROPERTY(EditAnywhere, Category = Stiffness, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float AnimDriveSpringStiffness = 0.001f;

	// Enable bending elements
	UPROPERTY(EditAnywhere, Category = ClothEnableFlags)
	bool bUseBendingElements = false;

	// Enable tetrahral constraints
	UPROPERTY(EditAnywhere, Category = ClothEnableFlags)
	bool bUseTetrahedralConstraints = false;

	// Enable thin shell volume constraints 
	UPROPERTY(EditAnywhere, Category = ClothEnableFlags)
	bool bUseThinShellVolumeConstraints = false;

	// Enable self collision
	UPROPERTY(EditAnywhere, Category = ClothEnableFlags)
	bool bUseSelfCollisions = false;

	// Enable continuous collision detection
	UPROPERTY(EditAnywhere, Category = ClothEnableFlags)
	bool bUseContinuousCollisionDetection = false;
};

/*
These settings are shared between all instances on a skeletal mesh
*/
UCLASS()
class CHAOSCLOTH_API UChaosClothSharedSimConfig : public UClothSharedSimConfigBase
{
	GENERATED_BODY()
public:
	UChaosClothSharedSimConfig() : Gravity(FVector(0.0f, 0.0f, -490)) {};
	virtual ~UChaosClothSharedSimConfig() {};

	// The number of solver iterations
	// This will increase the stiffness of all constraints but will increase the CPU cost
	UPROPERTY(EditAnywhere, Category = Simulation, meta = (UIMin = "0", UIMax = "20", ClampMin = "0", ClampMax = "100"))
	int32 IterationCount = 1;

	// The radius of the spheres used in self collision 
	UPROPERTY(EditAnywhere, Category = Collision, meta = (UIMin = "0", UIMax = "100", ClampMin = "0", ClampMax = "1000"))
	float SelfCollisionThickness = 2.0f;

	// The radius of cloth points when considering collisions against collider shapes
	UPROPERTY(EditAnywhere, Category = Collision, meta = (UIMin = "0", UIMax = "100", ClampMin = "0", ClampMax = "1000"))
	float CollisionThickness = 1.0f;

	//The amount of cloth damping
	UPROPERTY(EditAnywhere, Category = Simulation, meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float Damping = 0.01f;

	// The gravitational acceleration vector [cm/s^2]
	UPROPERTY(EditAnywhere, Category = Simulation)
	FVector Gravity;
};

