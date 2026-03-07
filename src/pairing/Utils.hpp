#ifndef UTILS_HPP
#define UTILS_HPP

#include <cstdint>

// Function to initialize device discovery for gamepads
void initializeDeviceDiscovery();

// Function to check the connection status of a gamepad
bool isGamepadConnected(uint8_t gamepadId);

// Function to get the number of connected gamepads
uint8_t getConnectedGamepadCount();

// Function to reset the connection for a specific gamepad
void resetGamepadConnection(uint8_t gamepadId);

#endif // UTILS_HPP