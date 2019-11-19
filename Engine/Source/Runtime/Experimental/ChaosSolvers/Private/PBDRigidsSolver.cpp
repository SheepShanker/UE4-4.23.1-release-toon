// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "PBDRigidsSolver.h"

#include "Async/AsyncWork.h"
#include "Chaos/ChaosArchive.h"
#include "Chaos/PBDCollisionConstraintUtil.h"
#include "Chaos/Utilities.h"
#include "Chaos/ChaosDebugDraw.h"
#include "ChaosStats.h"
#include "ChaosSolversModule.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicsProxy/SkeletalMeshPhysicsProxy.h"
#include "PhysicsProxy/StaticMeshPhysicsProxy.h"
#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "PhysicsProxy/FieldSystemPhysicsProxy.h"
#include "EventDefaults.h"

DEFINE_LOG_CATEGORY_STATIC(LogPBDRigidsSolverSolver, Log, All);

// DebugDraw CVars
#if CHAOS_DEBUG_DRAW
int32 ChaosSolverDrawCollisions = 0;
int32 ChaosSolverDrawBPBounds = 0;
FAutoConsoleVariableRef CVarChaosSolverDrawCollisions(TEXT("p.Chaos.Solver.DebugDrawCollisions"), ChaosSolverDrawCollisions, TEXT("Draw Collisions (0 = never; 1 = end of frame)."));
FAutoConsoleVariableRef CVarChaosSolverDrawBPBounds(TEXT("p.Chaos.Solver.DrawBPBounds"), ChaosSolverDrawBPBounds, TEXT("Draw bounding volumes inside the broadphase (0 = never; 1 = end of frame)."));
#endif

float ChaosSolverCollisionDefaultIterationsCVar = 1;
FAutoConsoleVariableRef CVarChaosSolverCollisionDefaultIterations(TEXT("p.ChaosSolverCollisionDefaultIterations"), ChaosSolverCollisionDefaultIterationsCVar, TEXT("Default collision iterations for the solver.[def:1]"));


namespace Chaos
{

	class AdvanceOneTimeStepTask : public FNonAbandonableTask
	{
		friend class FAutoDeleteAsyncTask<AdvanceOneTimeStepTask>;
	public:
		AdvanceOneTimeStepTask(
			FPBDRigidsSolver* Scene,
			const float DeltaTime)
			: MSolver(Scene)
			, MDeltaTime(DeltaTime)
		{
			UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("AdvanceOneTimeStepTask::AdvanceOneTimeStepTask()"));
		}

		void DoWork()
		{
			UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("AdvanceOneTimeStepTask::DoWork()"));

			{
				//SCOPE_CYCLE_COUNTER(STAT_UpdateParams);
				//MSolver->ParameterUpdateCallback(MSolver->GetSolverTime());
			}

			{
				//SCOPE_CYCLE_COUNTER(STAT_BeginFrame);
				//MSolver->StartFrameCallback(MDeltaTime, MSolver->GetSolverTime());
			}

			{
				SCOPE_CYCLE_COUNTER(STAT_EvolutionAndKinematicUpdate);
				while (MDeltaTime > MSolver->GetMaxDeltaTime())
				{
					//MSolver->ForceUpdateCallback(MSolver->GetSolverTime());
					//MSolver->GetEvolution()->ReconcileIslands();
					//MSolver->KinematicUpdateCallback(MSolver->GetMaxDeltaTime(), MSolver->GetSolverTime());
					MSolver->GetEvolution()->AdvanceOneTimeStep(MSolver->GetMaxDeltaTime());
					MDeltaTime -= MSolver->GetMaxDeltaTime();
				}
				//MSolver->ForceUpdateCallback(MSolver->GetSolverTime());
				//MSolver->GetEvolution()->ReconcileIslands();
				//MSolver->KinematicUpdateCallback(MDeltaTime, MSolver->GetSolverTime());
				MSolver->GetEvolution()->AdvanceOneTimeStep(MDeltaTime);
			}

			{
				SCOPE_CYCLE_COUNTER(STAT_EventDataGathering);
				{
					SCOPE_CYCLE_COUNTER(STAT_FillProducerData);
					MSolver->GetEventManager()->FillProducerData(MSolver);
				}
				{
					SCOPE_CYCLE_COUNTER(STAT_FlipBuffersIfRequired);
					MSolver->GetEventManager()->FlipBuffersIfRequired();
				}
			}

			{
				SCOPE_CYCLE_COUNTER(STAT_EndFrame);
				MSolver->GetEvolution()->EndFrame(MDeltaTime);
			}

			MSolver->GetSolverTime() += MDeltaTime;
			MSolver->GetCurrentFrame()++;
			MSolver->PostTickDebugDraw();
		}

	protected:

		TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(AdvanceOneTimeStepTask, STATGROUP_ThreadPoolAsyncTasks);
		}

		FPBDRigidsSolver* MSolver;
		float MDeltaTime;
		TSharedPtr<FCriticalSection> PrevLock, CurrentLock;
		TSharedPtr<FEvent> PrevEvent, CurrentEvent;
	};




	FPBDRigidsSolver::FPBDRigidsSolver(const EMultiBufferMode BufferingModeIn)
		: Super(BufferingModeIn)
		, CurrentFrame(0)
		, MTime(0.0)
		, MLastDt(0.0)
		, MMaxDeltaTime(0.0)
		, TimeStepMultiplier(1.0)
		, bEnabled(false)
		, bHasFloor(true)
		, bIsFloorAnalytic(false)
		, FloorHeight(0.f)
		, MEvolution(new FPBDRigidsEvolution(Particles, ChaosSolverCollisionDefaultIterationsCVar, BufferingModeIn == Chaos::EMultiBufferMode::Single))
		, MEventManager(new FEventManager(BufferingModeIn))
		, MSolverEventFilters(new FSolverEventFilters())
		, MActiveParticlesBuffer(new FActiveParticlesBuffer(BufferingModeIn))
		, MCurrentLock(new FCriticalSection())
	{
		UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("PBDRigidsSolver::PBDRigidsSolver()"));
		Reset();
	}

	float MaxBoundsForTree = 10000;
	FAutoConsoleVariableRef CVarMaxBoundsForTree(
		TEXT("p.MaxBoundsForTree"),
		MaxBoundsForTree,
		TEXT("The max bounds before moving object into a large objects structure. Only applies on object registration")
		TEXT(""),
		ECVF_Default);

	void FPBDRigidsSolver::RegisterObject(TGeometryParticle<float, 3>* GTParticle)
	{
		UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("FPBDRigidsSolver::RegisterObject()"));

		// Make sure this particle doesn't already have a proxy
		checkSlow(GTParticle->Proxy == nullptr);

		if (GTParticle->Geometry() && GTParticle->Geometry()->HasBoundingBox() && GTParticle->Geometry()->BoundingBox().Extents().Max() >= MaxBoundsForTree)
		{
			GTParticle->SetSpatialIdx(FSpatialAccelerationIdx{ 1,0 });
		}

		// Grab the particle's type
		const EParticleType InParticleType = GTParticle->ObjectType();

		IPhysicsProxyBase* ProxyBase = nullptr;
		// NOTE: Do we really need this list of proxies if we can just
		// access them through the GTParticles list?
		
		Chaos::FParticleData* ProxyData;

		if (!ensure(GTParticle->IsParticleValid()))
		{
			return;
		}

		// Make a physics proxy, giving it our particle and particle handle
		if (InParticleType == EParticleType::Dynamic)
		{
			auto Proxy = new FRigidParticlePhysicsProxy(GTParticle->AsDynamic(), nullptr);
			RigidParticlePhysicsProxies.Add((FRigidParticlePhysicsProxy*)Proxy);
			ProxyData = Proxy->NewData();
			ProxyBase = Proxy;
		}
		else if (InParticleType == EParticleType::Kinematic)
		{
			auto Proxy = new FKinematicGeometryParticlePhysicsProxy(GTParticle->AsKinematic(), nullptr);
			KinematicGeometryParticlePhysicsProxies.Add((FKinematicGeometryParticlePhysicsProxy*)Proxy);
			ProxyData = Proxy->NewData();
			ProxyBase = Proxy;
		}
		else // Assume it's a static (geometry) if it's not dynamic or kinematic
		{
			auto Proxy = new FGeometryParticlePhysicsProxy(GTParticle, nullptr);
			GeometryParticlePhysicsProxies.Add((FGeometryParticlePhysicsProxy*)Proxy);
			ProxyData = Proxy->NewData();
			ProxyBase = Proxy;
		}

		ProxyBase->SetSolver(this);

		// Associate the proxy with the particle
		GTParticle->Proxy = ProxyBase;

		//enqueue onto physics thread for finalizing registration
		FChaosSolversModule::GetModule()->GetDispatcher()->EnqueueCommandImmediate(this, [GTParticle, InParticleType, ProxyBase, ProxyData](FPBDRigidsSolver* Solver)
		{
			UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("FPBDRigidsSolver::RegisterObject() ~ Dequeue"));

			// Create a handle for the new particle
			TGeometryParticleHandle<float, 3>* Handle;

			// Insert the particle ptr into the auxiliary array
			//
			// TODO: Is this array even necessary?
			//       Proxy should be able to map back to Particle.
			//

			if (InParticleType == EParticleType::Dynamic)
			{
				Handle = Solver->Particles.CreateDynamicParticles(1)[0];
				auto Proxy = static_cast<FRigidParticlePhysicsProxy*>(ProxyBase);
				Proxy->SetHandle(Handle->AsDynamic());
				Proxy->PushToPhysicsState(ProxyData);
			}
			else if (InParticleType == EParticleType::Kinematic)
			{
				Handle = Solver->Particles.CreateKinematicParticles(1)[0];
				auto Proxy = static_cast<FKinematicGeometryParticlePhysicsProxy*>(ProxyBase);
				Proxy->SetHandle(Handle->AsKinematic());
				Proxy->PushToPhysicsState(ProxyData);
			}
			else // Assume it's a static (geometry) if it's not dynamic or kinematic
			{
				Handle = Solver->Particles.CreateStaticParticles(1)[0];

				auto Proxy = static_cast<FGeometryParticlePhysicsProxy*>(ProxyBase);
				Proxy->SetHandle(Handle);
				Proxy->PushToPhysicsState(ProxyData);
			}

			Handle->GTGeometryParticle() = GTParticle;

			Solver->GetEvolution()->CreateParticle(Handle);

			delete ProxyData;
		});
	}

	void FPBDRigidsSolver::UnregisterObject(TGeometryParticle<float, 3>* GTParticle)
	{
		UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("FPBDRigidsSolver::UnregisterObject()"));

		// Get the proxy associated with this particle
		IPhysicsProxyBase* InProxy = GTParticle->Proxy;
		check(InProxy);

		// Grab the particle's type
		const EParticleType InParticleType = GTParticle->ObjectType();

		// remove the proxy from the invalidation list
		RemoveDirtyProxy(GTParticle->Proxy);

		// Null out the particle's proxy pointer
		GTParticle->Proxy = nullptr;

		// Remove the proxy from the GT proxy map
		if (InParticleType == EParticleType::Dynamic)
		{
			RigidParticlePhysicsProxies.Remove((FRigidParticlePhysicsProxy*)InProxy);
		}
		else if (InParticleType == EParticleType::Kinematic)
		{
			KinematicGeometryParticlePhysicsProxies.Remove((FKinematicGeometryParticlePhysicsProxy*)InProxy);
		}
		else
		{
			GeometryParticlePhysicsProxies.Remove((FGeometryParticlePhysicsProxy*)InProxy);
		}

		// Enqueue a command to remove the particle and delete the proxy
		FChaosSolversModule::GetModule()->GetDispatcher()->EnqueueCommandImmediate(this, [InProxy, InParticleType](FPBDRigidsSolver* Solver)
		{
			UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("FPBDRigidsSolver::UnregisterObject() ~ Dequeue"));

			// Get the physics thread-handle from the proxy, and then delete the proxy.
			//
			// NOTE: We have to delete the proxy from its derived version, because the
			// base destructor is protected. This makes everything just a bit uglier,
			// maybe that extra safety is not needed if we continue to contain all
			// references to proxy instances entirely in Chaos?
			TGeometryParticleHandle<float, 3>* Handle;
			if (InParticleType == Chaos::EParticleType::Dynamic)
			{
				auto Proxy = (FRigidParticlePhysicsProxy*)InProxy;
				Handle = Proxy->GetHandle();
				delete Proxy;
			}
			else if (InParticleType == Chaos::EParticleType::Kinematic)
			{
				auto Proxy = (FKinematicGeometryParticlePhysicsProxy*)InProxy;
				Handle = Proxy->GetHandle();
				delete Proxy;
			}
			else
			{
				auto Proxy = (FGeometryParticlePhysicsProxy*)InProxy;
				Handle = Proxy->GetHandle();
				delete Proxy;
			}

			// Use the handle to destroy the particle data
			Solver->GetEvolution()->DestroyParticle(Handle);
		});
	}

	bool FPBDRigidsSolver::IsSimulating() const
	{
		for (FGeometryParticlePhysicsProxy* Obj : GeometryParticlePhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FKinematicGeometryParticlePhysicsProxy* Obj : KinematicGeometryParticlePhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FRigidParticlePhysicsProxy* Obj : RigidParticlePhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FSkeletalMeshPhysicsProxy* Obj : SkeletalMeshPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FStaticMeshPhysicsProxy* Obj : StaticMeshPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FGeometryCollectionPhysicsProxy* Obj : GeometryCollectionPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		for (FFieldSystemPhysicsProxy* Obj : FieldSystemPhysicsProxies)
			if (Obj->IsSimulating())
				return true;
		return false;
	}



	void FPBDRigidsSolver::Reset()
	{

		UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("PBDRigidsSolver::Reset()"));

		MTime = 0;
		MLastDt = 0.0f;
		bEnabled = false;
		CurrentFrame = 0;
		MMaxDeltaTime = 1;
		TimeStepMultiplier = 1;
		MEvolution = TUniquePtr<FPBDRigidsEvolution>(new FPBDRigidsEvolution(Particles, ChaosSolverCollisionDefaultIterationsCVar, BufferMode == EMultiBufferMode::Single)); 

		FEventDefaults::RegisterSystemEvents(*GetEventManager());
	}

	void FPBDRigidsSolver::ChangeBufferMode(Chaos::EMultiBufferMode InBufferMode)
	{
		// This seems unused inside the solver? #BH
		BufferMode = InBufferMode;
	}

	void FPBDRigidsSolver::AdvanceSolverBy(float DeltaTime)
	{
		UE_LOG(LogPBDRigidsSolverSolver, Verbose, TEXT("PBDRigidsSolver::Tick(%3.5f)"), DeltaTime);
		if (bEnabled)
		{
			MLastDt = DeltaTime;

			int32 NumTimeSteps = (int32)(1.f*TimeStepMultiplier);
			float dt = FMath::Min(DeltaTime, float(5.f / 30.f)) / (float)NumTimeSteps;
			for (int i = 0; i < NumTimeSteps; i++)
			{
				AdvanceOneTimeStepTask(this, DeltaTime).DoWork();
			}
		}

	}

	void FPBDRigidsSolver::SyncEvents_GameThread()
	{
		GetEventManager()->DispatchEvents();
	}

	template<typename ProxyType>
	void PushPhysicsStateExec(FPBDRigidsSolver * Solver, ProxyType* Proxy, Chaos::IDispatcher* Dispatcher)
	{
		if (Chaos::FParticleData* ProxyData = Proxy->NewData())
		{
			if (auto* RigidHandle = static_cast<Chaos::TGeometryParticleHandle<float, 3>*>(Proxy->GetHandle()))
			{
				if (Dispatcher)
				{
					Dispatcher->EnqueueCommandImmediate([Proxy, ProxyData, Solver](Chaos::FPersistentPhysicsTask* PhysThread)
					{
						// make sure the handle is still valid
						if (auto* Handle = static_cast<Chaos::TGeometryParticleHandle<float, 3>*>(Proxy->GetHandle()))
						{
							Solver->GetEvolution()->DirtyParticle(*Handle);
							Proxy->PushToPhysicsState(ProxyData);
						}
						delete ProxyData;
					});
				}
				else
				{
					Solver->GetEvolution()->DirtyParticle(*RigidHandle);
					Proxy->PushToPhysicsState(ProxyData);
					delete ProxyData;
				}
				Solver->RemoveDirtyProxy(Proxy);
			}

			Proxy->ClearAccumulatedData();
		}
	}


	void FPBDRigidsSolver::PushPhysicsState(IDispatcher* Dispatcher)
	{
		TArray< IPhysicsProxyBase *> DirtyProxiesArray = DirtyProxiesSet.Array();
		for (auto & Proxy : DirtyProxiesArray)
		{
			switch (Proxy->GetType())
			{
			case EPhysicsProxyType::SingleRigidParticleType:
				PushPhysicsStateExec(this, static_cast<FSingleParticlePhysicsProxy< Chaos::TPBDRigidParticle<float, 3> >*>(Proxy), Dispatcher);
				break;
			case EPhysicsProxyType::SingleKinematicParticleType:
				PushPhysicsStateExec(this, static_cast<FSingleParticlePhysicsProxy< Chaos::TKinematicGeometryParticle<float, 3> >*>(Proxy), Dispatcher);
				break;
			case EPhysicsProxyType::SingleGeometryParticleType:
				PushPhysicsStateExec(this, static_cast<FSingleParticlePhysicsProxy< Chaos::TGeometryParticle<float, 3> >*>(Proxy), Dispatcher);
				break;
			default:
				ensure("Unknown proxy type in physics solver.");
			}
		}
	}


	void FPBDRigidsSolver::BufferPhysicsResults()
	{
		//ensure(IsInPhysicsThread());

		for (Chaos::TGeometryParticleHandleImp < float, 3, false>& ActiveObject : GetParticles().GetActiveParticlesView())
		{
			switch (ActiveObject.Type)
			{
			case Chaos::EParticleType::Dynamic:
				((FRigidParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->BufferPhysicsResults();
				break;
			case Chaos::EParticleType::Kinematic:
				((FKinematicGeometryParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->BufferPhysicsResults();
				break;
			case Chaos::EParticleType::Static:
				((FGeometryParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->BufferPhysicsResults();
				break;
			default:
				check(false);
			}
		}
	}

	void FPBDRigidsSolver::FlipBuffers()
	{
		//ensure(IsInPhysicsThread());

		for (Chaos::TGeometryParticleHandleImp < float, 3, false>& ActiveObject : GetParticles().GetActiveParticlesView())
		{
			switch (ActiveObject.Type)
			{
			case Chaos::EParticleType::Dynamic:
				((FRigidParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->FlipBuffer();
				break;
			case Chaos::EParticleType::Kinematic:
				((FKinematicGeometryParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->FlipBuffer();
				break;
			case Chaos::EParticleType::Static:
				((FGeometryParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->FlipBuffer();
				break;
			default:
				check(false);
			}
		}
	}

	void FPBDRigidsSolver::UpdateGameThreadStructures()
	{
		//ensure(IsInGameThread());

		for (Chaos::TGeometryParticleHandleImp < float, 3, false>& ActiveObject : GetParticles().GetActiveParticlesView())
		{
			switch (ActiveObject.Type)
			{
			case Chaos::EParticleType::Dynamic:
				((FRigidParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->PullFromPhysicsState();
				break;
			case Chaos::EParticleType::Kinematic:
				((FKinematicGeometryParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->PullFromPhysicsState();
				break;
			case Chaos::EParticleType::Static:
				((FGeometryParticlePhysicsProxy*)(ActiveObject.GTGeometryParticle()->Proxy))->PullFromPhysicsState();
				break;
			default:
				check(false);
			}
		}
	}


	void FPBDRigidsSolver::PostTickDebugDraw() const
	{
#if CHAOS_DEBUG_DRAW
		if (ChaosSolverDrawCollisions == 1) {
			DebugDraw::DrawCollisions(TRigidTransform<float, 3>(), GetEvolution()->GetCollisionConstraints(), 1.f);
		}
#endif
	}


	void FPBDRigidsSolver::UpdateMaterial(Chaos::FMaterialHandle InHandle, const Chaos::FChaosPhysicsMaterial& InNewData)
	{
		*SimMaterials.Get(InHandle.InnerHandle) = InNewData;
	}

	void FPBDRigidsSolver::CreateMaterial(Chaos::FMaterialHandle InHandle, const Chaos::FChaosPhysicsMaterial& InNewData)
	{
		ensure(SimMaterials.Create(InNewData) == InHandle.InnerHandle);
	}

	void FPBDRigidsSolver::DestroyMaterial(Chaos::FMaterialHandle InHandle)
	{
		SimMaterials.Destroy(InHandle.InnerHandle);
	}

	void FPBDRigidsSolver::SyncQueryMaterials()
	{
		TSolverQueryMaterialScope<ELockType::Write> Scope(this);
		QueryMaterials = SimMaterials;
	}

}; // namespace Chaos
