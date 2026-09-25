/*****************************************************************************
Regression test for the audio-bypass bug fixed in VSTPlugin's locking scheme.

Scope / why this isn't a VSTPlugin integration test
-----------------------------------------------------
The real VSTPlugin (../VSTPlugin.cpp) can't be exercised in isolation: it links
against libobs (obs-module.h) and a concrete grpc_vst_communicatorClient whose
header pulls in real gRPC/protobuf-generated code. There is no existing test
harness in this repo that stubs those out, and building one is a much bigger
project than this fix. This binary has no OBS or gRPC dependency at all.

Instead, this test extracts the actual synchronization *pattern* used by
VSTPlugin -- a std::shared_mutex where the audio thread's process() takes a
shared lock via try_to_lock, and state-read RPCs (getChunk()/setChunk()/
setProgram()/getProgram(), invoked from vst_save()/vst_update()) also take a
shared lock -- and reproduces the reported bug scenario against it:

  "vst_save() calls getChunk() three times, including after settings updates.
   If an audio callback arrives during one of those calls, process() fails its
   try_lock and returns the input without running the VST."

SharedLockEffect below is that fixed pattern. ExclusiveLockEffect is the
pre-fix pattern (a single exclusive lock taken by both process() and the
state-read calls) purely so this test can demonstrate it reproduces the bug --
i.e. this test would have failed against the code before the fix.

Build (standalone, no OBS/gRPC needed):
  g++ -std=c++17 -O2 -pthread lock_regression_test.cpp -o lock_regression_test
  ./lock_regression_test
Or via CMake: cmake -DSLVST_BUILD_TESTS=ON <path to plugins/sl-vst>, then
`ctest` or run the `sl-vst-lock-regression-test` target directly.
*****************************************************************************/

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <thread>

using namespace std::chrono_literals;

// Simulates one ~10ms audio buffer arriving roughly 100 times/sec, and one
// OBS autosave whose getChunk() calls take long enough (e.g. a VST with many
// parameters, each fetched via its own synchronous RPC) to span several of
// those buffers -- exactly the vst_save() scenario from the report.
constexpr auto kAudioBufferInterval = 10ms;
constexpr auto kSaveDuration = 120ms; // >> kAudioBufferInterval, so overlap is guaranteed, not timing luck
constexpr auto kTotalRunTime = 400ms;
constexpr auto kSaveStartDelay = 100ms; // start the save well inside the run, audio thread already going

// ---------------------------------------------------------------------------
// Fixed design (current VSTPlugin.cpp): shared_mutex, shared locks on both
// the audio path and the state-read/write RPCs.
// ---------------------------------------------------------------------------
class SharedLockEffect {
public:
	// Mirrors VSTPlugin::process(): shared try_to_lock, skip this buffer if unavailable.
	bool process()
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return false;
		std::this_thread::sleep_for(1ms); // stand-in for processReplacing()'s RPC
		return true;
	}

	// Mirrors vst_save() -> three getChunk() calls, one of which loops per-parameter
	// over synchronous RPCs -- modeled here as one shared-locked span of kSaveDuration.
	void save() const
	{
		std::shared_lock<std::shared_mutex> lock(m_mutex);
		std::this_thread::sleep_for(kSaveDuration);
	}

private:
	mutable std::shared_mutex m_mutex;
};

// ---------------------------------------------------------------------------
// Pre-fix design: a single exclusive lock shared by process() (try_lock) and
// the state-read RPCs (lock_guard). Reproduces the reported bug.
// ---------------------------------------------------------------------------
class ExclusiveLockEffect {
public:
	bool process()
	{
		std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return false;
		std::this_thread::sleep_for(1ms);
		return true;
	}

	void save() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::this_thread::sleep_for(kSaveDuration);
	}

private:
	mutable std::mutex m_mutex;
};

struct RunResult {
	int attempts = 0;
	int dropped = 0; // process() calls that failed to acquire the lock (unprocessed audio passed through)
};

template<typename Effect> RunResult runScenario()
{
	Effect effect;
	RunResult result;
	std::atomic<bool> stop{false};

	std::thread audioThread([&]() {
		const auto start = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - start < kTotalRunTime) {
			result.attempts++;
			if (!effect.process())
				result.dropped++;
			std::this_thread::sleep_for(kAudioBufferInterval);
		}
	});

	std::thread saveThread([&]() {
		std::this_thread::sleep_for(kSaveStartDelay);
		effect.save();
	});

	audioThread.join();
	saveThread.join();
	return result;
}

int main()
{
	int failures = 0;

	printf("Running audio thread against the FIXED (shared_mutex) locking design...\n");
	RunResult fixed = runScenario<SharedLockEffect>();
	printf("  attempts=%d dropped=%d\n", fixed.attempts, fixed.dropped);
	if (fixed.dropped != 0) {
		printf("  FAIL: expected zero dropped audio buffers during a save with the fixed "
		       "design -- process() was starved, the reported bypass regressed.\n");
		failures++;
	} else {
		printf("  PASS: no audio buffers were bypassed during the save.\n");
	}

	printf("Running audio thread against the PRE-FIX (single exclusive mutex) design...\n");
	RunResult broken = runScenario<ExclusiveLockEffect>();
	printf("  attempts=%d dropped=%d\n", broken.attempts, broken.dropped);
	if (broken.dropped == 0) {
		printf("  FAIL: expected the pre-fix design to drop audio buffers during a save "
		       "(this scenario is supposed to demonstrate the original bug) -- test is not "
		       "actually exercising the regression.\n");
		failures++;
	} else {
		printf("  PASS (expected failure reproduced): pre-fix design dropped %d buffer(s) "
		       "during the save, confirming this test would have caught the original bug.\n",
		       broken.dropped);
	}

	if (failures == 0) {
		printf("\nAll checks passed.\n");
		return 0;
	}

	printf("\n%d check(s) failed.\n", failures);
	return 1;
}
