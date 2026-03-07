#pragma once

#include <string>

// Lightweight wrapper around CCR pairing functionality.
class DrcPairing {
public:
    enum State {
        STATE_IDLE,
        STATE_PAIRING,
        STATE_DONE,
        STATE_ERROR
    };

    DrcPairing();
    ~DrcPairing();

    // Start pairing the second DRCPad (slot 1). Returns true on async-start.
    bool startPairing(unsigned timeoutSeconds = 120);
    // Stop/cancel any active pairing.
    void stopPairing();

    // Get current pairing state.
    State getState();

    // If pairing has started, returns a 4-symbol string using suits (♠♥♦♣).
    // Empty string if no pin available.
    std::string getPinSymbols() const;

private:
    State state;
    unsigned timeoutSeconds;
    uint32_t pincode;
};