#include "Utils.hpp"

void initializeDeviceDiscovery() {
}

bool isGamepadConnected(uint8_t gamepadId) {
    return gamepadId < 2;
}

uint8_t getConnectedGamepadCount() {
    return 1;
}

void resetGamepadConnection(uint8_t gamepadId) {
    (void)gamepadId;
}