#include "DrcPairing.hpp"

#include <nn/ccr.h>

DrcPairing::DrcPairing()
    : state(STATE_IDLE), timeoutSeconds(0), pincode(0)
{
    CCRSysInit();
}

DrcPairing::~DrcPairing()
{
    CCRSysExit();
}

bool DrcPairing::startPairing(unsigned timeout)
{
    if (state == STATE_PAIRING) return false;
    timeoutSeconds = timeout;

    // Get pincode
    uint32_t code = 0;
    if (CCRSysGetPincode(&code) != 0) {
        state = STATE_ERROR;
        return false;
    }
    pincode = code;

    // Start pairing on slot 1 (second controller)
    if (CCRSysStartPairing(1, timeoutSeconds) != 0) {
        state = STATE_ERROR;
        return false;
    }

    state = STATE_PAIRING;
    return true;
}

void DrcPairing::stopPairing()
{
    if (state != STATE_PAIRING) return;
    CCRSysStopPairing();
    state = STATE_IDLE;
}

DrcPairing::State DrcPairing::getState()
{
    if (state != STATE_PAIRING) return state;

    CCRSysPairingState s = CCRSysGetPairingState();

    if (s == CCR_SYS_PAIRING_TIMED_OUT) {
        CCRSysStopPairing();
        state = STATE_ERROR;
        return state;
    }

    if (s == CCR_SYS_PAIRING_FINISHED) {
        state = STATE_DONE;
        return state;
    }

    return STATE_PAIRING;
}

std::string DrcPairing::getPinSymbols() const
{
    if (pincode == 0) return std::string();
    static const char* pinSymbols[] = {"\u2660", "\u2665", "\u2666", "\u2663"};

    // Compose UTF-8 symbols (each symbol here is multi-byte UTF-8 literal)
    std::string out;
    uint32_t digits[4] = { (pincode / 1000) % 10, (pincode / 100) % 10, (pincode / 10) % 10, pincode % 10 };
    for (int i = 0; i < 4; ++i) {
        const char* sym = pinSymbols[digits[i] % 4];
        out += sym;
    }
    return out;
}