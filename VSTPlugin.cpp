/*****************************************************************************
Copyright (C) 2016-2017 by Colin Edwards.
Additional Code Copyright (C) 2016-2017 by c3r1c3 <c3r1c3@nevermindonline.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include <vector>

#include "headers/VSTPlugin.h"
#include "win/VstWinDefs.h"
#include "headers/grpc_vst_communicatorClient.h"

#define CBASE64_IMPLEMENTATION
#include "cbase64.h"
#ifdef WIN32
#include <cstringt.h>
#include <windows.h>
#endif
#include <functional>
#include <filesystem>
#include <chrono>

VSTPlugin::VSTPlugin(obs_source_t *sourceContext) : m_sourceContext{sourceContext}, m_effect{nullptr}, m_is_open{false}
{
	memset(m_effectName, 0, sizeof(m_effectName));
	memset(m_vendorString, 0, sizeof(m_vendorString));

	int numChannels = VST_MAX_CHANNELS;
	int blocksize = BLOCK_SIZE;

	m_inputs = (float **)malloc(sizeof(float **) * numChannels);
	m_outputs = (float **)malloc(sizeof(float **) * numChannels);

	for (int channel = 0; channel < numChannels; channel++) {
		m_inputs[channel] = (float *)malloc(sizeof(float *) * blocksize);
		m_outputs[channel] = (float *)malloc(sizeof(float *) * blocksize);
	}
}

VSTPlugin::~VSTPlugin()
{
	{
		std::unique_lock<std::shared_mutex> grd(m_effectStatusMutex);
		m_shuttingDown.store(true, std::memory_order_release);
	}

	while (m_pendingTeardowns.load(std::memory_order_acquire) != 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	int numChannels = VST_MAX_CHANNELS;

	for (int channel = 0; channel < numChannels; channel++) {
		if (m_inputs[channel]) {
			free(m_inputs[channel]);
			m_inputs[channel] = NULL;
		}

		if (m_outputs[channel]) {
			free(m_outputs[channel]);
			m_outputs[channel] = NULL;
		}
	}

	if (m_inputs) {
		free(m_inputs);
		m_inputs = NULL;
	}

	if (m_outputs) {
		free(m_outputs);
		m_outputs = NULL;
	}
}

void VSTPlugin::loadEffectFromPath(std::string path)
{
	std::unique_lock<std::shared_mutex> grd(m_effectStatusMutex);

	if (m_proxyDisconnected || m_effect != nullptr)
		return;

	blog(LOG_DEBUG, "VST Plug-in: loadEffectFromPath from pluginPath %s ", path.c_str());
	m_pluginPath = path;

	unloadEffectLocked();
	m_loadGeneration++;
	loadEffect();

	if (!verifyProxyLocked()) {
		blog(LOG_WARNING, "VST Plug-in: loadEffectFromPath Can't load effect!");
		return;
	}

	// Check plug-in's magic number
	// If incorrect, then the file either was not loaded properly, is not a real VST plug-in, or is otherwise corrupt.
	if (m_remote->getEffectMagic() != kEffectMagic) {
		blog(LOG_WARNING, "VST Plug-in: loadEffectFromPath magic number is bad");
		return;
	}

	m_remote->dispatcher(m_effect.get(), effGetEffectName, 0, 0, m_effectName, 0, 64);
	m_remote->dispatcher(m_effect.get(), effGetVendorString, 0, 0, m_vendorString, 0, 64);
	m_remote->dispatcher(m_effect.get(), effOpen, 0, 0, nullptr, 0.0f, 0);

	// Set some default properties
	auto sampleRate = audio_output_get_sample_rate(obs_get_audio());
	m_remote->dispatcher(m_effect.get(), effSetSampleRate, 0, 0, nullptr, static_cast<float>(sampleRate), 0);

	int blocksize = BLOCK_SIZE;
	m_remote->dispatcher(m_effect.get(), effSetBlockSize, 0, blocksize, nullptr, 0.0f, 0);
	m_remote->dispatcher(m_effect.get(), effMainsChanged, 0, 1, nullptr, 0, 0);

	if (!verifyProxyLocked())
		return;

	if (m_openInterfaceWhenActive)
		openEditorLocked();
}

void VSTPlugin::showErrorPopupAsync(std::string msg)
{
#ifdef WIN32
	std::thread popupThread;
	try {
		popupThread = std::thread([msg = std::move(msg)]() { ::MessageBoxA(NULL, msg.c_str(), "VST Filter Error", MB_ICONERROR | MB_SYSTEMMODAL); });
		popupThread.detach();
	} catch (const std::exception &e) {
		if (popupThread.joinable())
			popupThread.join();
		blog(LOG_ERROR, "VST Plug-in: unable to show error popup asynchronously: %s", e.what());
	}
#else
	blog(LOG_ERROR, "VST Plug-in: %s", msg.c_str());
#endif
}

bool VSTPlugin::verifyProxy()
{
	std::shared_lock<std::shared_mutex> grd(m_effectStatusMutex);
	return verifyProxyLocked();
}

// Caller must hold m_effectStatusMutex (shared or exclusive), so this must never lock
// it itself.
bool VSTPlugin::verifyProxyLocked()
{
	if (m_shuttingDown.load(std::memory_order_acquire) || m_effect == nullptr)
		return false;

	if (m_proxyDisconnected)
		return false;

	if (m_remote != nullptr) {
		if (m_remote->m_connected)
			return true;

		bool expected = false;
		if (m_proxyDisconnected.compare_exchange_strong(expected, true)) {
			showErrorPopupAsync(
				std::filesystem::path(m_pluginPath).filename().string() +
				" has stopped working.\n\nThe filter has been disabled. You may restart the application or recreate the filter to enable it again.");

			const uint64_t generation = m_loadGeneration;
			auto teardownStartState = std::make_shared<std::atomic<int>>(0);
			m_pendingTeardowns.fetch_add(1, std::memory_order_relaxed);
			std::thread teardownThread;
			try {
				teardownThread = std::thread([this, generation, teardownStartState]() {
					for (;;) {
						const int startState = teardownStartState->load(std::memory_order_acquire);
						if (startState == 1)
							break;
						if (startState == 2)
							return;
						std::this_thread::yield();
					}

					try {
						std::unique_lock<std::shared_mutex> grd(m_effectStatusMutex);
						if (m_loadGeneration == generation)
							stopProxy();
					} catch (...) {
						blog(LOG_ERROR, "VST Plug-in: deferred proxy teardown failed");
					}
					m_pendingTeardowns.fetch_sub(1, std::memory_order_release);
				});
				teardownThread.detach();
				teardownStartState->store(1, std::memory_order_release);
			} catch (...) {
				if (teardownThread.joinable()) {
					teardownStartState->store(2, std::memory_order_release);
					teardownThread.join();
				}
				m_pendingTeardowns.fetch_sub(1, std::memory_order_release);
				throw;
			}
		}

		return false;
	}

	return false;
}

void silenceChannel(float **channelData, int numChannels, long numFrames)
{
	for (int channel = 0; channel < numChannels; ++channel) {
		for (long frame = 0; frame < numFrames; ++frame) {
			channelData[channel][frame] = 0.0f;
		}
	}
}

obs_audio_data *VSTPlugin::process(struct obs_audio_data *audio)
{
	std::shared_lock<std::shared_mutex> lock(m_effectStatusMutex, std::try_to_lock);
	if (!lock.owns_lock())
		return audio;

	if (m_effect != nullptr && m_remote != nullptr) {
		uint32_t passes = (audio->frames + BLOCK_SIZE - 1) / BLOCK_SIZE;
		uint32_t extra = audio->frames % BLOCK_SIZE;

		for (uint32_t pass = 0; pass < passes; pass++) {
			uint32_t frames = pass == passes - 1 && extra ? extra : BLOCK_SIZE;
			silenceChannel(m_outputs, VST_MAX_CHANNELS, BLOCK_SIZE);

			float *adata[VST_MAX_CHANNELS];

			for (size_t d = 0; d < VST_MAX_CHANNELS; d++) {
				if (audio->data[d] != nullptr)
					adata[d] = ((float *)audio->data[d]) + (pass * BLOCK_SIZE);
				else
					adata[d] = m_inputs[d];
			};

			m_remote->processReplacing(m_effect.get(), adata, m_outputs, frames, VST_MAX_CHANNELS);

			if (!verifyProxyLocked())
				return audio;

			for (size_t c = 0; c < VST_MAX_CHANNELS; c++) {
				if (audio->data[c] != nullptr) {
					for (size_t i = 0; i < frames; i++)
						adata[c][i] = m_outputs[c][i];
				}
			}
		}
	}

	return audio;
}

void VSTPlugin::unloadEffect()
{
	std::unique_lock<std::shared_mutex> grd(m_effectStatusMutex);
	unloadEffectLocked();
}

// Assumes the caller already holds an exclusive lock on m_effectStatusMutex.
void VSTPlugin::unloadEffectLocked()
{
	m_windowCreated = false;
	m_proxyDisconnected = false;

	if (m_effect != nullptr && m_remote != nullptr) {
		m_remote->dispatcher(m_effect.get(), effStopProcess, 0, 0, nullptr, 0, 0);
		m_remote->dispatcher(m_effect.get(), effMainsChanged, 0, 0, nullptr, 0, 0);
		m_remote->dispatcher(m_effect.get(), effClose, 0, 0, nullptr, 0.0f, 0);
	}

	stopProxy();
}

bool VSTPlugin::hasEffect()
{
	std::shared_lock<std::shared_mutex> grd(m_effectStatusMutex);
	return m_effect != nullptr;
}

bool VSTPlugin::isEditorOpen()
{
	return m_is_open;
}

bool VSTPlugin::hasWindowOpen()
{
	if (m_windowCreated == false)
		return false;

	return m_is_open;
}

void VSTPlugin::openEditor()
{
	std::unique_lock<std::shared_mutex> grd(m_effectStatusMutex);
	openEditorLocked();
}

// Assumes the caller already holds an exclusive lock on m_effectStatusMutex.
void VSTPlugin::openEditorLocked()
{
	if (isProxyDisconnected())
		return;

	if (m_effect != nullptr && m_remote != nullptr) {
		if (!m_windowCreated) {
			m_remote->sendHwndMsg(m_effect.get(), VstProxy::WM_USER_MSG::WM_USER_CREATE_WINDOW);
			m_windowCreated = true;
		}

		m_remote->sendHwndMsg(m_effect.get(), VstProxy::WM_USER_MSG::WM_USER_SHOW);
		m_is_open = true;
	}

	verifyProxyLocked();
}

void VSTPlugin::hideEditor()
{
	std::unique_lock<std::shared_mutex> grd(m_effectStatusMutex);

	if (isProxyDisconnected())
		return;

	if (m_windowCreated && m_effect != nullptr && m_remote != nullptr) {
		m_remote->sendHwndMsg(m_effect.get(), VstProxy::WM_USER_MSG::WM_USER_HIDE);
		m_is_open = false;
	}

	verifyProxyLocked();
}

void VSTPlugin::closeEditor()
{
	std::unique_lock<std::shared_mutex> grd(m_effectStatusMutex);

	m_is_open = false;

	if (m_windowCreated && m_effect != nullptr && m_remote != nullptr) {
		m_remote->sendHwndMsg(m_effect.get(), VstProxy::WM_USER_MSG::WM_USER_CLOSE);
		m_windowCreated = false;
	}

	verifyProxyLocked();
}

std::string VSTPlugin::getChunk(VstChunkType type)
{
	std::shared_lock<std::shared_mutex> grd(m_effectStatusMutex);

	cbase64_encodestate encoder;
	std::string encodedData;

	if (m_effect == nullptr || m_remote == nullptr) {
		blog(LOG_WARNING, "VST Plug-in: getChunk, no effect loaded");
		return "";
	}

	cbase64_init_encodestate(&encoder);

	const int effectFlags = m_remote->getEffectFlags();
	if (effectFlags & effFlagsProgramChunks && type != VstChunkType::Parameter) {
		void *buf = nullptr;
		intptr_t chunkSize = m_remote->dispatcher(m_effect.get(), effGetChunk, int(type), 0, &buf, 0.0, 0);

		if (!verifyProxyLocked())
			return "";

		if (!buf || chunkSize == 0) {
			blog(LOG_WARNING, "VST Plug-in: effGetChunk failed");
			return "";
		}

		encodedData.resize(cbase64_calc_encoded_length(uint32_t(chunkSize)));

		int blockEnd = cbase64_encode_block((const unsigned char *)buf, uint32_t(chunkSize), &encodedData[0], &encoder);
		cbase64_encode_blockend(&encodedData[blockEnd], &encoder);
		return encodedData;
	} else if (!(effectFlags & effFlagsProgramChunks) && type == VstChunkType::Parameter) {
		std::vector<float> params;
		const int numParams = m_remote->getEffectNumParams();

		for (int i = 0; i < numParams; i++) {
			float parameter = m_remote->getParameter(m_effect.get(), i);
			params.push_back(parameter);
		}

		if (!verifyProxyLocked())
			return "";

		if (!params.empty()) {
			const char *bytes = reinterpret_cast<const char *>(&params[0]);
			size_t size = sizeof(float) * params.size();

			encodedData.resize(cbase64_calc_encoded_length(uint32_t(size)));

			int blockEnd = cbase64_encode_block((const unsigned char *)bytes, uint32_t(size), &encodedData[0], &encoder);
			cbase64_encode_blockend(&encodedData[blockEnd], &encoder);
		} else {
			blog(LOG_WARNING, "VST Plug-in: getChunk params.empty()");
		}

		return encodedData;
	}

	// Not every VST uses every type, we don't need to know about that in the logs
	//blog(LOG_INFO, "VST Plug-in: getChunk option unavailable");
	return "";
}

void VSTPlugin::setChunk(VstChunkType type, std::string &data)
{
	// Shared lock -- see getChunk().
	std::shared_lock<std::shared_mutex> grd(m_effectStatusMutex);

	if (data.size() == 0) {
		blog(LOG_DEBUG, "VST Plug-in: setChunk with empty data chunk ignored");
		return;
	}

	cbase64_decodestate decoder;
	cbase64_init_decodestate(&decoder);
	std::string decodedData;

	if (m_effect == nullptr || m_remote == nullptr) {
		blog(LOG_ERROR, "VST Plug-in: setChunk effect is not ready yet");
		return;
	}

	decodedData.resize(cbase64_calc_decoded_length(data.data(), uint32_t(data.size())));
	cbase64_decode_block(data.data(), uint32_t(data.size()), (unsigned char *)&decodedData[0], &decoder);
	data = "";

	const int effectFlags = m_remote->getEffectFlags();
	if (effectFlags & effFlagsProgramChunks && type != VstChunkType::Parameter) {
		auto ret = m_remote->dispatcher(m_effect.get(), effSetChunk, type == VstChunkType::Bank ? 0 : 1, decodedData.length(), &decodedData[0], 0.0,
						decodedData.length());
	} else if (!(effectFlags & effFlagsProgramChunks) && type == VstChunkType::Parameter) {
		const char *p_chars = &decodedData[0];
		const float *p_floats = reinterpret_cast<const float *>(p_chars);

		int size = uint32_t(decodedData.length()) / sizeof(float);

		std::vector<float> params(p_floats, p_floats + size);
		const int numParams = m_remote->getEffectNumParams();

		if (params.size() != (size_t)numParams) {
			blog(LOG_WARNING, "VST Plug-in: setChunk wrong number of params");
			return;
		}

		// Bound by params.size(), not numParams: every RPC reply (including these
		// setParameter calls and a concurrent processReplacing) refreshes the cached
		// metadata.
		for (size_t i = 0; i < params.size(); i++)
			m_remote->setParameter(m_effect.get(), int(i), params[i]);
	}

	verifyProxyLocked();
}

void VSTPlugin::setProgram(const int programNumber)
{
	// Shared lock -- see getChunk().
	std::shared_lock<std::shared_mutex> grd(m_effectStatusMutex);

	if (m_effect == nullptr || m_remote == nullptr) {
		blog(LOG_ERROR, "VST Plug-in: setProgram effect is not ready yet");
		return;
	}

	if (programNumber >= 0 && programNumber < m_remote->getEffectNumPrograms()) {
		intptr_t ret = m_remote->dispatcher(m_effect.get(), effSetProgram, 0, programNumber, nullptr, 0.0f, 0);
		blog(LOG_ERROR, "VST Plug-in: setProgram get %lld from effSetProgram", ret);
	} else {
		blog(LOG_ERROR, "VST Plug-in: setProgram Failed to load program, number was outside possible program range.");
	}

	verifyProxyLocked();
}

int VSTPlugin::getProgram()
{
	// Shared lock -- see getChunk().
	std::shared_lock<std::shared_mutex> grd(m_effectStatusMutex);

	if (m_effect == nullptr || m_remote == nullptr) {
		blog(LOG_WARNING, "VST Plug-in: getProgram effect is not ready yet");
		return 0;
	}

	intptr_t ret = m_remote->dispatcher(m_effect.get(), effGetProgram, 0, 0, nullptr, 0.0f, 0);
	verifyProxyLocked();
	return static_cast<int>(ret);
}

void VSTPlugin::getSourceNames()
{
	/* Only call inside the vst_filter_audio function! */
	m_sourceName = obs_source_get_name(obs_filter_get_target(m_sourceContext));
	m_filterName = obs_source_get_name(m_sourceContext);
}

std::string VSTPlugin::getPluginPath()
{
	return m_pluginPath;
}
