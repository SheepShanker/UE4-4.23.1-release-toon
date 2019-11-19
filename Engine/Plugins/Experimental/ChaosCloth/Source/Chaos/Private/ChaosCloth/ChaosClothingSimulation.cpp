// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#include "ChaosCloth/ChaosClothingSimulation.h"
#include "ChaosCloth/ChaosClothPrivate.h"

#include "Async/ParallelFor.h"
#include "ClothingAsset.h"
#include "ClothingSimulation.h" // ClothingSystemRuntimeInterface
#include "Utils/ClothingMeshUtils.h" // ClothingSystemRuntimeCommon
#include "Components/SkeletalMeshComponent.h"
#include "Materials/Material.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Chaos/Box.h"
#include "Chaos/Capsule.h"
#include "Chaos/Cylinder.h"
#include "Chaos/ImplicitObjectIntersection.h"
#include "Chaos/ImplicitObjectUnion.h"
#include "Chaos/Levelset.h"
#include "Chaos/PBDAnimDriveConstraint.h"
#include "Chaos/PBDAxialSpringConstraints.h"
#include "Chaos/PBDBendingConstraints.h"
#include "Chaos/PBDParticles.h"
#include "Chaos/PBDSphericalConstraint.h"
#include "Chaos/PBDSpringConstraints.h"
#include "Chaos/PBDVolumeConstraint.h"
#include "Chaos/PerParticleGravity.h"
#include "Chaos/PerParticlePBDLongRangeConstraints.h"
#include "Chaos/PerParticlePBDShapeConstraints.h"
#include "Chaos/Plane.h"
#include "Chaos/Sphere.h"
#include "Chaos/TaperedCylinder.h"
#include "Chaos/Convex.h"
#include "Chaos/Transform.h"
#include "Chaos/Utilities.h"
#include "Chaos/Vector.h"
#include "ChaosCloth/ChaosClothConfig.h"

#if WITH_PHYSX && !PLATFORM_LUMIN && !PLATFORM_ANDROID
#include "PhysXIncludes.h"
#endif

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "Chaos/ErrorReporter.h"

using namespace Chaos;

ClothingSimulation::ClothingSimulation()
	: ClothSharedSimConfig(nullptr)
	, ExternalCollisionsOffset(0)
{
#if WITH_EDITOR
	DebugClothMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EditorMaterials/Cloth/CameraLitDoubleSided.CameraLitDoubleSided"), nullptr, LOAD_None, nullptr);  // LOAD_EditorOnly
	DebugClothMaterialVertex = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EditorMaterials/WidgetVertexColorMaterial"), nullptr, LOAD_None, nullptr);  // LOAD_EditorOnly
#endif  // #if WITH_EDITOR
}

ClothingSimulation::~ClothingSimulation()
{}

void ClothingSimulation::Initialize()
{

	// Default parameters. Will be overwritten when cloth assets are loaded
	int32 NumIterations = 1;
	float SelfCollisionThickness = 2.0f;
	float CollisionThickness = 1.2f;
	float CoefficientOfFriction = 0.0f;
	float Damping = 0.01f;
	float GravityMagnitude = 490.0f;

    Chaos::TPBDParticles<float, 3> LocalParticles;
    Chaos::TKinematicGeometryClothParticles<float, 3> TRigidParticles;
    Evolution.Reset(
		new Chaos::TPBDEvolution<float, 3>(
			MoveTemp(LocalParticles),
			MoveTemp(TRigidParticles),
			{}, // CollisionTriangles
			NumIterations,
			CollisionThickness,
			SelfCollisionThickness,
			CoefficientOfFriction,
			Damping));
    Evolution->CollisionParticles().AddArray(&BoneIndices);
	Evolution->CollisionParticles().AddArray(&BaseTransforms);
    Evolution->GetGravityForces().SetAcceleration(Chaos::TVector<float, 3>(0.f, 0.f, -1.f)*GravityMagnitude);

    Evolution->SetKinematicUpdateFunction(
		[&](Chaos::TPBDParticles<float, 3>& ParticlesInput, const float Dt, const float LocalTime, const int32 Index)
		{
			if (!OldAnimationPositions.IsValidIndex(Index) || ParticlesInput.InvM(Index) > 0)
				return;
			const float Alpha = (LocalTime - Time) / DeltaTime;
			ParticlesInput.X(Index) = Alpha * AnimationPositions[Index] + (1.f - Alpha) * OldAnimationPositions[Index];
		});

	Evolution->SetCollisionKinematicUpdateFunction(
//		[&](Chaos::TKinematicGeometryParticles<float, 3>& ParticlesInput, const float Dt, const float LocalTime, const int32 Index)
		[&](Chaos::TKinematicGeometryClothParticles<float, 3>& ParticlesInput, const float Dt, const float LocalTime, const int32 Index)
		{
			checkSlow(Dt > SMALL_NUMBER && DeltaTime > SMALL_NUMBER);
			const float Alpha = (LocalTime - Time) / DeltaTime;
			const Chaos::TVector<float, 3> NewX =
				Alpha * CollisionTransforms[Index].GetTranslation() + (1.f - Alpha) * OldCollisionTransforms[Index].GetTranslation();
			ParticlesInput.V(Index) = (NewX - ParticlesInput.X(Index)) / Dt;
			ParticlesInput.X(Index) = NewX;
			Chaos::TRotation<float, 3> NewR = FQuat::Slerp(OldCollisionTransforms[Index].GetRotation(), CollisionTransforms[Index].GetRotation(), Alpha);
			Chaos::TRotation<float, 3> Delta = NewR * ParticlesInput.R(Index).Inverse();
			Chaos::TVector<float, 3> Axis;
			float Angle;
			Delta.ToAxisAndAngle(Axis, Angle);
			ParticlesInput.W(Index) = Axis * Angle / Dt;
			ParticlesInput.R(Index) = NewR;
		});

    MaxDeltaTime = 1.0f;
    ClampDeltaTime = 0.f;
    Time = 0.f;
}

void ClothingSimulation::Shutdown()
{
	Assets.Reset();
	AnimDriveSpringStiffness.Reset();
	ExtractedCollisions.Reset();
	ExternalCollisions.Reset();
	OldCollisionTransforms.Reset();
	CollisionTransforms.Reset();
	BoneIndices.Reset();
	BaseTransforms.Reset();
	OldAnimationPositions.Reset();
	AnimationPositions.Reset();
	AnimationNormals.Reset();
	IndexToRangeMap.Reset();
	Meshes.Reset();
	FaceNormals.Reset();
	PointNormals.Reset();
	Evolution.Reset();
	ExternalCollisionsOffset = 0;
	ClothSharedSimConfig = nullptr;
}

void ClothingSimulation::DestroyActors()
{
	Shutdown();
	Initialize();
}

void ClothingSimulation::CreateActor(USkeletalMeshComponent* InOwnerComponent, UClothingAssetBase* InAsset, int32 InSimDataIndex)
{
	UE_LOG(LogChaosCloth, Verbose, TEXT("Adding Cloth LOD asset to %s in sim slot %d"), InOwnerComponent->GetOwner() ? *InOwnerComponent->GetOwner()->GetName() : TEXT("None"), InSimDataIndex);

	//Evolution->SetCCD(ChaosClothSimConfig->bUseContinuousCollisionDetection);
	//Evolution->SetCCD(true); // ryan!!!

	UClothingAssetCommon* Asset = Cast<UClothingAssetCommon>(InAsset);
	const UChaosClothConfig* const ChaosClothSimConfig = Cast<UChaosClothConfig>(Asset->ChaosClothSimConfig);
	check(ChaosClothSimConfig);

	ClothingSimulationContext Context;
	FillContext(InOwnerComponent, 0, &Context);

	if (Assets.Num() <= InSimDataIndex)
	{
		Assets.SetNumZeroed(InSimDataIndex + 1);
		AnimDriveSpringStiffness.SetNumZeroed(InSimDataIndex + 1);
	}
	Assets[InSimDataIndex] = Asset;
	AnimDriveSpringStiffness[InSimDataIndex] = ChaosClothSimConfig->AnimDriveSpringStiffness;

	check(Asset->GetNumLods() == 1);
	UClothLODDataBase* AssetLodData = Asset->ClothLodData[0];
	check(AssetLodData->PhysicalMeshData);
	UClothPhysicalMeshDataBase* PhysMesh = AssetLodData->PhysicalMeshData;

	// SkinPhysicsMesh() strips scale from RootBoneTransform ("Ignore any user scale.
	// It's already accounted for in our skinning matrices."), and returns all points
	// in that space.
	TArray<Chaos::TVector<float, 3>> TempAnimationPositions;
	TArray<Chaos::TVector<float, 3>> TempAnimationNormals;

	FTransform RootBoneTransform = Context.BoneTransforms[Asset->ReferenceBoneIndex];
	ClothingMeshUtils::SkinPhysicsMesh(
		Asset->UsedBoneIndices,
		*PhysMesh, // curr pos and norm
		RootBoneTransform,
		Context.RefToLocals.GetData(),
		Context.RefToLocals.Num(),
		reinterpret_cast<TArray<FVector>&>(TempAnimationPositions),
		reinterpret_cast<TArray<FVector>&>(TempAnimationNormals));

	// Transform points & normals to world space
	RootBoneTransform.SetScale3D(FVector(1.0f));
	const FTransform RootBoneWorldTransform = RootBoneTransform * Context.ComponentToWorld;
	ParallelFor(TempAnimationPositions.Num(),
		[&](int32 Index)
	{
		TempAnimationPositions[Index] = RootBoneWorldTransform.TransformPosition(TempAnimationPositions[Index]);
		TempAnimationNormals[Index] = RootBoneWorldTransform.TransformVector(TempAnimationNormals[Index]);
	});

	// Add particles
	TPBDParticles<float, 3>& Particles = Evolution->Particles();
	const uint32 Offset = Particles.Size();
	Particles.AddParticles(PhysMesh->Vertices.Num());

	// ClothSharedSimConfig should either be a nullptr, or point to an object common to the whole skeletal mesh
	if (ClothSharedSimConfig == nullptr)
	{
		ClothSharedSimConfig = Cast<UChaosClothSharedSimConfig>(Asset->ClothSharedSimConfig);
	}

	AnimationPositions.SetNum(Particles.Size());
	AnimationNormals.SetNum(Particles.Size());

	if (IndexToRangeMap.Num() <= InSimDataIndex)
		IndexToRangeMap.SetNum(InSimDataIndex + 1);
	IndexToRangeMap[InSimDataIndex] = Chaos::TVector<uint32, 2>(Offset, Particles.Size());

	for (uint32 i = Offset; i < Particles.Size(); ++i)
	{
		AnimationPositions[i] = TempAnimationPositions[i - Offset];
		AnimationNormals[i] = TempAnimationNormals[i - Offset];
		Particles.X(i) = AnimationPositions[i];
		Particles.V(i) = Chaos::TVector<float, 3>(0.f, 0.f, 0.f);
		// Initialize mass to 0, to be overridden later
		Particles.M(i) = 0.f;
	}

	OldAnimationPositions = AnimationPositions;  // Also update the old positions array to avoid any interpolation issues

	const int32 NumTriangles = PhysMesh->Indices.Num() / 3;
	TArray<Chaos::TVector<int32, 3>> InputSurfaceElements;
	InputSurfaceElements.Reserve(NumTriangles);
	for (int i = 0; i < NumTriangles; ++i)
	{
		const int32 Index = 3 * i;
		InputSurfaceElements.Add(
			{ static_cast<int32>(Offset + PhysMesh->Indices[Index]),
			 static_cast<int32>(Offset + PhysMesh->Indices[Index + 1]),
			 static_cast<int32>(Offset + PhysMesh->Indices[Index + 2]) });
	}
	check(InputSurfaceElements.Num() == NumTriangles);
	if (Meshes.Num() <= InSimDataIndex)
	{
		Meshes.SetNum(InSimDataIndex + 1);
		FaceNormals.SetNum(InSimDataIndex + 1);
		PointNormals.SetNum(InSimDataIndex + 1);
	}
	TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[InSimDataIndex];
	Mesh.Reset(new Chaos::TTriangleMesh<float>(MoveTemp(InputSurfaceElements)));
	check(Mesh->GetNumElements() == NumTriangles);
	const auto& SurfaceElements = Mesh->GetSurfaceElements();
	Mesh->GetPointToTriangleMap(); // Builds map for later use by GetPointNormals().

	// Assign per particle mass proportional to connected area.
	float TotalArea = 0.0;
	for (const Chaos::TVector<int32, 3>& Tri : SurfaceElements)
	{
		const float TriArea = 0.5 * Chaos::TVector<float, 3>::CrossProduct(
			Particles.X(Tri[1]) - Particles.X(Tri[0]),
			Particles.X(Tri[2]) - Particles.X(Tri[0])).Size();
		TotalArea += TriArea;
		const float ThirdTriArea = TriArea / 3.0;
		Particles.M(Tri[0]) += ThirdTriArea;
		Particles.M(Tri[1]) += ThirdTriArea;
		Particles.M(Tri[2]) += ThirdTriArea;
	}
	const TSet<int32> Vertices = Mesh->GetVertices();
	switch (ChaosClothSimConfig->MassMode)
	{
	case EClothMassMode::UniformMass:
		for (const int32 Vertex : Vertices)
		{
			Particles.M(Vertex) = ChaosClothSimConfig->UniformMass;
		}
		break;
	case EClothMassMode::TotalMass:
	{
		const float MassPerUnitArea = TotalArea > 0.0 ? ChaosClothSimConfig->TotalMass / TotalArea : 1.0;
		for (const int32 Vertex : Vertices)
		{
			Particles.M(Vertex) *= MassPerUnitArea;
		}
		break;
	}
	case EClothMassMode::Density:
		for (const int32 Vertex : Vertices)
		{
			Particles.M(Vertex) *= ChaosClothSimConfig->Density;
		}
		break;
	};
	// Clamp and enslave
	for (uint32 i = Offset; i < Particles.Size(); i++)
	{
		Particles.M(i) = FMath::Max(Particles.M(i), ChaosClothSimConfig->MinPerParticleMass);
		Particles.InvM(i) = PhysMesh->IsFixed(i - Offset) ? 0.0 : 1.0 / Particles.M(i);
	}

	// Add Model
	if (ChaosClothSimConfig->ShapeTargetStiffness)
	{
		check(ChaosClothSimConfig->ShapeTargetStiffness > 0.f && ChaosClothSimConfig->ShapeTargetStiffness <= 1.f);
		Evolution->AddPBDConstraintFunction([ShapeConstraints = Chaos::TPerParticlePBDShapeConstraints<float, 3>(Evolution->Particles(), AnimationPositions, ChaosClothSimConfig->ShapeTargetStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
			ShapeConstraints.Apply(InParticles, Dt);
		});
	}
	if (ChaosClothSimConfig->EdgeStiffness)
	{
		check(ChaosClothSimConfig->EdgeStiffness > 0.f && ChaosClothSimConfig->EdgeStiffness <= 1.f);
		Evolution->AddPBDConstraintFunction([SpringConstraints = Chaos::TPBDSpringConstraints<float, 3>(Evolution->Particles(), SurfaceElements, ChaosClothSimConfig->EdgeStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
			SpringConstraints.Apply(InParticles, Dt);
		});
	}
	if (ChaosClothSimConfig->BendingStiffness)
	{
		check(ChaosClothSimConfig->BendingStiffness > 0.f && ChaosClothSimConfig->BendingStiffness <= 1.f);
		if (ChaosClothSimConfig->bUseBendingElements)
		{
			TArray<Chaos::TVector<int32, 4>> BendingConstraints = Mesh->GetUniqueAdjacentElements();
			Evolution->AddPBDConstraintFunction([BendConstraints = Chaos::TPBDBendingConstraints<float>(Evolution->Particles(), MoveTemp(BendingConstraints))](TPBDParticles<float, 3>& InParticles, const float Dt) {
				BendConstraints.Apply(InParticles, Dt);
			});
		}
		else
		{
			TArray<Chaos::TVector<int32, 2>> BendingConstraints = Mesh->GetUniqueAdjacentPoints();
			Evolution->AddPBDConstraintFunction([SpringConstraints = Chaos::TPBDSpringConstraints<float, 3>(Evolution->Particles(), MoveTemp(BendingConstraints), ChaosClothSimConfig->BendingStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
				SpringConstraints.Apply(InParticles, Dt);
			});
		}
	}
	if (ChaosClothSimConfig->AreaStiffness)
	{
		TArray<Chaos::TVector<int32, 3>> SurfaceConstraints = SurfaceElements;
		Evolution->AddPBDConstraintFunction([SurfConstraints = Chaos::TPBDAxialSpringConstraints<float, 3>(Evolution->Particles(), MoveTemp(SurfaceConstraints), ChaosClothSimConfig->AreaStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
			SurfConstraints.Apply(InParticles, Dt);
		});
	}
	if (ChaosClothSimConfig->VolumeStiffness)
	{
		check(ChaosClothSimConfig->VolumeStiffness > 0.f && ChaosClothSimConfig->VolumeStiffness <= 1.f);
		if (ChaosClothSimConfig->bUseTetrahedralConstraints)
		{
			// TODO(mlentine): Need to tetrahedralize surface to support this
			check(false);
		}
		else if (ChaosClothSimConfig->bUseThinShellVolumeConstraints)
		{
			TArray<Chaos::TVector<int32, 2>> BendingConstraints = Mesh->GetUniqueAdjacentPoints();
			TArray<Chaos::TVector<int32, 2>> DoubleBendingConstraints;
			{
				TMap<int32, TArray<int32>> BendingHash;
				for (int32 i = 0; i < BendingConstraints.Num(); ++i)
				{
					BendingHash.FindOrAdd(BendingConstraints[i][0]).Add(BendingConstraints[i][1]);
					BendingHash.FindOrAdd(BendingConstraints[i][1]).Add(BendingConstraints[i][0]);
				}
				TSet<Chaos::TVector<int32, 2>> Visited;
				for (auto Elem : BendingHash)
				{
					for (int32 i = 0; i < Elem.Value.Num(); ++i)
					{
						for (int32 j = i + 1; j < Elem.Value.Num(); ++j)
						{
							if (Elem.Value[i] == Elem.Value[j])
								continue;
							auto NewElem = Chaos::TVector<int32, 2>(Elem.Value[i], Elem.Value[j]);
							if (!Visited.Contains(NewElem))
							{
								DoubleBendingConstraints.Add(NewElem);
								Visited.Add(NewElem);
								Visited.Add(Chaos::TVector<int32, 2>(Elem.Value[j], Elem.Value[i]));
							}
						}
					}
				}
			}
			Evolution->AddPBDConstraintFunction([SpringConstraints = Chaos::TPBDSpringConstraints<float, 3>(Evolution->Particles(), MoveTemp(DoubleBendingConstraints), ChaosClothSimConfig->VolumeStiffness)](TPBDParticles<float, 3>& InParticles, const float Dt) {
				SpringConstraints.Apply(InParticles, Dt);
			});
		}
		else
		{
			TArray<Chaos::TVector<int32, 3>> SurfaceConstraints = SurfaceElements;
			Chaos::TPBDVolumeConstraint<float> PBDVolumeConstraint(Evolution->Particles(), MoveTemp(SurfaceConstraints));
			Evolution->AddPBDConstraintFunction([PBDVolumeConstraint = MoveTemp(PBDVolumeConstraint)](TPBDParticles<float, 3>& InParticles, const float Dt)
			{
				PBDVolumeConstraint.Apply(InParticles, Dt);
			});
		}
	}
	if (ChaosClothSimConfig->StrainLimitingStiffness)
	{
		check(Mesh->GetNumElements() > 0);
		Chaos::TPerParticlePBDLongRangeConstraints<float, 3> PerParticlePBDLongRangeConstraints(
			Evolution->Particles(),
			Mesh->GetPointToNeighborsMap(),
			10, // The max number of connected neighbors per particle.  ryan - What should this be?  Was k...
			ChaosClothSimConfig->StrainLimitingStiffness);

		Evolution->AddPBDConstraintFunction([PerParticlePBDLongRangeConstraints = MoveTemp(PerParticlePBDLongRangeConstraints)](TPBDParticles<float, 3>& InParticles, const float Dt)
		{
			PerParticlePBDLongRangeConstraints.Apply(InParticles, Dt);
		});
	}

	// Maximum Distance Constraints
	const UEnum* const MeshTargets = PhysMesh->GetFloatArrayTargets();
	const uint32 PhysMeshMaxDistanceIndex = MeshTargets->GetValueByName(TEXT("MaxDistance"));
	if (PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex)->Num() > 0)
	{
		check(Mesh->GetNumElements() > 0);
		Chaos::PBDSphericalConstraint<float, 3> SphericalContraint(Offset, PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex)->Num(), true, &AnimationPositions, PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex));
		Evolution->AddPBDConstraintFunction([SphericalContraint = MoveTemp(SphericalContraint)](TPBDParticles<float, 3>& InParticles, const float Dt)
		{
			SphericalContraint.Apply(InParticles, Dt);
		});
	}

	// Backstop Constraints
	const uint32 PhysMeshBackstopDistanceIndex = MeshTargets->GetValueByName(TEXT("BackstopDistance"));
	const uint32 PhysMeshBackstopRadiusIndex = MeshTargets->GetValueByName(TEXT("BackstopRadius"));
	if (PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex)->Num() > 0 && PhysMesh->GetFloatArray(PhysMeshBackstopDistanceIndex)->Num() > 0)
	{
		check(Mesh->GetNumElements() > 0);
		check(PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex)->Num() == PhysMesh->GetFloatArray(PhysMeshBackstopDistanceIndex)->Num());

		Chaos::PBDSphericalConstraint<float, 3> SphericalContraint(Offset, PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex)->Num(), false, &AnimationPositions,
			PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex), PhysMesh->GetFloatArray(PhysMeshBackstopDistanceIndex), &AnimationNormals);
		Evolution->AddPBDConstraintFunction([SphericalContraint = MoveTemp(SphericalContraint)](TPBDParticles<float, 3>& InParticles, const float Dt)
		{
			SphericalContraint.Apply(InParticles, Dt);
		});
	}

	// Animation Drive Constraints
	const uint32 PhysMeshAnimDriveIndex = MeshTargets->GetValueByName(TEXT("AnimDriveMultiplier"));
	if (PhysMesh->GetFloatArray(PhysMeshAnimDriveIndex)->Num() > 0)
	{
		check(Mesh->GetNumElements() > 0);
		TPBDAnimDriveConstraint<float, 3> PBDAnimDriveConstraint(Offset, &AnimationPositions, PhysMesh->GetFloatArray(PhysMeshAnimDriveIndex), AnimDriveSpringStiffness[InSimDataIndex]);
		Evolution->AddPBDConstraintFunction(
			[PBDAnimDriveConstraint = MoveTemp(PBDAnimDriveConstraint), &Stiffness = AnimDriveSpringStiffness[InSimDataIndex]](TPBDParticles<float, 3>& InParticles, const float Dt) mutable
		{
			PBDAnimDriveConstraint.SetSpringStiffness(Stiffness);
			PBDAnimDriveConstraint.Apply(InParticles, Dt);
		});
	}

    // Add Self Collisions
    if (ChaosClothSimConfig->bUseSelfCollisions)
    {
        // TODO(mlentine): Parallelize these for multiple meshes
        Evolution->CollisionTriangles().Append(SurfaceElements);
        for (uint32 i = Offset; i < Particles.Size(); ++i)
        {
            auto Neighbors = Mesh->GetNRing(i, 5);
            for (const auto& Element : Neighbors)
            {
                check(i != Element);
                Evolution->DisabledCollisionElements().Add(Chaos::TVector<int32, 2>(i, Element));
                Evolution->DisabledCollisionElements().Add(Chaos::TVector<int32, 2>(Element, i));
            }
        }
    }

	// Collision particles
	TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();

	// Pull collisions from the specified physics asset inside the clothing asset
	ExtractPhysicsAssetCollisions(Asset);

	// Extract the legacy Apex collision from the clothing asset
	ExtractLegacyAssetCollisions(Asset, InOwnerComponent);

	// Set the external collision starting index
	checkf(ExternalCollisions.Spheres.Num() == 0 &&
		ExternalCollisions.SphereConnections.Num() == 0 &&
		ExternalCollisions.Convexes.Num() == 0 &&
		ExternalCollisions.Boxes.Num() == 0, TEXT("There cannot be any external collisions added before all the cloth assets collisions are processed."));
	ExternalCollisionsOffset = CollisionParticles.Size();
}

void ClothingSimulation::PostActorCreationInitialize()
{
	// Let all assets point to the same shared configuration
	for (UClothingAssetCommon* Asset : Assets)
	{
		if (Asset)
		{
			// If we don't have a shared sim config, create one. This is only possible if none of the cloth Assets had a configuration during Actor creation
			if (ClothSharedSimConfig == nullptr)
			{
				// None of the cloth assets had a clothSharedSimConfig, so we will create it
				ClothSharedSimConfig = NewObject<UChaosClothSharedSimConfig>(Asset, UChaosClothSharedSimConfig::StaticClass()->GetFName());
				check(ClothSharedSimConfig);
			}
			Asset->ClothSharedSimConfig = ClothSharedSimConfig;
		}
	}

	if (ClothSharedSimConfig) // ClothSharedSimConfig will be a null pointer if all cloth instances are disabled in which case we will use default Evolution parameters
	{
		// Now set all the common parameters on the simulation
		Evolution->SetIterations(ClothSharedSimConfig->IterationCount);
		Evolution->SetSelfCollisionThickness(ClothSharedSimConfig->SelfCollisionThickness);
		Evolution->SetCollisionThickness(ClothSharedSimConfig->CollisionThickness);
		Evolution->SetDamping(ClothSharedSimConfig->Damping);
		Evolution->GetGravityForces().SetAcceleration(Chaos::TVector<float, 3>(ClothSharedSimConfig->Gravity));
	}	
}

void ClothingSimulation::UpdateCollisionTransforms(const ClothingSimulationContext& Context)
{
	TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();

	// Resize the transform arrays
	const int32 PrevNumCollisions = CollisionTransforms.Num();
	const int32 NumCollisions = BaseTransforms.Num();
	check(NumCollisions == int32(CollisionParticles.Size()));  // BaseTransforms should always automatically grow with the number of collision particles (collection array)

	if (NumCollisions != PrevNumCollisions)
	{
		CollisionTransforms.SetNum(NumCollisions);
		OldCollisionTransforms.SetNum(NumCollisions);
	}

	// Update the collision transforms
	for (int32 Index = 0; Index < NumCollisions; ++Index)
	{
		const int32 BoneIndex = BoneIndices[Index];
		if (Context.BoneTransforms.IsValidIndex(BoneIndex))
		{
			const FTransform& BoneTransform = Context.BoneTransforms[BoneIndex];
			CollisionTransforms[Index] = BaseTransforms[Index] * BoneTransform * Context.ComponentToWorld;
		}
		else
		{
			CollisionTransforms[Index] = BaseTransforms[Index] * Context.ComponentToWorld;  // External collisions often don't map to a bone
		}
	}

	// Set the new collision particles and transforms initial state
	for (int32 Index = PrevNumCollisions; Index < NumCollisions; ++Index)
	{
		CollisionParticles.X(Index) = CollisionTransforms[Index].GetTranslation();
		CollisionParticles.R(Index) = CollisionTransforms[Index].GetRotation();

		OldCollisionTransforms[Index] = CollisionTransforms[Index];
	}
}

void ClothingSimulation::ExtractPhysicsAssetCollisions(UClothingAssetCommon* Asset)
{
	ExtractedCollisions.Reset();

	TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();

	// TODO(mlentine): Support collision body activation on a per particle basis, preferably using a map but also can be a particle attribute
	if (const UPhysicsAsset* const PhysAsset = Asset->PhysicsAsset)
	{
		const USkeletalMesh* const TargetMesh = CastChecked<USkeletalMesh>(Asset->GetOuter());

		TArray<int32> UsedBoneIndices;
		UsedBoneIndices.Reserve(PhysAsset->SkeletalBodySetups.Num());

		for (const USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
		{
			if (!BodySetup)
				continue;

			const int32 MeshBoneIndex = TargetMesh->RefSkeleton.FindBoneIndex(BodySetup->BoneName);
			const int32 MappedBoneIndex = UsedBoneIndices.Add(MeshBoneIndex);
			
			// Add capsules
			const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
			if (AggGeom.SphylElems.Num())
			{
				for (const FKSphylElem& SphylElem : AggGeom.SphylElems)
				{
					if (SphylElem.Length == 0.0f)
					{
						// Add extracted sphere collision data
						FClothCollisionPrim_Sphere Sphere;
						Sphere.LocalPosition = SphylElem.Center;
						Sphere.Radius = SphylElem.Radius;
						Sphere.BoneIndex = MappedBoneIndex;
						ExtractedCollisions.Spheres.Add(Sphere);
					}
					else
					{
						// Add extracted spheres collision data
						FClothCollisionPrim_Sphere Sphere0;
						FClothCollisionPrim_Sphere Sphere1;
						const FVector OrientedDirection = SphylElem.Rotation.RotateVector(FVector::UpVector);
						const FVector HalfDim = OrientedDirection * (SphylElem.Length / 2.f);
						Sphere0.LocalPosition = SphylElem.Center - HalfDim;
						Sphere1.LocalPosition = SphylElem.Center + HalfDim;
						Sphere0.Radius = SphylElem.Radius;
						Sphere1.Radius = SphylElem.Radius;
						Sphere0.BoneIndex = MappedBoneIndex;
						Sphere1.BoneIndex = MappedBoneIndex;

						// Add extracted sphere connection collision data
						FClothCollisionPrim_SphereConnection SphereConnection;
						SphereConnection.SphereIndices[0] = ExtractedCollisions.Spheres.Add(Sphere0);
						SphereConnection.SphereIndices[1] = ExtractedCollisions.Spheres.Add(Sphere1);
						ExtractedCollisions.SphereConnections.Add(SphereConnection);
					}
				}
			}

			// Add spheres
			for (const FKSphereElem& SphereElem : AggGeom.SphereElems)
			{
				// Add extracted sphere collision data
				FClothCollisionPrim_Sphere Sphere;
				Sphere.LocalPosition = SphereElem.Center;
				Sphere.Radius = SphereElem.Radius;
				Sphere.BoneIndex = MappedBoneIndex;
				ExtractedCollisions.Spheres.Add(Sphere);
			}

			// Add boxes
			for (const FKBoxElem& BoxElem : AggGeom.BoxElems)
			{
				// Add extracted box collision data
				FClothCollisionPrim_Box Box;
				Box.LocalPosition = BoxElem.Center;
				Box.LocalRotation = BoxElem.Rotation.Quaternion();
				Box.HalfExtents = FVector(BoxElem.X, BoxElem.Y, BoxElem.Z) * 0.5f;
				Box.BoneIndex = MappedBoneIndex;
				ExtractedCollisions.Boxes.Add(Box);
			}

			// Add tapered capsules
			for (const FKTaperedCapsuleElem& TaperedCapsuleElem : AggGeom.TaperedCapsuleElems)
			{
				if (TaperedCapsuleElem.Length == 0)
				{
					// Add extracted sphere collision data
					FClothCollisionPrim_Sphere Sphere;
					Sphere.LocalPosition = TaperedCapsuleElem.Center;
					Sphere.Radius = FMath::Max(TaperedCapsuleElem.Radius0, TaperedCapsuleElem.Radius1);
					Sphere.BoneIndex = MappedBoneIndex;
					ExtractedCollisions.Spheres.Add(Sphere);
				}
				else
				{
					// Add extracted spheres collision data
					FClothCollisionPrim_Sphere Sphere0;
					FClothCollisionPrim_Sphere Sphere1;
					const FVector OrientedDirection = TaperedCapsuleElem.Rotation.RotateVector(FVector::UpVector);
					const FVector HalfDim = OrientedDirection * (TaperedCapsuleElem.Length / 2.f);
					Sphere0.LocalPosition = TaperedCapsuleElem.Center + HalfDim;
					Sphere1.LocalPosition = TaperedCapsuleElem.Center - HalfDim;
					Sphere0.Radius = TaperedCapsuleElem.Radius0;
					Sphere1.Radius = TaperedCapsuleElem.Radius1;
					Sphere0.BoneIndex = MappedBoneIndex;
					Sphere1.BoneIndex = MappedBoneIndex;

					// Add extracted sphere connection collision data
					FClothCollisionPrim_SphereConnection SphereConnection;
					SphereConnection.SphereIndices[0] = ExtractedCollisions.Spheres.Add(Sphere0);
					SphereConnection.SphereIndices[1] = ExtractedCollisions.Spheres.Add(Sphere1);
					ExtractedCollisions.SphereConnections.Add(SphereConnection);
				}
			}

#if !PLATFORM_LUMIN && !PLATFORM_ANDROID  // TODO(Kriss.Gossart): Compile on Android and fix whatever errors the following code is causing
			// Add convexes
			for (const FKConvexElem& ConvexElem : AggGeom.ConvexElems)
			{
				// Add stub for extracted collision data
				FClothCollisionPrim_Convex Convex;
				Convex.BoneIndex = MappedBoneIndex;
#if WITH_PHYSX
				// Collision bodies are stored in PhysX specific data structures so they can only be imported if we enable PhysX.
				const physx::PxConvexMesh* const PhysXMesh = ConvexElem.GetConvexMesh();  // TODO(Kriss.Gossart): Deal with this legacy structure in a different place, so that there's only TConvex
				const int32 NumPolygons = int32(PhysXMesh->getNbPolygons());
				Convex.Planes.SetNumUninitialized(NumPolygons);
				for (int32 i = 0; i < NumPolygons; ++i)
				{
					physx::PxHullPolygon Poly;
					PhysXMesh->getPolygonData(i, Poly);
					check(Poly.mNbVerts == 3);
					const auto Indices = PhysXMesh->getIndexBuffer() + Poly.mIndexBase;

					Convex.Planes[i] = FPlane(
						ConvexElem.VertexData[Indices[0]],
						ConvexElem.VertexData[Indices[1]],
						ConvexElem.VertexData[Indices[2]]);
				}

				// Rebuild surface points
				Convex.RebuildSurfacePoints();

#elif WITH_CHAOS  // #if WITH_PHYSX
				const Chaos::FImplicitObject& ChaosConvexMesh = *ConvexElem.GetChaosConvexMesh();
				const Chaos::TConvex<float, 3>& ChaosConvex = ChaosConvexMesh.GetObjectChecked<Chaos::TConvex<float, 3>>();

				// Copy planes
				const TArray<TPlane<float, 3>>& Planes = ChaosConvex.GetFaces();
				Convex.Planes.Reserve(Planes.Num());
				for (const TPlane<float, 3>& Plane : Planes)
				{
					Convex.Planes.Add(FPlane(Plane.X(), Plane.Normal()));
				}

				// Copy surface points
				const uint32 NumSurfacePoints = ChaosConvex.GetSurfaceParticles().Size();
				Convex.SurfacePoints.Reserve(NumSurfacePoints);
				for (uint32 ParticleIndex = 0; ParticleIndex < NumSurfacePoints; ++ParticleIndex)
				{
					Convex.SurfacePoints.Add(ChaosConvex.GetSurfaceParticles().X(ParticleIndex));
				}
#endif  // #if WITH_PHYSX #elif WITH_CHAOS

				// Add extracted collision data
				ExtractedCollisions.Convexes.Add(Convex);
			}
#endif  // #if !PLATFORM_LUMIN && !PLATFORM_ANDROID

		}  // End for PhysAsset->SkeletalBodySetups

		// Add collisions particles
		UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Adding physics asset collisions..."));
		AddCollisions(ExtractedCollisions, UsedBoneIndices);

	}  // End if Asset->PhysicsAsset
}

void ClothingSimulation::ExtractLegacyAssetCollisions(UClothingAssetCommon* Asset, const USkeletalMeshComponent* InOwnerComponent)
{
	check(Asset->GetNumLods() > 0);
	ensure(Asset->GetNumLods() == 1);
	UE_CLOG(Asset->GetNumLods() != 1,
		LogChaosCloth, Warning, TEXT("More than one LOD with the current cloth asset. Only LOD 0 is supported with the current system."));

	if (const UClothLODDataBase* const AssetLodData = Asset->ClothLodData[0])
	{
		const FClothCollisionData& LodCollData = AssetLodData->CollisionData;
		if (LodCollData.Spheres.Num() || LodCollData.SphereConnections.Num() || LodCollData.Convexes.Num())
		{
			UE_LOG(LogChaosCloth, Warning,
				TEXT("Actor '%s' component '%s' has %d sphere, %d capsule, and %d "
					"convex collision objects for physics authored as part of a LOD construct, "
					"probably by the Apex cloth authoring system.  This is deprecated.  "
					"Please update your asset!"),
				InOwnerComponent->GetOwner() ? *InOwnerComponent->GetOwner()->GetName() : TEXT("None"),
				*InOwnerComponent->GetName(),
				LodCollData.Spheres.Num(),
				LodCollData.SphereConnections.Num(),
				LodCollData.Convexes.Num());

			UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Adding legacy cloth asset collisions..."));
			AddCollisions(LodCollData, Asset->UsedBoneIndices);
		}
	}
}

void ClothingSimulation::AddCollisions(const FClothCollisionData& ClothCollisionData, const TArray<int32>& UsedBoneIndices)
{
	TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();

	// Capsules
	TSet<int32> CapsuleEnds;
	const int32 NumCapsules = ClothCollisionData.SphereConnections.Num();
	if (NumCapsules)
	{
		const uint32 Offset = CollisionParticles.Size();
		CollisionParticles.AddParticles(NumCapsules);

		CapsuleEnds.Reserve(NumCapsules * 2);
		for (uint32 i = Offset; i < CollisionParticles.Size(); ++i)
		{
			const FClothCollisionPrim_SphereConnection& Connection = ClothCollisionData.SphereConnections[i - Offset];

			const int32 SphereIndex0 = Connection.SphereIndices[0];
			const int32 SphereIndex1 = Connection.SphereIndices[1];
			checkSlow(SphereIndex0 != SphereIndex1);
			const FClothCollisionPrim_Sphere& Sphere0 = ClothCollisionData.Spheres[SphereIndex0];
			const FClothCollisionPrim_Sphere& Sphere1 = ClothCollisionData.Spheres[SphereIndex1];

			const int32 MappedIndex = UsedBoneIndices.IsValidIndex(Sphere0.BoneIndex) ? UsedBoneIndices[Sphere0.BoneIndex] : INDEX_NONE;

			BoneIndices[i] = GetMappedBoneIndex(UsedBoneIndices, Sphere0.BoneIndex);
			checkSlow(Sphere0.BoneIndex == Sphere1.BoneIndex);
			UE_CLOG(Sphere0.BoneIndex != Sphere1.BoneIndex,
				LogChaosCloth, Warning, TEXT("Found a legacy Apex cloth asset with a collision capsule spanning across two bones. This is not supported with the current system."));
			UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Found collision capsule on bone index %d."), BoneIndices[i]);

			const Chaos::TVector<float, 3> X0 = Sphere0.LocalPosition;
			const Chaos::TVector<float, 3> X1 = Sphere1.LocalPosition;

			const float Radius0 = Sphere0.Radius;
			const float Radius1 = Sphere1.Radius;

			if (FMath::Abs(Radius0 - Radius1) < SMALL_NUMBER)
			{
				// Capsule
				const Chaos::TVector<float, 3> Center = (X0 + X1) * 0.5f;  // Construct a capsule centered at the origin along the Z axis
				const Chaos::TVector<float, 3> Axis = X1 - X0;
				const Chaos::TRotation<float, 3> Rotation = Chaos::TRotation<float, 3>::FromRotatedVector(
					Chaos::TVector<float, 3>::AxisVector(2),
					Axis.GetSafeNormal());

				BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(Center, Rotation);

				const float HalfHeight = Axis.Size() * 0.5f;
				CollisionParticles.SetDynamicGeometry(
					i,
					MakeUnique<Chaos::TCapsule<float>>(
						Chaos::TVector<float, 3>(0.f, 0.f, -HalfHeight), // Min
						Chaos::TVector<float, 3>(0.f, 0.f, HalfHeight), // Max
						Radius0));
			}
			else
			{
				// Tapered capsule
				TArray<TUniquePtr<Chaos::FImplicitObject>> Objects;
				Objects.Reserve(3);
				Objects.Add(TUniquePtr<Chaos::FImplicitObject>(
					new Chaos::TTaperedCylinder<float>(X0, X1, Radius0, Radius1)));
				Objects.Add(TUniquePtr<Chaos::FImplicitObject>(
					new Chaos::TSphere<float, 3>(X0, Radius0)));
				Objects.Add(TUniquePtr<Chaos::FImplicitObject>(
					new Chaos::TSphere<float, 3>(X1, Radius1)));
				CollisionParticles.SetDynamicGeometry(
					i,
					MakeUnique<Chaos::TImplicitObjectUnion<float, 3>>(MoveTemp(Objects)));  // TODO(Kriss.Gossart): Replace this once a TTaperedCapsule implicit type is implemented
			}

			// Skip spheres added as end caps for the capsule.
			CapsuleEnds.Add(SphereIndex0);
			CapsuleEnds.Add(SphereIndex1);
		}
	}

	// Spheres
	const int32 NumSpheres = ClothCollisionData.Spheres.Num() - CapsuleEnds.Num();
	if (NumSpheres != 0)
	{
		const uint32 Offset = CollisionParticles.Size();
		CollisionParticles.AddParticles(NumSpheres);
		// i = Spheres index, j = CollisionParticles index
		for (uint32 i = 0, j = Offset; i < (uint32)ClothCollisionData.Spheres.Num(); ++i)
		{
			// Skip spheres that are the end caps of capsules.
			if (CapsuleEnds.Contains(i))
				continue;

			const FClothCollisionPrim_Sphere& Sphere = ClothCollisionData.Spheres[i];

			BoneIndices[j] = GetMappedBoneIndex(UsedBoneIndices, Sphere.BoneIndex);
			UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Found collision sphere on bone index %d."), BoneIndices[i]);

			BaseTransforms[j] = Chaos::TRigidTransform<float, 3>(FTransform::Identity);

			CollisionParticles.SetDynamicGeometry(
				j,
				MakeUnique<Chaos::TSphere<float, 3>>(
					Sphere.LocalPosition,
					Sphere.Radius));

			++j;
		}
	}

	// Convexes
	const uint32 NumConvexes = ClothCollisionData.Convexes.Num();
	if (NumConvexes != 0)
	{
		const uint32 Offset = CollisionParticles.Size();
		CollisionParticles.AddParticles(NumConvexes);
		for (uint32 i = Offset; i < CollisionParticles.Size(); ++i)
		{
			const FClothCollisionPrim_Convex& Convex = ClothCollisionData.Convexes[i - Offset];

			BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(FTransform::Identity);

			BoneIndices[i] = GetMappedBoneIndex(UsedBoneIndices, Convex.BoneIndex);
			UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Found collision convex on bone index %d."), BoneIndices[i]);

			const int32 NumSurfacePoints = Convex.SurfacePoints.Num();
			const int32 NumPlanes = Convex.Planes.Num();

			if (NumSurfacePoints < 4)
			{
				UE_LOG(LogChaosCloth, Warning, TEXT("Invalid convex collision: not enough surface points."));
			}
			else if (NumPlanes < 4)
			{
				UE_LOG(LogChaosCloth, Warning, TEXT("Invalid convex collision: not enough planes."));
			}
			else
			{
				// Retrieve convex planes
				TArray<TPlane<float, 3>> Planes;
				Planes.Reserve(Convex.Planes.Num());
				for (const FPlane& Plane : Convex.Planes)
				{
					FPlane NormalizedPlane(Plane);
					if (NormalizedPlane.Normalize())
					{
						const Chaos::TVector<float, 3> Normal(static_cast<FVector>(NormalizedPlane));
						const Chaos::TVector<float, 3> Base = Normal * NormalizedPlane.W;

						Planes.Add(Chaos::TPlane<float, 3>(Base, Normal));
					}
					else
					{
						UE_LOG(LogChaosCloth, Warning, TEXT("Invalid convex collision: bad plane normal."));
						break;
					}
				}

				if (Planes.Num() == Convex.Planes.Num())
				{
					// Retrieve particles
					TParticles<float, 3> SurfaceParticles;
					SurfaceParticles.Resize(NumSurfacePoints);
					for (int32 ParticleIndex = 0; ParticleIndex < NumSurfacePoints; ++ParticleIndex)
					{
						SurfaceParticles.X(ParticleIndex) = Convex.SurfacePoints[ParticleIndex];
					}

					// Setup the collision particle geometry
					CollisionParticles.SetDynamicGeometry(i, MakeUnique<Chaos::TConvex<float, 3>>(MoveTemp(Planes), MoveTemp(SurfaceParticles)));
				}
			}

			if (!CollisionParticles.DynamicGeometry(i))
			{
				UE_LOG(LogChaosCloth, Warning, TEXT("Replacing invalid convex collision by a default unit sphere."));
				CollisionParticles.SetDynamicGeometry(i, MakeUnique<Chaos::TSphere<float, 3>>(Chaos::TVector<float, 3>(0.0f), 1.0f));  // Default to a unit sphere to replace the faulty convex
			}
		}
	}

	// Boxes
	const uint32 NumBoxes = ClothCollisionData.Boxes.Num();
	if (NumBoxes != 0)
	{
		const uint32 Offset = CollisionParticles.Size();
		CollisionParticles.AddParticles(NumBoxes);
		for (uint32 i = Offset; i < CollisionParticles.Size(); ++i)
		{
			const FClothCollisionPrim_Box& Box = ClothCollisionData.Boxes[i - Offset];
			CollisionParticles.X(i) = Chaos::TVector<float, 3>(0.f);
			CollisionParticles.R(i) = Chaos::TRotation<float, 3>::FromIdentity();
			
			BaseTransforms[i] = Chaos::TRigidTransform<float, 3>(Box.LocalPosition, Box.LocalRotation);
			
			BoneIndices[i] = GetMappedBoneIndex(UsedBoneIndices, Box.BoneIndex);
			UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Found collision box on bone index %d."), BoneIndices[i]);

			CollisionParticles.SetDynamicGeometry(i, MakeUnique<Chaos::TBox<float, 3>>(-Box.HalfExtents, Box.HalfExtents));
		}
	}

	UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Added collisions: %d spheres, %d capsules, %d convexes, %d boxes."), NumSpheres, NumCapsules, NumConvexes, NumBoxes);
}

void ClothingSimulation::FillContext(USkeletalMeshComponent* InComponent, float InDeltaTime, IClothingSimulationContext* InOutContext)
{
    ClothingSimulationContext* Context = static_cast<ClothingSimulationContext*>(InOutContext);
    Context->ComponentToWorld = InComponent->GetComponentToWorld();
    Context->DeltaTime = ClampDeltaTime > 0 ? std::min(InDeltaTime, ClampDeltaTime) : InDeltaTime;

	Context->RefToLocals.Reset();
    InComponent->GetCurrentRefToLocalMatrices(Context->RefToLocals, 0);

	const USkeletalMesh* SkelMesh = InComponent->SkeletalMesh;
    if (USkinnedMeshComponent* MasterComponent = InComponent->MasterPoseComponent.Get())
    {
		const TArray<int32>& MasterBoneMap = InComponent->GetMasterBoneMap();
        int32 NumBones = MasterBoneMap.Num();
        if (NumBones == 0)
        {
            if (InComponent->SkeletalMesh)
            {
                // This case indicates an invalid master pose component (e.g. no skeletal mesh)
                NumBones = InComponent->SkeletalMesh->RefSkeleton.GetNum();
            }
			Context->BoneTransforms.Reset(NumBones);
			Context->BoneTransforms.AddDefaulted(NumBones);
        }
        else
        {
            Context->BoneTransforms.Reset(NumBones);
            Context->BoneTransforms.AddDefaulted(NumBones);
			const TArray<FTransform>& MasterTransforms = MasterComponent->GetComponentSpaceTransforms();
            for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
            {
                bool bFoundMaster = false;
                if (MasterBoneMap.IsValidIndex(BoneIndex))
                {
                    const int32 MasterIndex = MasterBoneMap[BoneIndex];
                    if (MasterTransforms.IsValidIndex(MasterIndex))
                    {
                        Context->BoneTransforms[BoneIndex] = MasterTransforms[MasterIndex];
                        bFoundMaster = true;
                    }
                }

                if (!bFoundMaster && SkelMesh)
                {
                    const int32 ParentIndex = SkelMesh->RefSkeleton.GetParentIndex(BoneIndex);
					check(ParentIndex < BoneIndex);
					Context->BoneTransforms[BoneIndex] =
						Context->BoneTransforms.IsValidIndex(ParentIndex) && ParentIndex < BoneIndex ?
						Context->BoneTransforms[ParentIndex] * SkelMesh->RefSkeleton.GetRefBonePose()[BoneIndex] :
                        SkelMesh->RefSkeleton.GetRefBonePose()[BoneIndex];
                }
            }
        }
    }
    else
    {
        Context->BoneTransforms = InComponent->GetComponentSpaceTransforms();
    }
}

void ClothingSimulation::Simulate(IClothingSimulationContext* InContext)
{
	ClothingSimulationContext* Context = static_cast<ClothingSimulationContext*>(InContext);
	if (Context->DeltaTime == 0)
		return;

	// Get New Animation Positions and Normals
	OldCollisionTransforms = CollisionTransforms;
	OldAnimationPositions = AnimationPositions;

	for (int32 Index = 0; Index < IndexToRangeMap.Num(); ++Index)
	{
		const UClothingAssetCommon* const Asset = Assets[Index];
		if (!Asset)
			continue;

		const UClothLODDataBase* AssetLodData = Asset->ClothLodData[0];
		check(AssetLodData->PhysicalMeshData);
		const UClothPhysicalMeshDataBase* PhysMesh = AssetLodData->PhysicalMeshData;

		TArray<Chaos::TVector<float, 3>> TempAnimationPositions;
		TArray<Chaos::TVector<float, 3>> TempAnimationNormals;

		FTransform RootBoneTransform = Context->BoneTransforms[Asset->ReferenceBoneIndex];
		ClothingMeshUtils::SkinPhysicsMesh(
			Asset->UsedBoneIndices,
			*PhysMesh,
			RootBoneTransform,
			Context->RefToLocals.GetData(),
			Context->RefToLocals.Num(),
			reinterpret_cast<TArray<FVector>&>(TempAnimationPositions),
			reinterpret_cast<TArray<FVector>&>(TempAnimationNormals));

		RootBoneTransform.SetScale3D(FVector(1.0f));

		// Removing Context->ComponentToWorld means the sim doesn't see updates to the component level xf
		const FTransform RootBoneWorldTransform = RootBoneTransform * Context->ComponentToWorld;

		const int32 Offset = IndexToRangeMap[Index][0];
		check(TempAnimationPositions.Num() == IndexToRangeMap[Index][1] - IndexToRangeMap[Index][0]);

		ParallelFor(TempAnimationPositions.Num(),
		[&](int32 AnimationElementIndex)
		{
			AnimationPositions[Offset + AnimationElementIndex] = RootBoneWorldTransform.TransformPosition(TempAnimationPositions[AnimationElementIndex]);
			AnimationNormals[Offset + AnimationElementIndex] = RootBoneWorldTransform.TransformVector(TempAnimationNormals[AnimationElementIndex]);
		});
	}

	// Update collision tranforms
	UpdateCollisionTransforms(*Context);

	// Advance Sim
	DeltaTime = Context->DeltaTime;
	while (Context->DeltaTime > MaxDeltaTime)
	{
		Evolution->AdvanceOneTimeStep(MaxDeltaTime);
		Context->DeltaTime -= MaxDeltaTime;
	}
	Evolution->AdvanceOneTimeStep(Context->DeltaTime);
	Time += DeltaTime;
}

void ClothingSimulation::GetSimulationData(
	TMap<int32, FClothSimulData>& OutData,
	USkeletalMeshComponent* InOwnerComponent,
	USkinnedMeshComponent* InOverrideComponent) const
{
	const FTransform& OwnerTransform = InOwnerComponent->GetComponentTransform();
	for (int32 i = 0; i < IndexToRangeMap.Num(); ++i)
	{
		const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[i];
		if (!Mesh)
			continue;
		Mesh->GetFaceNormals(FaceNormals[i], Evolution->Particles().X(), false);  // No need to add a point index offset here since that is baked into the triangles
		Mesh->GetPointNormals(PointNormals[i], FaceNormals[i], /*bReturnEmptyOnError =*/ false, /*bFillAtStartIndex =*/ false);

		FClothSimulData& Data = OutData.FindOrAdd(i);
		Data.Reset();

		const UClothingAssetCommon* const Asset = Assets[i];
		if (!Asset)
			continue;

		const TArray<FTransform>& ComponentSpaceTransforms = InOverrideComponent ?
			InOverrideComponent->GetComponentSpaceTransforms() :
			InOwnerComponent->GetComponentSpaceTransforms();
		if (!ComponentSpaceTransforms.IsValidIndex(Asset->ReferenceBoneIndex))
		{
			UE_LOG(LogSkeletalMesh, Warning,
				TEXT("Failed to write back clothing simulation data for component '%s' as bone transforms are invalid."),
				*InOwnerComponent->GetName());
			check(false);
			continue;
		}

		FTransform RootBoneTransform = ComponentSpaceTransforms[Asset->ReferenceBoneIndex];
		RootBoneTransform.SetScale3D(FVector(1.0f));
		RootBoneTransform *= OwnerTransform;
		Data.Transform = RootBoneTransform;
		Data.ComponentRelativeTransform = OwnerTransform.Inverse();

		const Chaos::TVector<uint32, 2>& VertexDomain = IndexToRangeMap[i];
		const uint32 VertexRange = VertexDomain[1] - VertexDomain[0];
		Data.Positions.SetNum(VertexRange);
        Data.Normals.SetNum(VertexRange);
		for (uint32 j = VertexDomain[0]; j < VertexDomain[1]; ++j)
        {
			const uint32 LocalIndex = j - VertexDomain[0];
            Data.Positions[LocalIndex] = Evolution->Particles().X(j);
            Data.Normals[LocalIndex] = PointNormals[i][LocalIndex];
		}
    }
}

void ClothingSimulation::AddExternalCollisions(const FClothCollisionData& InData)
{
	// Keep track of the external collisions data
	ExternalCollisions.Append(InData);

	// Setup the new collisions particles
	UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Adding external collisions..."));
	const TArray<int32> UsedBoneIndices;  // There is no bone mapping available for external collisions
	AddCollisions(InData, UsedBoneIndices);
}

void ClothingSimulation::ClearExternalCollisions()
{
	// Remove all external collision particles, starting from the external collision offset
	// But do not resize CollisionTransforms as it is only resized in UpdateCollisionTransforms()
	TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();
	CollisionParticles.Resize(ExternalCollisionsOffset);  // This will also resize BoneIndices and BaseTransforms

	// Reset external collisions
	ExternalCollisionsOffset = CollisionParticles.Size();
	ExternalCollisions.Reset();

	UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Cleared all external collisions."));
}

void ClothingSimulation::GetCollisions(FClothCollisionData& OutCollisions, bool bIncludeExternal) const
{
	OutCollisions.Reset();

	// Add internal asset collisions
	for (const UClothingAssetCommon* Asset : Assets)
	{
		if (const UClothLODDataBase* const ClothLodData = !Asset ? nullptr : Asset->ClothLodData[0])
		{
			OutCollisions.Append(ClothLodData->CollisionData);
		}
	}

	// Add collisions extracted from the physics asset
	// TODO(Kriss.Gossart): Including the following code seems to be the correct behaviour, but this did not appear
	// in the NvCloth implementation, so best to leave it commented out for now.
	//OutCollisions.Append(ExtractedCollisions);

	// Add external asset collisions
	if (bIncludeExternal)
	{
		OutCollisions.Append(ExternalCollisions);
	}

	UE_LOG(LogChaosCloth, VeryVerbose, TEXT("Returned collisions: %d spheres, %d capsules, %d convexes, %d boxes."), OutCollisions.Spheres.Num() - 2 * OutCollisions.SphereConnections.Num(), OutCollisions.SphereConnections.Num(), OutCollisions.Convexes.Num(), OutCollisions.Boxes.Num());
}

void ClothingSimulation::SetAnimDriveSpringStiffness(float InStiffness)
{
	for (float& stiffness : AnimDriveSpringStiffness)
	{
		stiffness = InStiffness;
	}
}

void ClothingSimulation::SetGravityOverride(const TVector<float, 3>& InGravityOverride) 
{
	Evolution->GetGravityForces().SetAcceleration(InGravityOverride);
}

void ClothingSimulation::DisableGravityOverride()
{
	Evolution->GetGravityForces().SetAcceleration(ClothSharedSimConfig->Gravity);
}

#if WITH_EDITOR
void ClothingSimulation::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(DebugClothMaterial);
}

void ClothingSimulation::DebugDrawPhysMeshWired(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();

			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
			{
				const auto& Element = Elements[ElementIndex];

				const FVector& Pos0 = Particles.X(Element.X);
				const FVector& Pos1 = Particles.X(Element.Y);
				const FVector& Pos2 = Particles.X(Element.Z);

				PDI->DrawLine(Pos0, Pos1, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
				PDI->DrawLine(Pos1, Pos2, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
				PDI->DrawLine(Pos2, Pos0, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawPhysMeshShaded(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	if (!DebugClothMaterial) { return; }

	FDynamicMeshBuilder MeshBuilder(PDI->View->GetFeatureLevel());
	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	int32 VertexIndex = 0;
	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();

			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex, VertexIndex += 3)
			{
				const auto& Element = Elements[ElementIndex];

				const FVector& Pos0 = Particles.X(Element.X);
				const FVector& Pos1 = Particles.X(Element.Y);
				const FVector& Pos2 = Particles.X(Element.Z);

				const FVector& Normal = FVector::CrossProduct(Pos1 - Pos0, Pos2 - Pos0).GetSafeNormal();
				const FVector Tangent = ((Pos1 + Pos2) * 0.5f - Pos0).GetSafeNormal();

				MeshBuilder.AddVertex(FDynamicMeshVertex(Pos0, Tangent, Normal, FVector2D(0.f, 0.f), FColor::White));
				MeshBuilder.AddVertex(FDynamicMeshVertex(Pos1, Tangent, Normal, FVector2D(0.f, 1.f), FColor::White));
				MeshBuilder.AddVertex(FDynamicMeshVertex(Pos2, Tangent, Normal, FVector2D(1.f, 1.f), FColor::White));
				MeshBuilder.AddTriangle(VertexIndex, VertexIndex + 1, VertexIndex + 2);
			}
		}
	}

	MeshBuilder.Draw(PDI, FMatrix::Identity, DebugClothMaterial->GetRenderProxy(), SDPG_World, false, false);
}

void ClothingSimulation::DebugDrawPointNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (Meshes[MeshIndex])
		{
			const Chaos::TVector<uint32, 2> Range = IndexToRangeMap[MeshIndex];
			const TArray<Chaos::TVector<float, 3>>& MeshPointNormals = PointNormals[MeshIndex];

			for (uint32 ParticleIndex = Range[0]; ParticleIndex < Range[1]; ++ParticleIndex)
			{
				const TVector<float, 3>& Pos = Particles.X(ParticleIndex);
				const TVector<float, 3>& Normal = MeshPointNormals[ParticleIndex - Range[0]];

				PDI->DrawLine(Pos, Pos + Normal * 20.0f, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawInversedPointNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (Meshes[MeshIndex])
		{
			const Chaos::TVector<uint32, 2> Range = IndexToRangeMap[MeshIndex];
			const TArray<Chaos::TVector<float, 3>>& MeshPointNormals = PointNormals[MeshIndex];

			for (uint32 ParticleIndex = Range[0]; ParticleIndex < Range[1]; ++ParticleIndex)
			{
				const TVector<float, 3>& Pos = Particles.X(ParticleIndex);
				const TVector<float, 3>& Normal = MeshPointNormals[ParticleIndex - Range[0]];

				PDI->DrawLine(Pos, Pos - Normal * 20.0f, FLinearColor::White, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawFaceNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<Chaos::TVector<float, 3>>& MeshFaceNormals = FaceNormals[MeshIndex];

			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();
			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
			{
				const TVector<int32, 3>& Element = Elements[ElementIndex];

				const TVector<float, 3> Pos = (
					Particles.X(Element.X) +
					Particles.X(Element.Y) +
					Particles.X(Element.Z)) / 3.f;
				const TVector<float, 3>& Normal = MeshFaceNormals[ElementIndex];

				PDI->DrawLine(Pos, Pos + Normal * 20.0f, FLinearColor::Yellow, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawInversedFaceNormals(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	check(Meshes.Num() == IndexToRangeMap.Num());

	const TPBDParticles<float, 3>& Particles = Evolution->Particles();

	for (int32 MeshIndex = 0; MeshIndex < Meshes.Num(); ++MeshIndex)
	{
		if (const TUniquePtr<Chaos::TTriangleMesh<float>>& Mesh = Meshes[MeshIndex])
		{
			const TArray<Chaos::TVector<float, 3>>& MeshFaceNormals = FaceNormals[MeshIndex];

			const TArray<TVector<int32, 3>>& Elements = Mesh->GetElements();
			for (int32 ElementIndex = 0; ElementIndex < Elements.Num(); ++ElementIndex)
			{
				const TVector<int32, 3>& Element = Elements[ElementIndex];

				const TVector<float, 3> Pos = (
					Particles.X(Element.X) +
					Particles.X(Element.Y) +
					Particles.X(Element.Z)) / 3.f;
				const TVector<float, 3>& Normal = MeshFaceNormals[ElementIndex];

				PDI->DrawLine(Pos, Pos - Normal * 20.0f, FLinearColor::Yellow, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawCollision(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	auto DrawSphere = [&PDI](const Chaos::TSphere<float, 3>& Sphere, const TRotation<float, 3>& Rotation, const Chaos::TVector<float, 3>& Position, const FLinearColor& Color)
	{
		const float Radius = Sphere.GetRadius();
		const Chaos::TVector<float, 3> Center = Sphere.GetCenter();
		const FTransform Transform(Rotation, Position + Rotation.RotateVector(Center));
		DrawWireSphere(PDI, Transform, Color, Radius, 12, SDPG_World, 0.0f, 0.001f, false);
	};

	auto DrawBox = [&PDI](const Chaos::TBox<float, 3>& Box, const TRotation<float, 3>& Rotation, const Chaos::TVector<float, 3>& Position, const FLinearColor& Color)
	{
		const FMatrix BoxToWorld = FTransform(Rotation, Position).ToMatrixNoScale();
		const FVector Radii = Box.Extents() * 0.5f;
		DrawWireBox(PDI, BoxToWorld, FBox(Box.Min(), Box.Max()), Color, SDPG_World, 0.0f, 0.001f, false);
	};

	auto DrawCapsule = [&PDI](const Chaos::TCapsule<float>& Capsule, const TRotation<float, 3>& Rotation, const Chaos::TVector<float, 3>& Position, const FLinearColor& Color)
	{
		const float HalfHeight = Capsule.GetHeight() * 0.5f;
		const float Radius = Capsule.GetRadius();
		const FVector X = Rotation.RotateVector(FVector::ForwardVector);
		const FVector Y = Rotation.RotateVector(FVector::RightVector);
		const FVector Z = Rotation.RotateVector(FVector::UpVector);
		DrawWireCapsule(PDI, Position, X, Y, Z, Color, Radius, HalfHeight + Radius, 12, SDPG_World, 0.0f, 0.001f, false);
	};

	auto DrawTaperedCylinder = [&PDI](const Chaos::TTaperedCylinder<float>& TaperedCylinder, const TRotation<float, 3>& Rotation, const Chaos::TVector<float, 3>& Position, const FLinearColor& Color)
	{
		const float HalfHeight = TaperedCylinder.GetHeight() * 0.5f;
		const float Radius1 = TaperedCylinder.GetRadius1();
		const float Radius2 = TaperedCylinder.GetRadius2();
		const FVector Position1 = Position + Rotation.RotateVector(TaperedCylinder.GetX1());
		const FVector Position2 = Position + Rotation.RotateVector(TaperedCylinder.GetX2());
		const FQuat Q = (Position2 - Position1).ToOrientationQuat();
		const FVector I = Q.GetRightVector();
		const FVector J = Q.GetUpVector();

		static const int32 NumSides = 12;
		static const float	AngleDelta = 2.0f * PI / NumSides;
		FVector	LastVertex1 = Position1 + I * Radius1;
		FVector	LastVertex2 = Position2 + I * Radius2;

		for (int32 SideIndex = 1; SideIndex <= NumSides; ++SideIndex)
		{
			const float Angle = AngleDelta * float(SideIndex);
			const FVector ArcPos = I * FMath::Cos(Angle) + J * FMath::Sin(Angle);
			const FVector Vertex1 = Position1 + ArcPos * Radius1;
			const FVector Vertex2 = Position2 + ArcPos * Radius2;

			PDI->DrawLine(LastVertex1, Vertex1, Color, SDPG_World, 0.0f, 0.001f, false);
			PDI->DrawLine(LastVertex2, Vertex2, Color, SDPG_World, 0.0f, 0.001f, false);
			PDI->DrawLine(LastVertex1, LastVertex2, Color, SDPG_World, 0.0f, 0.001f, false);

			LastVertex1 = Vertex1;
			LastVertex2 = Vertex2;
		}
	};

	auto DrawConvex = [&PDI](const Chaos::TConvex<float, 3>& Convex, const TRotation<float, 3>& Rotation, const Chaos::TVector<float, 3>& Position, const FLinearColor& Color)
	{
		const TArray<Chaos::TPlane<float, 3>>& Planes = Convex.GetFaces();
		for (int32 PlaneIndex1 = 0; PlaneIndex1 < Planes.Num(); ++PlaneIndex1)
		{
			const Chaos::TPlane<float, 3>& Plane1 = Planes[PlaneIndex1];

			for (int32 PlaneIndex2 = PlaneIndex1 + 1; PlaneIndex2 < Planes.Num(); ++PlaneIndex2)
			{
				const Chaos::TPlane<float, 3>& Plane2 = Planes[PlaneIndex2];

				// Find the two surface points that belong to both Plane1 and Plane2
				uint32 ParticleIndex1 = INDEX_NONE;

				const Chaos::TParticles<float, 3>& SurfaceParticles = Convex.GetSurfaceParticles();
				for (uint32 ParticleIndex = 0; ParticleIndex < SurfaceParticles.Size(); ++ParticleIndex)
				{
					const Chaos::TVector<float, 3>& X = SurfaceParticles.X(ParticleIndex);

					if (FMath::Square(Plane1.SignedDistance(X)) < KINDA_SMALL_NUMBER && 
						FMath::Square(Plane2.SignedDistance(X)) < KINDA_SMALL_NUMBER)
					{
						if (ParticleIndex1 != INDEX_NONE)
						{
							const Chaos::TVector<float, 3>& X1 = SurfaceParticles.X(ParticleIndex1);
							const FVector Position1 = Position + Rotation.RotateVector(X1);
							const FVector Position2 = Position + Rotation.RotateVector(X);
							PDI->DrawLine(Position1, Position2, Color, SDPG_World, 0.0f, 0.001f, false);
							break;
						}
						ParticleIndex1 = ParticleIndex;
					}
				}
			}
		}
	};

	static const FLinearColor MappedColor(FColor::Cyan);
	static const FLinearColor UnmappedColor(FColor::Red);

	const TGeometryClothParticles<float, 3>& CollisionParticles = Evolution->CollisionParticles();

	for (uint32 Index = 0; Index < CollisionParticles.Size(); ++Index)
	{
		if (const Chaos::FImplicitObject* const Object = CollisionParticles.DynamicGeometry(Index).Get())
		{
			const uint32 BoneIndex = BoneIndices[Index];
			const FLinearColor Color = (BoneIndex != INDEX_NONE) ? MappedColor : UnmappedColor;

			const Chaos::TVector<float, 3>& Position = CollisionParticles.X(Index);
			const TRotation<float, 3>& Rotation = CollisionParticles.R(Index);

			switch (Object->GetType())
			{
			case Chaos::ImplicitObjectType::Sphere:
				DrawSphere(Object->GetObjectChecked<Chaos::TSphere<float, 3>>(), Rotation, Position, Color);
				break;

			case Chaos::ImplicitObjectType::Box:
				DrawBox(Object->GetObjectChecked<Chaos::TBox<float, 3>>(), Rotation, Position, Color);
				break;

			case Chaos::ImplicitObjectType::Capsule:
				DrawCapsule(Object->GetObjectChecked<Chaos::TCapsule<float>>(), Rotation, Position, Color);
				break;

			case Chaos::ImplicitObjectType::Union:  // Union only used as collision tappered capsules
				for (const TUniquePtr<Chaos::FImplicitObject>& SubObjectPtr : Object->GetObjectChecked<Chaos::TImplicitObjectUnion<float, 3>>().GetObjects())
				{
					if (const Chaos::FImplicitObject* const SubObject = SubObjectPtr.Get())
					{
						switch (SubObject->GetType())
						{
						case Chaos::ImplicitObjectType::Sphere:
							DrawSphere(SubObject->GetObjectChecked<Chaos::TSphere<float, 3>>(), Rotation, Position, Color);
							break;

						case Chaos::ImplicitObjectType::TaperedCylinder:
							DrawTaperedCylinder(SubObject->GetObjectChecked<Chaos::TTaperedCylinder<float>>(), Rotation, Position, Color);
							break;

						default:
							break;
						}
					}
				}
				break;
	
			case Chaos::ImplicitObjectType::Convex:
				DrawConvex(Object->GetObjectChecked<Chaos::TConvex<float, 3>>(), Rotation, Position, Color);
				break;

			default:
				DrawCoordinateSystem(PDI, Position, FRotator(Rotation), 10.0f, SDPG_World, 0.1f);  // Draw everything else as a coordinate for now
				break;
			}
		}
	}
}

void ClothingSimulation::DebugDrawBackstops(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	for (int32 i = 0; i < IndexToRangeMap.Num(); ++i)
	{
		const UClothingAssetCommon* const Asset = Assets[i];
		if (Asset == nullptr)
		{
			continue;
		}

		// Get Backstop Distances
		const UClothLODDataBase* const AssetLodData = Asset->ClothLodData[0];
		check(AssetLodData);
		check(AssetLodData->PhysicalMeshData);
		const UClothPhysicalMeshDataBase* const PhysMesh = AssetLodData->PhysicalMeshData;
		const UEnum* const MeshTargets = PhysMesh->GetFloatArrayTargets();
		const uint32 PhysMeshBackstopIndex = MeshTargets->GetValueByName(TEXT("BackstopDistance"));
		if (PhysMesh->GetFloatArray(PhysMeshBackstopIndex)->Num() == 0)
		{
			continue;
		}

		const uint32 PhysMeshBackstopRadiusIndex = MeshTargets->GetValueByName(TEXT("BackstopRadius"));
		if (PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex)->Num() == 0)
		{
			continue;
		}

		for (uint32 ParticleIndex = IndexToRangeMap[i][0]; ParticleIndex < IndexToRangeMap[i][1]; ++ParticleIndex)
		{
			const float Radius = (*PhysMesh->GetFloatArray(PhysMeshBackstopRadiusIndex))[ParticleIndex - IndexToRangeMap[i][0]];
			const float Distance = (*PhysMesh->GetFloatArray(PhysMeshBackstopIndex))[ParticleIndex - IndexToRangeMap[i][0]];
			PDI->DrawLine(AnimationPositions[ParticleIndex], AnimationPositions[ParticleIndex] - AnimationNormals[ParticleIndex] * (Distance - Radius), FColor::White, SDPG_World, 0.0f, 0.001f);
			if (Radius > 0.0f)
			{
				const FVector& Normal = AnimationNormals[ParticleIndex];
				const FVector& Position = AnimationPositions[ParticleIndex];
				auto DrawBackstop = [Radius, Distance, &Normal, &Position, PDI](const FVector& Axis, const FColor& Color)
				{
					const float ArcLength = 5.0f; // Arch length in cm
					const float ArcAngle = ArcLength * 360.0f / (Radius * 2.0f * PI);
					
					const float MaxCosAngle = 0.99f;
					if (FMath::Abs(FVector::DotProduct(Normal, Axis)) < MaxCosAngle)
					{
						DrawArc(PDI, Position - Normal * Distance, Normal, FVector::CrossProduct(Axis, Normal).GetSafeNormal(), -ArcAngle / 2.0f, ArcAngle / 2.0f, Radius, 10, Color, SDPG_World);
					}
				};
				DrawBackstop(FVector::ForwardVector, FColor::Blue);
				DrawBackstop(FVector::UpVector, FColor::Blue);
				DrawBackstop(FVector::RightVector, FColor::Blue);
			}
		}
	}
}

void ClothingSimulation::DebugDrawMaxDistances(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	for (int32 i = 0; i < IndexToRangeMap.Num(); ++i)
	{
		const UClothingAssetCommon* const Asset = Assets[i];
		if (Asset == nullptr)
		{
			continue;
		}

		// Get Maximum Distances
		const UClothLODDataBase* const AssetLodData = Asset->ClothLodData[0];
		check(AssetLodData);
		check(AssetLodData->PhysicalMeshData);
		UClothPhysicalMeshDataBase* PhysMesh = AssetLodData->PhysicalMeshData;
		const UEnum* const MeshTargets = PhysMesh->GetFloatArrayTargets();
		const uint32 PhysMeshMaxDistanceIndex = MeshTargets->GetValueByName(TEXT("MaxDistance"));
		if (PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex)->Num() == 0)
		{
			continue;
		}
		
		for (uint32 ParticleIndex = IndexToRangeMap[i][0]; ParticleIndex < IndexToRangeMap[i][1]; ++ParticleIndex)
		{
			const float Distance = (*PhysMesh->GetFloatArray(PhysMeshMaxDistanceIndex))[ParticleIndex - IndexToRangeMap[i][0]];
			if (Distance == 0.0f)
			{
				DrawSphere(PDI, AnimationPositions[ParticleIndex], FRotator::ZeroRotator, FVector(0.5f, 0.5f, 0.5f), 10, 10, DebugClothMaterialVertex->GetRenderProxy(), SDPG_World, false);
			}
			else
			{
				PDI->DrawLine(AnimationPositions[ParticleIndex], AnimationPositions[ParticleIndex] + AnimationNormals[ParticleIndex] * Distance, FColor::White, SDPG_World, 0.0f, 0.001f);
			}
		}
	}
}

void ClothingSimulation::DebugDrawAnimDrive(USkeletalMeshComponent* OwnerComponent, FPrimitiveDrawInterface* PDI) const
{
	const TPBDParticles<float, 3>& Particles = Evolution->Particles();
	for (int32 i = 0; i < IndexToRangeMap.Num(); ++i)
	{
		const UClothingAssetCommon* const Asset = Assets[i];
		if (Asset == nullptr)
		{
			continue;
		}

		// Get Animdrive Multiplier
		const UClothLODDataBase* const AssetLodData = Asset->ClothLodData[0];
		check(AssetLodData);
		check(AssetLodData->PhysicalMeshData);
		const UClothPhysicalMeshDataBase* const PhysMesh = AssetLodData->PhysicalMeshData;
		const UEnum* const MeshTargets = PhysMesh->GetFloatArrayTargets();
		const uint32 PhysMeshAnimDriveIndex = MeshTargets->GetValueByName(TEXT("AnimDriveMultiplier"));
		if (PhysMesh->GetFloatArray(PhysMeshAnimDriveIndex)->Num() == 0)
		{
			continue;
		}

		for (uint32 ParticleIndex = IndexToRangeMap[i][0]; ParticleIndex < IndexToRangeMap[i][1]; ++ParticleIndex)
		{
			const float Multiplier = (*PhysMesh->GetFloatArray(PhysMeshAnimDriveIndex))[ParticleIndex - IndexToRangeMap[i][0]];
			PDI->DrawLine(AnimationPositions[ParticleIndex], Particles.X(ParticleIndex), Multiplier * AnimDriveSpringStiffness[i] * FColor::Cyan, SDPG_World, 0.0f, 0.001f);
		}
	}
}
#endif  // #if WITH_EDITOR
