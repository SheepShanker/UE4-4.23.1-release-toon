// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Chaos/PBDRigidDynamicSpringConstraints.h"

using namespace Chaos;

template<class T, int d>
TVector<TGeometryParticleHandle<T, d>*, 2> TPBDRigidDynamicSpringConstraintHandle<T,d>::GetConstrainedParticles() const
{
	return ConstraintContainer->GetConstrainedParticles(ConstraintIndex);
}

template<class T, int d>
void TPBDRigidDynamicSpringConstraints<T, d>::UpdatePositionBasedState(const T Dt)
{
	const int32 NumConstraints = Constraints.Num();
	for (int32 ConstraintIndex = 0; ConstraintIndex < NumConstraints; ++ConstraintIndex)
	{
		TGeometryParticleHandle<T, d>* Static0 = Constraints[ConstraintIndex][0];
		TGeometryParticleHandle<T, d>* Static1 = Constraints[ConstraintIndex][1];
		TPBDRigidParticleHandle<T, d>* PBDRigid0 = Static0->AsDynamic();
		TPBDRigidParticleHandle<T, d>* PBDRigid1 = Static1->AsDynamic();

		// Do not create springs between objects with no geometry
		if (!Static0->Geometry() || !Static1->Geometry())
		{
			continue;
		}

		const TRotation<T, d>& Q0 = PBDRigid0 ? PBDRigid0->Q() : Static0->R();
		const TRotation<T, d>& Q1 = PBDRigid1 ? PBDRigid1->Q() : Static1->R();
		const TVector<T, d>& P0 = PBDRigid0 ? PBDRigid0->P() : Static0->X();
		const TVector<T, d>& P1 = PBDRigid1 ? PBDRigid1->P() : Static1->X();

		// Delete constraints
		const int32 NumSprings = SpringDistances[ConstraintIndex].Num();
		for (int32 SpringIndex = NumSprings - 1; SpringIndex >= 0; --SpringIndex)
		{
			const TVector<T, d>& Distance0 = Distances[ConstraintIndex][SpringIndex][0];
			const TVector<T, d>& Distance1 = Distances[ConstraintIndex][SpringIndex][1];
			const TVector<T, d> WorldSpaceX1 = Q0.RotateVector(Distance0) + P0;
			const TVector<T, d> WorldSpaceX2 = Q1.RotateVector(Distance1) + P1;
			const TVector<T, d> Difference = WorldSpaceX2 - WorldSpaceX1;
			float Distance = Difference.Size();
			if (Distance > CreationThreshold * 2)
			{
				Distances[ConstraintIndex].RemoveAtSwap(SpringIndex);
				SpringDistances[ConstraintIndex].RemoveAtSwap(SpringIndex);
			}
		}

		if (SpringDistances[ConstraintIndex].Num() == MaxSprings)
		{
			continue;
		}

		TRigidTransform<T, d> Transform1(P0, Q0);
		TRigidTransform<T, d> Transform2(P1, Q1);

		// Create constraints
		if (Static0->Geometry()->HasBoundingBox() && Static1->Geometry()->HasBoundingBox())
		{
			// Matrix multiplication is reversed intentionally to be compatible with unreal
			TBox<T, d> Box1 = Static0->Geometry()->BoundingBox().TransformedBox(Transform1 * Transform2.Inverse());
			Box1.Thicken(CreationThreshold);
			TBox<T, d> Box2 = Static1->Geometry()->BoundingBox();
			Box2.Thicken(CreationThreshold);
			if (!Box1.Intersects(Box2))
			{
				continue;
			}
		}
		const TVector<T, d> Midpoint = (P0 + P1) / (T)2;
		TVector<T, d> Normal1;
		const T Phi1 = Static0->Geometry()->PhiWithNormal(Transform1.InverseTransformPosition(Midpoint), Normal1);
		Normal1 = Transform2.TransformVector(Normal1);
		TVector<T, d> Normal2;
		const T Phi2 = Static1->Geometry()->PhiWithNormal(Transform2.InverseTransformPosition(Midpoint), Normal2);
		Normal2 = Transform2.TransformVector(Normal2);
		if ((Phi1 + Phi2) > CreationThreshold)
		{
			continue;
		}
		TVector<T, d> Location0 = Midpoint - Phi1 * Normal1;
		TVector<T, d> Location1 = Midpoint - Phi2 * Normal2;
		TVector<TVector<T, 3>, 2> Distance;
		Distance[0] = Q0.Inverse().RotateVector(Location0 - P0);
		Distance[1] = Q0.Inverse().RotateVector(Location1 - P1);
		Distances[ConstraintIndex].Add(MoveTemp(Distance));
		SpringDistances[ConstraintIndex].Add((Location0 - Location1).Size());
	}
}

template<class T, int d>
TVector<T, d> TPBDRigidDynamicSpringConstraints<T, d>::GetDelta(const TVector<T, d>& WorldSpaceX1, const TVector<T, d>& WorldSpaceX2, const int32 ConstraintIndex, const int32 SpringIndex) const
{
	TPBDRigidParticleHandle<T, d>* PBDRigid0 = Constraints[ConstraintIndex][0]->AsDynamic();
	TPBDRigidParticleHandle<T, d>* PBDRigid1 = Constraints[ConstraintIndex][1]->AsDynamic();

	if (!PBDRigid0 && !PBDRigid1)
	{
		return TVector<T, d>(0);
	}

	const TVector<T, d> Difference = WorldSpaceX2 - WorldSpaceX1;
	float Distance = Difference.Size();
	check(Distance > 1e-7);

	const T InvM0 = PBDRigid0 ? PBDRigid0->InvM() : (T)0;
	const T InvM1 = PBDRigid1 ? PBDRigid1->InvM() : (T)0;
	const TVector<T, d> Direction = Difference / Distance;
	const TVector<T, d> Delta = (Distance - SpringDistances[ConstraintIndex][SpringIndex]) * Direction;
	return Stiffness * Delta / (InvM0 + InvM1);
}

template<class T, int d>
void TPBDRigidDynamicSpringConstraints<T, d>::ApplySingle(const T Dt, int32 ConstraintIndex) const
{
	TGeometryParticleHandle<T, d>* Static0 = Constraints[ConstraintIndex][0];
	TGeometryParticleHandle<T, d>* Static1 = Constraints[ConstraintIndex][1];
	TPBDRigidParticleHandle<T, d>* PBDRigid0 = Static0->AsDynamic();
	TPBDRigidParticleHandle<T, d>* PBDRigid1 = Static1->AsDynamic();
	check(PBDRigid0 || PBDRigid1);
	check(!PBDRigid0 || !PBDRigid1 || (PBDRigid0->Island() == PBDRigid1->Island()));

	TRotation<T, d>& Q0 = PBDRigid0 ? PBDRigid0->Q() : Static0->R();
	TRotation<T, d>& Q1 = PBDRigid1 ? PBDRigid1->Q() : Static1->R();
	TVector<T, d>& P0 = PBDRigid0 ? PBDRigid0->P() : Static0->X();
	TVector<T, d>& P1 = PBDRigid1 ? PBDRigid1->P() : Static1->X();

	const int32 NumSprings = SpringDistances[ConstraintIndex].Num();
	const PMatrix<T, d, d> WorldSpaceInvI1 = PBDRigid0? (Q0 * FMatrix::Identity) * PBDRigid0->InvI() * (Q0 * FMatrix::Identity).GetTransposed() : PMatrix<T, d, d>(0);
	const PMatrix<T, d, d> WorldSpaceInvI2 = PBDRigid1 ? (Q1 * FMatrix::Identity) * PBDRigid1->InvI() * (Q1 * FMatrix::Identity).GetTransposed() : PMatrix<T, d, d>(0);;
	for (int32 SpringIndex = 0; SpringIndex < NumSprings; ++SpringIndex)
	{
		const TVector<T, d>& Distance0 = Distances[ConstraintIndex][SpringIndex][0];
		const TVector<T, d>& Distance1 = Distances[ConstraintIndex][SpringIndex][1];
		const TVector<T, d> WorldSpaceX1 = Q0.RotateVector(Distance0) + P0;
		const TVector<T, d> WorldSpaceX2 = Q1.RotateVector(Distance1) + P1;
		const TVector<T, d> Delta = GetDelta(WorldSpaceX1, WorldSpaceX2, ConstraintIndex, SpringIndex);

		if (PBDRigid0)
		{
			const TVector<T, d> Radius = WorldSpaceX1 - P0;
			P0 += PBDRigid0->InvM() * Delta;
			Q0 += TRotation<T, d>::FromElements(WorldSpaceInvI1 * TVector<T, d>::CrossProduct(Radius, Delta), 0.f) * Q0 * T(0.5);
			Q0.Normalize();
		}

		if (PBDRigid1)
		{
			const TVector<T, d> Radius = WorldSpaceX2 - P1;
			P1 -= PBDRigid1->InvM() * Delta;
			Q1 += TRotation<T, d>::FromElements(WorldSpaceInvI2 * TVector<T, d>::CrossProduct(Radius, -Delta), 0.f) * Q1 * T(0.5);
			Q1.Normalize();
		}
	}
}

namespace Chaos
{
	template class TPBDRigidDynamicSpringConstraintHandle<float, 3>;
	template class TPBDRigidDynamicSpringConstraints<float, 3>;
}
