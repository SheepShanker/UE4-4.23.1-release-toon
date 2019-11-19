// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Physics/ImmediatePhysics/ImmediatePhysicsChaos/ImmediatePhysicsSimulation_Chaos.h"
#include "Physics/ImmediatePhysics/ImmediatePhysicsChaos/ImmediatePhysicsActorHandle_Chaos.h"
#include "Physics/ImmediatePhysics/ImmediatePhysicsChaos/ImmediatePhysicsJointHandle_Chaos.h"
#include "Physics/ImmediatePhysics/ImmediatePhysicsStats.h"

#include "Chaos/ChaosDebugDraw.h"
#include "Chaos/DebugDrawQueue.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDCollisionConstraint.h"
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/PBDRigidParticles.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "ChaosLog.h"

#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"

//////////////////////////////////////////////////////////////////////////
// @todo(ccaulfield): remove when finished
//
float ChaosImmediate_Evolution_DeltaTime = 0.03f;
int32 ChaosImmediate_Evolution_Iterations = 2;
int32 ChaosImmediate_Evolution_PushOutIterations = 10;
FAutoConsoleVariableRef CVarChaosImmPhysDeltaTime(TEXT("p.Chaos.ImmPhys.DeltaTime"), ChaosImmediate_Evolution_DeltaTime, TEXT("Override chaos immediate physics delta time if non-zero"));
FAutoConsoleVariableRef CVarChaosImmPhysIterations(TEXT("p.Chaos.ImmPhys.Iterations"), ChaosImmediate_Evolution_Iterations, TEXT("Number of constraint solver loops in immediate physics"));
FAutoConsoleVariableRef CVarChaosImmPhysPushOutIterations(TEXT("p.Chaos.ImmPhys.PushOutIterations"), ChaosImmediate_Evolution_PushOutIterations, TEXT("Set the ApplyPushOut() (position correction) iteration count"));

int32 ChaosImmediate_Collision_Enabled = 1;
int32 ChaosImmediate_Collision_PairIterations = 0;
int32 ChaosImmediate_Collision_PushOutPairIterations = 2;
int32 ChaosImmediate_Collision_Priority = 1;
float ChaosImmediate_Collision_Thickness = 0;
FAutoConsoleVariableRef CVarChaosImmPhysDisableCollisions(TEXT("p.Chaos.ImmPhys.Collision.Enabled"), ChaosImmediate_Collision_Enabled, TEXT("Enable/Disable collisions in Immediate Physics."));
FAutoConsoleVariableRef CVarChaosImmPhysCollisionPairIterations(TEXT("p.Chaos.ImmPhys.Collision.PairIterations"), ChaosImmediate_Collision_PairIterations, TEXT("Set the Apply() (velocity correction) internal pair iteration count (0 to disable)"));
FAutoConsoleVariableRef CVarChaosImmPhysCollisionPushOutPairIterations(TEXT("p.Chaos.ImmPhys.Collision.PushOutPairIterations"), ChaosImmediate_Collision_PushOutPairIterations, TEXT("Set the ApplyPushOut() (position correction) internal pair iteration count (0 to disable)"));
FAutoConsoleVariableRef CVarChaosImmPhysCollisionPriority(TEXT("p.Chaos.ImmPhys.Collision.Priority"), ChaosImmediate_Collision_Priority, TEXT("Set the Collision constraint sort order (Joints have priority 0)"));
FAutoConsoleVariableRef CVarChaosImmPhysThickness(TEXT("p.Chaos.ImmPhys.Collision.Thickness"), ChaosImmediate_Collision_Thickness, TEXT("ChaosImmediateThickness"));

int32 ChaosImmediate_Joint_PairIterations = 0;
int32 ChaosImmediate_Joint_PushOutPairIterations = 2;
float ChaosImmediate_Joint_SwingTwistAngleTolerance = 1.0e-6f;
int32 ChaosImmediate_Joint_EnableVelocitySolve = 0;
int32 ChaosImmediate_Joint_EnableTwistLimits = 1;
int32 ChaosImmediate_Joint_EnableSwingLimits = 1;
int32 ChaosImmediate_Joint_EnableDrives = 1;
int32 ChaosImmediate_Joint_ProjectionPhase = (int32)Chaos::EJointProjectionPhase::ApplyPushOut;
float ChaosImmediate_Joint_LinearProjection = 0.0f;
float ChaosImmediate_Joint_AngularProjection = 0.0f;
float ChaosImmediate_Joint_Stiffness = 1.0f;
float ChaosImmediate_Joint_SoftLinearStiffness = 0.0f;
float ChaosImmediate_Joint_SoftAngularStiffness = 0.0f;
float ChaosImmediate_Joint_DriveStiffness = 0.0f;
float ChaosImmediate_Joint_MinParentMassRatio = 0.5f;
float ChaosImmediate_Joint_MaxInertiaRatio = 5.0f;
FAutoConsoleVariableRef CVarChaosImmPhysPairIterations(TEXT("p.Chaos.ImmPhys.Joint.PairIterations"), ChaosImmediate_Joint_PairIterations, TEXT("PairIterations."));
FAutoConsoleVariableRef CVarChaosImmPhysPushOutPairIterations(TEXT("p.Chaos.ImmPhys.Joint.PushOutPairIterations"), ChaosImmediate_Joint_PushOutPairIterations, TEXT("PushOutPairIterations."));
FAutoConsoleVariableRef CVarChaosImmPhysSwingTwistAngleTolerance(TEXT("p.Chaos.ImmPhys.Joint.SwingTwistAngleTolerance"), ChaosImmediate_Joint_SwingTwistAngleTolerance, TEXT("SwingTwistAngleTolerance."));
FAutoConsoleVariableRef CVarChaosImmPhysEnableVelocitySolve(TEXT("p.Chaos.ImmPhys.Joint.EnableVelocitySolve"), ChaosImmediate_Joint_EnableVelocitySolve, TEXT("EnableVelocitySolve."));
FAutoConsoleVariableRef CVarChaosImmPhysEnableTwistLimits(TEXT("p.Chaos.ImmPhys.Joint.EnableTwistLimits"), ChaosImmediate_Joint_EnableTwistLimits, TEXT("EnableTwistLimits."));
FAutoConsoleVariableRef CVarChaosImmPhysEnableSwingLimits(TEXT("p.Chaos.ImmPhys.Joint.EnableSwingLimits"), ChaosImmediate_Joint_EnableSwingLimits, TEXT("EnableSwingLimits."));
FAutoConsoleVariableRef CVarChaosImmPhysEnableDrives(TEXT("p.Chaos.ImmPhys.Joint.EnableDrives"), ChaosImmediate_Joint_EnableDrives, TEXT("EnableDrives."));
FAutoConsoleVariableRef CVarChaosImmPhysProjectionPhase(TEXT("p.Chaos.ImmPhys.Joint.ProjectionPhase"), ChaosImmediate_Joint_ProjectionPhase, TEXT("Projection Phase (0=Never; 1=Apply; 2=ApplyPushOut)."));
FAutoConsoleVariableRef CVarChaosImmPhysLinearProjection(TEXT("p.Chaos.ImmPhys.Joint.LinearProjection"), ChaosImmediate_Joint_LinearProjection, TEXT("6Dof joint projection amount override (if > 0)."));
FAutoConsoleVariableRef CVarChaosImmPhysAngularProjection(TEXT("p.Chaos.ImmPhys.Joint.AngularProjection"), ChaosImmediate_Joint_AngularProjection, TEXT("6Dof joint projection amount override (if > 0)."));
FAutoConsoleVariableRef CVarChaosImmPhysStiffness(TEXT("p.Chaos.ImmPhys.Joint.Stiffness"), ChaosImmediate_Joint_Stiffness, TEXT("6Dof joint stiffness override (if > 0)."));
FAutoConsoleVariableRef CVarChaosImmPhysSoftLinearStiffness(TEXT("p.Chaos.ImmPhys.Joint.SoftLinearStiffness"), ChaosImmediate_Joint_SoftLinearStiffness, TEXT("6Dof joint soft linear stiffness override (if > 0)."));
FAutoConsoleVariableRef CVarChaosImmPhysSoftAngularStiffness(TEXT("p.Chaos.ImmPhys.Joint.SoftAngularStiffness"), ChaosImmediate_Joint_SoftAngularStiffness, TEXT("6Dof joint soft angular stiffness override (if > 0)."));
FAutoConsoleVariableRef CVarChaosImmPhysDriveStiffness(TEXT("p.Chaos.ImmPhys.Joint.DriveStiffness"), ChaosImmediate_Joint_DriveStiffness, TEXT("6Dof joint drive stiffness override (if > 0)."));
FAutoConsoleVariableRef CVarChaosImmPhysMinParentMassRatio(TEXT("p.Chaos.ImmPhys.Joint.MinParentMassRatio"), ChaosImmediate_Joint_MinParentMassRatio, TEXT("6Dof joint MinParentMassRatio (if > 0)"));
FAutoConsoleVariableRef CVarChaosImmPhysMaxInertiaRatio(TEXT("p.Chaos.ImmPhys.Joint.MaxInertiaRatio"), ChaosImmediate_Joint_MaxInertiaRatio, TEXT("6Dof joint MaxInertiaRatio (if > 0)"));
//
// end remove when finished
//
//////////////////////////////////////////////////////////////////////////


// DebugDraw CVars
#if UE_BUILD_DEBUG
int32 ChaosImmediate_DebugDrawParticles = 0;
int32 ChaosImmediate_DebugDrawShapes = 1;
int32 ChaosImmediate_DebugDrawCollisions = 0;
int32 ChaosImmediate_DebugDrawJoints = 1;
#else
int32 ChaosImmediate_DebugDrawParticles = 0;
int32 ChaosImmediate_DebugDrawShapes = 0;
int32 ChaosImmediate_DebugDrawCollisions = 0;
int32 ChaosImmediate_DebugDrawJoints = 0;
#endif

#if CHAOS_DEBUG_DRAW
int32 ChaosImmediate_DebugDrawJointFeatures = (int32)Chaos::DebugDraw::EDebugDrawJointFeature::Default;
#else
int32 ChaosImmediate_DebugDrawJointFeatures = 0;
#endif

FAutoConsoleVariableRef CVarChaosImmPhysDebugDrawParticles(TEXT("p.Chaos.ImmPhys.DebugDrawParticles"), ChaosImmediate_DebugDrawParticles, TEXT("Draw Particle Transforms (0 = never; 1 = end of frame; 2 = begin and end of frame; 3 = post-integate, post-apply and post-applypushout;)."));
FAutoConsoleVariableRef CVarChaosImmPhysDebugDrawShapes(TEXT("p.Chaos.ImmPhys.DebugDrawShapes"), ChaosImmediate_DebugDrawShapes, TEXT("Draw Shapes (0 = never; 1 = end of frame; 2 = begin and end of frame; 3 = post-integate, post-apply and post-applypushout;"));
FAutoConsoleVariableRef CVarChaosImmPhysDebugDrawCollisions(TEXT("p.Chaos.ImmPhys.DebugDrawCollisions"), ChaosImmediate_DebugDrawCollisions, TEXT("Draw Collisions (0 = never; 1 = end of frame; 2 = begin and end of frame; 3 = post-integate, post-apply and post-applypushout;)"));
FAutoConsoleVariableRef CVarChaosImmPhysDebugDrawJoints(TEXT("p.Chaos.ImmPhys.DebugDrawJoints"), ChaosImmediate_DebugDrawJoints, TEXT("Draw Joints. (0 = never; 1 = end of frame; 2 = begin and end of frame; 3 = post-integate, post-apply and post-applypushout; 4 = each Apply step)."));
FAutoConsoleVariableRef CVarChaosImmPhysDebugDrawJointFeatures(TEXT("p.Chaos.ImmPhys.DebugDrawJointFeatures"), ChaosImmediate_DebugDrawJointFeatures, TEXT("Joint features mask (see EDebugDrawJointFeature)."));

namespace ImmediatePhysics_Chaos
{
	Chaos::EJointProjectionPhase ToProjectionPhase(const int32 Index)
	{
		using namespace Chaos;

		if (Index == 1)
		{
			return EJointProjectionPhase::Apply;
		}
		else if (Index == 2)
		{
			return EJointProjectionPhase::ApplyPushOut;
		}
		return EJointProjectionPhase::None;
	}

	template<typename T, int d>
	bool ShouldIgnoreCollisionConstraint(
		const Chaos::TGeometryParticleHandle<T, d>* ParticleA,
		const Chaos::TGeometryParticleHandle<T, d>* ParticleB,
		const TMap<const Chaos::TGeometryParticleHandle<T, d>*, TSet<const Chaos::TGeometryParticleHandle<T, d>*>>& IgnoreSetMap)
	{
		using namespace Chaos;

		if (!ChaosImmediate_Collision_Enabled)
		{
			return true;
		}
		if (const TSet<const TGeometryParticleHandle<T, d>*>* IgnoreSet = IgnoreSetMap.Find(ParticleA))
		{
			return IgnoreSet->Contains(ParticleB);
		}
		if (const TSet<const TGeometryParticleHandle<T, d>*>* IgnoreSet = IgnoreSetMap.Find(ParticleB))
		{
			return IgnoreSet->Contains(ParticleA);
		}
		return false;
	}

	//
	//
	//

	FSimulation::FSimulation()
		: NumActiveActorHandles(0)
		, SimulationSpaceTransform(FTransform::Identity)
	{
		using namespace Chaos;

		Particles = MakeUnique<TPBDRigidsSOAs<FReal, Dimensions>>();
		Joints = MakeUnique<FPBDJointConstraints>(FPBDJointSolverSettings());
		JointsRule = MakeUnique<TPBDConstraintIslandRule<FPBDJointConstraints>>(*Joints);
		Evolution = MakeUnique<TPBDRigidsEvolutionGBF<FReal, Dimensions>>(*Particles.Get(), 1);
		TPBDCollisionConstraint<FReal, Dimensions>& Collisions = Evolution->GetCollisionConstraints();

		Evolution->AddConstraintRule(JointsRule.Get());

		// We want collisions to override joints
		JointsRule->SetPriority(0);
		Evolution->GetCollisionConstraintsRule().SetPriority(1);

		// Filter collisions after detection
		// @todo(ccaulfield): Eventually we will build lists of potentially colliding pairs and won't need this
		Collisions.SetPostComputeCallback(
			[this]()
			{
				Evolution->GetCollisionConstraints().ApplyCollisionModifier(
					[this](const TPBDCollisionConstraint<FReal, Dimensions>::FConstraintContainerHandle* Handle)
					{
						TVector<const TGeometryParticleHandle<FReal, Dimensions>*, 2> ConstrainedParticles = Handle->GetConstrainedParticles();
						if (ShouldIgnoreCollisionConstraint(ConstrainedParticles[0], ConstrainedParticles[1], IgnoreCollisionParticlePairTable))
						{
							return ECollisionModifierResult::Disabled;
						}
						return ECollisionModifierResult::Unchanged;
					});
			});

#if CHAOS_DEBUG_DRAW
		Evolution->SetPostIntegrateCallback(
			[this]()
			{
				// Dynamic only - Kinematics get drawn once at the end of frame
				DebugDrawParticles(3, 3, 0.3f, false, true);
				DebugDrawConstraints(3, 3, 0.3f);
			});
		Evolution->SetPostApplyCallback(
			[this](int32 Island)
			{
				// Dynamic only - Kinematics get drawn once at the end of frame
				DebugDrawIslandParticles(Island, 3, 3, 0.6f, false, true);
				DebugDrawIslandConstraints(Island, 3, 3, 0.6f);
			});
		Evolution->SetPostApplyPushOutCallback(
			[this](int32 Island)
			{
				// Dynamic only - Kinematics get drawn once at the end of frame
				DebugDrawIslandParticles(Island, 3, 3, 1.0f, false, true);
				DebugDrawIslandConstraints(Island, 3, 3, 1.0f);
			});
		Collisions.SetPostApplyCallback(
			[this](const float Dt, const TArray<TPBDCollisionConstraintHandle<float, 3>*>& InConstraintHandles)
			{
				if (ChaosImmediate_DebugDrawCollisions == 4)
				{
					DebugDraw::DrawCollisions(SimulationSpaceTransform, InConstraintHandles, 0.3f);
				}
			});
		Collisions.SetPostApplyPushOutCallback(
			[this](const float Dt, const TArray<TPBDCollisionConstraintHandle<float, 3>*>& InConstraintHandles, bool bRequiresAnotherIteration)
			{
				if (ChaosImmediate_DebugDrawCollisions == 4)
				{
					DebugDraw::DrawCollisions(SimulationSpaceTransform, InConstraintHandles, 0.6f);
				}
			});
		Joints->SetPreApplyCallback(
			[this](const float Dt, const TArray<FPBDJointConstraintHandle*>& InConstraintHandles)
			{
				if (ChaosImmediate_DebugDrawJoints == 4)
				{
					DebugDraw::DrawJointConstraints(SimulationSpaceTransform, InConstraintHandles, 0.3f);
				}
			});
		Joints->SetPostApplyCallback(
			[this](const float Dt, const TArray<FPBDJointConstraintHandle*>& InConstraintHandles)
			{
				if (ChaosImmediate_DebugDrawJoints == 4)
				{
					DebugDraw::DrawJointConstraints(SimulationSpaceTransform, InConstraintHandles, 0.6f);
				}
			});
#endif
	}

	FSimulation::~FSimulation()
	{
		using namespace Chaos;

		Evolution->GetCollisionConstraints().ClearPostComputeCallback();
		Evolution->GetCollisionConstraints().ClearPostApplyCallback();
		Evolution->GetCollisionConstraints().ClearPostApplyPushOutCallback();

		//Evolution->RemoveConstraintRule(JointsRule.Get());

		for (FActorHandle* ActorHandle : ActorHandles)
		{
			delete ActorHandle;
		}
		ActorHandles.Empty();

		for (FJointHandle* JointHandle : JointHandles)
		{
			delete JointHandle;
		}
		JointHandles.Empty();

		Evolution.Reset();
		JointsRule.Reset();
		Joints.Reset();
		Particles.Reset();
	}

	FActorHandle* FSimulation::CreateStaticActor(FBodyInstance* BodyInstance)
	{
		return CreateActor(EActorType::StaticActor, BodyInstance, BodyInstance->GetUnrealWorldTransform());
	}

	FActorHandle* FSimulation::CreateKinematicActor(FBodyInstance* BodyInstance, const FTransform& Transform)
	{
		return CreateActor(EActorType::KinematicActor, BodyInstance, Transform);
	}

	FActorHandle* FSimulation::CreateDynamicActor(FBodyInstance* BodyInstance, const FTransform& Transform)
	{
		return CreateActor(EActorType::DynamicActor, BodyInstance, Transform);
	}

	FActorHandle* FSimulation::CreateActor(EActorType ActorType, FBodyInstance* BodyInstance, const FTransform& Transform)
	{
		FActorHandle* ActorHandle = new FActorHandle(Evolution.Get(), ActorType, BodyInstance, Transform);
		int ActorIndex = ActorHandles.Add(ActorHandle);
		return ActorHandle;
	}

	void FSimulation::DestroyActor(FActorHandle* ActorHandle)
	{
		// @todo(ccaulfield): FActorHandle could remember its index to optimize this
		ActorHandles.Remove(ActorHandle);
		delete ActorHandle;
	}

	FJointHandle* FSimulation::CreateJoint(FConstraintInstance* ConstraintInstance, FActorHandle* Body1, FActorHandle* Body2)
	{
		FJointHandle* JointHandle = new FJointHandle(Joints.Get(), ConstraintInstance, Body1, Body2);
		JointHandles.Add(JointHandle);
		return JointHandle;
	}

	void FSimulation::DestroyJoint(FJointHandle* JointHandle)
	{
		// @todo(ccaulfield): FJointHandle could remember its index to optimize this
		JointHandles.Remove(JointHandle);
		delete JointHandle;
	}

	void FSimulation::SetNumActiveBodies(int32 InNumActiveActorHandles)
	{
		if (InNumActiveActorHandles < NumActiveActorHandles)
		{
			for (int ActorHandleIndex = InNumActiveActorHandles; ActorHandleIndex < NumActiveActorHandles; ++ActorHandleIndex)
			{
				GetActorHandle(ActorHandleIndex)->SetEnabled(false);
			}
		}
		else if (InNumActiveActorHandles > NumActiveActorHandles)
		{
			check(InNumActiveActorHandles <= ActorHandles.Num());
			for (int ActorHandleIndex = NumActiveActorHandles; ActorHandleIndex < InNumActiveActorHandles; ++ActorHandleIndex)
			{
				GetActorHandle(ActorHandleIndex)->SetEnabled(true);
			}
		}
	
		NumActiveActorHandles = InNumActiveActorHandles;
	}

	void FSimulation::SetIgnoreCollisionPairTable(const TArray<FIgnorePair>& InIgnoreCollisionPairTable)
	{
		using namespace Chaos;

		IgnoreCollisionParticlePairTable.Reset();
		for (const FIgnorePair& IgnorePair : InIgnoreCollisionPairTable)
		{
			if(!IgnorePair.A || !IgnorePair.B)
			{
				continue;
			}

			const TGeometryParticleHandle<FReal, Dimensions>* ParticleA = IgnorePair.A->GetParticle();
			const TGeometryParticleHandle<FReal, Dimensions>* ParticleB = IgnorePair.B->GetParticle();

			IgnoreCollisionParticlePairTable.FindOrAdd(ParticleA).Add(ParticleB); 
			IgnoreCollisionParticlePairTable.FindOrAdd(ParticleB).Add(ParticleA);
		}
	}

	void FSimulation::SetIgnoreCollisionActors(const TArray<FActorHandle*>& InIgnoreCollisionActors)
	{
		using namespace Chaos;

		for (FActorHandle* ActorHandle : InIgnoreCollisionActors)
		{
			TPBDRigidParticleHandle<FReal, Dimensions>* Particle = ActorHandle->GetParticle()->AsDynamic();
			if (Particle != nullptr)
			{
				Particle->SetCollisionGroup(INDEX_NONE);
			}
		}
	}

	void FSimulation::ConditionConstraints()
	{
		// Assign levels to actors based on connection-distance to a non-dynamic actor
		// This is used to sort constraints and set parent/child relationship on the constrained particles in a constraint
		// @todo(ccaulfield): this should use the constraint graph and should only update when constraint connectivity changes or particles change type
		using namespace Chaos;

		TMap<FActorHandle*, TArray<FJointHandle*>> ActorJoints;
		TArray<FActorHandle*> ActorQueue;
		ActorQueue.Reserve(ActorHandles.Num());

		// Reset all actor levels
		for (FActorHandle* ActorHandle : ActorHandles)
		{
			ActorHandle->SetLevel(INDEX_NONE);
			ActorJoints.Emplace(ActorHandle);

			if (!ActorHandle->GetParticle()->AsDynamic())
			{
				ActorHandle->SetLevel(0);
				ActorQueue.Add(ActorHandle);
			}
		}

		// Build a list of joints per actor
		for (FJointHandle* JointHandle : JointHandles)
		{
			TVector<FActorHandle*, 2> JointActorHandles = JointHandle->GetActorHandles();
			ActorJoints[JointActorHandles[0]].Add(JointHandle);
			ActorJoints[JointActorHandles[1]].Add(JointHandle);
		}

		// Breadth-first assign level
		for (int32 ActorQueueIndex = 0; ActorQueueIndex < ActorQueue.Num(); ++ActorQueueIndex)
		{
			FActorHandle* ActorHandle = ActorQueue[ActorQueueIndex];
			for (FJointHandle* JointHandle : ActorJoints[ActorHandle])
			{
				TVector<FActorHandle*, 2> JointActorHandles = JointHandle->GetActorHandles();
				if (JointActorHandles[0]->GetLevel() == INDEX_NONE)
				{
					JointActorHandles[0]->SetLevel(ActorHandle->GetLevel() + 1);
					ActorQueue.Add(JointActorHandles[0]);
				}
				if (JointActorHandles[1]->GetLevel() == INDEX_NONE)
				{
					JointActorHandles[1]->SetLevel(ActorHandle->GetLevel() + 1);
					ActorQueue.Add(JointActorHandles[1]);
				}
			}
		}

		// Update constraint levels
		for (FJointHandle* JointHandle : JointHandles)
		{
			JointHandle->UpdateLevels();
		}
	}

	DECLARE_CYCLE_STAT(TEXT("FSimulation::Simulate_Chaos"), STAT_ImmediateSimulate_Chaos, STATGROUP_ImmediatePhysics);

	void FSimulation::Simulate(float DeltaTime, float MaxDeltaTime, int32 MaxSubSteps, const FVector& InGravity)
	{
		SCOPE_CYCLE_COUNTER(STAT_ImmediateSimulate_Chaos);
		using namespace Chaos;

		// TEMP: overrides
		{
			if (ChaosImmediate_Evolution_DeltaTime > 0)
			{
				// Round Dt to the nearest multiple of fixed step size...
				const float NumDts = FMath::RoundToFloat(DeltaTime / ChaosImmediate_Evolution_DeltaTime);
				DeltaTime = NumDts * ChaosImmediate_Evolution_DeltaTime;
				MaxDeltaTime = ChaosImmediate_Evolution_DeltaTime;
			}
			UE_LOG(LogChaosJoint, Verbose, TEXT("Simulate dt = %f"), DeltaTime);

			FPBDJointSolverSettings JointsSettings = Joints->GetSettings();
			JointsSettings.ApplyPairIterations = ChaosImmediate_Joint_PairIterations;
			JointsSettings.ApplyPushOutPairIterations = ChaosImmediate_Joint_PushOutPairIterations;
			JointsSettings.SwingTwistAngleTolerance = ChaosImmediate_Joint_SwingTwistAngleTolerance;
			JointsSettings.MinParentMassRatio = ChaosImmediate_Joint_MinParentMassRatio;
			JointsSettings.MaxInertiaRatio = ChaosImmediate_Joint_MaxInertiaRatio;
			JointsSettings.bEnableVelocitySolve = ChaosImmediate_Joint_EnableVelocitySolve != 0;
			JointsSettings.bEnableTwistLimits = ChaosImmediate_Joint_EnableTwistLimits != 0;
			JointsSettings.bEnableSwingLimits = ChaosImmediate_Joint_EnableSwingLimits != 0;
			JointsSettings.bEnableDrives = ChaosImmediate_Joint_EnableDrives != 0;
			JointsSettings.ProjectionPhase = ToProjectionPhase(ChaosImmediate_Joint_ProjectionPhase);
			JointsSettings.LinearProjection = ChaosImmediate_Joint_LinearProjection;
			JointsSettings.AngularProjection = ChaosImmediate_Joint_AngularProjection;
			JointsSettings.Stiffness = ChaosImmediate_Joint_Stiffness;
			JointsSettings.SoftLinearStiffness = ChaosImmediate_Joint_SoftLinearStiffness;
			JointsSettings.SoftAngularStiffness = ChaosImmediate_Joint_SoftAngularStiffness;
			JointsSettings.DriveStiffness = ChaosImmediate_Joint_DriveStiffness;
			Joints->SetSettings(JointsSettings);

			Evolution->SetNumIterations(ChaosImmediate_Evolution_Iterations);
			Evolution->SetNumPushOutIterations(ChaosImmediate_Evolution_PushOutIterations);

			Evolution->GetCollisionConstraints().SetThickness(ChaosImmediate_Collision_Thickness);
			Evolution->GetCollisionConstraints().SetPairIterations(ChaosImmediate_Collision_PairIterations);
			Evolution->GetCollisionConstraints().SetPushOutPairIterations(ChaosImmediate_Collision_PushOutPairIterations);
			Evolution->GetCollisionConstraintsRule().SetPriority(ChaosImmediate_Collision_Priority);
		
			// TEMP until we can remove constraints again, or I add broad-phase filtering. (FilterCollisionConstraints will crash since the persistent collision changes)
			Evolution->GetCollisionConstraints().SetCollisionsEnabled(ChaosImmediate_Collision_Enabled != 0);
		}

		DebugDrawParticles(2, 2, 0.7f, true, true);
		DebugDrawConstraints(2, 2, 0.7f);

		ConditionConstraints();

		Evolution->GetGravityForces().SetAcceleration(InGravity);
		
		Evolution->Advance(DeltaTime, MaxDeltaTime, MaxSubSteps);
		
		Evolution->EndFrame(DeltaTime);

		DebugDrawParticles(1, 2, 1.0f, true, true);
		DebugDrawConstraints(1, 2, 1.0f);
		DebugDrawParticles(3, 3, 1.0f, true, false);	// Kinematics only
	}

	void FSimulation::DebugDrawParticles(const int32 MinDebugLevel, const int32 MaxDebugLevel, const float ColorScale, bool bDrawKinematic, bool bDrawDynamic)
	{
#if CHAOS_DEBUG_DRAW
		using namespace Chaos;
		if (FDebugDrawQueue::IsDebugDrawingEnabled())
		{
			if ((ChaosImmediate_DebugDrawParticles >= MinDebugLevel) && (ChaosImmediate_DebugDrawParticles <= MaxDebugLevel))
			{
				DebugDraw::DrawParticleTransforms(SimulationSpaceTransform, Evolution->GetParticles().GetAllParticlesView(), ColorScale, bDrawKinematic, bDrawDynamic);
			}
			if ((ChaosImmediate_DebugDrawShapes >= MinDebugLevel) && (ChaosImmediate_DebugDrawShapes <= MaxDebugLevel))
			{
				DebugDraw::DrawParticleShapes(SimulationSpaceTransform, Evolution->GetParticles().GetAllParticlesView(), ColorScale, bDrawKinematic, bDrawDynamic);
			}
		}
#endif
	}

	void FSimulation::DebugDrawConstraints(const int32 MinDebugLevel, const int32 MaxDebugLevel, const float ColorScale)
	{
#if CHAOS_DEBUG_DRAW
		using namespace Chaos;
		if (FDebugDrawQueue::IsDebugDrawingEnabled())
		{
			if ((ChaosImmediate_DebugDrawCollisions >= MinDebugLevel) && (ChaosImmediate_DebugDrawCollisions <= MaxDebugLevel))
			{
				DebugDraw::DrawCollisions(SimulationSpaceTransform, Evolution->GetCollisionConstraints(), ColorScale);
			}
			if ((ChaosImmediate_DebugDrawJoints >= MinDebugLevel) && (ChaosImmediate_DebugDrawJoints <= MaxDebugLevel))
			{
				DebugDraw::DrawJointConstraints(SimulationSpaceTransform, *Joints, ColorScale, (uint32)ChaosImmediate_DebugDrawJointFeatures);
			}
		}
#endif
	}

	void FSimulation::DebugDrawIslandParticles(const int32 Island, const int32 MinDebugLevel, const int32 MaxDebugLevel, const float ColorScale, bool bDrawKinematic, bool bDrawDynamic)
	{
#if CHAOS_DEBUG_DRAW
		using namespace Chaos;
		if (FDebugDrawQueue::IsDebugDrawingEnabled())
		{
			if ((ChaosImmediate_DebugDrawParticles >= MinDebugLevel) && (ChaosImmediate_DebugDrawParticles <= MaxDebugLevel))
			{
				DebugDraw::DrawParticleTransforms(SimulationSpaceTransform, Evolution->GetIslandParticles(Island), ColorScale, bDrawKinematic, bDrawDynamic);
			}
			if ((ChaosImmediate_DebugDrawShapes >= MinDebugLevel) && (ChaosImmediate_DebugDrawShapes <= MaxDebugLevel))
			{
				DebugDraw::DrawParticleShapes(SimulationSpaceTransform, Evolution->GetIslandParticles(Island), ColorScale, bDrawKinematic, bDrawDynamic);
			}
		}
#endif
	}

	void FSimulation::DebugDrawIslandConstraints(const int32 Island, const int32 MinDebugLevel, const int32 MaxDebugLevel, const float ColorScale)
	{
#if CHAOS_DEBUG_DRAW
		using namespace Chaos;
		if (FDebugDrawQueue::IsDebugDrawingEnabled())
		{
			if ((ChaosImmediate_DebugDrawCollisions >= MinDebugLevel) && (ChaosImmediate_DebugDrawCollisions <= MaxDebugLevel))
			{
				Evolution->GetCollisionConstraintsRule().VisitIslandConstraints(Island,
					[this, ColorScale](const TArray<TPBDCollisionConstraintHandle<float, 3>*>& ConstraintHandles)
					{
						DebugDraw::DrawCollisions(SimulationSpaceTransform, ConstraintHandles, ColorScale);
					});
			}
			if ((ChaosImmediate_DebugDrawJoints >= MinDebugLevel) && (ChaosImmediate_DebugDrawJoints <= MaxDebugLevel))
			{
				JointsRule->VisitIslandConstraints(Island,
					[this, ColorScale](const TArray<FPBDJointConstraintHandle*>& ConstraintHandles)
					{
						DebugDraw::DrawJointConstraints(SimulationSpaceTransform, *Joints, ColorScale, (uint32)ChaosImmediate_DebugDrawJointFeatures);
					});
			}
		}
#endif
	}
}
