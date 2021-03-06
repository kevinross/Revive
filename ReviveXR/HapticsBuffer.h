#pragma once

#include "OVR_CAPI.h"

#include <atomic>
#include <mutex>

class HapticsBuffer
{
public:
	HapticsBuffer();
	~HapticsBuffer() { }

	void AddSamples(const ovrHapticsBuffer* buffer);
	float GetSample();
	ovrHapticsPlaybackState GetState();

private:
	// Lock-less circular buffer, single producer/consumer
	std::atomic_uint8_t m_ReadIndex;
	std::atomic_uint8_t m_WriteIndex;
	uint8_t m_Buffer[OVR_HAPTICS_BUFFER_SAMPLES_MAX];
};

static_assert(OVR_HAPTICS_BUFFER_SAMPLES_MAX == 256, "The Haptics Buffer is designed for 256 samples");
