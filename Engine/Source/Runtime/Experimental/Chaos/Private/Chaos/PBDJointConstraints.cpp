// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#include "Chaos/PBDJointConstraints.h"
#include "Chaos/ChaosDebugDraw.h"
#include "Chaos/DebugDrawQueue.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDJointConstraintUtilities.h"
#include "Chaos/Utilities.h"
#include "ChaosLog.h"
#include "ChaosStats.h"

#include "HAL/IConsoleManager.h"

//#pragma optimize("", off)

namespace Chaos
{
	DECLARE_CYCLE_STAT(TEXT("TPBDJointConstraints::Apply"), STAT_ApplyJointConstraints, STATGROUP_Chaos);
	DECLARE_CYCLE_STAT(TEXT("TPBDJointConstraints::Apply"), STAT_ApplyPushOutJointConstraints, STATGROUP_Chaos);

	FReal GetLinearStiffness(
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const FReal SolverStiffness = (SolverSettings.Stiffness > (FReal)0) ? SolverSettings.Stiffness : JointSettings.Motion.Stiffness;
		const FReal SoftSolverStiffness = (SolverSettings.SoftLinearStiffness > (FReal)0) ? SolverSettings.SoftLinearStiffness : JointSettings.Motion.SoftLinearStiffness;
		const bool bIsSoft = JointSettings.Motion.bSoftLinearLimitsEnabled && ((JointSettings.Motion.LinearMotionTypes[0] == EJointMotionType::Limited) || (JointSettings.Motion.LinearMotionTypes[1] == EJointMotionType::Limited) || (JointSettings.Motion.LinearMotionTypes[2] == EJointMotionType::Limited));
		const FReal Stiffness = bIsSoft ? SolverStiffness * SoftSolverStiffness : SolverStiffness;
		return Stiffness;
	}


	FReal GetTwistStiffness(
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const FReal SolverStiffness = (SolverSettings.Stiffness > (FReal)0) ? SolverSettings.Stiffness : JointSettings.Motion.Stiffness;
		const FReal SoftSolverStiffness = (SolverSettings.SoftAngularStiffness > (FReal)0) ? SolverSettings.SoftAngularStiffness : JointSettings.Motion.SoftTwistStiffness;
		const bool bIsSoft = JointSettings.Motion.bSoftTwistLimitsEnabled && (JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist] == EJointMotionType::Limited);
		const FReal Stiffness = bIsSoft ? SolverStiffness * SoftSolverStiffness : SolverStiffness;
		return Stiffness;
	}


	FReal GetSwingStiffness(
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
	{
		const FReal SolverStiffness = (SolverSettings.Stiffness > (FReal)0) ? SolverSettings.Stiffness : JointSettings.Motion.Stiffness;
		const FReal SoftSolverStiffness = (SolverSettings.SoftAngularStiffness > (FReal)0) ? SolverSettings.SoftAngularStiffness : JointSettings.Motion.SoftSwingStiffness;
		const bool bIsSoft = JointSettings.Motion.bSoftSwingLimitsEnabled && ((JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1] == EJointMotionType::Limited) || (JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2] == EJointMotionType::Limited));
		const FReal Stiffness = bIsSoft ? SolverStiffness * SoftSolverStiffness : SolverStiffness;
		return Stiffness;
	}

	//
	// Constraint Handle
	//

	
	FPBDJointConstraintHandle::FPBDJointConstraintHandle()
	{
	}

	
	FPBDJointConstraintHandle::FPBDJointConstraintHandle(FConstraintContainer* InConstraintContainer, int32 InConstraintIndex)
		: TContainerConstraintHandle<FPBDJointConstraints>(InConstraintContainer, InConstraintIndex)
	{
	}

	
	void FPBDJointConstraintHandle::CalculateConstraintSpace(FVec3& OutXa, FMatrix33& OutRa, FVec3& OutXb, FMatrix33& OutRb, FVec3& OutCR) const
	{
		ConstraintContainer->CalculateConstraintSpace(ConstraintIndex, OutXa, OutRa, OutXb, OutRb, OutCR);
	}

	
	void FPBDJointConstraintHandle::SetParticleLevels(const TVector<int32, 2>& ParticleLevels)
	{
		ConstraintContainer->SetParticleLevels(ConstraintIndex, ParticleLevels);
	}

	
	int32 FPBDJointConstraintHandle::GetConstraintLevel() const
	{
		return ConstraintContainer->GetConstraintLevel(ConstraintIndex);
	}

	
	const FPBDJointSettings& FPBDJointConstraintHandle::GetSettings() const
	{
		return ConstraintContainer->GetConstraintSettings(ConstraintIndex);
	}

	TVector<TGeometryParticleHandle<float,3>*, 2> FPBDJointConstraintHandle::GetConstrainedParticles() const 
	{ 
		return ConstraintContainer->GetConstrainedParticles(ConstraintIndex); 
	}

	//
	// Constraint Settings
	//

	
	FPBDJointMotionSettings::FPBDJointMotionSettings()
		: Stiffness((FReal)1)
		, LinearProjection((FReal)0)
		, AngularProjection((FReal)0)
		, LinearMotionTypes({ EJointMotionType::Locked, EJointMotionType::Locked, EJointMotionType::Locked })
		, LinearLimit(FLT_MAX)
		, AngularMotionTypes({ EJointMotionType::Free, EJointMotionType::Free, EJointMotionType::Free })
		, AngularLimits(FVec3(FLT_MAX, FLT_MAX, FLT_MAX))
		, bSoftLinearLimitsEnabled(false)
		, bSoftTwistLimitsEnabled(false)
		, bSoftSwingLimitsEnabled(false)
		, SoftLinearStiffness(0)
		, SoftTwistStiffness(0)
		, SoftSwingStiffness(0)
		, AngularDriveTarget(FRotation3::FromIdentity())
		, AngularDriveTargetAngles(FVec3(0, 0, 0))
		, bAngularSLerpDriveEnabled(false)
		, bAngularTwistDriveEnabled(false)
		, bAngularSwingDriveEnabled(false)
		, AngularDriveStiffness(0)
	{
	}

	
	FPBDJointMotionSettings::FPBDJointMotionSettings(const TVector<EJointMotionType, 3>& InLinearMotionTypes, const TVector<EJointMotionType, 3>& InAngularMotionTypes)
		: Stiffness((FReal)1)
		, LinearProjection((FReal)0)
		, AngularProjection((FReal)0)
		, LinearMotionTypes(InLinearMotionTypes)
		, LinearLimit(FLT_MAX)
		, AngularMotionTypes({ EJointMotionType::Free, EJointMotionType::Free, EJointMotionType::Free })
		, AngularLimits(FVec3(FLT_MAX, FLT_MAX, FLT_MAX))
		, bSoftLinearLimitsEnabled(false)
		, bSoftTwistLimitsEnabled(false)
		, bSoftSwingLimitsEnabled(false)
		, SoftLinearStiffness(0)
		, SoftTwistStiffness(0)
		, SoftSwingStiffness(0)
		, AngularDriveTarget(FRotation3::FromIdentity())
		, AngularDriveTargetAngles(FVec3(0, 0, 0))
		, bAngularSLerpDriveEnabled(false)
		, bAngularTwistDriveEnabled(false)
		, bAngularSwingDriveEnabled(false)
		, AngularDriveStiffness(0)
	{
	}

	
	FPBDJointSettings::FPBDJointSettings()
		: ConstraintFrames({ FTransform::Identity, FTransform::Identity })
	{
	}

	
	FPBDJointState::FPBDJointState()
		: Level(INDEX_NONE)
		, ParticleLevels({ INDEX_NONE, INDEX_NONE })
	{
	}

	//
	// Solver Settings
	//

	
	FPBDJointSolverSettings::FPBDJointSolverSettings()
		: ApplyPairIterations(1)
		, ApplyPushOutPairIterations(1)
		, SwingTwistAngleTolerance((FReal)1.0e-6)
		, MinParentMassRatio(0)
		, MaxInertiaRatio(0)
		, bEnableVelocitySolve(false)
		, bEnableTwistLimits(true)
		, bEnableSwingLimits(true)
		, bEnableDrives(true)
		, ProjectionPhase(EJointProjectionPhase::None)
		, LinearProjection((FReal)0)
		, AngularProjection((FReal)0)
		, Stiffness((FReal)0)
		, DriveStiffness((FReal)0)
		, SoftLinearStiffness((FReal)0)
		, SoftAngularStiffness((FReal)0)
	{
	}

	//
	// Constraint Container
	//

	
	FPBDJointConstraints::FPBDJointConstraints(const FPBDJointSolverSettings& InSettings)
		: Settings(InSettings)
		, PreApplyCallback(nullptr)
		, PostApplyCallback(nullptr)
	{
	}

	
	FPBDJointConstraints::~FPBDJointConstraints()
	{
	}

	
	const FPBDJointSolverSettings& FPBDJointConstraints::GetSettings() const
	{
		return Settings;
	}

	
	void FPBDJointConstraints::SetSettings(const FPBDJointSolverSettings& InSettings)
	{
		Settings = InSettings;
	}

	
	int32 FPBDJointConstraints::NumConstraints() const
	{
		return ConstraintParticles.Num();
	}

	
	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::AddConstraint(const FParticlePair& InConstrainedParticles, const FRigidTransform3& WorldConstraintFrame)
	{
		FTransformPair ConstraintFrames;
		ConstraintFrames[0] = FRigidTransform3(
			WorldConstraintFrame.GetTranslation() - InConstrainedParticles[0]->X(),
			WorldConstraintFrame.GetRotation() * InConstrainedParticles[0]->R().Inverse()
			);
		ConstraintFrames[1] = FRigidTransform3(
			WorldConstraintFrame.GetTranslation() - InConstrainedParticles[1]->X(),
			WorldConstraintFrame.GetRotation() * InConstrainedParticles[1]->R().Inverse()
			);
		return AddConstraint(InConstrainedParticles, ConstraintFrames);
	}

	
	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::AddConstraint(const FParticlePair& InConstrainedParticles, const FTransformPair& ConstraintFrames)
	{
		int ConstraintIndex = Handles.Num();
		Handles.Add(HandleAllocator.AllocHandle(this, ConstraintIndex));
		ConstraintParticles.Add(InConstrainedParticles);
		ConstraintSettings.Add(FPBDJointSettings());
		ConstraintSettings[ConstraintIndex].ConstraintFrames = ConstraintFrames;
		ConstraintStates.Add(FPBDJointState());
		return Handles.Last();
	}

	
	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::AddConstraint(const FParticlePair& InConstrainedParticles, const FPBDJointSettings& InConstraintSettings)
	{
		int ConstraintIndex = Handles.Num();
		Handles.Add(HandleAllocator.AllocHandle(this, ConstraintIndex));
		ConstraintParticles.Add(InConstrainedParticles);
		ConstraintSettings.Add(InConstraintSettings);
		ConstraintStates.Add(FPBDJointState());
		return Handles.Last();
	}

	
	void FPBDJointConstraints::RemoveConstraint(int ConstraintIndex)
	{
		FConstraintContainerHandle* ConstraintHandle = Handles[ConstraintIndex];
		if (ConstraintHandle != nullptr)
		{
			// Release the handle for the freed constraint
			HandleAllocator.FreeHandle(ConstraintHandle);
			Handles[ConstraintIndex] = nullptr;
		}

		// Swap the last constraint into the gap to keep the array packed
		ConstraintParticles.RemoveAtSwap(ConstraintIndex);
		ConstraintSettings.RemoveAtSwap(ConstraintIndex);
		ConstraintStates.RemoveAtSwap(ConstraintIndex);
		Handles.RemoveAtSwap(ConstraintIndex);

		// Update the handle for the constraint that was moved
		if (ConstraintIndex < Handles.Num())
		{
			SetConstraintIndex(Handles[ConstraintIndex], ConstraintIndex);
		}
	}

	
	void FPBDJointConstraints::RemoveConstraints(const TSet<TGeometryParticleHandle<FReal, 3>*>& RemovedParticles)
	{
	}


	
	void FPBDJointConstraints::SetPreApplyCallback(const FJointPreApplyCallback& Callback)
	{
		PreApplyCallback = Callback;
	}

	
	void FPBDJointConstraints::ClearPreApplyCallback()
	{
		PreApplyCallback = nullptr;
	}

	
	void FPBDJointConstraints::SetPostApplyCallback(const FJointPostApplyCallback& Callback)
	{
		PostApplyCallback = Callback;
	}

	
	void FPBDJointConstraints::ClearPostApplyCallback()
	{
		PostApplyCallback = nullptr;
	}

	
	const typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::GetConstraintHandle(int32 ConstraintIndex) const
	{
		return Handles[ConstraintIndex];
	}

	
	typename FPBDJointConstraints::FConstraintContainerHandle* FPBDJointConstraints::GetConstraintHandle(int32 ConstraintIndex)
	{
		return Handles[ConstraintIndex];
	}

	
	const typename FPBDJointConstraints::FParticlePair& FPBDJointConstraints::GetConstrainedParticles(int32 ConstraintIndex) const
	{
		return ConstraintParticles[ConstraintIndex];
	}

	
	const FPBDJointSettings& FPBDJointConstraints::GetConstraintSettings(int32 ConstraintIndex) const
	{
		return ConstraintSettings[ConstraintIndex];
	}

	
	int32 FPBDJointConstraints::GetConstraintLevel(int32 ConstraintIndex) const
	{
		return ConstraintStates[ConstraintIndex].Level;
	}

	
	void FPBDJointConstraints::SetParticleLevels(int32 ConstraintIndex, const TVector<int32, 2>& ParticleLevels)
	{
		ConstraintStates[ConstraintIndex].Level = FMath::Min(ParticleLevels[0], ParticleLevels[1]);
		ConstraintStates[ConstraintIndex].ParticleLevels = ParticleLevels;
	}

	
	void FPBDJointConstraints::UpdatePositionBasedState(const FReal Dt)
	{
	}

	
	void FPBDJointConstraints::CalculateConstraintSpace(int32 ConstraintIndex, FVec3& OutX0, FMatrix33& OutR0, FVec3& OutX1, FMatrix33& OutR1, FVec3& OutCR) const
	{
		const int32 Index0 = 1;
		const int32 Index1 = 0;
		TGenericParticleHandle<FReal, 3> Particle0 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index0]);
		TGenericParticleHandle<FReal, 3> Particle1 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index1]);
		FVec3 P0 = Particle0->P();
		FRotation3 Q0 = Particle0->Q();
		FVec3 P1 = Particle1->P();
		FRotation3 Q1 = Particle1->Q();

		const FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];
		EJointMotionType Swing1Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
		EJointMotionType Swing2Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];
		if ((Swing1Motion == EJointMotionType::Limited) && (Swing2Motion == EJointMotionType::Limited))
		{
			FPBDJointUtilities::CalculateConeConstraintSpace(Settings, ConstraintSettings[ConstraintIndex], Index0, Index1, P0, Q0, P1, Q1, OutX0, OutR0, OutX1, OutR1, OutCR);
		}
		else
		{
			FPBDJointUtilities::CalculateSwingConstraintSpace(Settings, ConstraintSettings[ConstraintIndex], Index0, Index1, P0, Q0, P1, Q1, OutX0, OutR0, OutX1, OutR1, OutCR);
		}
	}

	
	void FPBDJointConstraints::Apply(const FReal Dt, const TArray<FConstraintContainerHandle*>& InConstraintHandles, const int32 It, const int32 NumIts)
	{
		SCOPE_CYCLE_COUNTER(STAT_ApplyJointConstraints);

		// @todo(ccaulfield): make sorting optional
		// @todo(ccaulfield): handles should be sorted by level by the constraint rule/graph
		// @todo(ccaulfield): the best sort order depends on whether we are freezing.
		// If we are freezing we want the root-most (nearest to kinematic) bodies solved first.
		// For normal update we want the root body last, otherwise it gets dragged away from the root by the other bodies

		TArray<FConstraintContainerHandle*> SortedConstraintHandles = InConstraintHandles;
		SortedConstraintHandles.Sort([](const FConstraintContainerHandle& L, const FConstraintContainerHandle& R)
			{
				// Sort bodies from leaf to root
				return L.GetConstraintLevel() > R.GetConstraintLevel();
			});

		if (PreApplyCallback != nullptr)
		{
			PreApplyCallback(Dt, SortedConstraintHandles);
		}

		for (FConstraintContainerHandle* ConstraintHandle : SortedConstraintHandles)
		{
			if (Settings.bEnableVelocitySolve)
			{
				SolveVelocity(Dt, ConstraintHandle->GetConstraintIndex(), Settings.ApplyPairIterations, It, NumIts);
			}
			else
			{
				SolvePosition(Dt, ConstraintHandle->GetConstraintIndex(), Settings.ApplyPairIterations, It, NumIts);
			}
		}

		if (Settings.ProjectionPhase == EJointProjectionPhase::Apply)
		{
			int32 ProjectionIt = NumIts - 1;
			if (It == ProjectionIt)
			{
				ApplyProjection(Dt, InConstraintHandles);
			}
		}

		if (PostApplyCallback != nullptr)
		{
			PostApplyCallback(Dt, SortedConstraintHandles);
		}
	}

	
	bool FPBDJointConstraints::ApplyPushOut(const FReal Dt, const TArray<FConstraintContainerHandle*>& InConstraintHandles, const int32 It, const int32 NumIts)
	{
		SCOPE_CYCLE_COUNTER(STAT_ApplyPushOutJointConstraints);

		// @todo(ccaulfield): track whether we are sufficiently solved
		bool bNeedsAnotherIteration = true;

		if (Settings.ApplyPushOutPairIterations > 0)
		{
			TArray<FConstraintContainerHandle*> SortedConstraintHandles = InConstraintHandles;
			SortedConstraintHandles.Sort([](const FConstraintContainerHandle& L, const FConstraintContainerHandle& R)
				{
					// Sort bodies from root to leaf
					return L.GetConstraintLevel() < R.GetConstraintLevel();
				});

			for (FConstraintContainerHandle* ConstraintHandle : SortedConstraintHandles)
			{
				SolvePosition(Dt, ConstraintHandle->GetConstraintIndex(), Settings.ApplyPushOutPairIterations, It, NumIts);
			}
		}

		if (Settings.ProjectionPhase == EJointProjectionPhase::ApplyPushOut)
		{
			int32 ProjectionIt = (Settings.ApplyPushOutPairIterations > 0) ? NumIts - 1 : 0;
			if (It == ProjectionIt)
			{
				ApplyProjection(Dt, InConstraintHandles);
			}
		}

		return bNeedsAnotherIteration;
	}

	
	void FPBDJointConstraints::ApplyProjection(const FReal Dt, const TArray<FConstraintContainerHandle*>& InConstraintHandles)
	{
		TArray<FConstraintContainerHandle*> SortedConstraintHandles = InConstraintHandles;
		SortedConstraintHandles.Sort([](const FConstraintContainerHandle& L, const FConstraintContainerHandle& R)
			{
				// Sort bodies from root to leaf
				return L.GetConstraintLevel() < R.GetConstraintLevel();
			});

		for (FConstraintContainerHandle* ConstraintHandle : SortedConstraintHandles)
		{
			ProjectPosition(Dt, ConstraintHandle->GetConstraintIndex(), 0, 1);
		}
	}

	
	void FPBDJointConstraints::SolveVelocity(const FReal Dt, const int32 ConstraintIndex, const int32 NumPairIts, const int32 It, const int32 NumIts)
	{
		const TVector<TGeometryParticleHandle<FReal, 3>*, 2>& Constraint = ConstraintParticles[ConstraintIndex];
		UE_LOG(LogChaosJoint, Verbose, TEXT("Solve Joint Constraint %d %s %s (dt = %f; it = %d / %d)"), ConstraintIndex, *Constraint[0]->ToString(), *Constraint[1]->ToString(), Dt, It, NumIts);

		const FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];

		// Switch particles - internally we assume the first body is the parent (i.e., the space in which constraint limits are specified)
		const int32 Index0 = 1;
		const int32 Index1 = 0;
		TGenericParticleHandle<FReal, 3> Particle0 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index0]);
		TGenericParticleHandle<FReal, 3> Particle1 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index1]);
		TPBDRigidParticleHandle<FReal, 3>* Rigid0 = ConstraintParticles[ConstraintIndex][Index0]->AsDynamic();
		TPBDRigidParticleHandle<FReal, 3>* Rigid1 = ConstraintParticles[ConstraintIndex][Index1]->AsDynamic();

		FVec3 P0 = Particle0->P();
		FRotation3 Q0 = Particle0->Q();
		FVec3 V0 = Particle0->V();
		FVec3 W0 = Particle0->W();
		FVec3 P1 = Particle1->P();
		FRotation3 Q1 = Particle1->Q();
		FVec3 V1 = Particle1->V();
		FVec3 W1 = Particle1->W();
		float InvM0 = Particle0->InvM();
		float InvM1 = Particle1->InvM();
		FMatrix33 InvIL0 = Particle0->InvI();
		FMatrix33 InvIL1 = Particle1->InvI();

		Q1.EnforceShortestArcWith(Q0);

		FReal LinearStiffness = GetLinearStiffness(Settings, JointSettings);
		FReal TwistStiffness = GetTwistStiffness(Settings, JointSettings);
		FReal SwingStiffness = GetSwingStiffness(Settings, JointSettings);

		// Adjust mass for stability
		const int32 Level0 = ConstraintStates[ConstraintIndex].ParticleLevels[Index0];
		const int32 Level1 = ConstraintStates[ConstraintIndex].ParticleLevels[Index1];
		if (Level0 < Level1)
		{
			FPBDJointUtilities::GetConditionedInverseMass(Particle0->M(), Particle0->I().GetDiagonal(), Particle1->M(), Particle1->I().GetDiagonal(), InvM0, InvM1, InvIL0, InvIL1, Settings.MinParentMassRatio, Settings.MaxInertiaRatio);
		}
		else if (Level0 > Level1)
		{
			FPBDJointUtilities::GetConditionedInverseMass(Particle1->M(), Particle1->I().GetDiagonal(), Particle0->M(), Particle0->I().GetDiagonal(), InvM1, InvM0, InvIL1, InvIL0, Settings.MinParentMassRatio, Settings.MaxInertiaRatio);
		}
		else
		{
			FPBDJointUtilities::GetConditionedInverseMass(Particle0->M(), Particle0->I().GetDiagonal(), Particle1->M(), Particle1->I().GetDiagonal(), InvM0, InvM1, InvIL0, InvIL1, (FReal)0, Settings.MaxInertiaRatio);
		}

		const TVector<EJointMotionType, 3>& LinearMotion = JointSettings.Motion.LinearMotionTypes;
		EJointMotionType TwistMotion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist];
		EJointMotionType Swing1Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
		EJointMotionType Swing2Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];

		for (int32 PairIt = 0; PairIt < NumPairIts; ++PairIt)
		{
			// Apply angular drives (NOTE: modifies position, not velocity)
			if (Settings.bEnableDrives)
			{
				bool bTwistLocked = TwistMotion == EJointMotionType::Locked;
				bool bSwing1Locked = Swing1Motion == EJointMotionType::Locked;
				bool bSwing2Locked = Swing2Motion == EJointMotionType::Locked;

				// No SLerp drive if we have a locked rotation (it will be grayed out in the editor in this case, but could still have been set before the rotation was locked)
				if (JointSettings.Motion.bAngularSLerpDriveEnabled && !bTwistLocked && !bSwing1Locked && !bSwing2Locked)
				{
					FPBDJointUtilities::ApplyJointSLerpDrive(Dt, Settings, JointSettings, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}

				if (JointSettings.Motion.bAngularTwistDriveEnabled && !bTwistLocked)
				{
					FPBDJointUtilities::ApplyJointTwistDrive(Dt, Settings, JointSettings, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}

				if (JointSettings.Motion.bAngularSwingDriveEnabled && !bSwing1Locked && !bSwing2Locked)
				{
					FPBDJointUtilities::ApplyJointConeDrive(Dt, Settings, JointSettings, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}
				else if (JointSettings.Motion.bAngularSwingDriveEnabled && !bSwing1Locked)
				{
					//FPBDJointUtilities::ApplyJointSwingDrive(Dt, Settings, JointSettings, Index0, Index1, EJointAngularConstraintIndex::Swing1, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
				}
				else if (JointSettings.Motion.bAngularSwingDriveEnabled && !bSwing2Locked)
				{
					//FPBDJointUtilities::ApplyJointSwingDrive(Dt, Settings, JointSettings, Index0, Index1, EJointAngularConstraintIndex::Swing2, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
				}
			}

			// Apply twist velocity constraint
			if (Settings.bEnableTwistLimits)
			{
				if (TwistMotion != EJointMotionType::Free)
				{
					FPBDJointUtilities::ApplyJointTwistVelocityConstraint(Dt, Settings, JointSettings, TwistStiffness, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}
			}

			// Apply swing velocity constraints
			if (Settings.bEnableSwingLimits)
			{
				if ((Swing1Motion == EJointMotionType::Limited) && (Swing2Motion == EJointMotionType::Limited))
				{
					// Swing Cone
					FPBDJointUtilities::ApplyJointConeVelocityConstraint(Dt, Settings, JointSettings, SwingStiffness, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}
				else
				{
					if (Swing1Motion != EJointMotionType::Free)
					{
						// Swing Arc/Lock
						FPBDJointUtilities::ApplyJointSwingVelocityConstraint(Dt, Settings, JointSettings, SwingStiffness, Index0, Index1, EJointAngularConstraintIndex::Swing1, EJointAngularAxisIndex::Swing1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
					}
					if (Swing2Motion != EJointMotionType::Free)
					{
						// Swing Arc/Lock
						FPBDJointUtilities::ApplyJointSwingVelocityConstraint(Dt, Settings, JointSettings, SwingStiffness, Index0, Index1, EJointAngularConstraintIndex::Swing2, EJointAngularAxisIndex::Swing2, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
					}
				}
			}

			// Apply linear velocity  constraints
			if ((LinearMotion[0] != EJointMotionType::Free) || (LinearMotion[1] != EJointMotionType::Free) || (LinearMotion[2] != EJointMotionType::Free))
			{
				FPBDJointUtilities::ApplyJointVelocityConstraint(Dt, Settings, JointSettings, LinearStiffness, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
			}
		}

		// Update the particles
		if (Rigid0)
		{
			Rigid0->SetP(P0);
			Rigid0->SetQ(Q0);
			Rigid0->SetV(V0);
			Rigid0->SetW(W0);
		}
		if (Rigid1)
		{
			Rigid1->SetP(P1);
			Rigid1->SetQ(Q1);
			Rigid1->SetV(V1);
			Rigid1->SetW(W1);
		}
	}

	
	void FPBDJointConstraints::SolvePosition(const FReal Dt, const int32 ConstraintIndex, const int32 NumPairIts, const int32 It, const int32 NumIts)
	{
		const TVector<TGeometryParticleHandle<FReal, 3>*, 2>& Constraint = ConstraintParticles[ConstraintIndex];
		UE_LOG(LogChaosJoint, Verbose, TEXT("Solve Joint Constraint %d %s %s (dt = %f; it = %d / %d)"), ConstraintIndex, *Constraint[0]->ToString(), *Constraint[1]->ToString(), Dt, It, NumIts);

		const FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];

		// Switch particles - internally we assume the first body is the parent (i.e., the space in which constraint limits are specified)
		const int32 Index0 = 1;
		const int32 Index1 = 0;
		TGenericParticleHandle<FReal, 3> Particle0 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index0]);
		TGenericParticleHandle<FReal, 3> Particle1 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index1]);
		TPBDRigidParticleHandle<FReal, 3>* Rigid0 = ConstraintParticles[ConstraintIndex][Index0]->AsDynamic();
		TPBDRigidParticleHandle<FReal, 3>* Rigid1 = ConstraintParticles[ConstraintIndex][Index1]->AsDynamic();

		FVec3 P0 = Particle0->P();
		FRotation3 Q0 = Particle0->Q();
		FVec3 V0 = Particle0->V();
		FVec3 W0 = Particle0->W();
		FVec3 P1 = Particle1->P();
		FRotation3 Q1 = Particle1->Q();
		FVec3 V1 = Particle1->V();
		FVec3 W1 = Particle1->W();
		float InvM0 = Particle0->InvM();
		float InvM1 = Particle1->InvM();
		FMatrix33 InvIL0 = Particle0->InvI();
		FMatrix33 InvIL1 = Particle1->InvI();

		Q1.EnforceShortestArcWith(Q0);

		FReal LinearStiffness = GetLinearStiffness(Settings, JointSettings);
		FReal TwistStiffness = GetTwistStiffness(Settings, JointSettings);
		FReal SwingStiffness = GetSwingStiffness(Settings, JointSettings);

		// Adjust mass for stability
		const int32 Level0 = ConstraintStates[ConstraintIndex].ParticleLevels[Index0];
		const int32 Level1 = ConstraintStates[ConstraintIndex].ParticleLevels[Index1];
		if (Level0 < Level1)
		{
			FPBDJointUtilities::GetConditionedInverseMass(Particle0->M(), Particle0->I().GetDiagonal(), Particle1->M(), Particle1->I().GetDiagonal(), InvM0, InvM1, InvIL0, InvIL1, Settings.MinParentMassRatio, Settings.MaxInertiaRatio);
		}
		else if (Level0 > Level1)
		{
			FPBDJointUtilities::GetConditionedInverseMass(Particle1->M(), Particle1->I().GetDiagonal(), Particle0->M(), Particle0->I().GetDiagonal(), InvM1, InvM0, InvIL1, InvIL0, Settings.MinParentMassRatio, Settings.MaxInertiaRatio);
		}
		else
		{
			FPBDJointUtilities::GetConditionedInverseMass(Particle0->M(), Particle0->I().GetDiagonal(), Particle1->M(), Particle1->I().GetDiagonal(), InvM0, InvM1, InvIL0, InvIL1, (FReal)0, Settings.MaxInertiaRatio);
		}

		const TVector<EJointMotionType, 3>& LinearMotion = JointSettings.Motion.LinearMotionTypes;
		EJointMotionType TwistMotion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist];
		EJointMotionType Swing1Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
		EJointMotionType Swing2Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];

		for (int32 PairIt = 0; PairIt < NumPairIts; ++PairIt)
		{
			// Apply angular drives (NOTE: modifies position, not velocity)
			if (!Settings.bEnableVelocitySolve && Settings.bEnableDrives)
			{
				bool bTwistLocked = TwistMotion == EJointMotionType::Locked;
				bool bSwing1Locked = Swing1Motion == EJointMotionType::Locked;
				bool bSwing2Locked = Swing2Motion == EJointMotionType::Locked;

				// No SLerp drive if we have a locked rotation (it will be grayed out in the editor in this case, but could still have been set before the rotation was locked)
				if (JointSettings.Motion.bAngularSLerpDriveEnabled && !bTwistLocked && !bSwing1Locked && !bSwing2Locked)
				{
					FPBDJointUtilities::ApplyJointSLerpDrive(Dt, Settings, JointSettings, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}

				if (JointSettings.Motion.bAngularTwistDriveEnabled && !bTwistLocked)
				{
					FPBDJointUtilities::ApplyJointTwistDrive(Dt, Settings, JointSettings, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}

				if (JointSettings.Motion.bAngularSwingDriveEnabled && !bSwing1Locked && !bSwing2Locked)
				{
					FPBDJointUtilities::ApplyJointConeDrive(Dt, Settings, JointSettings, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}
				else if (JointSettings.Motion.bAngularSwingDriveEnabled && !bSwing1Locked)
				{
					//FPBDJointUtilities::ApplyJointSwingDrive(Dt, Settings, JointSettings, Index0, Index1, EJointAngularConstraintIndex::Swing1, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
				}
				else if (JointSettings.Motion.bAngularSwingDriveEnabled && !bSwing2Locked)
				{
					//FPBDJointUtilities::ApplyJointSwingDrive(Dt, Settings, JointSettings, Index0, Index1, EJointAngularConstraintIndex::Swing2, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
				}
			}

			// Apply twist constraint
			if (Settings.bEnableTwistLimits)
			{
				if (TwistMotion != EJointMotionType::Free)
				{
					FPBDJointUtilities::ApplyJointTwistConstraint(Dt, Settings, JointSettings, TwistStiffness, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}
			}

			// Apply swing constraints
			if (Settings.bEnableSwingLimits)
			{
				if ((Swing1Motion == EJointMotionType::Limited) && (Swing2Motion == EJointMotionType::Limited))
				{
					// Swing Cone
					FPBDJointUtilities::ApplyJointConeConstraint(Dt, Settings, JointSettings, SwingStiffness, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
				}
				else
				{
					if (Swing1Motion != EJointMotionType::Free)
					{
						// Swing Arc/Lock
						FPBDJointUtilities::ApplyJointSwingConstraint(Dt, Settings, JointSettings, SwingStiffness, Index0, Index1, EJointAngularConstraintIndex::Swing1, EJointAngularAxisIndex::Swing1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
					}
					if (Swing2Motion != EJointMotionType::Free)
					{
						// Swing Arc/Lock
						FPBDJointUtilities::ApplyJointSwingConstraint(Dt, Settings, JointSettings, SwingStiffness, Index0, Index1, EJointAngularConstraintIndex::Swing2, EJointAngularAxisIndex::Swing2, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
					}
				}
			}

			// Apply linear constraints
			if ((LinearMotion[0] != EJointMotionType::Free) || (LinearMotion[1] != EJointMotionType::Free) || (LinearMotion[2] != EJointMotionType::Free))
			{
				FPBDJointUtilities::ApplyJointPositionConstraint(Dt, Settings, JointSettings, LinearStiffness, Index0, Index1, P0, Q0, V0, W0, P1, Q1, V1, W1, InvM0, InvIL0, InvM1, InvIL1);
			}
		}

		// Update the particles
		if (Rigid0)
		{
			Rigid0->SetP(P0);
			Rigid0->SetQ(Q0);
			Rigid0->SetV(V0);
			Rigid0->SetW(W0);
		}
		if (Rigid1)
		{
			Rigid1->SetP(P1);
			Rigid1->SetQ(Q1);
			Rigid1->SetV(V1);
			Rigid1->SetW(W1);
		}
	}

	
	void FPBDJointConstraints::ProjectPosition(const FReal Dt, const int32 ConstraintIndex, const int32 It, const int32 NumIts)
	{
		const FPBDJointSettings& JointSettings = ConstraintSettings[ConstraintIndex];

		// Scale projection up to ProjectionSetting over NumProjectionIts
		const FReal LinearProjectionFactor = (Settings.LinearProjection > 0) ? Settings.LinearProjection : JointSettings.Motion.LinearProjection;
		const FReal AngularProjectionFactor = (Settings.AngularProjection > 0) ? Settings.AngularProjection : JointSettings.Motion.AngularProjection;
		if ((LinearProjectionFactor == (FReal)0) && (AngularProjectionFactor == (FReal)0))
		{
			return;
		}

		const TVector<TGeometryParticleHandle<FReal, 3>*, 2>& Constraint = ConstraintParticles[ConstraintIndex];
		UE_LOG(LogChaosJoint, Verbose, TEXT("Project Joint Constraint %d %f %f (it = %d / %d)"), ConstraintIndex, LinearProjectionFactor, AngularProjectionFactor, It, NumIts);

		// Switch particles - internally we assume the first body is the parent (i.e., the space in which constraint limits are specified)
		const int32 Index0 = 1;
		const int32 Index1 = 0;
		TGenericParticleHandle<FReal, 3> Particle0 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index0]);
		TGenericParticleHandle<FReal, 3> Particle1 = TGenericParticleHandle<FReal, 3>(ConstraintParticles[ConstraintIndex][Index1]);

		FVec3 P0 = Particle0->P();
		FRotation3 Q0 = Particle0->Q();
		FVec3 P1 = Particle1->P();
		FRotation3 Q1 = Particle1->Q();
		float InvM0 = Particle0->InvM();
		float InvM1 = Particle1->InvM();
		FMatrix33 InvIL0 = Particle0->InvI();
		FMatrix33 InvIL1 = Particle1->InvI();

		// Freeze the closest to kinematic connection if there is a difference
		const int32 Level0 = ConstraintStates[ConstraintIndex].ParticleLevels[Index0];
		const int32 Level1 = ConstraintStates[ConstraintIndex].ParticleLevels[Index1];
		if (Level0 < Level1)
		{
			InvM0 = 0;
			InvIL0 = FMatrix33(0, 0, 0);
		}
		else if (Level1 < Level0)
		{
			InvM1 = 0;
			InvIL1 = FMatrix33(0, 0, 0);
		}

		const TVector<EJointMotionType, 3>& LinearMotion = JointSettings.Motion.LinearMotionTypes;
		EJointMotionType TwistMotion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist];
		EJointMotionType Swing1Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
		EJointMotionType Swing2Motion = JointSettings.Motion.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];

		if (AngularProjectionFactor > 0)
		{
			if (Settings.bEnableTwistLimits)
			{
				// Remove Twist Error
				FPBDJointUtilities::ApplyJointTwistProjection(Dt, Settings, JointSettings, AngularProjectionFactor, Index0, Index1, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
			}

			if (Settings.bEnableSwingLimits)
			{
				// Remove Swing Error
				if ((Swing1Motion == EJointMotionType::Limited) && (Swing2Motion == EJointMotionType::Limited))
				{
					FPBDJointUtilities::ApplyJointConeProjection(Dt, Settings, JointSettings, AngularProjectionFactor, Index0, Index1, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
				}
				else
				{
					if (Swing1Motion != EJointMotionType::Free)
					{
						FPBDJointUtilities::ApplyJointSwingProjection(Dt, Settings, JointSettings, AngularProjectionFactor, Index0, Index1, EJointAngularConstraintIndex::Swing1, EJointAngularAxisIndex::Swing1, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
					}
					if (Swing2Motion != EJointMotionType::Free)
					{
						FPBDJointUtilities::ApplyJointSwingProjection(Dt, Settings, JointSettings, AngularProjectionFactor, Index0, Index1, EJointAngularConstraintIndex::Swing2, EJointAngularAxisIndex::Swing2, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
					}
				}
			}
		}

		// Remove Position Error
		if (LinearProjectionFactor > 0)
		{
			if ((LinearMotion[0] != EJointMotionType::Free) || (LinearMotion[1] != EJointMotionType::Free) || (LinearMotion[2] != EJointMotionType::Free))
			{
				FPBDJointUtilities::ApplyJointPositionProjection(Dt, Settings, JointSettings, LinearProjectionFactor, Index0, Index1, P0, Q0, P1, Q1, InvM0, InvIL0, InvM1, InvIL1);
			}
		}

		// Update the particles
		TPBDRigidParticleHandle<FReal, 3>* Rigid0 = ConstraintParticles[ConstraintIndex][Index0]->AsDynamic();
		TPBDRigidParticleHandle<FReal, 3>* Rigid1 = ConstraintParticles[ConstraintIndex][Index1]->AsDynamic();
		if (Rigid0)
		{
			Rigid0->SetP(P0);
			Rigid0->SetQ(Q0);
		}
		if (Rigid1)
		{
			Rigid1->SetP(P1);
			Rigid1->SetQ(Q1);
		}
	}
}

namespace Chaos
{
	template class TContainerConstraintHandle<FPBDJointConstraints>;
}
