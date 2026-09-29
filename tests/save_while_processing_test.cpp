// Regression test: saving filter settings must not interrupt live audio.
//
// vst_save() calls getChunk(), which makes blocking RPCs to the proxy. getChunk()
// takes a shared lifetime lock so process() can continue while settings are
// saved; changing that lock to exclusive would make process() bypass audio.
// This test runs the real VSTPlugin and grpc_vst_communicatorClient against an
// in-process fake proxy server:
//
//  - an "audio thread" calls process() in a loop; the fake proxy halves every
//    sample, so a bypassed buffer is easy to spot (1.0 instead of 0.5).
//  - the main thread runs the same calls as vst_save().
//  - the fake effGetChunk handler does not reply until the audio thread has made
//    kBuffersDuringChunk processReplacing calls *during* that effGetChunk. If
//    process() is blocked by the save, that never happens and the handler times
//    out, so the test is driven by events rather than sleeps.
//  - buffers are only counted if the whole process() call happened while an
//    effGetChunk was blocked in the fake proxy (i.e. while getChunk() held the
//    lock), so the counts don't depend on thread scheduling.
//
// Build with -DBUILD_TESTING=ON and run: ctest -L sl-vst --output-on-failure
//
// To confirm the test catches the bug, temporarily make getChunk() take
// ExclusiveLock instead of std::shared_lock; the test should then fail with
// timed-out effGetChunk calls and bypassed buffers.

#include "../headers/VSTPlugin.h"
#include "../headers/grpc_vst_communicatorClient.h"

#include <obs_vst_api.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// VSTPlugin-win.cpp calls obs_current_module(), which the plugin normally gets
// from obs-vst.cpp.
OBS_DECLARE_MODULE()

using Clock = std::chrono::steady_clock;

namespace {

constexpr int kBuffersDuringChunk = 20;
// Only reached if the test is failing; generous so a slow machine can't fail it.
constexpr auto kChunkTimeout = std::chrono::seconds(10);
constexpr float kInputSample = 1.0f;
constexpr float kProcessedSample = 0.5f;

int g_failures = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAILED: %s (line %d)\n", #cond, __LINE__); \
			g_failures++;                                                    \
		}                                                                        \
	} while (0)

class FakeVstProxy final : public grpc_vst_communicator::Service {
public:
	std::atomic<int> processCalls{0};
	std::atomic<int> chunkCalls{0};
	std::atomic<int> chunkTimeouts{0};
	// True while an effGetChunk handler is waiting, so getChunk() is inside its RPC.
	std::atomic<bool> chunkInFlight{false};
	// Report a chunk size larger than the data actually sent, like a buggy proxy.
	std::atomic<bool> overstateChunkSize{false};

	grpc::Status com_grpc_dispatcher(grpc::ServerContext *, const grpc_dispatcher_Request *request, grpc_dispatcher_Reply *reply) override
	{
		fillMetadata(reply);

		if (request->param1() == effGetChunk && overstateChunkSize) {
			const std::string chunk = "short";
			reply->set_ptr_data(chunk);
			reply->set_returnval(int64_t(chunk.size() + 4096));
		} else if (request->param1() == effGetChunk) {
			chunkCalls++;

			// Set before taking the snapshot below; see the processedDuringChunk check.
			chunkInFlight = true;
			const int start = processCalls.load();
			const auto deadline = Clock::now() + kChunkTimeout;
			while (processCalls.load() - start < kBuffersDuringChunk) {
				if (Clock::now() > deadline) {
					chunkTimeouts++;
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			chunkInFlight = false;

			const std::string chunk = "fake-chunk-data";
			reply->set_ptr_data(chunk);
			reply->set_returnval(int64_t(chunk.size()));
		}

		return grpc::Status::OK;
	}

	grpc::Status com_grpc_processReplacing(grpc::ServerContext *, const grpc_processReplacing_Request *request, grpc_processReplacing_Reply *reply) override
	{
		// Copy through a float vector rather than casting the byte buffers to float*.
		const std::string &in = request->adata();
		std::vector<float> samples(in.size() / sizeof(float));
		std::memcpy(samples.data(), in.data(), samples.size() * sizeof(float));
		for (float &sample : samples)
			sample *= kProcessedSample;
		const std::string out(reinterpret_cast<const char *>(samples.data()), samples.size() * sizeof(float));

		reply->set_frames(request->frames());
		reply->set_arraysize(request->arraysize());
		reply->set_adata(in);
		reply->set_bdata(out);
		fillMetadata(reply);

		processCalls++;
		return grpc::Status::OK;
	}

	grpc::Status com_grpc_setParameter(grpc::ServerContext *, const grpc_setParameter_Request *, grpc_setParameter_Reply *reply) override
	{
		fillMetadata(reply);
		return grpc::Status::OK;
	}

	grpc::Status com_grpc_getParameter(grpc::ServerContext *, const grpc_getParameter_Request *, grpc_getParameter_Reply *reply) override
	{
		fillMetadata(reply);
		return grpc::Status::OK;
	}

	grpc::Status com_grpc_updateAEffect(grpc::ServerContext *, const grpc_updateAEffect_Request *, grpc_updateAEffect_Reply *reply) override
	{
		fillMetadata(reply);
		return grpc::Status::OK;
	}

	grpc::Status com_grpc_sendHwndMsg(grpc::ServerContext *, const grpc_sendHwndMsg_Request *, grpc_sendHwndMsg_Reply *) override
	{
		return grpc::Status::OK;
	}

	grpc::Status com_grpc_stopServer(grpc::ServerContext *, const grpc_stopServer_Request *, grpc_stopServer_Reply *) override { return grpc::Status::OK; }

private:
	// Every reply refreshes the client's cached metadata, so all of them must
	// report program-chunk support or getChunk() would stop calling effGetChunk.
	template<typename Reply> static void fillMetadata(Reply *reply)
	{
		reply->set_magic(kEffectMagic);
		reply->set_flags(effFlagsProgramChunks);
		reply->set_numprograms(1);
		reply->set_numparams(0);
		reply->set_numinputs(2);
		reply->set_numoutputs(2);
	}
};

} // namespace

class VSTPluginTestAccess {
public:
	// Stands in for loadEffect(): attach a client connected to the fake proxy.
	static bool attachProxy(VSTPlugin &plugin, const std::string &address)
	{
#ifdef WIN32
		plugin.m_winServer = {};
#endif
		plugin.m_effect = std::make_unique<AEffect>();
		plugin.m_remote = std::make_unique<grpc_vst_communicatorClient>(grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
		plugin.m_remote->updateAEffect(plugin.m_effect.get());
		return plugin.m_remote->m_connected;
	}

	// Drop the client without unloadEffect(), which would try to stop a real
	// proxy process.
	static void detachProxy(VSTPlugin &plugin)
	{
		plugin.m_remote.reset();
		plugin.m_effect.reset();
	}
};

namespace {

struct AudioStats {
	std::atomic<int> processed{0};
	std::atomic<int> processedDuringChunk{0};
	std::atomic<int> bypassedDuringChunk{0};
};

void runAudioThread(VSTPlugin &plugin, const FakeVstProxy &proxy, std::atomic<bool> &stop, AudioStats &stats)
{
	std::vector<float> left(BLOCK_SIZE), right(BLOCK_SIZE);

	while (!stop) {
		std::fill(left.begin(), left.end(), kInputSample);
		std::fill(right.begin(), right.end(), kInputSample);

		obs_audio_data audio{};
		audio.data[0] = reinterpret_cast<uint8_t *>(left.data());
		audio.data[1] = reinterpret_cast<uint8_t *>(right.data());
		audio.frames = BLOCK_SIZE;

		const bool chunkBefore = proxy.chunkInFlight.load();
		plugin.process(&audio);
		const bool chunkAfter = proxy.chunkInFlight.load();

		bool processed = true;
		for (int i = 0; i < BLOCK_SIZE; i++) {
			if (left[i] != kProcessedSample || right[i] != kProcessedSample) {
				processed = false;
				break;
			}
		}

		if (processed)
			stats.processed++;

		// Only count calls that ran entirely while getChunk() was inside its RPC.
		if (chunkBefore && chunkAfter)
			(processed ? stats.processedDuringChunk : stats.bypassedDuringChunk)++;

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

} // namespace

int main()
{
	FakeVstProxy proxy;
	int port = 0;

	grpc::ServerBuilder builder;
	builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
	builder.RegisterService(&proxy);
	std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
	if (!server || port == 0) {
		std::fprintf(stderr, "FAILED: could not start fake proxy server\n");
		return 1;
	}

	int result = 0;
	{
		VSTPlugin plugin(nullptr);
		if (!VSTPluginTestAccess::attachProxy(plugin, "127.0.0.1:" + std::to_string(port))) {
			std::fprintf(stderr, "FAILED: client could not connect to fake proxy\n");
			server->Shutdown();
			return 1;
		}

		std::atomic<bool> stop{false};
		AudioStats stats;
		std::thread audioThread(runAudioThread, std::ref(plugin), std::cref(proxy), std::ref(stop), std::ref(stats));

		// Make sure audio is actually being processed before the save starts, so
		// a broken setup fails here with a clear message instead of later.
		const auto warmupDeadline = Clock::now() + kChunkTimeout;
		while (stats.processed < 5 && Clock::now() < warmupDeadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(stats.processed >= 5);

		// Same calls, in the same order, as vst_save().
		const std::string bank = plugin.getChunk(VstChunkType::Bank);
		const std::string program = plugin.getChunk(VstChunkType::Program);
		const std::string parameter = plugin.getChunk(VstChunkType::Parameter);
		const bool proxyOk = plugin.verifyProxy();

		stop = true;
		audioThread.join();

		std::printf("effGetChunk calls: %d (timed out: %d)\n", proxy.chunkCalls.load(), proxy.chunkTimeouts.load());
		std::printf("buffers during effGetChunk: %d processed, %d bypassed\n", stats.processedDuringChunk.load(), stats.bypassedDuringChunk.load());

		CHECK(!bank.empty());
		CHECK(!program.empty());
		CHECK(parameter.empty()); // program-chunk plug-ins don't save a parameter chunk
		CHECK(proxyOk);
		CHECK(proxy.chunkCalls == 2);
		CHECK(proxy.chunkTimeouts == 0);
		// Each effGetChunk waits for kBuffersDuringChunk server-side calls. The audio
		// thread makes them one at a time, so all but the first (which may have started
		// before chunkInFlight was set) and the last (which may return after it was
		// cleared) ran entirely inside the window.
		CHECK(stats.processedDuringChunk >= 2 * (kBuffersDuringChunk - 2));
		CHECK(stats.bypassedDuringChunk == 0);

		// getChunk() must not read past the data the proxy actually sent.
		proxy.overstateChunkSize = true;
		CHECK(plugin.getChunk(VstChunkType::Bank).empty());

		VSTPluginTestAccess::detachProxy(plugin);
	}

	server->Shutdown();

	if (g_failures == 0) {
		std::printf("PASSED: audio kept processing during save\n");
	} else {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		result = 1;
	}
	return result;
}
