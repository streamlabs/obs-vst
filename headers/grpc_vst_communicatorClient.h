#pragma once

#include <atomic>
#include <mutex>

#include <obs_vst_api.grpc.pb.h>
#include <grpcpp/grpcpp.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

class AEffect;

class grpc_vst_communicatorClient {
public:
	struct EffectMetadata {
		int magic = 0;
		int numPrograms = 0;
		int numParams = 0;
		int numInputs = 0;
		int numOutputs = 0;
		int flags = 0;
		int initialDelay = 0;
		int uniqueID = 0;
		int version = 0;
	};

	grpc_vst_communicatorClient(std::shared_ptr<Channel> channel);

	// When ptr points to a null void*, the reply data is malloc()ed into it and its
	// size is written to allocated_size (if given). The caller must free() it.
	intptr_t dispatcher(AEffect *a, int b, int c, intptr_t d, void *ptr, float f, size_t ptr_size, size_t *allocated_size = nullptr);

	float getParameter(AEffect *a, int b);

	void setParameter(AEffect *a, int b, float c);
	void processReplacing(AEffect *a, float **adata, float **bdata, int frames, int arraySize);
	void sendHwndMsg(AEffect *a, int msgType);
	void updateAEffect(AEffect *a);
	void stopServer(AEffect *a);
	int getEffectMagic() const;
	int getEffectFlags() const;
	int getEffectNumParams() const;
	int getEffectNumPrograms() const;

	std::atomic<bool> m_connected{false};

private:
	void syncEffectMetadata(const EffectMetadata &metadata);

	std::unique_ptr<grpc_vst_communicator::Stub> stub_;
	mutable std::mutex m_effectMetadataMutex;
	EffectMetadata m_effectMetadata;
};
