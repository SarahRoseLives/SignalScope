#pragma once
#include <cstdint>
#include <functional>
#include <string>

// POCSAG protocol state machine. Feed bits; callbacks fire on complete messages.
class PocsagDecoder {
public:
    PocsagDecoder();

    // Set charset (US, DE, DK, SE, FR, SI). Default "US".
    void setCharset(const char* cs);
    void setErrorCorrection(int n); // 0=none, 1=1-bit, 2=2-bit

    // Feed one NRZ bit (0 or 1). The decoder accumulates bits and
    // fires the message callback when a complete POCSAG message is received.
    void feedBit(int bit);

    // Callbacks — called from feedBit().
    std::function<void(uint32_t address, int function, const std::string& text, const std::string& numeric, bool sync)> onMessage;
    std::function<void()> onSync;
    std::function<void()> onLostSync;
    std::function<void()> onRecoveredSync;

    // Stats
    uint32_t totalBits() const;
    uint32_t bitsInSync() const;
    uint32_t totalErrors() const;
    uint32_t correctedErrors() const;
    uint32_t uncorrectedErrors() const;
    bool inSync() const;

    void reset();

private:
    // State machine enum
    enum State : unsigned char {
        NO_SYNC = 0, SYNC = 64, LOSING_SYNC = 65,
        LOST_SYNC = 66, ADDRESS = 67, MESSAGE = 68, END_OF_MESSAGE = 69
    };

    static constexpr uint32_t POCSAG_SYNC = 0x7cd215d8;
    static constexpr uint32_t POCSAG_IDLE = 0x7a89c197;
    static constexpr uint32_t MESSAGE_FLAG = 0x80000000;

    // Port the full state machine from pocsag.c do_one_bit():
    // - NO_SYNC: search for sync word with normal+inverted polarity, BCH correction
    // - SYNC/ADDRESS: decode address words (function bits, address extraction)
    // - MESSAGE: accumulate message data (20 bits per codeword, 5 nibbles)
    // - END_OF_MESSAGE: fire callback, reset
    // Handle sync loss (LOSING_SYNC → LOST_SYNC → NO_SYNC)
    // BCH ECC via bch.h
    // Character set translation (trtab from pocsag.c)
    // Alpha/numeric auto-detection (guesstimate_* functions)
    // SKYPER (ROT-1) support
    // Message too long protection (>512 bytes)

    void do_one_bit(uint32_t rx_data);
    int brute_repair(uint32_t* data);
    void printmessage(bool sync);
    bool word_complete();
    const char* translate_alpha(unsigned char chr) const;
    int print_msg_numeric(std::string& out) const;
    int print_msg_alpha(std::string& out, int caesar) const;

    uint32_t rx_data_ = 0;
    State state_ = NO_SYNC;
    unsigned char rx_bit_ = 0;
    unsigned char rx_word_ = 0;
    unsigned char inverted_ = 0;
    int function_ = -1;
    int address_ = -1;
    unsigned char buffer_[512];
    uint32_t numnibbles_ = 0;

    // Stats
    uint32_t total_bits_ = 0;
    uint32_t bits_in_sync_ = 0;
    uint32_t total_errors_ = 0;
    uint32_t corrected_errors_ = 0;
    uint32_t uncorrected_errors_ = 0;
    uint32_t corrected_1bit_ = 0;
    uint32_t corrected_2bit_ = 0;
    uint32_t bits_not_synced_ = 0;

    int error_correction_ = 2;
    int charset_idx_ = 0; // US
    bool invert_input_ = false;
};
