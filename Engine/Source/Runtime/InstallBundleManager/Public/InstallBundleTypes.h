// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/EnumRange.h"

enum class EInstallBundleSourceType : int
{
	Bulk,
	BuildPatchServices,
	Count
};
ENUM_RANGE_BY_COUNT(EInstallBundleSourceType, EInstallBundleSourceType::Count);
INSTALLBUNDLEMANAGER_API const TCHAR* LexToString(EInstallBundleSourceType Type);

enum class EInstallBundleManagerInitState : int
{
	NotInitialized,
	Failed,
	Succeeded
};

enum class EInstallBundleManagerInitResult : int
{
	OK,
	BuildMetaDataNotFound,
	BuildMetaDataDownloadError,
	BuildMetaDataParsingError,
	DistributionRootParseError,
	DistributionRootDownloadError,
	ManifestArchiveError,
	ManifestCreationError,
	ManifestDownloadError,
	BackgroundDownloadsIniDownloadError,
	NoInternetConnectionError,
	ConfigurationError,
	Count
};
INSTALLBUNDLEMANAGER_API const TCHAR* LexToString(EInstallBundleManagerInitResult Result);

// TODO: Needs to be renamed to EInstallBundleState
enum class EBundleState : int
{
	NotInstalled,
	NeedsUpdate,
	NeedsMount,
	Mounted,
	Count,
};
INSTALLBUNDLEMANAGER_API const TCHAR* LexToString(EBundleState Val);

enum class EInstallBundleContentState : int
{
	InitializationError,
	NotInstalled,
	NeedsUpdate,
	UpToDate,
	Count,
};
INSTALLBUNDLEMANAGER_API const TCHAR* LexToString(EInstallBundleContentState State);

struct INSTALLBUNDLEMANAGER_API FInstallBundleContentState
{
	EInstallBundleContentState State = EInstallBundleContentState::InitializationError;
	float Weight = 0.0f;
	TMap<EInstallBundleSourceType, FString> Version;
};

struct INSTALLBUNDLEMANAGER_API FInstallBundleCombinedContentState
{
	EInstallBundleContentState State = EInstallBundleContentState::InitializationError;
	TMap<FName, FInstallBundleContentState> IndividualBundleStates;
	TMap<EInstallBundleSourceType, FString> CurrentVersion;
	uint64 DownloadSize = 0;
	uint64 InstallSize = 0;
	uint64 InstallOverheadSize = 0;
	uint64 FreeSpace = 0;
};

enum class EInstallBundleGetContentStateFlags : uint32
{
	None = 0,
	ForceNoPatching = (1 << 0),
};
ENUM_CLASS_FLAGS(EInstallBundleGetContentStateFlags);

DECLARE_DELEGATE_OneParam(FInstallBundleGetContentStateDelegate, FInstallBundleCombinedContentState);

enum class EInstallBundleRequestInfoFlags : int32
{
	None = 0,
	EnqueuedBundlesForInstall = (1 << 0),
	SkippedAlreadyMountedBundles = (1 << 1),
	SkippedUnknownBundles = (1 << 2),
	InitializationError = (1 << 3), // Can't enqueue because the bundle manager failed to initialize
};
ENUM_CLASS_FLAGS(EInstallBundleRequestInfoFlags);

enum class EInstallBundleResult : int
{
	OK,
	FailedPrereqRequiresLatestClient,
	InstallError,
	InstallerOutOfDiskSpaceError,
	ManifestArchiveError,
	UserCancelledError,
	InitializationError,
	Count,
};
INSTALLBUNDLEMANAGER_API const TCHAR* LexToString(EInstallBundleResult Result);

enum class EInstallBundleRequestFlags : uint32
{
	None = 0,
	CheckForCellularDataUsage = (1 << 0),
	UseBackgroundDownloads = (1 << 1),
	SendNotificationIfDownloadCompletesInBackground = (1 << 2),
	ForceNoPatching = (1 << 3),
	Defaults = UseBackgroundDownloads,
};
ENUM_CLASS_FLAGS(EInstallBundleRequestFlags)

struct FInstallBundleRequestInfo
{
	EInstallBundleRequestInfoFlags InfoFlags = EInstallBundleRequestInfoFlags::None;
	TArray<FName> BundlesQueuedForInstall;
};

struct FInstallBundleSourceRequestResultInfo
{
	FName BundleName;
	EInstallBundleResult Result = EInstallBundleResult::OK;

	// Forward any errors from the underlying implementation for a specific source
	// Currently, these just forward BPT Error info
	FText OptionalErrorText;
	FString OptionalErrorCode;
};

enum class EInstallBundleCancelFlags : int32
{
	None = 0,
	Resumable = (1 << 0),
};
ENUM_CLASS_FLAGS(EInstallBundleCancelFlags);

enum class EInstallBundlePauseFlags : uint32
{
	None = 0,
	OnCellularNetwork = (1 << 0),
	NoInternetConnection = (1 << 1),
	UserPaused = (1 << 2)
};
ENUM_CLASS_FLAGS(EInstallBundlePauseFlags);

struct FInstallBundleSourcePauseInfo
{
	FName BundleName;
	EInstallBundlePauseFlags PauseFlags = EInstallBundlePauseFlags::None;
	bool bDidPauseChange = false;
};
