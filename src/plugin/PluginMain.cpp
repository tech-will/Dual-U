#include "../pairing/DrcPairing.hpp"

#include <cstdio>
#include <algorithm>
#include <gx2/display.h>
#include <nsysccr/cdc.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <wups.h>
#include <wups/function_patching.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>
#include <wups/storage.h>

WUPS_PLUGIN_NAME("Dual U");
WUPS_PLUGIN_DESCRIPTION("Pair and use a second Wii U gamepad");
WUPS_PLUGIN_VERSION("v0.1");
WUPS_PLUGIN_AUTHOR("tech-will");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("dual_drc_home_menu");

namespace {
enum ControllerMode : uint32_t {
    CONTROLLER_MODE_MIRRORED = 0,
    CONTROLLER_MODE_SEPARATE_PRO = 1,
};

constexpr bool kDefaultPluginEnabled = false;
constexpr uint32_t kDefaultControllerMode = CONTROLLER_MODE_SEPARATE_PRO;
constexpr const char *kStorageKeyEnabled = "dual_drc_enabled";
constexpr const char *kStorageKeyControllerMode = "dual_drc_controller_mode";
constexpr const char *kStorageKeyEnableOnce = "dual_drc_enable_once";
constexpr uint32_t kEmergencyDisableCombo = VPAD_BUTTON_STICK_L | VPAD_BUTTON_STICK_R;
constexpr WPADChan kSyntheticControllerChannel = WPAD_CHAN_0;

bool sPluginEnabled = kDefaultPluginEnabled;
bool sExperimentalPatchEnabled = true;

uint32_t sControllerMode = kDefaultControllerMode;

ConfigItemMultipleValuesPair sControllerModeValues[] = {
        {CONTROLLER_MODE_MIRRORED, "Mirrored"},
    {CONTROLLER_MODE_SEPARATE_PRO, "Seperate"},
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
uint32_t sGX2CalcDRCSizeCalls = 0;
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
bool sEmergencyDisableComboLatched = false;
bool sReloadMenuRequested = false;

void ApplyDualDrcMode(bool enabled);

void SaveSettingsToStorage() {
    // Keep enabled state volatile so power cycles always recover to default off.
    WUPSStorageAPI_StoreBool(nullptr, kStorageKeyEnabled, kDefaultPluginEnabled);
    WUPSStorageAPI_StoreU32(nullptr, kStorageKeyControllerMode, sControllerMode);
    WUPSStorageAPI_SaveStorage(false);
}

void LoadSettingsFromStorage() {
    bool pluginEnabled = kDefaultPluginEnabled;
    if (WUPSStorageAPI_GetBool(nullptr, kStorageKeyEnabled, &pluginEnabled) == WUPS_STORAGE_ERROR_SUCCESS) {
        sPluginEnabled = pluginEnabled;
    }

    bool enableOnce = false;
    if (WUPSStorageAPI_GetBool(nullptr, kStorageKeyEnableOnce, &enableOnce) == WUPS_STORAGE_ERROR_SUCCESS && enableOnce) {
        sPluginEnabled = true;
        WUPSStorageAPI_StoreBool(nullptr, kStorageKeyEnableOnce, false);
        WUPSStorageAPI_SaveStorage(false);
    }

    sExperimentalPatchEnabled = true;

    uint32_t controllerMode = kDefaultControllerMode;
    if (WUPSStorageAPI_GetU32(nullptr, kStorageKeyControllerMode, &controllerMode) == WUPS_STORAGE_ERROR_SUCCESS) {
        if (controllerMode <= CONTROLLER_MODE_SEPARATE_PRO) {
            sControllerMode = controllerMode;
        }
    }
}

void DisablePluginImmediately() {
    if (!sPluginEnabled) {
        return;
    }
    sPluginEnabled = false;
    ApplyDualDrcMode(false);
    if (sPairing.getState() == DrcPairing::STATE_PAIRING) {
        sPairing.stopPairing();
    }
    SaveSettingsToStorage();
}

void CheckEmergencyDisableCombo(const VPADStatus *buffers, int32_t readCount) {
    if (buffers == nullptr || readCount <= 0) {
        sEmergencyDisableComboLatched = false;
        return;
    }

    bool comboPressed = false;
    for (int32_t i = 0; i < readCount; i++) {
        if ((buffers[i].hold & kEmergencyDisableCombo) == kEmergencyDisableCombo) {
            comboPressed = true;
            break;
        }
    }

    if (comboPressed && !sEmergencyDisableComboLatched) {
        DisablePluginImmediately();
    }
    sEmergencyDisableComboLatched = comboPressed;
}

bool IsMirroredMode() {
    return sControllerMode == CONTROLLER_MODE_MIRRORED;
}

bool IsSeparateMode() {
    return sControllerMode == CONTROLLER_MODE_SEPARATE_PRO;
}

bool IsSyntheticControllerChannel(WPADChan chan) {
    return chan == kSyntheticControllerChannel;
}

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
    VPADReadError drc1Error = VPAD_READ_SUCCESS;
    sVPADDrc1ReadAttempts++;
    int32_t drc1ReadCount = VPADRead(VPAD_CHAN_1, &drc1, 1, &drc1Error);
    sLastDrc1ReadCount = drc1ReadCount;
    sLastDrc1ReadError = drc1Error;
    if (drc1ReadCount <= 0 || drc1Error != VPAD_READ_SUCCESS) {
        switch (drc1Error) {
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
        return false;
    }

    sVPADDrc1ReadSuccess++;
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

DECL_FUNCTION(GX2DrcRenderMode, GX2GetSystemDRCMode) {
    sGX2HookCalls++;
    GX2DrcRenderMode mode = real_GX2GetSystemDRCMode();
    if (sPluginEnabled && sExperimentalPatchEnabled) {
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
    if (sPluginEnabled && sExperimentalPatchEnabled) {
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
    if (sPluginEnabled && sExperimentalPatchEnabled) {
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
    if (sPluginEnabled && sExperimentalPatchEnabled) {
        real_GX2SetDRCBuffer(buffer,
                             size,
                             GX2_DRC_RENDER_MODE_DOUBLE,
                             surfaceFormat,
                             bufferingMode);
        return;
    }
    real_GX2SetDRCBuffer(buffer, size, drcRenderMode, surfaceFormat, bufferingMode);
}
WUPS_MUST_REPLACE_FOR_PROCESS(GX2SetDRCBuffer,
                              WUPS_LOADER_LIBRARY_GX2,
                              GX2SetDRCBuffer,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError) {
    sVPADHookCalls++;
    int32_t readCount = real_VPADRead(chan, buffers, count, outError);

    CheckEmergencyDisableCombo(buffers, readCount);

    if (!sPluginEnabled || !sExperimentalPatchEnabled || !IsMirroredMode()) {
        return readCount;
    }
    if (chan != VPAD_CHAN_0 || buffers == nullptr || count == 0 || readCount <= 0) {
        return readCount;
    }

    const uint32_t drc1Count = std::min<uint32_t>(count, 4);
    VPADStatus drc1Buffer[4] = {};
    VPADReadError drc1Error = VPAD_READ_SUCCESS;
    sVPADDrc1ReadAttempts++;
    int32_t drc1ReadCount = real_VPADRead(VPAD_CHAN_1, drc1Buffer, drc1Count, &drc1Error);
    sLastDrc1ReadCount = drc1ReadCount;
    sLastDrc1ReadError = drc1Error;
    if (drc1ReadCount <= 0 || drc1Error != VPAD_READ_SUCCESS) {
        switch (drc1Error) {
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
        return readCount;
    }
    sVPADDrc1ReadSuccess++;

    uint32_t samplesToMerge = std::min<uint32_t>(count, static_cast<uint32_t>(readCount));
    samplesToMerge = std::min<uint32_t>(samplesToMerge, static_cast<uint32_t>(drc1ReadCount));
    for (uint32_t i = 0; i < samplesToMerge; i++) {
        buffers[i].hold |= drc1Buffer[i].hold;
        buffers[i].trigger |= drc1Buffer[i].trigger;
        buffers[i].release |= drc1Buffer[i].release;
    }
    sVPADMergedCalls++;

    return readCount;
}
WUPS_MUST_REPLACE_FOR_PROCESS(VPADRead,
                              WUPS_LOADER_LIBRARY_VPAD,
                              VPADRead,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(WPADError, WPADProbe, WPADChan channel, WPADExtensionType *outExtensionType) {
    WPADError result = real_WPADProbe(channel, outExtensionType);

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel(channel)) {
        return result;
    }

    if (outExtensionType != nullptr) {
        *outExtensionType = WPAD_EXT_PRO_CONTROLLER;
    }
    return WPAD_ERROR_NONE;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADProbe,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADProbe,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(WPADError, WPADGetInfo, WPADChan channel, WPADInfo *outInfo) {
    WPADError result = real_WPADGetInfo(channel, outInfo);

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel(channel)) {
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

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel(channel)) {
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

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel(channel)) {
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

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel(channel)) {
        return result;
    }

    return WPAD_FMT_PRO_CONTROLLER;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADGetDataFormat,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADGetDataFormat,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(uint8_t, WPADGetBatteryLevel, WPADChan channel) {
    uint8_t result = real_WPADGetBatteryLevel(channel);

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel(channel)) {
        return result;
    }

    // Avoid "0% battery" for the synthetic controller in game UIs.
    return 4;
}
WUPS_MUST_REPLACE_FOR_PROCESS(WPADGetBatteryLevel,
                              WUPS_LOADER_LIBRARY_PADSCORE,
                              WPADGetBatteryLevel,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

DECL_FUNCTION(uint32_t, KPADReadEx, KPADChan chan, KPADStatus *data, uint32_t size, KPADError *error) {
    sKPADReadCalls++;
    uint32_t readCount = real_KPADReadEx(chan, data, size, error);

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel((WPADChan) chan) || data == nullptr || size == 0) {
        return readCount;
    }
    if (readCount > 0) {
        return readCount;
    }

    KPADStatus synthetic = {};
    if (!BuildSyntheticKpadFromDrc1(&synthetic)) {
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

    if (!sPluginEnabled || !IsSeparateMode() || !IsSyntheticControllerChannel((WPADChan) chan) || data == nullptr || size == 0) {
        return readCount;
    }
    if (readCount > 0) {
        return readCount;
    }

    KPADStatus synthetic = {};
    if (!BuildSyntheticKpadFromDrc1(&synthetic)) {
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

int32_t ReloadMenu_getCurrentValueDisplay(void *, char *out_buf, int32_t out_size) {
    snprintf(out_buf, out_size, "  Press A");
    return 0;
}

int32_t ReloadMenu_getCurrentValueSelectedDisplay(void *, char *out_buf, int32_t out_size) {
    snprintf(out_buf, out_size, "< Press A >");
    return 0;
}

void PairNow_onInput(void *, WUPSConfigSimplePadData input) {
    if ((input.buttons_d & WUPS_CONFIG_BUTTON_A) == WUPS_CONFIG_BUTTON_A) {
        if (sPluginEnabled) {
            ApplyDualDrcMode(true);
            sPairing.startPairing(120);
        }
    }
}

void ReloadMenu_onInput(void *, WUPSConfigSimplePadData input) {
    if ((input.buttons_d & WUPS_CONFIG_BUTTON_A) == WUPS_CONFIG_BUTTON_A) {
        // Defer until ConfigMenuClosedCallback so changed config items are committed first.
        sReloadMenuRequested = true;
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

    {
        WUPSConfigItemHandle itemHandle;
        WUPSConfigAPIItemCallbacksV2 callbacks = {
            .getCurrentValueDisplay         = &ReloadMenu_getCurrentValueDisplay,
            .getCurrentValueSelectedDisplay = &ReloadMenu_getCurrentValueSelectedDisplay,
                .onSelected                     = nullptr,
                .restoreDefault                 = nullptr,
                .isMovementAllowed              = nullptr,
                .onCloseCallback                = nullptr,
            .onInput                        = &ReloadMenu_onInput,
                .onInputEx                      = nullptr,
                .onDelete                       = nullptr,
        };
        WUPSConfigAPIItemOptionsV2 options = {
            .displayName = "Reload Wii U (use after enabling plugin)",
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

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback() {
    if (!sReloadMenuRequested) {
        return;
    }

    sReloadMenuRequested = false;
    // Preserve final enabled state for the immediate relaunch only.
    WUPSStorageAPI_StoreBool(nullptr, kStorageKeyEnableOnce, sPluginEnabled);
    SaveSettingsToStorage();
    SYSLaunchMenu();
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