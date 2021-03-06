// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreTypes.h"
#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

#if PLATFORM_UNIX
#include "Unix/UnixSignalHeartBeat.h"
#endif

/**
 * Our own local clock.
 * Platforms that support suspend/resume have problems where a suspended title acts like
 * a long hitch, causing the hang detector to fire incorrectly when the title is resumed.
 *
 * To solve this, we accumulate our own time on the hang detector thread.
 * When the title is suspended, this thread is also suspended, and the local clock stops.
 * The delta is clamped so if we are resumed, the clock continues from where it left off.
 */
class CORE_API FThreadHeartBeatClock
{
	uint64 CurrentCycles;
	uint64 LastRealTickCycles;
	const uint64 MaxTimeStepCycles;

public:
	FThreadHeartBeatClock(double InMaxTimeStep);

	void Tick();
	double Seconds();
};

/**
 * Thread heartbeat check class.
 * Used by crash handling code to check for hangs.
 */
class CORE_API FThreadHeartBeat : public FRunnable
{
	static FThreadHeartBeat* Singleton;

	/** Holds per-thread info about the heartbeat */
	struct FHeartBeatInfo
	{
		FHeartBeatInfo()
			: LastHeartBeatTime(0.0)
			, LastHangTime(0.0)
			, SuspendedCount(0)
			, HangDuration(0)
		{}

		/** Time we last received a heartbeat for the current thread */
		double LastHeartBeatTime;
		/** Time we last detected a hang due to lack of heartbeats for the current thread */
		double LastHangTime;
		/** Suspended counter */
		int32 SuspendedCount;
		/** The timeout for this thread */
		double HangDuration;

		/** Suspends this thread's heartbeat */
		void Suspend()
		{
			SuspendedCount++;
		}
		/** Resumes this thread's heartbeat */
		void Resume(double CurrentTime)
		{
			check(SuspendedCount > 0);
			if (--SuspendedCount == 0)
			{
				LastHeartBeatTime = CurrentTime;
			}
		}
	};
	/** Thread to run the worker FRunnable on */
	FRunnableThread* Thread;
	/** Stops this thread */
	FThreadSafeCounter StopTaskCounter;
	/** Synch object for the heartbeat */
	FCriticalSection HeartBeatCritical;
	/** Keeps track of the last heartbeat time for threads */
	TMap<uint32, FHeartBeatInfo> ThreadHeartBeat;
	/** The last heartbeat time for the rendering or RHI thread frame present. */
	FHeartBeatInfo PresentHeartBeat;
	/** True if heartbeat should be measured */
	FThreadSafeBool bReadyToCheckHeartbeat;
	/** Max time the thread is allowed to not send the heartbeat*/
	double ConfigHangDuration;
	double CurrentHangDuration;
	double ConfigPresentDuration;
	double CurrentPresentDuration;
	double HangDurationMultiplier;
	
	/** CRC of the last hang's callstack */
	uint32 LastHangCallstackCRC;
	/** Id of the last thread that hung */
	uint32 LastHungThreadId;

	bool bHangsAreFatal;

	/** Global suspended count */
	FThreadSafeCounter GlobalSuspendCount;

	FThreadHeartBeatClock Clock;

	FThreadHeartBeat();
	virtual ~FThreadHeartBeat();

	void InitSettings();

	void FORCENOINLINE OnHang(double HangDuration, uint32 ThreadThatHung);
	void FORCENOINLINE OnPresentHang(double HangDuration);

public:

	enum EConstants
	{
		/** Invalid thread Id used by CheckHeartBeat */
		InvalidThreadId = (uint32)-1,

		/** Id used to track presented frames (supported platforms only). */
		PresentThreadId = (uint32)-2
	};

	/** Gets the heartbeat singleton */
	static FThreadHeartBeat& Get();
	static FThreadHeartBeat* GetNoInit();

	/** Begin measuring heartbeat */
	void Start();
	/** Called from a thread once per frame to update the heartbeat time */
	void HeartBeat(bool bReadConfig = false);
	/** Called from the rendering or RHI thread when the platform RHI presents a frame (supported platforms only). */
	void PresentFrame();
	/** Called by a supervising thread to check the threads' health */
	uint32 CheckHeartBeat(double& OutHangDuration);
	/** Called by a thread when it's no longer expecting to be ticked */
	void KillHeartBeat();
	/** 
	 * Suspend heartbeat measuring for the current thread if the thread has already had a heartbeat 
	 * @param bAllThreads If true, suspends heartbeat for all threads, not only the current one
	 */
	void SuspendHeartBeat(bool bAllThreads = false);
	/** 
	 * Resume heartbeat measuring for the current thread 
	 * @param bAllThreads If true, resumes heartbeat for all threads, not only the current one
	 */
	void ResumeHeartBeat(bool bAllThreads = false);

	/**
	* Returns true/false if this thread is currently performing heartbeat monitoring
	*/
	bool IsBeating();

	/** 
	 * Sets a multiplier to the hang duration (>= 1.0).
	 * Can be used to extend the duration during loading screens etc.
	 */
	void SetDurationMultiplier(double NewMultiplier);

	/*
	* Get the Id of the last thread to trigger the hang detector.
	* Returns InvalidThreadId if hang detector has not been triggered.
	*/
	uint32 GetLastHungThreadId() const { return LastHungThreadId; }

	//~ Begin FRunnable Interface.
	virtual bool Init();
	virtual uint32 Run();
	virtual void Stop();
	//~ End FRunnable Interface
};

/** Suspends heartbeat measuring for the current thread in the current scope */
struct FSlowHeartBeatScope
{
private:
	bool bSuspendedAllThreads;
public:
	FORCEINLINE FSlowHeartBeatScope(bool bAllThreads = false)
		: bSuspendedAllThreads(bAllThreads)
	{
		if (FThreadHeartBeat* HB = FThreadHeartBeat::GetNoInit())
		{
			HB->SuspendHeartBeat(bSuspendedAllThreads);
		}
	}
	FORCEINLINE ~FSlowHeartBeatScope()
	{
		if (FThreadHeartBeat* HB = FThreadHeartBeat::GetNoInit())
		{
			HB->ResumeHeartBeat(bSuspendedAllThreads);
		}
	}
};

// When 1, performs a full symbol lookup in hitch call stacks, otherwise only
// a backtrace is performed and the raw addresses are written to the log.
#ifndef LOOKUP_SYMBOLS_IN_HITCH_STACK_WALK
#define LOOKUP_SYMBOLS_IN_HITCH_STACK_WALK 0
#endif

class CORE_API FGameThreadHitchHeartBeatThreaded : public FRunnable
{
	static FGameThreadHitchHeartBeatThreaded* Singleton;

	/** Thread to run the worker FRunnable on */
	FRunnableThread* Thread;
	/** Stops this thread */
	FThreadSafeCounter StopTaskCounter;
	/** Synch object for the heartbeat */
	FCriticalSection HeartBeatCritical;
	/** Max time the game thread is allowed to not send the heartbeat*/
	float HangDuration;

	bool bWalkStackOnHitch;

	double FirstStartTime;
	double FrameStartTime;

	int32 SuspendedCount;

	FThreadHeartBeatClock Clock;

#if LOOKUP_SYMBOLS_IN_HITCH_STACK_WALK
	static const SIZE_T StackTraceSize = 65535;
	ANSICHAR StackTrace[StackTraceSize];
#else
	static const uint32 MaxStackDepth = 128;
	uint64 StackTrace[MaxStackDepth];
#endif

	void InitSettings();

	FGameThreadHitchHeartBeatThreaded();
	virtual ~FGameThreadHitchHeartBeatThreaded();

public:

	enum EConstants
	{
		/** Invalid thread Id used by CheckHeartBeat */
		InvalidThreadId = (uint32)-1
	};

	/** Gets the heartbeat singleton */
	static FGameThreadHitchHeartBeatThreaded& Get();
	static FGameThreadHitchHeartBeatThreaded* GetNoInit();

	/**
	* Called at the start of a frame to register the time we are looking to detect a hitch
	*/
	void FrameStart(bool bSkipThisFrame = false);

	double GetFrameStartTime();
	double GetCurrentTime();

	/**
	* Suspend heartbeat hitch detection. Must call ResumeHeartBeat later to resume.
	*/
	void SuspendHeartBeat();

	/**
	* Resume heartbeat hitch detection. Call only after first calling SuspendHeartBeat.
	*/
	void ResumeHeartBeat();

	// No-op, used in FUnixSignalGameHitchHeartBeat
	void Restart() {}

	//~ Begin FRunnable Interface.
	virtual bool Init();
	virtual uint32 Run();
	virtual void Stop();
	//~ End FRunnable Interface
};

#if PLATFORM_UNIX
typedef FUnixSignalGameHitchHeartBeat FGameThreadHitchHeartBeat;
#else
typedef FGameThreadHitchHeartBeatThreaded FGameThreadHitchHeartBeat;
#endif

/** Suspends hitch detection in the current scope */
struct FDisableHitchDetectorScope
{
	FORCEINLINE FDisableHitchDetectorScope()
	{
		FGameThreadHitchHeartBeat::Get().SuspendHeartBeat();
	}
	FORCEINLINE ~FDisableHitchDetectorScope()
	{
		FGameThreadHitchHeartBeat::Get().ResumeHeartBeat();
	}
};
