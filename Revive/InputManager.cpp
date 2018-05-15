#include "InputManager.h"
#include "Session.h"
#include "SessionDetails.h"
#include "Settings.h"
#include "SettingsManager.h"
#include "CompositorBase.h"
#include "OVR_CAPI.h"
#include "REV_Math.h"
#include "rcu_ptr.h"

#include <openvr.h>
#include <Windows.h>
#include <Xinput.h>
#include <assert.h>

InputManager::InputManager()
	: m_InputDevices()
	, m_LastPoses()
{
	for (ovrPoseStatef& pose : m_LastPoses)
		pose.ThePose = OVR::Posef::Identity();

	// TODO: This might change if a new HMD is connected (unlikely)
	m_fVsyncToPhotons = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SecondsFromVsyncToPhotons_Float);

	vr::VRActionSetHandle_t handle;
	vr::EVRInputError err = vr::VRInput()->GetActionSetHandle("/actions/touch", &handle);
	if (err == vr::VRInputError_None)
		m_InputDevices.push_back(new XboxGamepad(handle));
	err = vr::VRInput()->GetActionSetHandle("/actions/touch", &handle);
	if (err == vr::VRInputError_None)
		m_InputDevices.push_back(new OculusRemote(handle));
	err = vr::VRInput()->GetActionSetHandle("/actions/touch", &handle);
	if (err == vr::VRInputError_None)
	{
		m_InputDevices.push_back(new OculusTouch(handle, vr::TrackedControllerRole_LeftHand));
		m_InputDevices.push_back(new OculusTouch(handle, vr::TrackedControllerRole_RightHand));
	}

	UpdateConnectedControllers();
}

InputManager::~InputManager()
{
	for (InputDevice* device : m_InputDevices)
		delete device;
}

void InputManager::UpdateConnectedControllers()
{
	uint32_t types = 0;
	for (InputDevice* device : m_InputDevices)
	{
		if (device->IsConnected())
			types |= device->GetType();
	}
	ConnectedControllers = types;
}

void InputManager::UpdateInputState()
{
	std::vector<vr::VRActiveActionSet_t> sets;
	for (InputDevice* device : m_InputDevices)
	{
		if (ConnectedControllers & device->GetType())
		{
			vr::VRActiveActionSet_t set;
			set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
			set.ulActionSet = device->ActionSet;
			sets.push_back(set);
		}
	}

	vr::VRInput()->UpdateActionState(sets.data(), sizeof(vr::VRActiveActionSet_t), (uint32_t)sets.size());
}

ovrResult InputManager::SetControllerVibration(ovrSession session, ovrControllerType controllerType, float frequency, float amplitude)
{
	// Clamp the input
	frequency = min(max(frequency, 0.0f), 1.0f);
	amplitude = min(max(amplitude, 0.0f), 1.0f);

	for (InputDevice* device : m_InputDevices)
	{
		if (controllerType & device->GetType() && ConnectedControllers & device->GetType())
			device->SetVibration(frequency, amplitude);
	}

	return ovrSuccess;
}

ovrResult InputManager::GetInputState(ovrSession session, ovrControllerType controllerType, ovrInputState* inputState)
{
	memset(inputState, 0, sizeof(ovrInputState));

	uint32_t types = 0;
	for (InputDevice* device : m_InputDevices)
	{
		if (controllerType & device->GetType() && ConnectedControllers & device->GetType())
		{
			if (device->GetInputState(session, inputState))
				types |= device->GetType();
		}
	}

	inputState->TimeInSeconds = ovr_GetTimeInSeconds();
	inputState->ControllerType = (ovrControllerType)types;
	return ovrSuccess;
}

ovrResult InputManager::SubmitControllerVibration(ovrSession session, ovrControllerType controllerType, const ovrHapticsBuffer* buffer)
{
	for (InputDevice* device : m_InputDevices)
	{
		if (controllerType & device->GetType() && ConnectedControllers & device->GetType())
			device->SubmitVibration(buffer);
	}

	return ovrSuccess;
}

ovrResult InputManager::GetControllerVibrationState(ovrSession session, ovrControllerType controllerType, ovrHapticsPlaybackState* outState)
{
	memset(outState, 0, sizeof(ovrHapticsPlaybackState));

	for (InputDevice* device : m_InputDevices)
	{
		if (controllerType & device->GetType() && ConnectedControllers & device->GetType())
			device->GetVibrationState(outState);
	}

	return ovrSuccess;
}

ovrTouchHapticsDesc InputManager::GetTouchHapticsDesc(ovrControllerType controllerType)
{
	ovrTouchHapticsDesc desc = { 0 };

	if (controllerType & ovrControllerType_Touch)
	{
		desc.SampleRateHz = REV_HAPTICS_SAMPLE_RATE;
		desc.SampleSizeInBytes = sizeof(uint8_t);
		desc.SubmitMaxSamples = OVR_HAPTICS_BUFFER_SAMPLES_MAX;
		desc.SubmitMinSamples = 1;
		desc.SubmitOptimalSamples = 20;
		desc.QueueMinSizeToAvoidStarvation = 5;
	}

	return desc;
}

unsigned int InputManager::TrackedDevicePoseToOVRStatusFlags(vr::TrackedDevicePose_t pose)
{
	unsigned int result = 0;

	if (pose.bPoseIsValid)
	{
		if (pose.bDeviceIsConnected)
			result |= ovrStatus_OrientationTracked;
		if (pose.eTrackingResult != vr::TrackingResult_Calibrating_OutOfRange &&
			pose.eTrackingResult != vr::TrackingResult_Running_OutOfRange)
			result |= ovrStatus_PositionTracked;
	}

	return result;
}

ovrPoseStatef InputManager::TrackedDevicePoseToOVRPose(vr::TrackedDevicePose_t pose, ovrPoseStatef& lastPose, double time)
{
	ovrPoseStatef result = { OVR::Posef::Identity() };
	if (!pose.bPoseIsValid)
		return result;

	OVR::Matrix4f matrix = REV::Matrix4f(pose.mDeviceToAbsoluteTracking);

	// Make sure the orientation stays in the same hemisphere as the previous orientation, this prevents
	// linear interpolations from suddenly flipping the long way around in Oculus Medium.
	OVR::Quatf q(matrix);
	q.EnsureSameHemisphere(lastPose.ThePose.Orientation);

	result.ThePose.Orientation = q;
	result.ThePose.Position = matrix.GetTranslation();
	result.AngularVelocity = (REV::Vector3f)pose.vAngularVelocity;
	result.LinearVelocity = (REV::Vector3f)pose.vVelocity;
	result.AngularAcceleration = ((REV::Vector3f)pose.vAngularVelocity - lastPose.AngularVelocity) / float(time - lastPose.TimeInSeconds);
	result.LinearAcceleration = ((REV::Vector3f)pose.vVelocity - lastPose.LinearVelocity) / float(time - lastPose.TimeInSeconds);
	result.TimeInSeconds = time;

	// Store the last pose
	lastPose = result;

	return result;
}

void InputManager::GetTrackingState(ovrSession session, ovrTrackingState* outState, double absTime)
{
	if (session->Details->UseHack(SessionDetails::HACK_WAIT_IN_TRACKING_STATE))
		vr::VRCompositor()->WaitGetPoses(nullptr, 0, nullptr, 0);

	// Calculate the relative prediction time
	float relTime = 0.0f;
	if (absTime > 0.0f)
		relTime = float(absTime - ovr_GetTimeInSeconds());
	if (relTime > 0.0f)
		relTime += m_fVsyncToPhotons;

	// Get the device poses
	vr::ETrackingUniverseOrigin origin = session->TrackingOrigin;
	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
	vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(origin, relTime, poses, vr::k_unMaxTrackedDeviceCount);

	// Convert the head pose
	outState->HeadPose = TrackedDevicePoseToOVRPose(poses[vr::k_unTrackedDeviceIndex_Hmd], m_LastPoses[vr::k_unTrackedDeviceIndex_Hmd], absTime);
	outState->StatusFlags = TrackedDevicePoseToOVRStatusFlags(poses[vr::k_unTrackedDeviceIndex_Hmd]);

	// Convert the hand poses
	rcu_ptr<InputSettings> settings = session->Settings->Input;
	vr::TrackedDeviceIndex_t hands[] = { vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand),
		vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) };
	for (int i = 0; i < ovrHand_Count; i++)
	{
		vr::TrackedDeviceIndex_t deviceIndex = hands[i];
		if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid)
		{
			outState->HandPoses[i].ThePose = OVR::Posef::Identity();
			continue;
		}

		vr::TrackedDevicePose_t pose;
		vr::VRSystem()->ApplyTransform(&pose, &poses[deviceIndex], &settings->TouchOffset[i]);
		outState->HandPoses[i] = TrackedDevicePoseToOVRPose(pose, m_LastPoses[deviceIndex], absTime);
		outState->HandStatusFlags[i] = TrackedDevicePoseToOVRStatusFlags(poses[deviceIndex]);
	}

	// TODO: Calibrate the origin ourselves instead of relying on OpenVR.
	outState->CalibratedOrigin.Orientation = OVR::Quatf::Identity();
	outState->CalibratedOrigin.Position = OVR::Vector3f();
}

ovrResult InputManager::GetDevicePoses(ovrTrackedDeviceType* deviceTypes, int deviceCount, double absTime, ovrPoseStatef* outDevicePoses)
{
	// Get the device poses
	vr::ETrackingUniverseOrigin space = vr::VRCompositor()->GetTrackingSpace();
	float relTime = float(absTime - ovr_GetTimeInSeconds());
	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
	vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(space, relTime, poses, vr::k_unMaxTrackedDeviceCount);

	// Get the generic tracker indices
	vr::TrackedDeviceIndex_t trackers[vr::k_unMaxTrackedDeviceCount];
	vr::VRSystem()->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_GenericTracker, trackers, vr::k_unMaxTrackedDeviceCount);

	for (int i = 0; i < deviceCount; i++)
	{
		// Get the index for device types we recognize
		uint32_t index = vr::k_unTrackedDeviceIndexInvalid;
		switch (deviceTypes[i])
		{
		case ovrTrackedDevice_HMD:
			index = vr::k_unTrackedDeviceIndex_Hmd;
			break;
		case ovrTrackedDevice_LTouch:
			index = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
			break;
		case ovrTrackedDevice_RTouch:
			index = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
			break;
		case ovrTrackedDevice_Object0:
			index = trackers[0];
			break;
		case ovrTrackedDevice_Object1:
			index = trackers[1];
			break;
		case ovrTrackedDevice_Object2:
			index = trackers[2];
			break;
		case ovrTrackedDevice_Object3:
			index = trackers[3];
			break;
		}

		// If the tracking index is invalid it will fall outside of the range of the array
		if (index >= vr::k_unMaxTrackedDeviceCount)
			return ovrError_DeviceUnavailable;
		outDevicePoses[i] = TrackedDevicePoseToOVRPose(poses[index], m_LastPoses[index], absTime);
	}

	return ovrSuccess;
}

/* Controller child-classes */

void InputManager::OculusTouch::HapticsThread(OculusTouch* device)
{
	std::chrono::microseconds freq(std::chrono::seconds(1));
	freq /= REV_HAPTICS_SAMPLE_RATE;

	while (device->m_bHapticsRunning)
	{
		vr::TrackedDeviceIndex_t touch = vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(device->Role);

		uint16_t duration = (uint16_t)((float)freq.count() * device->m_Haptics.GetSample());
		if (duration > 0)
			vr::VRSystem()->TriggerHapticPulse(touch, 0, duration);

		std::this_thread::sleep_for(freq);
	}
}

InputManager::OculusTouch::OculusTouch(vr::VRActionSetHandle_t actionSet, vr::ETrackedControllerRole role)
	: InputDevice(actionSet)
	, Role(role)
	, m_bHapticsRunning(true)
{
	vr::VRInput()->GetActionHandle("/actions/touch/in/Button_Enter", &m_Button_Enter);

#define GET_HANDED_ACTION(x, r, l) vr::VRInput()->GetActionHandle( \
	role == vr::TrackedControllerRole_RightHand ? "/actions/touch/in/" #r : "/actions/touch/in/" #l, x)

	GET_HANDED_ACTION(&m_Button_AX, Button_A, Button_X);
	GET_HANDED_ACTION(&m_Button_BY, Button_B, Button_Y);
	GET_HANDED_ACTION(&m_Button_Thumb, Button_RThumb, Button_LThumb);

	GET_HANDED_ACTION(&m_Touch_AX, Touch_A, Touch_X);
	GET_HANDED_ACTION(&m_Touch_BY, Touch_B, Touch_Y);
	GET_HANDED_ACTION(&m_Touch_Thumb, Touch_RThumb, Touch_LThumb);
	GET_HANDED_ACTION(&m_Touch_ThumbRest, Touch_RThumbRest, Touch_LThumbRest);
	GET_HANDED_ACTION(&m_Touch_IndexTrigger, Touch_RIndexTrigger, Touch_LIndexTrigger);
	GET_HANDED_ACTION(&m_Touch_IndexPointing, Touch_RIndexPointing, Touch_LIndexPointing);
	GET_HANDED_ACTION(&m_Touch_ThumbUp, Touch_RThumbUp, Touch_LThumbUp);

	GET_HANDED_ACTION(&m_IndexTrigger, RIndexTrigger, LIndexTrigger);
	GET_HANDED_ACTION(&m_HandTrigger, RHandTrigger, LHandTrigger);
	GET_HANDED_ACTION(&m_Thumbstick, RThumbstick, LThumbstick);

	GET_HANDED_ACTION(&m_Button_IndexTrigger, Button_RIndexTrigger, Button_LIndexTrigger);
	GET_HANDED_ACTION(&m_Button_HandTrigger, Button_RHandTrigger, Button_LHandTrigger);

	GET_HANDED_ACTION(&m_Vibration, RVibration, LVibration);

#undef GET_HANDED_ACTION

	m_HapticsThread = std::thread(HapticsThread, this);
}

InputManager::OculusTouch::~OculusTouch()
{
	m_bHapticsRunning = false;
	m_HapticsThread.join();
}

ovrControllerType InputManager::OculusTouch::GetType()
{
	return ovrControllerType_Touch;
}

bool InputManager::OculusTouch::IsConnected() const
{
	// Check if a Vive controller is available
	uint32_t controllerCount = vr::VRSystem()->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_Controller, nullptr, 0);

	// If only one controller is available, the Oculus Remote is connected
	return controllerCount > 1;
}

// TODO: Make GetInputState reentrant (thread-safe).
bool InputManager::OculusTouch::GetInputState(ovrSession session, ovrInputState* inputState)
{
	rcu_ptr<InputSettings> settings = session->Settings->Input;
	ovrHandType hand = (Role == vr::TrackedControllerRole_LeftHand) ? ovrHand_Left : ovrHand_Right;

	if (GetDigital(m_Button_Enter))
		inputState->Buttons |= ovrButton_Enter;

	unsigned int buttons, touches;

	if (GetDigital(m_Button_AX))
		buttons |= ovrButton_A;

	if (GetDigital(m_Touch_AX))
		touches |= ovrTouch_A;

	if (GetDigital(m_Button_BY))
		buttons |= ovrButton_B;

	if (GetDigital(m_Touch_BY))
		touches |= ovrTouch_B;

	if (GetDigital(m_Button_Thumb))
		buttons |= ovrButton_RThumb;

	if (GetDigital(m_Touch_Thumb))
		touches |= ovrTouch_RThumb;

	if (GetDigital(m_Touch_ThumbRest))
		touches |= ovrTouch_RThumbRest;

	if (GetDigital(m_Touch_IndexTrigger))
		touches |= ovrTouch_RIndexTrigger;

	if (!GetDigital(m_Touch_IndexPointing))
		touches |= ovrTouch_RIndexPointing;

	if (!GetDigital(m_Touch_ThumbUp))
		touches |= ovrTouch_RThumbUp;

	inputState->Buttons |= (hand == ovrHand_Left) ? buttons << 8 : buttons;
	inputState->Touches |= (hand == ovrHand_Left) ? touches << 8 : touches;

	inputState->IndexTrigger[hand] = GetAnalog(m_IndexTrigger).x;
	inputState->HandTrigger[hand] = GetAnalog(m_HandTrigger).x;
	inputState->Thumbstick[hand] = GetAnalog(m_Thumbstick);
	inputState->ThumbstickNoDeadzone[hand] = GetAnalog(m_Thumbstick);

	if (GetDigital(m_Button_IndexTrigger))
		inputState->IndexTrigger[hand] = 1.0f;

	if (GetDigital(m_Button_HandTrigger))
		inputState->HandTrigger[hand] = 1.0f;

	// We don't apply deadzones yet on triggers and grips
	inputState->IndexTriggerNoDeadzone[hand] = inputState->IndexTrigger[hand];
	inputState->HandTriggerNoDeadzone[hand] = inputState->HandTrigger[hand];

	// We have no way to get raw values
	inputState->ThumbstickRaw[hand] = inputState->ThumbstickNoDeadzone[hand];
	inputState->IndexTriggerRaw[hand] = inputState->IndexTriggerNoDeadzone[hand];
	inputState->HandTriggerRaw[hand] = inputState->HandTriggerNoDeadzone[hand];

	return true;
}

InputManager::OculusRemote::OculusRemote(vr::VRActionSetHandle_t actionSet)
	: InputDevice(actionSet)
{
#define GET_REMOTE_ACTION(x) vr::VRInput()->GetActionHandle( \
	"/actions/remote/in/" #x, &m_##x)

	GET_REMOTE_ACTION(Button_Up);
	GET_REMOTE_ACTION(Button_Down);
	GET_REMOTE_ACTION(Button_Left);
	GET_REMOTE_ACTION(Button_Right);
	GET_REMOTE_ACTION(Button_Enter);
	GET_REMOTE_ACTION(Button_Back);
	GET_REMOTE_ACTION(Button_VolUp);
	GET_REMOTE_ACTION(Button_VolDown);

#undef GET_REMOTE_ACTION
}

bool InputManager::OculusRemote::IsConnected() const
{
	// Check if a Vive controller is available
	uint32_t controllerCount = vr::VRSystem()->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_Controller, nullptr, 0);

	// If only one controller is available, the Oculus Remote is connected
	return controllerCount == 1;
}

bool InputManager::OculusRemote::GetInputState(ovrSession session, ovrInputState* inputState)
{
	unsigned int buttons;

	if (GetDigital(m_Button_Up))
		buttons |= ovrButton_Up;

	if (GetDigital(m_Button_Down))
		buttons |= ovrButton_Down;

	if (GetDigital(m_Button_Left))
		buttons |= ovrButton_Left;

	if (GetDigital(m_Button_Right))
		buttons |= ovrButton_Right;

	if (GetDigital(m_Button_Enter))
		buttons |= ovrButton_Enter;

	if (GetDigital(m_Button_Back))
		buttons |= ovrButton_Back;

	if (GetDigital(m_Button_VolUp))
		buttons |= ovrButton_VolUp;

	if (GetDigital(m_Button_VolDown))
		buttons |= ovrButton_VolDown;

	inputState->Buttons |= buttons;
	return true;
}

InputManager::XboxGamepad::~XboxGamepad()
{
}

bool InputManager::XboxGamepad::GetInputState(ovrSession session, ovrInputState* inputState)
{
	return false;
}

void InputManager::XboxGamepad::SetVibration(float frequency, float amplitude)
{
}
