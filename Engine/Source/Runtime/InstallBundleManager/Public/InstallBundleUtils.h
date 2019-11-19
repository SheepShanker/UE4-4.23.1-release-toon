// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "InstallBundleTypes.h"
#include "Async/AsyncWork.h"
#include "Misc/EmbeddedCommunication.h"

namespace InstallBundleUtil
{
	// Returns the app version in the same format as BPS versions
	INSTALLBUNDLEMANAGER_API FString GetAppVersion();

	INSTALLBUNDLEMANAGER_API bool HasInternetConnection(ENetworkConnectionType ConnectionType);

	INSTALLBUNDLEMANAGER_API bool StateSignifiesNeedsInstall(EBundleState StateIn);

	INSTALLBUNDLEMANAGER_API bool StateSignifiesNeedsInstall(EInstallBundleContentState StateIn);

	INSTALLBUNDLEMANAGER_API const TCHAR* GetInstallBundlePauseReason(EInstallBundlePauseFlags Flags);

	// It would really be nice to have these in core
	template<class EnumType>
	constexpr auto& CastAsUnderlying(EnumType &Type)
	{
		static_assert(TIsEnum<EnumType>::Value, "");
		using UnderType = __underlying_type(EnumType);
		return *reinterpret_cast<UnderType*>(&Type);
	}

	template<class EnumType>
	constexpr const auto& CastAsUnderlying(const EnumType &Type)
	{
		static_assert(TIsEnum<EnumType>::Value, "");
		using UnderType = __underlying_type(EnumType);
		return *reinterpret_cast<const UnderType*>(&Type);
	}

	template<class EnumType>
	constexpr auto CastToUnderlying(EnumType Type)
	{
		static_assert(TIsEnum<EnumType>::Value, "");
		using UnderType = __underlying_type(EnumType);
		return static_cast<UnderType>(Type);
	}

	// Keep the engine awake via RAII when running as an embedded app
	class INSTALLBUNDLEMANAGER_API FInstallBundleManagerKeepAwake : public FEmbeddedKeepAwake
	{
		static FName Tag;
		static FName TagWithRendering;
	public:
		FInstallBundleManagerKeepAwake(bool bNeedsRendering = false)
			: FEmbeddedKeepAwake(bNeedsRendering ? TagWithRendering : Tag, bNeedsRendering) {}
	};

	class INSTALLBUNDLEMANAGER_API FInstallBundleWork : public FNonAbandonableTask
	{
	public:
		FInstallBundleWork() = default;

		FInstallBundleWork(TUniqueFunction<void()> InWork, TUniqueFunction<void()> InOnComplete)
			: WorkFunc(MoveTemp(InWork))
			, OnCompleteFunc(MoveTemp(InOnComplete))
		{}

		void DoWork()
		{
			if (WorkFunc)
			{
				WorkFunc();
			}
		}

		void CallOnComplete()
		{
			if (OnCompleteFunc)
			{
				OnCompleteFunc();
			}
		}

		FORCEINLINE TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(FInstallBundleWork, STATGROUP_ThreadPoolAsyncTasks);
		}

	private:
		TUniqueFunction<void()> WorkFunc;
		TUniqueFunction<void()> OnCompleteFunc;
	};

	using FInstallBundleTask = FAsyncTask<FInstallBundleWork>;

	INSTALLBUNDLEMANAGER_API void StartInstallBundleAsyncIOTask(TArray<TUniquePtr<FInstallBundleTask>>& Tasks, TUniqueFunction<void()> WorkFunc, TUniqueFunction<void()> OnComplete);

	INSTALLBUNDLEMANAGER_API void FinishInstallBundleAsyncIOTasks(TArray<TUniquePtr<FInstallBundleTask>>& Tasks);

	INSTALLBUNDLEMANAGER_API void CleanupInstallBundleAsyncIOTasks(TArray<TUniquePtr<FInstallBundleTask>>& Tasks);
}
