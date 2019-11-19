// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/ParticleHandle.h"
#include "UObject/ExternalPhysicsCustomObjectVersion.h"

namespace Chaos
{

template <typename T, int d>
class TPBDRigidsSOAs
{
public:
	TPBDRigidsSOAs()
	{
#if CHAOS_DETERMINISTIC
		BiggestParticleID = 0;
#endif

		StaticParticles = MakeUnique<TGeometryParticles<T, d>>();
		StaticDisabledParticles = MakeUnique <TGeometryParticles<T, d>>();

		KinematicParticles = MakeUnique < TKinematicGeometryParticles<T, d>>();
		KinematicDisabledParticles = MakeUnique < TKinematicGeometryParticles<T, d>>();

		DynamicDisabledParticles = MakeUnique<TPBDRigidParticles<T, d>>();
		DynamicParticles = MakeUnique<TPBDRigidParticles<T, d>>();
		DynamicKinematicParticles = MakeUnique<TPBDRigidParticles<T, d>>();

		ClusteredParticles = MakeUnique< TPBDRigidClusteredParticles<T, d>>();
		ClusteredParticles->RemoveParticleBehavior() = ERemoveParticleBehavior::Remove;	//clustered particles maintain relative ordering
		UpdateViews();
	}

	TPBDRigidsSOAs(const TPBDRigidsSOAs<T,d>&) = delete;
	TPBDRigidsSOAs(TPBDRigidsSOAs<T, d>&& Other) = delete;

	void Reset()
	{
		check(0);
	}
	
	TArray<TGeometryParticleHandle<T, d>*> CreateStaticParticles(int32 NumParticles, const TGeometryParticleParameters<T, d>& Params = TGeometryParticleParameters<T, d>())
	{
		auto Results =  CreateParticlesHelper<TGeometryParticleHandle<T, d>>(NumParticles, Params.bDisabled ? StaticDisabledParticles : StaticParticles, Params);
		UpdateViews();
		return Results;
	}
	TArray<TKinematicGeometryParticleHandle<T, d>*> CreateKinematicParticles(int32 NumParticles, const TKinematicGeometryParticleParameters<T, d>& Params = TKinematicGeometryParticleParameters<T, d>())
	{
		auto Results = CreateParticlesHelper<TKinematicGeometryParticleHandle<T, d>>(NumParticles, Params.bDisabled ? KinematicDisabledParticles : KinematicParticles, Params);
		UpdateViews();
		return Results;
	}
	TArray<TPBDRigidParticleHandle<T, d>*> CreateDynamicParticles(int32 NumParticles, const TPBDRigidParticleParameters<T, d>& Params = TPBDRigidParticleParameters<T, d>())
	{
		auto Results = CreateParticlesHelper<TPBDRigidParticleHandle<T, d>>(NumParticles, Params.bDisabled ? DynamicDisabledParticles : DynamicParticles, Params);;

		if (!Params.bStartSleeping)
		{
			InsertToMapAndArray(Results, ActiveParticlesToIndex, ActiveParticlesArray);
		}
		UpdateViews();
		return Results;
	}

	/** Used specifically by PBDRigidClustering. These have special properties for maintaining relative order, efficiently switching from kinematic to dynamic, disable to enable, etc... */
	TArray<TPBDRigidClusteredParticleHandle<T, d>*> CreateClusteredParticles(int32 NumParticles, const TPBDRigidParticleParameters<T, d>& Params = TPBDRigidParticleParameters<T, d>())
	{
		auto NewClustered = CreateParticlesHelper<TPBDRigidClusteredParticleHandle<T, d>>(NumParticles, ClusteredParticles, Params);
		
			if (!Params.bDisabled)
			{
			InsertToMapAndArray(NewClustered, NonDisabledClusteredToIndex, NonDisabledClusteredArray);
			}

			if (!Params.bStartSleeping)
			{
			InsertToMapAndArray(reinterpret_cast<TArray<TPBDRigidParticleHandle<T,d>*>&>(NewClustered), ActiveParticlesToIndex, ActiveParticlesArray);
			InsertToMapAndArray(NewClustered, ActiveClusteredToIndex, ActiveClusteredArray);
		}

		UpdateViews();
		
		return NewClustered;
	}

	void DestroyParticle(TGeometryParticleHandle<T, d>* Particle)
	{
		check(Particle->AsClustered() == nullptr);	//not supported

		if (auto PBDRigid = Particle->AsDynamic())
		{
			RemoveFromMapAndArray(PBDRigid, ActiveParticlesToIndex, ActiveParticlesArray);
		}

		ParticleHandles.DestroyHandleSwap(Particle);
		UpdateViews();
	}

	void DisableParticle(TGeometryParticleHandle<T, d>* Particle)
	{
		if (auto PBDRigid = Particle->AsDynamic())
		{
			PBDRigid->Disabled() = true;
			PBDRigid->V() = TVector<T, d>(0);
			PBDRigid->W() = TVector<T, d>(0);

			if (auto PBDRigidClustered = Particle->AsClustered())
			{
				RemoveFromMapAndArray(PBDRigidClustered, NonDisabledClusteredToIndex, NonDisabledClusteredArray);
				RemoveFromMapAndArray(PBDRigidClustered, ActiveClusteredToIndex, ActiveClusteredArray);
			}
			else
			{
				Particle->MoveToSOA(*DynamicDisabledParticles);
			}
			RemoveFromMapAndArray(PBDRigid, ActiveParticlesToIndex, ActiveParticlesArray);
		}
		else if (Particle->AsKinematic())
		{
			Particle->MoveToSOA(*KinematicDisabledParticles);
		}
		else
		{
			Particle->MoveToSOA(*StaticDisabledParticles);
		}
		UpdateViews();
	}

	void EnableParticle(TGeometryParticleHandle<T, d>* Particle)
	{
		if (auto PBDRigid = Particle->AsDynamic())
		{
			if (auto PBDRigidClustered = Particle->AsClustered())
			{
				InsertToMapAndArray(PBDRigidClustered, NonDisabledClusteredToIndex, NonDisabledClusteredArray);
				if (!PBDRigid->Sleeping())
				{
					InsertToMapAndArray(PBDRigidClustered, ActiveClusteredToIndex, ActiveClusteredArray);
				}
			}
			else
			{
				SetDynamicParticleSOA(Particle->AsDynamic());
			}

			if (!PBDRigid->Sleeping())
			{
				InsertToMapAndArray(PBDRigid, ActiveParticlesToIndex, ActiveParticlesArray);
			}

			PBDRigid->Disabled() = false;
		}
		else if (Particle->AsKinematic())
		{
			Particle->MoveToSOA(*KinematicParticles);
		}
		else
		{
			Particle->MoveToSOA(*StaticParticles);
		}
		UpdateViews();
	}

	void ActivateParticle(TGeometryParticleHandle<T, d>* Particle)
	{
		if (auto PBDRigid = Particle->AsDynamic())
		{
			check(!PBDRigid->Disabled());
			if (auto PBDRigidClustered = Particle->AsClustered())
			{
				InsertToMapAndArray(PBDRigidClustered, ActiveClusteredToIndex, ActiveClusteredArray);
			}

			InsertToMapAndArray(PBDRigid, ActiveParticlesToIndex, ActiveParticlesArray);
		}
		
		UpdateViews();
	}

	void DeactivateParticle(TGeometryParticleHandle<T, d>* Particle)
	{
		if (auto PBDRigid = Particle->AsDynamic())
		{
			check(!PBDRigid->Disabled());
			if (auto PBDRigidClustered = Particle->AsClustered())
			{
				RemoveFromMapAndArray(PBDRigidClustered, ActiveClusteredToIndex, ActiveClusteredArray);
			}

			RemoveFromMapAndArray(PBDRigid, ActiveParticlesToIndex, ActiveParticlesArray);
		}

		UpdateViews();
	}

	void DeactivateParticles(const TArray<TGeometryParticleHandle<T, d>*>& Particles)
	{
		for (auto Particle : Particles)
		{
			DeactivateParticle(Particle);
		}
	}

	void SetDynamicParticleSOA(TPBDRigidParticleHandle<T, d>* Particle)
	{
		const EObjectStateType State = Particle->ObjectState();

		// Move to appropriate dynamic SOA
		switch (State)
		{
		case EObjectStateType::Kinematic:
			Particle->MoveToSOA(*DynamicKinematicParticles);
			break;

		case EObjectStateType::Dynamic:
			Particle->MoveToSOA(*DynamicParticles);
			break;

		default:
			// TODO: Special SOAs for sleeping and static particles?
			Particle->MoveToSOA(*DynamicParticles);
			break;
		}

		UpdateViews();
	}

	void Serialize(FChaosArchive& Ar)
	{
		static const FName SOAsName = TEXT("PBDRigidsSOAs");
		FChaosArchiveScopedMemory ScopedMemory(Ar, SOAsName, false);

		ParticleHandles.Serialize(Ar);

		Ar << StaticParticles;
		Ar << StaticDisabledParticles;
		Ar << KinematicParticles;
		Ar << KinematicDisabledParticles;
		Ar << DynamicParticles;
		Ar << DynamicDisabledParticles;

		Ar.UsingCustomVersion(FExternalPhysicsCustomObjectVersion::GUID);
		if (Ar.CustomVer(FExternalPhysicsCustomObjectVersion::GUID) >= FExternalPhysicsCustomObjectVersion::AddDynamicKinematicSOA)
		{
			Ar << DynamicKinematicParticles;
		}

		ensure(ClusteredParticles->Size() == 0);	//not supported yet
		//Ar << ClusteredParticles;

		SerializeMapAndArray(Ar, ActiveParticlesToIndex, ActiveParticlesArray);
		//SerializeMapAndArray(Ar, ActiveClusteredToIndex, ActiveClusteredArray);
		//SerializeMapAndArray(Ar, NonDisabledClusteredToIndex, NonDisabledClusteredArray);

		//todo: update deterministic ID

		UpdateViews();
	}


	const TParticleView<TGeometryParticles<T, d>>& GetNonDisabledView() const { return NonDisabledView; }

	const TParticleView<TPBDRigidParticles<T, d>>& GetNonDisabledDynamicView() const { return NonDisabledDynamicView; }

	const TParticleView<TPBDRigidParticles<T, d>>& GetActiveParticlesView() const { return ActiveParticlesView; }
	TParticleView<TPBDRigidParticles<T, d>>& GetActiveParticlesView() { return ActiveParticlesView; }

	const TParticleView<TGeometryParticles<T, d>>& GetAllParticlesView() const { return AllParticlesView; }

	const TParticleView<TKinematicGeometryParticles<T, d>>& GetActiveKinematicParticlesView() const { return ActiveKinematicParticlesView; }
	TParticleView<TKinematicGeometryParticles<T, d>>& GetActiveKinematicParticlesView() { return ActiveKinematicParticlesView; }

	const TGeometryParticleHandles<T, d>& GetParticleHandles() const { return ParticleHandles; }
	TGeometryParticleHandles<T, d>& GetParticleHandles() { return ParticleHandles; }

	const TPBDRigidParticles<T, d>& GetDynamicParticles() const { return *DynamicParticles; }
	TPBDRigidParticles<T, d>& GetDynamicParticles() { return *DynamicParticles; }

	const TGeometryParticles<T, d>& GetNonDisabledStaticParticles() const { return *StaticParticles; }
	TGeometryParticles<T, d>& GetNonDisabledStaticParticles() { return *StaticParticles; }

	//TEMP: only needed while clustering code continues to use direct indices
	const auto& GetActiveClusteredArray() const { return ActiveClusteredArray; }
	const auto& GetNonDisabledClusteredArray() const { return NonDisabledClusteredArray; }

	const auto& GetClusteredParticles() const { return *ClusteredParticles; }
	auto& GetClusteredParticles() { return *ClusteredParticles; }

private:
	template <typename TParticleHandleType, typename TParticles>
	TArray<TParticleHandleType*> CreateParticlesHelper(int32 NumParticles, TUniquePtr<TParticles>& Particles, const TGeometryParticleParameters<T, d>& Params)
	{
		const int32 ParticlesStartIdx = Particles->Size();
		Particles->AddParticles(NumParticles);
		TArray<TParticleHandleType*> ReturnHandles;
		ReturnHandles.AddUninitialized(NumParticles);

		const int32 HandlesStartIdx = ParticleHandles.Size();
		ParticleHandles.AddHandles(NumParticles);

		for (int32 Count = 0; Count < NumParticles; ++Count)
		{
			const int32 ParticleIdx = Count + ParticlesStartIdx;
			const int32 HandleIdx = Count + HandlesStartIdx;

			TUniquePtr<TParticleHandleType> NewParticleHandle = TParticleHandleType::CreateParticleHandle(MakeSerializable(Particles), ParticleIdx, HandleIdx);
#if CHAOS_DETERMINISTIC
			NewParticleHandle->ParticleID() = BiggestParticleID++;
#endif
			ReturnHandles[Count] = NewParticleHandle.Get();
			ParticleHandles.Handle(HandleIdx) = MoveTemp(NewParticleHandle);
		}

		return ReturnHandles;
	}

	template <typename TParticle>
	void InsertToMapAndArray(const TArray<TParticle*>& ParticlesToInsert, TMap<TParticle*, int32>& ParticleToIndex, TArray<TParticle*>& ParticleArray)
	{
		int32 NextIdx = ParticleArray.Num();
		for (auto Particle : ParticlesToInsert)
		{
			ParticleToIndex.Add(Particle, NextIdx++);
		}
		ParticleArray.Append(ParticlesToInsert);
	}

	template <typename TParticle>
	static void InsertToMapAndArray(TParticle* Particle, TMap<TParticle*, int32>& ParticleToIndex, TArray<TParticle*>& ParticleArray)
	{
		if (ParticleToIndex.Contains(Particle) == false)
			{
			ParticleToIndex.Add(Particle, ParticleArray.Num());
			ParticleArray.Add(Particle);
			}
		}

	template <typename TParticle>
	static void RemoveFromMapAndArray(TParticle* Particle, TMap<TParticle*, int32>& ParticleToIndex, TArray<TParticle*>& ParticleArray)
	{
		if (int32* IdxPtr = ParticleToIndex.Find(Particle))
		{
			int32 Idx = *IdxPtr;
			ParticleArray.RemoveAtSwap(Idx);
			if (Idx < ParticleArray.Num())
			{
				//update swapped element with new index
				ParticleToIndex[ParticleArray[Idx]] = Idx;
			}
			ParticleToIndex.Remove(Particle);
		}
	}

	template <typename TParticle>
	void SerializeMapAndArray(FChaosArchive& Ar, TMap<TParticle*, int32>& ParticleToIndex, TArray<TParticle*>& ParticleArray)
	{
		TArray<TSerializablePtr<TParticle>>& SerializableArray = AsAlwaysSerializableArray(ParticleArray);
		Ar << SerializableArray;

		int32 Idx = 0;
		for (auto Particle : ParticleArray)
		{
			ParticleToIndex.Add(Particle, Idx++);
		}
	}
	
	//should be called whenever particles are added / removed / reordered
	void UpdateViews()
	{
		//build various views. Group SOA types together for better branch prediction
		{
			TArray<TSOAView<TGeometryParticles<T, d>>> TmpArray = { StaticParticles.Get(), KinematicParticles.Get(), DynamicParticles.Get(), DynamicKinematicParticles.Get(), {&NonDisabledClusteredArray} };
			NonDisabledView = MakeParticleView(MoveTemp(TmpArray));
		}
		{
			TArray<TSOAView<TPBDRigidParticles<T, d>>> TmpArray = { DynamicParticles.Get(), {&NonDisabledClusteredArray} };
			NonDisabledDynamicView = MakeParticleView(MoveTemp(TmpArray));
		}
		{
			TArray<TSOAView<TPBDRigidParticles<T, d>>> TmpArray = { {&ActiveParticlesArray} };
			ActiveParticlesView = MakeParticleView(MoveTemp(TmpArray));
		}
		{
			TArray<TSOAView<TGeometryParticles<T, d>>> TmpArray = { StaticParticles.Get(), StaticDisabledParticles.Get(), KinematicParticles.Get(), KinematicDisabledParticles.Get(),
				DynamicParticles.Get(), DynamicDisabledParticles.Get(), DynamicKinematicParticles.Get(), ClusteredParticles.Get() };
			AllParticlesView = MakeParticleView(MoveTemp(TmpArray));
		}
		{
			TArray<TSOAView<TKinematicGeometryParticles<T, d>>> TmpArray = { KinematicParticles.Get(), DynamicKinematicParticles.Get() };
			ActiveKinematicParticlesView = MakeParticleView(MoveTemp(TmpArray));
		}
	}

	//Organized by SOA type
	TUniquePtr<TGeometryParticles<T, d>> StaticParticles;
	TUniquePtr<TGeometryParticles<T, d>> StaticDisabledParticles;

	TUniquePtr<TKinematicGeometryParticles<T, d>> KinematicParticles;
	TUniquePtr<TKinematicGeometryParticles<T, d>> KinematicDisabledParticles;

	TUniquePtr<TPBDRigidParticles<T, d>> DynamicParticles;
	TUniquePtr<TPBDRigidParticles<T, d>> DynamicKinematicParticles;
	TUniquePtr<TPBDRigidParticles<T, d>> DynamicDisabledParticles;

	TUniquePtr<TPBDRigidClusteredParticles<T, d>> ClusteredParticles;

	//Utility structures for maintaining an Active particles view
	TMap<TPBDRigidParticleHandle<T, d>*, int32> ActiveParticlesToIndex;
	TArray<TPBDRigidParticleHandle<T, d>*> ActiveParticlesArray;
	TMap<TPBDRigidClusteredParticleHandle<T, d>*, int32> ActiveClusteredToIndex;
	TArray<TPBDRigidClusteredParticleHandle<T, d>*> ActiveClusteredArray;

	//Utility structures for maintaining a NonDisabled particle view
	TMap<TPBDRigidClusteredParticleHandle<T, d>*, int32> NonDisabledClusteredToIndex;
	TArray<TPBDRigidClusteredParticleHandle<T, d>*> NonDisabledClusteredArray;

	//Particle Views
	TParticleView<TGeometryParticles<T, d>> NonDisabledView;							//all particles that are not disabled
	TParticleView<TPBDRigidParticles<T, d>> NonDisabledDynamicView;						//all dynamic particles that are not disabled
	TParticleView<TPBDRigidParticles<T, d>> ActiveParticlesView;						//all particles that are active
	TParticleView<TGeometryParticles<T, d>> AllParticlesView;							//all particles
	TParticleView<TKinematicGeometryParticles<T, d>> ActiveKinematicParticlesView;		//all kinematic particles that are not disabled

	//Auxiliary data synced with particle handles
	TGeometryParticleHandles<T, d> ParticleHandles;

#if CHAOS_DETERMINISTIC
	int32 BiggestParticleID;
#endif
};
}
