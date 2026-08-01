#include "../pairing/DrcPairing.hpp"

#include <cstdio>
#include <algorithm>
#include <gx2/display.h>
#include <gx2/registers.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <nsysccr/cdc.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include <wups.h>
#include <wups/function_patching.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>
#include <wups/storage.h>

WUPS_PLUGIN_NAME("Dual U");
WUPS_PLUGIN_DESCRIPTION("Pair and use a second Wii U gamepad");
WUPS_PLUGIN_VERSION("v1.0");
WUPS_PLUGIN_AUTHOR("tech-will");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("dual_drc_home_menu");

namespace {
enum ControllerMode : uint32_t {
    CONTROLLER_MODE_MIRRORED = 0,
    CONTROLLER_MODE_SEPARATE_PRO = 1,
};

enum VideoFeedMode : uint32_t {
    VIDEO_FEED_TV = 0,
    VIDEO_FEED_GAMEPAD = 1,
};

enum MotionMode : uint32_t {
    MOTION_MODE_OFF = 0,
    MOTION_MODE_MIRRORED = 1,
    MOTION_MODE_SPLIT_WIIMOTE = 2,
};

enum TouchMode : uint32_t {
    TOUCH_MODE_OFF = 0,
    TOUCH_MODE_MIRRORED = 1,
};

// The DRC1 video copy is a full GPU resolve/blit of an entire frame, done
// in addition to the game's own normal TV+DRC0 copies, on hardware that
// wasn't fast to begin with. Skipping it on a fraction of frames trades
// smoothness for real GPU time back -- the skipped pad just keeps showing
// its last copied frame in between, rather than freezing outright. This
// type is shared by both the first-GamePad (DRC0) and second-GamePad
// (DRC1) rate settings, despite the Drc1-flavored member names below --
// renaming those would mean touching every existing DRC1-specific
// variable/storage-key/callback name too, since they all share the same
// substring, so it's left as one shared enum instead of two near-identical
// ones.
enum Drc1VideoRate : uint32_t {
    DRC1_VIDEO_RATE_FULL = 0,
    DRC1_VIDEO_RATE_HALF = 1,
    DRC1_VIDEO_RATE_THIRD = 2,
};

constexpr bool kDefaultPluginEnabled = false;
constexpr uint32_t kDefaultControllerMode = CONTROLLER_MODE_SEPARATE_PRO;
constexpr uint32_t kDefaultVideoFeedMode = VIDEO_FEED_GAMEPAD;
constexpr uint32_t kDefaultMotionMode = MOTION_MODE_MIRRORED;
constexpr uint32_t kDefaultTouchMode = TOUCH_MODE_MIRRORED;
constexpr uint32_t kDefaultDrc0VideoRate = DRC1_VIDEO_RATE_FULL;
constexpr uint32_t kDefaultDrc1VideoRate = DRC1_VIDEO_RATE_FULL;
constexpr bool kDefaultPerformanceModeEnabled = false;
// Player 1 (WPAD_CHAN_0), matching the plugin's original fixed behavior.
constexpr uint32_t kDefaultControllerMappingChannel = WPAD_CHAN_0;
constexpr const char *kStorageKeyEnabled = "dual_drc_enabled";
constexpr const char *kStorageKeyControllerMode = "dual_drc_controller_mode";
constexpr const char *kStorageKeyVideoFeedMode = "dual_drc_video_feed_mode";
constexpr const char *kStorageKeyMotionMode = "dual_drc_motion_mode";
constexpr const char *kStorageKeyTouchMode = "dual_drc_touch_mode";
constexpr const char *kStorageKeyDrc0VideoRate = "dual_drc_drc0_video_rate";
constexpr const char *kStorageKeyDrc1VideoRate = "dual_drc_drc1_video_rate";
constexpr const char *kStorageKeyPerformanceMode = "dual_drc_performance_mode";
constexpr const char *kStorageKeyControllerMappingChannel = "dual_drc_controller_mapping_channel";
constexpr uint32_t kControllerModeToggleCombo = VPAD_BUTTON_STICK_L | VPAD_BUTTON_STICK_R;
constexpr uint32_t kVideoFeedToggleCombo = VPAD_BUTTON_RIGHT | VPAD_BUTTON_Y;


bool sPluginEnabled = kDefaultPluginEnabled;
bool sExperimentalPatchEnabled = true;
bool sPerformanceModeEnabled = kDefaultPerformanceModeEnabled;

uint32_t sControllerMode = kDefaultControllerMode;
uint32_t sVideoFeedMode = kDefaultVideoFeedMode;
uint32_t sMotionMode = kDefaultMotionMode;
uint32_t sTouchMode = kDefaultTouchMode;
uint32_t sDrc0VideoRate = kDefaultDrc0VideoRate;
uint32_t sDrc1VideoRate = kDefaultDrc1VideoRate;
// Which WPAD channel (Player 1-4) the synthetic Pro Controller occupies.
// Configurable via "Controller Mapping" so it can be moved off whatever
// slots real controllers are already using.
uint32_t sSyntheticControllerChannel = kDefaultControllerMappingChannel;

ConfigItemMultipleValuesPair sControllerModeValues[] = {
        {CONTROLLER_MODE_MIRRORED, "Mirrored"},
    {CONTROLLER_MODE_SEPARATE_PRO, "Seperate"},
};

ConfigItemMultipleValuesPair sVideoFeedValues[] = {
        {VIDEO_FEED_TV, "TV Video"},
    {VIDEO_FEED_GAMEPAD, "Gamepad Video"},
};

ConfigItemMultipleValuesPair sMotionModeValues[] = {
        {MOTION_MODE_OFF, "Off"},
    {MOTION_MODE_MIRRORED, "Mirrored"},
    {MOTION_MODE_SPLIT_WIIMOTE, "Split Wii Remote"},
};

ConfigItemMultipleValuesPair sTouchModeValues[] = {
        {TOUCH_MODE_OFF, "Off"},
    {TOUCH_MODE_MIRRORED, "Mirrored"},
};

ConfigItemMultipleValuesPair sDrc1VideoRateValues[] = {
        {DRC1_VIDEO_RATE_FULL, "Full (every frame)"},
    {DRC1_VIDEO_RATE_HALF, "Half (every 2nd frame)"},
    {DRC1_VIDEO_RATE_THIRD, "Third (every 3rd frame)"},
};

ConfigItemMultipleValuesPair sControllerMappingValues[] = {
        {WPAD_CHAN_0, "Player 1"},
    {WPAD_CHAN_1, "Player 2"},
    {WPAD_CHAN_2, "Player 3"},
    {WPAD_CHAN_3, "Player 4"},
};

DrcPairing sPairing;
int32_t sLastSetMultiResult = 0;
int32_t sLastSetStateResult = 0;
int32_t sLastWakeResult = 0;
uint8_t sLastRequestedState = 0;
uint8_t sLastMultiValue = 1;
uint32_t sGX2HookCalls = 0;
uint32_t sGX2SetDRCEnableCalls = 0;
uint32_t sGX2SetDRCBufferCalls = 0;
// GX2SetDRCBuffer is only called once, near a process's own startup, to
// establish the ACTUAL allocated size/mode of its scan buffer -- it is not
// re-called every frame. If "Plugin Enabled" gets toggled on mid-session
// (exactly what happens the moment you check the box in the config menu),
// the buffer that's already been allocated is still sized for SINGLE mode,
// even though sPluginEnabled is now true. Without this flag,
// GX2CopyColorBufferToScanBuffer would immediately start writing a second
// copy to GX2_SCAN_TARGET_DRC1 every frame -- into memory that was never
// allocated for it. That out-of-bounds write, happening continuously from
// the moment the toggle flips, is a far more likely explanation for a
// freeze than any reload-timing race. This flag is only set true when
// GX2SetDRCBuffer itself actually observed the plugin enabled and forced
// DOUBLE mode, so the extra copy only ever targets a buffer we know was
// really sized for it.
bool sDrcBufferIsDoubleSized = false;
uint32_t sGX2CalcDRCSizeCalls = 0;
uint32_t sGX2CopyToScanBufferCalls = 0;
uint32_t sGX2CopyToDrc1Calls = 0;
GX2ColorBuffer sCachedTvColorBuffer = {};
bool sHasCachedTvColorBuffer = false;
uint32_t sDrc0VideoFrameCounter = 0;
uint32_t sDrc1VideoFrameCounter = 0;
uint32_t sVPADHookCalls = 0;
uint32_t sVPADMergedCalls = 0;
uint32_t sVPADDrc1ReadAttempts = 0;
uint32_t sVPADDrc1ReadSuccess = 0;
uint32_t sVPADDrc1NoSamples = 0;
uint32_t sVPADDrc1Invalid = 0;
uint32_t sVPADDrc1Busy = 0;
uint32_t sVPADDrc1Uninitialized = 0;
uint32_t sVPADDrc1OtherError = 0;
int32_t sLastDrc1ReadCount = 0;
int32_t sLastDrc1ReadError = 0;
uint32_t sKPADReadCalls = 0;
uint32_t sKPADInjectedSamples = 0;
uint32_t sKPADInjectFailures = 0;
uint32_t sLastSeparateHold = 0;
uint32_t sLastWiimoteHold = 0;
// Shared cache so the three consumers of DRC1's raw VPAD data (button/
// stick/motion merging in Mirrored controller mode, the synthetic Pro
// Controller in Separate controller mode, and the synthetic Wii Remote in
// Split motion mode) never read VPAD_CHAN_1 more than once per frame.
// VPADRead only buffers a small backlog of samples, so reading it twice
// in the same frame can starve whichever caller asks second -- exactly
// the kind of bug that made DRC1's combos unreliable before. The cache is
// invalidated at the top of every VPADRead(chan=0) call, which reliably
// happens once per game frame.
VPADStatus sCachedDrc1Status = {};
VPADReadError sCachedDrc1Error = VPAD_READ_UNINITIALIZED;
int32_t sCachedDrc1ReadCount = 0;
bool sDrc1CacheValidThisFrame = false;
bool sControllerModeToggleComboLatched = false;
bool sControllerModeToggleComboLatchedDrc1 = false;
bool sVideoFeedToggleComboLatched = false;
bool sVideoFeedToggleComboLatchedDrc1 = false;
uint8_t sSyntheticRumblePatternOn = 0xFF;
uint8_t sSyntheticRumblePatternOff = 0x00;

void ApplyDualDrcMode(bool enabled);

void SaveSettingsToStorage() {
    WUPSStorageAPI_StoreBool(nullptr, kStorageKeyEnabled, sPluginEnabled);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyControllerMode, sControllerMode);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyVideoFeedMode, sVideoFeedMode);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyMotionMode, sMotionMode);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyTouchMode, sTouchMode);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyDrc0VideoRate, sDrc0VideoRate);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyDrc1VideoRate, sDrc1VideoRate);
    WUPSStorageAPI_StoreBool(nullptr, kStorageKeyPerformanceMode, sPerformanceModeEnabled);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyControllerMappingChannel, sSyntheticControllerChannel);
    WUPSStorageAPI_SaveStorage(false);
}

void LoadSettingsFromStorage() {
    bool pluginEnabled = kDefaultPluginEnabled;
    if (WUPSStorageAPI_GetBool(nullptr, kStorageKeyEnabled, &pluginEnabled) == WUPS_STORAGE_ERROR_SUCCESS) {
        sPluginEnabled = pluginEnabled;
    }

    sExperimentalPatchEnabled = true;

    uint32_t controllerMode = kDefaultControllerMode;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyControllerMode, &controllerMode) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (controllerMode <= CONTROLLER_MODE_SEPARATE_PRO) {
            sControllerMode = controllerMode;
        }
    }

    uint32_t videoFeedMode = kDefaultVideoFeedMode;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyVideoFeedMode, &videoFeedMode) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (videoFeedMode <= VIDEO_FEED_GAMEPAD) {
            sVideoFeedMode = videoFeedMode;
        }
    }

    uint32_t motionMode = kDefaultMotionMode;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyMotionMode, &motionMode) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (motionMode <= MOTION_MODE_SPLIT_WIIMOTE) {
            sMotionMode = motionMode;
        }
    }

    uint32_t touchMode = kDefaultTouchMode;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyTouchMode, &touchMode) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (touchMode <= TOUCH_MODE_MIRRORED) {
            sTouchMode = touchMode;
        }
    }

    uint32_t drc0VideoRate = kDefaultDrc0VideoRate;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyDrc0VideoRate, &drc0VideoRate) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (drc0VideoRate <= DRC1_VIDEO_RATE_THIRD) {
            sDrc0VideoRate = drc0VideoRate;
        }
    }

    uint32_t drc1VideoRate = kDefaultDrc1VideoRate;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyDrc1VideoRate, &drc1VideoRate) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (drc1VideoRate <= DRC1_VIDEO_RATE_THIRD) {
            sDrc1VideoRate = drc1VideoRate;
        }
    }

    bool performanceMode = kDefaultPerformanceModeEnabled;
    if (WUPSStorageAPI_GetBool(nullptr, kStorageKeyPerformanceMode, &performanceMode) == WUPS_STORAGE_ERROR_SUCCESS) {
        sPerformanceModeEnabled = performanceMode;
    }

    uint32_t controllerMappingChannel = kDefaultControllerMappingChannel;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyControllerMappingChannel, &controllerMappingChannel) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (controllerMappingChannel <= WPAD_CHAN_3) {
            sSyntheticControllerChannel = controllerMappingChannel;
        }
    }
}

void ToggleControllerMode() {
    if (sControllerMode == CONTROLLER_MODE_MIRRORED) {
        sControllerMode = CONTROLLER_MODE_SEPARATE_PRO;
    } else {
        sControllerMode = CONTROLLER_MODE_MIRRORED;
    }
    SaveSettingsToStorage();
}

void CheckControllerModeToggleCombo(const VPADStatus *buffers, int32_t readCount, bool &comboLatched) {
    if (buffers == nullptr || readCount <= 0) {
        comboLatched = false;
        return;
    }

    bool comboPressed = false;
    for (int32_t i = 0; i < readCount; i++) {
        if ((buffers[i].hold & kControllerModeToggleCombo) == kControllerModeToggleCombo) {
            comboPressed = true;
            break;
        }
    }

    if (comboPressed && !comboLatched) {
        ToggleControllerMode();
    }
    comboLatched = comboPressed;
}

void ToggleVideoFeedMode() {
    if (sVideoFeedMode == VIDEO_FEED_GAMEPAD) {
        sVideoFeedMode = VIDEO_FEED_TV;
    } else {
        sVideoFeedMode = VIDEO_FEED_GAMEPAD;
    }
    SaveSettingsToStorage();
}

void CheckVideoFeedToggleCombo(const VPADStatus *buffers, int32_t readCount, bool &comboLatched) {
    if (buffers == nullptr || readCount <= 0) {
        comboLatched = false;
        return;
    }

    bool comboPressed = false;
    for (int32_t i = 0; i < readCount; i++) {
        if ((buffers[i].hold & kVideoFeedToggleCombo) == kVideoFeedToggleCombo) {
            comboPressed = true;
            break;
        }
    }

    if (comboPressed && !comboLatched) {
        ToggleVideoFeedMode();
    }
    comboLatched = comboPressed;
}

bool IsMirroredMode() {
    return sControllerMode == CONTROLLER_MODE_MIRRORED;
}

bool IsSeparateMode() {
    return sControllerMode == CONTROLLER_MODE_SEPARATE_PRO;
}

bool IsSyntheticControllerChannel(WPADChan chan) {
    return chan == static_cast<WPADChan>(sSyntheticControllerChannel);
}

bool IsSplitWiimoteMode() {
    return sMotionMode == MOTION_MODE_SPLIT_WIIMOTE;
}

// The synthetic Wii Remote (Split Wii Remote motion mode) always needs a
// channel distinct from whatever "Controller Mapping" has the synthetic
// Pro Controller on, or the two would collide the moment both features
// are active at once. Rather than hardcoding a fixed channel that could
// land on the same slot the user picked for Controller Mapping, pick
// whichever of channel 0/1 ISN'T that slot -- guaranteed never to
// collide, regardless of which Player the Pro Controller is mapped to.
WPADChan GetSyntheticWiimoteChannel() {
    return (static_cast<WPADChan>(sSyntheticControllerChannel) == WPAD_CHAN_0) ? WPAD_CHAN_1 : WPAD_CHAN_0;
}

bool IsSyntheticWiimoteChannel(WPADChan chan) {
    return chan == GetSyntheticWiimoteChannel();
}

// Shared by every consumer of DRC1's raw VPAD data (button/stick/motion
// merging, the synthetic Pro Controller, and the synthetic Wii Remote) so
// VPAD_CHAN_1 is only ever read once per frame. See sDrc1CacheValidThisFrame
// for why: reading it twice in one frame can starve whichever caller asks
// second. Also checks DRC1's own combo presses exactly once per frame,
// since this is the one place a fresh DRC1 sample is guaranteed to exist.
// Returns true and fills outStatus if a valid sample is available.
//
// Forward-declared here, defined after the VPADRead hook below: its body
// needs real_VPADRead, which only exists once DECL_FUNCTION(VPADRead, ...)
// has run, but VPADRead's own hook body also calls this function -- a
// genuine circular need, resolved the standard way with a prototype now
// and the real definition later.
bool FetchDrc1Vpad(VPADStatus *outStatus);

void FillSyntheticWpadInfo(WPADInfo *outInfo) {
    if (outInfo == nullptr) {
        return;
    }

    outInfo->irEnabled = FALSE;
    outInfo->speakerEnabled = TRUE;
    outInfo->extensionAttached = TRUE;
    outInfo->batteryLow = FALSE;
    outInfo->speakerBufNearEmpty = FALSE;
    outInfo->batteryLevel = 4;
    outInfo->led = 0x02;
    outInfo->protocol = 0;
    outInfo->firmware = 0;
}

void ApplyDualDrcMode(bool enabled) {
    if (enabled) {
        sLastSetMultiResult = CCRCDCSetMultiDrc(2);
        sLastMultiValue = 2;

        CCRCDCWowlWakeDrcArg wakeArg = {};
        wakeArg.state = CCR_CDC_WAKE_STATE_ACTIVE;
        sLastWakeResult = CCRCDCWowlWakeDrc(&wakeArg);

        CCRCDCDrcState drc1State = {};
        drc1State.state = CCR_CDC_DRC_STATE_ACTIVE;
        sLastRequestedState = drc1State.state;
        sLastSetStateResult = CCRCDCSysSetDrcState(CCR_CDC_DESTINATION_DRC1, &drc1State);
        if (sLastSetStateResult != 0) {
            drc1State.state = CCR_CDC_DRC_STATE_WIIACTIVE;
            sLastRequestedState = drc1State.state;
            sLastSetStateResult = CCRCDCSysSetDrcState(CCR_CDC_DESTINATION_DRC1, &drc1State);
            if (sLastSetStateResult != 0) {
                drc1State.state = CCR_CDC_DRC_STATE_SUBACTIVE;
                sLastRequestedState = drc1State.state;
                sLastSetStateResult = CCRCDCSysSetDrcState(CCR_CDC_DESTINATION_DRC1, &drc1State);
            }
        }
        return;
    }

    // Some SDK versions don't expose a DRC disconnect enum; simply
    // disable multi-DRC mode which will put the system back to single DRC.
    sLastSetMultiResult = CCRCDCSetMultiDrc(1);
    sLastMultiValue = 1;
    sLastSetStateResult = 0;
    sLastWakeResult = 0;
    sLastRequestedState = 0;
}

void PluginEnabledChanged(ConfigItemBoolean *, bool newValue) {
    sPluginEnabled = newValue;
    ApplyDualDrcMode(sPluginEnabled);
    if (!sPluginEnabled && sPairing.getState() == DrcPairing::STATE_PAIRING) {
        sPairing.stopPairing();
    }
    SaveSettingsToStorage();
}

void ControllerModeChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    sControllerMode = newValue;
    SaveSettingsToStorage();
}

void VideoFeedModeChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    sVideoFeedMode = newValue;
    SaveSettingsToStorage();
}

void MotionModeChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    sMotionMode = newValue;
    SaveSettingsToStorage();
}

void TouchModeChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    sTouchMode = newValue;
    SaveSettingsToStorage();
}

void Drc0VideoRateChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    sDrc0VideoRate = newValue;
    SaveSettingsToStorage();
}

void Drc1VideoRateChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    sDrc1VideoRate = newValue;
    SaveSettingsToStorage();
}

void ControllerMappingChanged(ConfigItemMultipleValues *, uint32_t newValue) {
    sSyntheticControllerChannel = newValue;
    SaveSettingsToStorage();
}

void PerformanceModeChanged(ConfigItemBoolean *, bool newValue) {
    sPerformanceModeEnabled = newValue;
    SaveSettingsToStorage();
}

uint32_t MapVpadButtonsToWpadPro(uint32_t vpadButtons) {
    uint32_t out = 0;
    if (vpadButtons & VPAD_BUTTON_A) out |= WPAD_PRO_BUTTON_A;
    if (vpadButtons & VPAD_BUTTON_B) out |= WPAD_PRO_BUTTON_B;
    if (vpadButtons & VPAD_BUTTON_X) out |= WPAD_PRO_BUTTON_X;
    if (vpadButtons & VPAD_BUTTON_Y) out |= WPAD_PRO_BUTTON_Y;
    if (vpadButtons & VPAD_BUTTON_LEFT) out |= WPAD_PRO_BUTTON_LEFT;
    if (vpadButtons & VPAD_BUTTON_RIGHT) out |= WPAD_PRO_BUTTON_RIGHT;
    if (vpadButtons & VPAD_BUTTON_UP) out |= WPAD_PRO_BUTTON_UP;
    if (vpadButtons & VPAD_BUTTON_DOWN) out |= WPAD_PRO_BUTTON_DOWN;
    if (vpadButtons & VPAD_BUTTON_L) out |= WPAD_PRO_BUTTON_L;
    if (vpadButtons & VPAD_BUTTON_R) out |= WPAD_PRO_BUTTON_R;
    if (vpadButtons & VPAD_BUTTON_ZL) out |= WPAD_PRO_BUTTON_ZL;
    if (vpadButtons & VPAD_BUTTON_ZR) out |= WPAD_PRO_BUTTON_ZR;
    if (vpadButtons & VPAD_BUTTON_PLUS) out |= WPAD_PRO_BUTTON_PLUS;
    if (vpadButtons & VPAD_BUTTON_MINUS) out |= WPAD_PRO_BUTTON_MINUS;
    if (vpadButtons & VPAD_BUTTON_HOME) out |= WPAD_PRO_BUTTON_HOME;
    if (vpadButtons & VPAD_BUTTON_STICK_L) out |= WPAD_PRO_BUTTON_STICK_L;
    if (vpadButtons & VPAD_BUTTON_STICK_R) out |= WPAD_PRO_BUTTON_STICK_R;
    if (vpadButtons & VPAD_STICK_L_EMULATION_LEFT) out |= WPAD_PRO_STICK_L_EMULATION_LEFT;
    if (vpadButtons & VPAD_STICK_L_EMULATION_RIGHT) out |= WPAD_PRO_STICK_L_EMULATION_RIGHT;
    if (vpadButtons & VPAD_STICK_L_EMULATION_UP) out |= WPAD_PRO_STICK_L_EMULATION_UP;
    if (vpadButtons & VPAD_STICK_L_EMULATION_DOWN) out |= WPAD_PRO_STICK_L_EMULATION_DOWN;
    if (vpadButtons & VPAD_STICK_R_EMULATION_LEFT) out |= WPAD_PRO_STICK_R_EMULATION_LEFT;
    if (vpadButtons & VPAD_STICK_R_EMULATION_RIGHT) out |= WPAD_PRO_STICK_R_EMULATION_RIGHT;
    if (vpadButtons & VPAD_STICK_R_EMULATION_UP) out |= WPAD_PRO_STICK_R_EMULATION_UP;
    if (vpadButtons & VPAD_STICK_R_EMULATION_DOWN) out |= WPAD_PRO_STICK_R_EMULATION_DOWN;
    return out;
}

uint32_t MapVpadButtonsToWpadCore(uint32_t vpadButtons) {
    uint32_t out = 0;
    if (vpadButtons & VPAD_BUTTON_A) out |= WPAD_BUTTON_A;
    if (vpadButtons & VPAD_BUTTON_B) out |= WPAD_BUTTON_B;
    if (vpadButtons & VPAD_BUTTON_X) out |= WPAD_BUTTON_1;
    if (vpadButtons & VPAD_BUTTON_Y) out |= WPAD_BUTTON_2;
    if (vpadButtons & VPAD_BUTTON_LEFT) out |= WPAD_BUTTON_LEFT;
    if (vpadButtons & VPAD_BUTTON_RIGHT) out |= WPAD_BUTTON_RIGHT;
    if (vpadButtons & VPAD_BUTTON_UP) out |= WPAD_BUTTON_UP;
    if (vpadButtons & VPAD_BUTTON_DOWN) out |= WPAD_BUTTON_DOWN;
    if (vpadButtons & VPAD_BUTTON_PLUS) out |= WPAD_BUTTON_PLUS;
    if (vpadButtons & VPAD_BUTTON_MINUS) out |= WPAD_BUTTON_MINUS;
    if (vpadButtons & VPAD_BUTTON_HOME) out |= WPAD_BUTTON_HOME;
    if (vpadButtons & VPAD_BUTTON_ZL) out |= WPAD_BUTTON_B;
    return out;
}

bool BuildSyntheticKpadFromDrc1(KPADStatus *outStatus) {
    if (outStatus == nullptr || !sPluginEnabled || !IsSeparateMode()) {
        return false;
    }

    VPADStatus drc1 = {};
    if (!FetchDrc1Vpad(&drc1)) {
        return false;
    }

    *outStatus = {};

    if (sControllerMode == CONTROLLER_MODE_SEPARATE_PRO) {
        uint32_t hold = MapVpadButtonsToWpadPro(drc1.hold);
        outStatus->hold = hold;
        outStatus->trigger = (hold & ~sLastSeparateHold);
        outStatus->release = ((~hold) & sLastSeparateHold);
        outStatus->extensionType = WPAD_EXT_PRO_CONTROLLER;
        outStatus->format = WPAD_FMT_PRO_CONTROLLER;
        outStatus->pro.hold = hold;
        outStatus->pro.trigger = outStatus->trigger;
        outStatus->pro.release = outStatus->release;
        outStatus->pro.leftStick.x = drc1.leftStick.x;
        outStatus->pro.leftStick.y = drc1.leftStick.y;
        outStatus->pro.rightStick.x = drc1.rightStick.x;
        outStatus->pro.rightStick.y = drc1.rightStick.y;
        outStatus->pro.charging = 0;
        outStatus->pro.wired = 0;
        sLastSeparateHold = hold;
        return true;
    }

    uint32_t hold = MapVpadButtonsToWpadCore(drc1.hold);
    outStatus->hold = hold;
    outStatus->trigger = (hold & ~sLastSeparateHold);
    outStatus->release = ((~hold) & sLastSeparateHold);
    outStatus->extensionType = WPAD_EXT_CORE;
    outStatus->format = WPAD_FMT_CORE;
    outStatus->error = KPAD_ERROR_OK;
    outStatus->posValid = 0;
    sLastSeparateHold = hold;
    return true;
}

// "Split Wii Remote" motion mode: DRC1 becomes a standalone synthetic Wii
// Remote with Motion Plus, reporting real gyroscope data in addition to
// the accelerometer, decoupled from whatever Controller Mode is doing with
// DRC1's buttons/sticks. This is independent of Controller Mode by design
// -- it can layer on top of either Mirrored or Separate, the way a real
// second Wii Remote would sit alongside normal GamePad play for
// pointer/motion-only input.
bool BuildSyntheticWiimoteFromDrc1Motion(KPADStatus *outStatus) {
    if (outStatus == nullptr || !sPluginEnabled || !IsSplitWiimoteMode()) {
        return false;
    }

    VPADStatus drc1 = {};
    if (!FetchDrc1Vpad(&drc1)) {
        return false;
    }

    *outStatus = {};

    // A handful of buttons are still mapped across so the synthetic
    // Wiimote is actually usable (A/B/1/2/+/-/Home/D-pad), reusing the
    // same mapping the Separate-mode Pro Controller path uses for Core
    // format. Its own latch keeps this independent of the Pro Controller
    // synthesis, in case both are active on their separate channels at
    // once.
    uint32_t hold = MapVpadButtonsToWpadCore(drc1.hold);
    outStatus->hold = hold;
    outStatus->trigger = (hold & ~sLastWiimoteHold);
    outStatus->release = ((~hold) & sLastWiimoteHold);
    outStatus->extensionType = WPAD_EXT_MPLUS;
    outStatus->format = WPAD_FMT_MPLUS;
    outStatus->error = KPAD_ERROR_OK;
    // No sensor bar to point at, so no valid IR position/angle/distance --
    // exactly like a real Wii Remote reports when it can't see the bar.
    outStatus->posValid = 0;

    // Base accelerometer, same as a bare Wiimote would report. See the
    // axis-mapping note below -- it applies here too.
    outStatus->acc.x = drc1.accelorometer.acc.x;
    outStatus->acc.y = drc1.accelorometer.acc.y;
    outStatus->acc.z = drc1.accelorometer.acc.z;
    outStatus->accMagnitude = drc1.accelorometer.magnitude;
    outStatus->accVariation = drc1.accelorometer.variation;

    // Motion Plus adds real gyroscope data on top of the base
    // accelerometer -- this is the actual point of reporting as MPLUS
    // instead of a bare Wiimote: true 1:1 rotation tracking rather than
    // just tilt-from-gravity. KPADStatus::mplus.acc is documented as
    // "angular acceleration" (i.e. the gyro channel, not linear
    // acceleration -- that's the separate .acc field above), so DRC1's
    // real gyro maps directly onto it. .angles is the integrated
    // orientation Motion Plus derives from that gyro over time, which
    // VPAD's own .angle is a reasonable analog of (both are the device's
    // own fused/processed orientation estimate). dirX/dirY/dirZ together
    // form a 3x3 orientation basis (each a KPADVec3D, matching VPAD's
    // .direction field-for-field, just flattened to three top-level
    // fields instead of nested under one), so they copy straight across.
    //
    // Axis mapping / "facing up" assumption: per WiiBrew's Wiimote
    // documentation, a real Wii Remote's Y axis is its long axis (the tip
    // with the IR sensor vs. the bottom near the wrist strap), Z is
    // perpendicular to the button face, and X is width (left/right).
    // Held upright pointing at the screen -- the standard grip almost all
    // motion-aware software assumes by default -- gravity mostly loads
    // onto Y, with Z/X reflecting forward/side tilt. This maps DRC1's
    // sensors straight across (X->X, Y->Y, Z->Z, no sign flips), on the
    // assumption Nintendo kept a consistent axis convention across its
    // own motion-sensing controllers (Wiimote and GamePad alike). I can't
    // verify the exact sign convention without hardware in hand, though,
    // so if this reads upside-down, sideways, or mirrored on your end,
    // this is the block to adjust -- tell me exactly what you're seeing
    // (e.g. "tilting the GamePad forward tilts the Wiimote back") and I
    // can flip the specific axis/sign that's off.
    outStatus->mplus.acc.x = drc1.gyro.x;
    outStatus->mplus.acc.y = drc1.gyro.y;
    outStatus->mplus.acc.z = drc1.gyro.z;
    outStatus->mplus.angles.x = drc1.angle.x;
    outStatus->mplus.angles.y = drc1.angle.y;
    outStatus->mplus.angles.z = drc1.angle.z;
    outStatus->mplus.dirX.x = drc1.direction.x.x;
    outStatus->mplus.dirX.y = drc1.direction.x.y;
    outStatus->mplus.dirX.z = drc1.direction.x.z;
    outStatus->mplus.dirY.x = drc1.direction.y.x;
    outStatus->mplus.dirY.y = drc1.direction.y.y;
    outStatus->mplus.dirY.z = drc1.direction.y.z;
    outStatus->mplus.dirZ.x = drc1.direction.z.x;
    outStatus->mplus.dirZ.y = drc1.direction.z.y;
    outStatus->mplus.dirZ.z = drc1.direction.z.z;

    sLastWiimoteHold = hold;
    return true;
}

DECL_FUNCTION(GX2DrcRenderMode, GX2GetSystemDRCMode) {
    sGX2HookCalls++;
    GX2DrcRenderMode mode = real_GX2GetSystemDRCMode();
    if (sPluginEnabled && sExperimentalPatchEnabled && !sPerformanceModeEnabled) {
        return GX2_DRC_RENDER_MODE_DOUBLE;
    }
    return mode;
}
WUPS_MUST_REPLACE_FOR_PROCESS(GX2GetSystemDRCMode,
                              WUPS_LOADER_LIBRARY_GX2,
                              GX2GetSystemDRCMode,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(void, GX2SetDRCEnable, BOOL enable) {
    sGX2SetDRCEnableCalls++;
    if (sPluginEnabled && sExperimentalPatchEnabled && !sPerformanceModeEnabled) {
        real_GX2SetDRCEnable(TRUE);
        return;
    }
    real_GX2SetDRCEnable(enable);
}
WUPS_MUST_REPLACE_FOR_PROCESS(GX2SetDRCEnable,
                              WUPS_LOADER_LIBRARY_GX2,
                              GX2SetDRCEnable,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(void,
              GX2CalcDRCSize,
              GX2DrcRenderMode drcRenderMode,
              GX2SurfaceFormat surfaceFormat,
              GX2BufferingMode bufferingMode,
              uint32_t *size,
              uint32_t *unkOut) {
    sGX2CalcDRCSizeCalls++;
    if (sPluginEnabled && sExperimentalPatchEnabled && !sPerformanceModeEnabled) {
        real_GX2CalcDRCSize(GX2_DRC_RENDER_MODE_DOUBLE,
                            surfaceFormat,
                            bufferingMode,
                            size,
                            unkOut);
        return;
    }
    real_GX2CalcDRCSize(drcRenderMode, surfaceFormat, bufferingMode, size, unkOut);
}
WUPS_MUST_REPLACE_FOR_PROCESS(GX2CalcDRCSize,
                              WUPS_LOADER_LIBRARY_GX2,
                              GX2CalcDRCSize,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(void,
              GX2SetDRCBuffer,
              void *buffer,
              uint32_t size,
              GX2DrcRenderMode drcRenderMode,
              GX2SurfaceFormat surfaceFormat,
              GX2BufferingMode bufferingMode) {
    sGX2SetDRCBufferCalls++;
    if (sPluginEnabled && sExperimentalPatchEnabled && !sPerformanceModeEnabled) {
        real_GX2SetDRCBuffer(buffer,
                             size,
                             GX2_DRC_RENDER_MODE_DOUBLE,
                             surfaceFormat,
                             bufferingMode);
        sDrcBufferIsDoubleSized = true;
        return;
    }
    real_GX2SetDRCBuffer(buffer, size, drcRenderMode, surfaceFormat, bufferingMode);
    sDrcBufferIsDoubleSized = false;
}
WUPS_MUST_REPLACE_FOR_PROCESS(GX2SetDRCBuffer,
                              WUPS_LOADER_LIBRARY_GX2,
                              GX2SetDRCBuffer,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(void, GX2CopyColorBufferToScanBuffer, const GX2ColorBuffer *colorBuffer, GX2ScanTarget scanTarget) {
    sGX2CopyToScanBufferCalls++;

    // "First GamePad Video Rate" throttles DRC0's own copy the same way
    // "Second GamePad Video Rate" throttles DRC1's below -- skipping this
    // specific copy doesn't skip the game's own rendering (that already
    // happened before this hook fires), only the scan-out of this
    // particular frame to DRC0, so DRC0 just keeps showing its last frame
    // on skipped calls rather than losing any rendering work. Never
    // applies to the TV target -- TV output is never touched here.
    bool isDrc0Target = (scanTarget == GX2_SCAN_TARGET_DRC || scanTarget == GX2_SCAN_TARGET_DRC0);
    bool skipDrc0CopyThisCall = false;
    if (isDrc0Target && sPluginEnabled && sExperimentalPatchEnabled && sDrc0VideoRate != DRC1_VIDEO_RATE_FULL) {
        sDrc0VideoFrameCounter++;
        uint32_t drc0VideoDivisor = (sDrc0VideoRate == DRC1_VIDEO_RATE_HALF) ? 2 : 3;
        skipDrc0CopyThisCall = (sDrc0VideoFrameCounter % drc0VideoDivisor) != 0;
    }
    if (!skipDrc0CopyThisCall) {
        real_GX2CopyColorBufferToScanBuffer(colorBuffer, scanTarget);
    }

    // Keep a copy of the most recent TV frame around so it can be reused
    // as the DRC1 source when "Video Feed" is set to TV Video. This runs
    // regardless of plugin state so the cache is already warm the moment
    // the plugin/feed setting gets turned on.
    if (colorBuffer != nullptr && scanTarget == GX2_SCAN_TARGET_TV) {
        sCachedTvColorBuffer = *colorBuffer;
        sHasCachedTvColorBuffer = true;
    }

    if (!sPluginEnabled || !sExperimentalPatchEnabled || sPerformanceModeEnabled) {
        return;
    }
    if (!sDrcBufferIsDoubleSized) {
        // The active scan buffer hasn't actually been (re)established in
        // double mode yet -- GX2SetDRCBuffer only runs once near process
        // startup, so this happens whenever the plugin gets enabled
        // mid-session. Skip the extra copy entirely rather than write
        // into memory the process never allocated for it. This resolves
        // itself automatically the next time this process's GX2 buffers
        // get set up (e.g. after actually relaunching), no reload needed.
        return;
    }

    // The game only ever knows about a single GamePad, so it copies its
    // rendered frame to the primary DRC scan target exactly once per
    // frame. Forcing GX2_DRC_RENDER_MODE_DOUBLE (see the hooks above)
    // reserves a scan buffer big enough for two GamePad images, but that
    // reservation alone doesn't put any pixels into the second half of
    // it. Mirror a frame onto DRC1 here so it actually gets fresh video
    // every frame -- either the same GamePad frame DRC0 just got, or the
    // most recently captured TV frame, depending on "Video Feed".
    if (colorBuffer != nullptr && (scanTarget == GX2_SCAN_TARGET_DRC || scanTarget == GX2_SCAN_TARGET_DRC0)) {
        sGX2CopyToDrc1Calls++;

        // This copy is a full GPU resolve/blit of an entire frame, on top
        // of everything the game already does for TV+DRC0 -- the single
        // biggest cost this plugin adds, on hardware that wasn't fast to
        // begin with. "DRC1 Video Rate" trades some of that GPU time back
        // by skipping this specific copy on a fraction of frames; DRC1
        // just keeps showing its last copied frame in between rather than
        // freezing, the same way it would if the game itself were only
        // updating at a lower rate.
        sDrc1VideoFrameCounter++;
        uint32_t drc1VideoDivisor = 1;
        if (sDrc1VideoRate == DRC1_VIDEO_RATE_HALF) {
            drc1VideoDivisor = 2;
        } else if (sDrc1VideoRate == DRC1_VIDEO_RATE_THIRD) {
            drc1VideoDivisor = 3;
        }
        if ((sDrc1VideoFrameCounter % drc1VideoDivisor) != 0) {
            return;
        }

        const GX2ColorBuffer *drc1Source = colorBuffer;
        if (sVideoFeedMode == VIDEO_FEED_TV && sHasCachedTvColorBuffer) {
            drc1Source = &sCachedTvColorBuffer;
        }

        // GaryOderNichts' MultiDRCSpaceDemo (the reference this plugin is
        // built on) never calls GX2CopyColorBufferToScanBuffer without
        // first (re-)binding that exact buffer as the active render
        // target via GX2SetColorBuffer/GX2SetViewport/GX2SetScissor. The
        // copy-to-scanbuffer path depends on that binding, not just on
        // the pointer argument -- calling it "cold" a second time is what
        // was producing torn/garbled video instead of a clean mirror.
        // Re-asserting it here with the source buffer's own dimensions
        // (TV and GamePad resolutions differ, so this must match whichever
        // source we picked above) is a no-op relative to the game's own
        // state when the source is DRC0's own buffer, and a real rebind
        // when the source is the cached TV buffer -- either way it
        // doesn't disturb whatever the game binds next.
        GX2ColorBuffer *mutableColorBuffer = const_cast<GX2ColorBuffer *>(drc1Source);
        GX2SetColorBuffer(mutableColorBuffer, GX2_RENDER_TARGET_0);
        GX2SetViewport(0.0f,
                       0.0f,
                       static_cast<float>(drc1Source->surface.width),
                       static_cast<float>(drc1Source->surface.height),
                       0.0f,
                       1.0f);
        GX2SetScissor(0, 0, drc1Source->surface.width, drc1Source->surface.height);

        real_GX2CopyColorBufferToScanBuffer(drc1Source, GX2_SCAN_TARGET_DRC1);
    }
}
WUPS_MUST_REPLACE_FOR_PROCESS(GX2CopyColorBufferToScanBuffer,
                              WUPS_LOADER_LIBRARY_GX2,
                              GX2CopyColorBufferToScanBuffer,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError) {
    sVPADHookCalls++;
    int32_t readCount = real_VPADRead(chan, buffers, count, outError);

    if (chan == VPAD_CHAN_0) {
        // The game reliably calls VPADRead(chan=0) once per frame, so
        // treat this as the "new frame" boundary for the shared DRC1
        // fetch cache (see FetchDrc1Vpad).
        sDrc1CacheValidThisFrame = false;
        CheckControllerModeToggleCombo(buffers, readCount, sControllerModeToggleComboLatched);
        CheckVideoFeedToggleCombo(buffers, readCount, sVideoFeedToggleComboLatched);
    }

    if (!sPluginEnabled || !sExperimentalPatchEnabled) {
        return readCount;
    }
    if (chan != VPAD_CHAN_0 || buffers == nullptr || count == 0 || readCount <= 0) {
        return readCount;
    }

    // Buttons/sticks/motion only apply in Mirrored controller mode -- DRC1
    // becomes the same input identity as DRC0 there, which is what makes
    // merging those make sense. Touch is its own independent setting
    // though (per-request: it should keep working even in Separate
    // controller mode), so fetch DRC1 if either one actually needs it.
    bool needsControlsMerge = IsMirroredMode();
    bool needsTouchMerge = (sTouchMode == TOUCH_MODE_MIRRORED);
    if (!needsControlsMerge && !needsTouchMerge) {
        return readCount;
    }

    VPADStatus drc1 = {};
    if (!FetchDrc1Vpad(&drc1)) {
        return readCount;
    }

    uint32_t samplesToMerge = std::min<uint32_t>(count, static_cast<uint32_t>(readCount));
    for (uint32_t i = 0; i < samplesToMerge; i++) {
        if (needsControlsMerge) {
            buffers[i].hold |= drc1.hold;
            buffers[i].trigger |= drc1.trigger;
            buffers[i].release |= drc1.release;

            if (std::abs(drc1.leftStick.x) > std::abs(buffers[i].leftStick.x)) {
                buffers[i].leftStick.x = drc1.leftStick.x;
            }
            if (std::abs(drc1.leftStick.y) > std::abs(buffers[i].leftStick.y)) {
                buffers[i].leftStick.y = drc1.leftStick.y;
            }
            if (std::abs(drc1.rightStick.x) > std::abs(buffers[i].rightStick.x)) {
                buffers[i].rightStick.x = drc1.rightStick.x;
            }
            if (std::abs(drc1.rightStick.y) > std::abs(buffers[i].rightStick.y)) {
                buffers[i].rightStick.y = drc1.rightStick.y;
            }

            // Motion (accelerometer/gyro/angle/direction/magnetometer) is a
            // coherent bundle describing how a single physical pad is
            // oriented and moving, so it doesn't make sense to blend it
            // field-by-field the way sticks are above. Instead, pick
            // whichever pad is actively being moved right now and forward
            // its whole motion state. `accelorometer.variation` is the
            // length of the change in acceleration since the last sample,
            // making it a much better "is this pad moving" signal than raw
            // magnitude (which sits around 1g at rest from gravity alone).
            // Only when Motion Mode is Mirrored -- Off skips this entirely,
            // and Split Wii Remote forwards DRC1's motion on its own separate
            // synthetic channel instead of blending it in here.
            if (sMotionMode == MOTION_MODE_MIRRORED && drc1.accelorometer.variation > buffers[i].accelorometer.variation) {
                buffers[i].accelorometer = drc1.accelorometer;
                buffers[i].gyro = drc1.gyro;
                buffers[i].angle = drc1.angle;
                buffers[i].direction = drc1.direction;
                buffers[i].mag = drc1.mag;
            }
        }

        // Touch: wherever you press on the second GamePad presses on the
        // first. Only one touch state can be reported at a time (VPAD
        // isn't multi-touch across pads), so DRC1 takes priority whenever
        // it's actually being touched -- forwarding its whole touch state
        // (current position plus both filtered/smoothed levels, so
        // whichever field a game reads stays internally consistent)
        // rather than just tpNormal alone. When DRC1 isn't touched, DRC0's
        // own touchscreen is left completely alone and keeps working
        // normally on its own. Gated on its own "Touchscreen" setting,
        // independent of Controller Mode.
        if (needsTouchMerge && drc1.tpNormal.touched != 0) {
            buffers[i].tpNormal = drc1.tpNormal;
            buffers[i].tpFiltered1 = drc1.tpFiltered1;
            buffers[i].tpFiltered2 = drc1.tpFiltered2;
        }
    }
    sVPADMergedCalls++;

    return readCount;
}
WUPS_MUST_REPLACE_FOR_PROCESS(VPADRead,
                              WUPS_LOADER_LIBRARY_VPAD,
                              VPADRead,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

bool FetchDrc1Vpad(VPADStatus *outStatus) {
    if (!sDrc1CacheValidThisFrame) {
        sVPADDrc1ReadAttempts++;
        sCachedDrc1ReadCount = real_VPADRead(VPAD_CHAN_1, &sCachedDrc1Status, 1, &sCachedDrc1Error);
        sLastDrc1ReadCount = sCachedDrc1ReadCount;
        sLastDrc1ReadError = sCachedDrc1Error;
        sDrc1CacheValidThisFrame = true;

        if (sCachedDrc1ReadCount > 0 && sCachedDrc1Error == VPAD_READ_SUCCESS) {
            sVPADDrc1ReadSuccess++;
            // The second GamePad's own button combos (e.g. toggling
            // Controller Mode or Video Feed) should work no matter which
            // mode is currently active, so check them here -- this is the
            // only place in the hook chain a fresh DRC1 sample is
            // guaranteed to exist exactly once per frame.
            CheckControllerModeToggleCombo(&sCachedDrc1Status, sCachedDrc1ReadCount, sControllerModeToggleComboLatchedDrc1);
            CheckVideoFeedToggleCombo(&sCachedDrc1Status, sCachedDrc1ReadCount, sVideoFeedToggleComboLatchedDrc1);
        } else {
            switch (sCachedDrc1Error) {
                case VPAD_READ_NO_SAMPLES:
                    sVPADDrc1NoSamples++;
                    break;
                case VPAD_READ_INVALID_CONTROLLER:
                    sVPADDrc1Invalid++;
                    break;
                case VPAD_READ_BUSY:
                    sVPADDrc1Busy++;
                    break;
                case VPAD_READ_UNINITIALIZED:
                    sVPADDrc1Uninitialized++;
                    break;
                default:
                    sVPADDrc1OtherError++;
                    break;
            }
        }
    }

    if (sCachedDrc1ReadCount <= 0 || sCachedDrc1Error != VPAD_READ_SUCCESS) {
        return false;
    }
    if (outStatus != nullptr) {
        *outStatus = sCachedDrc1Status;
    }
    return true;
}

DECL_FUNCTION(WPADError, WPADProbe, WPADChan channel, WPADExtensionType *outExtensionType) {
    WPADError result = real_WPADProbe(channel, outExtensionType);

    if (!sPluginEnabled) {
        return result;
    }
    if (IsSeparateMode() && IsSyntheticControllerChannel(channel)) {
        if (outExtensionType != nullptr) {
            *outExtensionType = WPAD_EXT_PRO_CONTROLLER;
        }
        return WPAD_ERROR_NONE;
    }
    if (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel(channel)) {
        if (outExtensionType != nullptr) {
            *outExtensionType = WPAD_EXT_MPLUS;
        }
        return WPAD_ERROR_NONE;
    }
    return result;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADProbe,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADProbe,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(WPADError, WPADGetInfo, WPADChan channel, WPADInfo *outInfo) {
    WPADError result = real_WPADGetInfo(channel, outInfo);

    if (!sPluginEnabled) {
        return result;
    }
    bool isSynthetic = (IsSeparateMode() && IsSyntheticControllerChannel(channel)) ||
                        (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel(channel));
    if (!isSynthetic) {
        return result;
    }

    FillSyntheticWpadInfo(outInfo);
    return WPAD_ERROR_NONE;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADGetInfo,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADGetInfo,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(WPADError, WPADGetInfoAsync, WPADChan channel, WPADInfo *outInfo, WPADCallback callback) {
    WPADError result = real_WPADGetInfoAsync(channel, outInfo, callback);

    if (!sPluginEnabled) {
        return result;
    }
    bool isSynthetic = (IsSeparateMode() && IsSyntheticControllerChannel(channel)) ||
                        (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel(channel));
    if (!isSynthetic) {
        return result;
    }

    FillSyntheticWpadInfo(outInfo);
    return WPAD_ERROR_NONE;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADGetInfoAsync,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADGetInfoAsync,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(WPADError, WPADSetDataFormat, WPADChan channel, WPADDataFormat format) {
    WPADError result = real_WPADSetDataFormat(channel, format);

    if (!sPluginEnabled) {
        return result;
    }
    bool isSynthetic = (IsSeparateMode() && IsSyntheticControllerChannel(channel)) ||
                        (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel(channel));
    if (!isSynthetic) {
        return result;
    }

    return WPAD_ERROR_NONE;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADSetDataFormat,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADSetDataFormat,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(WPADDataFormat, WPADGetDataFormat, WPADChan channel) {
    WPADDataFormat result = real_WPADGetDataFormat(channel);

    if (!sPluginEnabled) {
        return result;
    }
    if (IsSeparateMode() && IsSyntheticControllerChannel(channel)) {
        return WPAD_FMT_PRO_CONTROLLER;
    }
    if (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel(channel)) {
        return WPAD_FMT_MPLUS;
    }
    return result;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADGetDataFormat,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADGetDataFormat,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

// Some games check specifically for MotionPlus (rather than just reading
// extensionType off WPADProbe/KPADReadEx) before trusting KPADStatus::mplus.
// Report it truthfully present+active for the synthetic Wiimote channel.
DECL_FUNCTION(int32_t, WPADIsMplsIntegrated, WPADChan channel) {
    if (sPluginEnabled && IsSplitWiimoteMode() && IsSyntheticWiimoteChannel(channel)) {
        return 1;
    }
    return real_WPADIsMplsIntegrated(channel);
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADIsMplsIntegrated,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADIsMplsIntegrated,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(KPADMplsMode, KPADGetMplsStatus, KPADChan chan) {
    if (sPluginEnabled && IsSplitWiimoteMode() && IsSyntheticWiimoteChannel((WPADChan) chan)) {
        return WPAD_MPLS_MODE_MPLS_ONLY;
    }
    return real_KPADGetMplsStatus(chan);
}
WUPS_MUST_REPLACE_FOR_PROCESS(KPADGetMplsStatus,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              KPADGetMplsStatus,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(uint8_t, WPADGetBatteryLevel, WPADChan channel) {
    uint8_t result = real_WPADGetBatteryLevel(channel);

    if (!sPluginEnabled) {
        return result;
    }
    bool isSynthetic = (IsSeparateMode() && IsSyntheticControllerChannel(channel)) ||
                        (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel(channel));
    if (!isSynthetic) {
        return result;
    }

    // Avoid "0% battery" for the synthetic controller in game UIs.
    return 4;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADGetBatteryLevel,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADGetBatteryLevel,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(int32_t, VPADControlMotor, VPADChan chan, uint8_t *pattern, uint8_t length) {
    int32_t result = real_VPADControlMotor(chan, pattern, length);

    // Mirror rumble to DRC1 whenever the primary gamepad is told to rumble.
    if (sPluginEnabled && chan == VPAD_CHAN_0) {
        // If the pattern indicates 'on' (any non-zero byte), send an amplified
        // pattern to make the DRC1 motor feel stronger.
        bool anyOn = false;
        if (pattern != nullptr && length > 0) {
            for (uint8_t i = 0; i < length; ++i) {
                if (pattern[i] != 0) {
                    anyOn = true;
                    break;
                }
            }
        }

        if (anyOn) {
            // Stronger pattern: repeated on bytes for a longer duration.
            uint8_t amplified[8];
            for (int i = 0; i < (int)sizeof(amplified); ++i) amplified[i] = 0xFF;
            real_VPADControlMotor(VPAD_CHAN_1, amplified, sizeof(amplified));
        } else {
            // Ensure motor is stopped on DRC1 when primary stops.
            uint8_t off = 0x00;
            real_VPADControlMotor(VPAD_CHAN_1, &off, 1);
        }
    }
    return result;
}
WUPS_MUST_REPLACE_FOR_PROCESS(VPADControlMotor,
                              WUPS_LOADER_LIBRARY_VPAD,
                              VPADControlMotor,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(void, WPADControlMotor, WPADChan channel, BOOL motorEnabled) {
    real_WPADControlMotor(channel, motorEnabled);
    // In separate mode the synthetic Pro controller maps to DRC1; forward its rumble.
    if (sPluginEnabled && IsSeparateMode() && IsSyntheticControllerChannel(channel)) {
        if (motorEnabled) {
            uint8_t amplified[8];
            for (int i = 0; i < (int)sizeof(amplified); ++i) amplified[i] = 0xFF;
            real_VPADControlMotor(VPAD_CHAN_1, amplified, sizeof(amplified));
        } else {
            uint8_t off = 0x00;
            real_VPADControlMotor(VPAD_CHAN_1, &off, 1);
        }
    }
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADControlMotor,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADControlMotor,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(void, VPADStopMotor, VPADChan chan) {
    real_VPADStopMotor(chan);
    if (sPluginEnabled && chan == VPAD_CHAN_0) {
        real_VPADStopMotor(VPAD_CHAN_1);
    }
}
WUPS_MUST_REPLACE_FOR_PROCESS(VPADStopMotor,
                              WUPS_LOADER_LIBRARY_VPAD,
                              VPADStopMotor,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(uint32_t, KPADReadEx, KPADChan chan, KPADStatus *data, uint32_t size, KPADError *error) {
    sKPADReadCalls++;
    uint32_t readCount = real_KPADReadEx(chan, data, size, error);

    if (!sPluginEnabled || data == nullptr || size == 0 || readCount > 0) {
        return readCount;
    }

    KPADStatus synthetic = {};
    bool built = false;
    if (IsSeparateMode() && IsSyntheticControllerChannel((WPADChan) chan)) {
        built = BuildSyntheticKpadFromDrc1(&synthetic);
    } else if (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel((WPADChan) chan)) {
        built = BuildSyntheticWiimoteFromDrc1Motion(&synthetic);
    } else {
        return readCount;
    }

    if (!built) {
        sKPADInjectFailures++;
        return readCount;
    }

    data[0] = synthetic;
    if (error != nullptr) {
        *error = KPAD_ERROR_OK;
    }
    sKPADInjectedSamples++;
    return 1;
}
WUPS_MUST_REPLACE_FOR_PROCESS(KPADReadEx,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              KPADReadEx,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

uint32_t my_KPADReadExHomeMenu(KPADChan chan, KPADStatus *data, uint32_t size, KPADError *error) {
    return my_KPADReadEx(chan, data, size, error);
}
WUPS_MUST_REPLACE_EX(NULL,
                     NULL,
                     real_KPADReadEx,
                     WUPS_LOADER_LIBRARY_PADSCORE,
                     my_KPADReadExHomeMenu,
                     KPADReadEx,
                     WUPS_FP_TARGET_PROCESS_HOME_MENU);

DECL_FUNCTION(uint32_t, KPADRead, KPADChan chan, KPADStatus *data, uint32_t size) {
    sKPADReadCalls++;
    uint32_t readCount = real_KPADRead(chan, data, size);

    if (!sPluginEnabled || data == nullptr || size == 0 || readCount > 0) {
        return readCount;
    }

    KPADStatus synthetic = {};
    bool built = false;
    if (IsSeparateMode() && IsSyntheticControllerChannel((WPADChan) chan)) {
        built = BuildSyntheticKpadFromDrc1(&synthetic);
    } else if (IsSplitWiimoteMode() && IsSyntheticWiimoteChannel((WPADChan) chan)) {
        built = BuildSyntheticWiimoteFromDrc1Motion(&synthetic);
    } else {
        return readCount;
    }

    if (!built) {
        sKPADInjectFailures++;
        return readCount;
    }

    data[0] = synthetic;
    sKPADInjectedSamples++;
    return 1;
}
WUPS_MUST_REPLACE_FOR_PROCESS(KPADRead,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              KPADRead,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

uint32_t my_KPADReadHomeMenu(KPADChan chan, KPADStatus *data, uint32_t size) {
    return my_KPADRead(chan, data, size);
}
WUPS_MUST_REPLACE_EX(NULL,
                     NULL,
                     real_KPADRead,
                     WUPS_LOADER_LIBRARY_PADSCORE,
                     my_KPADReadHomeMenu,
                     KPADRead,
                     WUPS_FP_TARGET_PROCESS_HOME_MENU);

int32_t PairNow_getCurrentValueDisplay(void *, char *out_buf, int32_t out_size) {
    snprintf(out_buf, out_size, "  Press A");
    return 0;
}

int32_t PairNow_getCurrentValueSelectedDisplay(void *, char *out_buf, int32_t out_size) {
    snprintf(out_buf, out_size, "< Press A >");
    return 0;
}

void PairNow_onInput(void *, WUPSConfigSimplePadData input) {
    if ((input.buttons_d & WUPS_CONFIG_BUTTON_A) == WUPS_CONFIG_BUTTON_A) {
        if (sPairing.getState() == DrcPairing::STATE_PAIRING) {
            // Pressing A again while a pairing attempt is already running
            // cancels it, rather than silently doing nothing (startPairing
            // already refuses to start a second attempt on top of a
            // running one). This is a real, working cancel path from the
            // GamePad itself. I looked for a way to hook the console's
            // physical SYNC button specifically -- checked wut's nn_ccr
            // export list and the WUPS button_combo API you shared -- and
            // found no exposed way to read it; it appears to be handled
            // entirely inside IOSU as part of the pairing handshake
            // itself, below anything homebrew can observe directly.
            sPairing.stopPairing();
            return;
        }
        // Allow pairing regardless of plugin enabled state — the user needs to
        // pair before using the plugin, so don't block on sPluginEnabled.
        ApplyDualDrcMode(true);
        sPairing.startPairing(120);
    }
}

int32_t PairStatus_getCurrentValueDisplay(void *, char *out_buf, int32_t out_size) {
    switch (sPairing.getState()) {
        case DrcPairing::STATE_PAIRING:
            snprintf(out_buf, out_size, "  Pairing...");
            break;
        case DrcPairing::STATE_DONE:
            snprintf(out_buf, out_size, "  Paired");
            break;
        case DrcPairing::STATE_ERROR:
            snprintf(out_buf, out_size, "  Failed/Cancelled");
            break;
        case DrcPairing::STATE_IDLE:
        default:
            snprintf(out_buf, out_size, "  Idle");
            break;
    }
    return 0;
}

int32_t PairStatus_getCurrentValueSelectedDisplay(void *ctx, char *out_buf, int32_t out_size) {
    return PairStatus_getCurrentValueDisplay(ctx, out_buf, out_size);
}

int32_t Pin_getCurrentValueDisplay(void *, char *out_buf, int32_t out_size) {
    const std::string pin = sPairing.getPinSymbols();
    if (pin.empty()) {
        snprintf(out_buf, out_size, "  ----");
    } else {
        snprintf(out_buf, out_size, "  %s", pin.c_str());
    }
    return 0;
}

int32_t Pin_getCurrentValueSelectedDisplay(void *ctx, char *out_buf, int32_t out_size) {
    return Pin_getCurrentValueDisplay(ctx, out_buf, out_size);
}

// Used for purely informational rows (section headers/instructions) that
// have no value of their own -- the text lives entirely in the item's
// displayName, so the value column is left blank.
int32_t StaticText_getCurrentValueDisplay(void *, char *out_buf, int32_t out_size) {
    if (out_buf != nullptr && out_size > 0) {
        out_buf[0] = '\0';
    }
    return 0;
}

int32_t StaticText_getCurrentValueSelectedDisplay(void *ctx, char *out_buf, int32_t out_size) {
    return StaticText_getCurrentValueDisplay(ctx, out_buf, out_size);
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle root) {
    if (sPluginEnabled) {
        ApplyDualDrcMode(true);
    }

    if (WUPSConfigItemBoolean_AddToCategoryEx(root,
                                               "dual_drc_enabled",
                                               "Plugin Enabled",
                                               kDefaultPluginEnabled,
                                               sPluginEnabled,
                                               &PluginEnabledChanged,
                                               "On",
                                               "Off") != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    WUPSConfigCategoryHandle pairingCategory;
    WUPSConfigAPICreateCategoryOptionsV1 pairingCategoryOptions = {.name = "Pairing"};
    if (WUPSConfigAPI_Category_Create(pairingCategoryOptions, &pairingCategory) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    {
        WUPSConfigItemHandle itemHandle;
        WUPSConfigAPIItemCallbacksV2 callbacks = {
                .getCurrentValueDisplay         = &PairNow_getCurrentValueDisplay,
                .getCurrentValueSelectedDisplay = &PairNow_getCurrentValueSelectedDisplay,
                .onSelected                     = nullptr,
                .restoreDefault                 = nullptr,
                .isMovementAllowed              = nullptr,
                .onCloseCallback                = nullptr,
                .onInput                        = &PairNow_onInput,
                .onInputEx                      = nullptr,
                .onDelete                       = nullptr,
        };
        WUPSConfigAPIItemOptionsV2 options = {
                .displayName = "Start Pairing",
                .context     = nullptr,
                .callbacks   = callbacks,
        };
        if (WUPSConfigAPI_Item_Create(options, &itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_Destroy(pairingCategory);
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
        if (WUPSConfigAPI_Category_AddItem(pairingCategory, itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Item_Destroy(itemHandle);
            WUPSConfigAPI_Category_Destroy(pairingCategory);
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    {
        WUPSConfigItemHandle itemHandle;
        WUPSConfigAPIItemCallbacksV2 callbacks = {
                .getCurrentValueDisplay         = &PairStatus_getCurrentValueDisplay,
                .getCurrentValueSelectedDisplay = &PairStatus_getCurrentValueSelectedDisplay,
                .onSelected                     = nullptr,
                .restoreDefault                 = nullptr,
                .isMovementAllowed              = nullptr,
                .onCloseCallback                = nullptr,
                .onInput                        = nullptr,
                .onInputEx                      = nullptr,
                .onDelete                       = nullptr,
        };
        WUPSConfigAPIItemOptionsV2 options = {
                .displayName = "Pairing Status",
                .context     = nullptr,
                .callbacks   = callbacks,
        };
        if (WUPSConfigAPI_Item_Create(options, &itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_Destroy(pairingCategory);
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
        if (WUPSConfigAPI_Category_AddItem(pairingCategory, itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Item_Destroy(itemHandle);
            WUPSConfigAPI_Category_Destroy(pairingCategory);
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    {
        WUPSConfigItemHandle itemHandle;
        WUPSConfigAPIItemCallbacksV2 callbacks = {
                .getCurrentValueDisplay         = &Pin_getCurrentValueDisplay,
                .getCurrentValueSelectedDisplay = &Pin_getCurrentValueSelectedDisplay,
                .onSelected                     = nullptr,
                .restoreDefault                 = nullptr,
                .isMovementAllowed              = nullptr,
                .onCloseCallback                = nullptr,
                .onInput                        = nullptr,
                .onInputEx                      = nullptr,
                .onDelete                       = nullptr,
        };
        WUPSConfigAPIItemOptionsV2 options = {
                .displayName = "PIN (♠♥♦♣)",
                .context     = nullptr,
                .callbacks   = callbacks,
        };
        if (WUPSConfigAPI_Item_Create(options, &itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Category_Destroy(pairingCategory);
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
        if (WUPSConfigAPI_Category_AddItem(pairingCategory, itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
            WUPSConfigAPI_Item_Destroy(itemHandle);
            WUPSConfigAPI_Category_Destroy(pairingCategory);
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }
    }

    if (WUPSConfigAPI_Category_AddCategory(root, pairingCategory) != WUPSCONFIG_API_RESULT_SUCCESS) {
        WUPSConfigAPI_Category_Destroy(pairingCategory);
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(root,
                                                    "dual_drc_controller_mode",
                                                    "Controller Mode",
                                                    static_cast<int>(kDefaultControllerMode),
                                                    static_cast<int>(sControllerMode),
                                                    sControllerModeValues,
                                                    sizeof(sControllerModeValues) / sizeof(sControllerModeValues[0]),
                                                    &ControllerModeChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(root,
                                                    "dual_drc_controller_mapping_channel",
                                                    "Controller Mapping",
                                                    static_cast<int>(kDefaultControllerMappingChannel),
                                                    static_cast<int>(sSyntheticControllerChannel),
                                                    sControllerMappingValues,
                                                    sizeof(sControllerMappingValues) / sizeof(sControllerMappingValues[0]),
                                                    &ControllerMappingChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(root,
                                                    "dual_drc_video_feed_mode",
                                                    "Video Feed",
                                                    static_cast<int>(kDefaultVideoFeedMode),
                                                    static_cast<int>(sVideoFeedMode),
                                                    sVideoFeedValues,
                                                    sizeof(sVideoFeedValues) / sizeof(sVideoFeedValues[0]),
                                                    &VideoFeedModeChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(root,
                                                    "dual_drc_motion_mode",
                                                    "Motion Controls",
                                                    static_cast<int>(kDefaultMotionMode),
                                                    static_cast<int>(sMotionMode),
                                                    sMotionModeValues,
                                                    sizeof(sMotionModeValues) / sizeof(sMotionModeValues[0]),
                                                    &MotionModeChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(root,
                                                    "dual_drc_touch_mode",
                                                    "Touchscreen",
                                                    static_cast<int>(kDefaultTouchMode),
                                                    static_cast<int>(sTouchMode),
                                                    sTouchModeValues,
                                                    sizeof(sTouchModeValues) / sizeof(sTouchModeValues[0]),
                                                    &TouchModeChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(root,
                                                    "dual_drc_drc0_video_rate",
                                                    "First GamePad Video Rate",
                                                    static_cast<int>(kDefaultDrc0VideoRate),
                                                    static_cast<int>(sDrc0VideoRate),
                                                    sDrc1VideoRateValues,
                                                    sizeof(sDrc1VideoRateValues) / sizeof(sDrc1VideoRateValues[0]),
                                                    &Drc0VideoRateChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemMultipleValues_AddToCategory(root,
                                                    "dual_drc_drc1_video_rate",
                                                    "Second GamePad Video Rate",
                                                    static_cast<int>(kDefaultDrc1VideoRate),
                                                    static_cast<int>(sDrc1VideoRate),
                                                    sDrc1VideoRateValues,
                                                    sizeof(sDrc1VideoRateValues) / sizeof(sDrc1VideoRateValues[0]),
                                                    &Drc1VideoRateChanged) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    if (WUPSConfigItemBoolean_AddToCategoryEx(root,
                                               "dual_drc_performance_mode",
                                               "Performance Mode (disable DRC1 video)",
                                               kDefaultPerformanceModeEnabled,
                                               sPerformanceModeEnabled,
                                               &PerformanceModeChanged,
                                               "On",
                                               "Off") != WUPSCONFIG_API_RESULT_SUCCESS) {
        return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
    }

    {
        static const char *const kControlsHelpLines[] = {
                "----------In game button Controls----------",
                "Press Left Stick and Right Stick together to toggle controller mode",
                "Press Right D-Pad + Y button to toggle video mode",
        };
        for (const char *line : kControlsHelpLines) {
            WUPSConfigItemHandle itemHandle;
            WUPSConfigAPIItemCallbacksV2 callbacks = {
                    .getCurrentValueDisplay         = &StaticText_getCurrentValueDisplay,
                    .getCurrentValueSelectedDisplay = &StaticText_getCurrentValueSelectedDisplay,
                    .onSelected                     = nullptr,
                    .restoreDefault                 = nullptr,
                    .isMovementAllowed              = nullptr,
                    .onCloseCallback                = nullptr,
                    .onInput                        = nullptr,
                    .onInputEx                      = nullptr,
                    .onDelete                       = nullptr,
            };
            WUPSConfigAPIItemOptionsV2 options = {
                    .displayName = line,
                    .context     = nullptr,
                    .callbacks   = callbacks,
            };
            if (WUPSConfigAPI_Item_Create(options, &itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
                return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
            }
            if (WUPSConfigAPI_Category_AddItem(root, itemHandle) != WUPSCONFIG_API_RESULT_SUCCESS) {
                WUPSConfigAPI_Item_Destroy(itemHandle);
                return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
            }
        }
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    SaveSettingsToStorage();
}
} // namespace

INITIALIZE_PLUGIN() {
    LoadSettingsFromStorage();
    ApplyDualDrcMode(sPluginEnabled);
    WUPSConfigAPIOptionsV1 configOptions = {.name = "Dual U"};
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);
}

ON_APPLICATION_START() {
    if (sPluginEnabled) {
        ApplyDualDrcMode(true);
    }
}

ON_ACQUIRED_FOREGROUND() {
    if (sPluginEnabled) {
        ApplyDualDrcMode(true);
    }
}

ON_RELEASE_FOREGROUND() {
    CCRCDCSetMultiDrc(1);
}

DEINITIALIZE_PLUGIN() {
    if (sPairing.getState() == DrcPairing::STATE_PAIRING) {
        sPairing.stopPairing();
    }
    ApplyDualDrcMode(false);
}