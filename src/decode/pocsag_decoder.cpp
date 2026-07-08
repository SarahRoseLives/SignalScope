// POCSAG (Post Office Code Standard Advisory Group) radio paging decoder.
//
// Ported from multimon-ng pocsag.c (Thomas Sailer / Elias Oenal / Tobias
// Girstmair / Jason Lingohr, GPLv2) into a self-contained C++17 state machine.
// The verbprintf/JSON output of the original is replaced by std::function
// callbacks; BCH error correction uses the unified bch.h library.

#include "decode/pocsag_decoder.h"
#include "decode/bch.h"

#include <cstring>
#include <string>

/* ---------------------------------------------------------------------- */
/* Local charset indices                                                  */
/* ---------------------------------------------------------------------- */
namespace {

enum { CS_US = 0, CS_DE = 1, CS_DK = 2, CS_SE = 3, CS_FR = 4, CS_SI = 5 };

// SKYPER messages are ROT-1 enciphered.
constexpr int CAESAR_ALPHA = 0;
constexpr int CAESAR_SKYPER = 1;

// ISO 646 national variant: US / IRV (1991). Base translation table.
const char* const kTrtabUS[128] = {
    "<NUL>", "<SOH>", "<STX>", "<ETX>", "<EOT>", "<ENQ>", "<ACK>", "<BEL>",
    "<BS>",  "<HT>",  "<LF>",  "<VT>",  "<FF>",  "<CR>",  "<SO>",  "<SI>",
    "<DLE>", "<DC1>", "<DC2>", "<DC3>", "<DC4>", "<NAK>", "<SYN>", "<ETB>",
    "<CAN>", "<EM>",  "<SUB>", "<ESC>", "<FS>",  "<GS>",  "<RS>",  "<US>",
    " ", "!", "\"", "#", "$", "%", "&", "'",
    "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", ":", ";", "<", "=", ">", "?",
    "@", "A", "B", "C", "D", "E", "F", "G",
    "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W",
    "X", "Y", "Z", "[", "\\", "]", "^", "_",
    "`", "a", "b", "c", "d", "e", "f", "g",
    "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w",
    "x", "y", "z", "{", "|", "}", "~", "<DEL>"
};

[[maybe_unused]] inline unsigned char even_parity(uint32_t data)
{
    unsigned int temp = data ^ (data >> 16);
    temp = temp ^ (temp >> 8);
    temp = temp ^ (temp >> 4);
    temp = temp ^ (temp >> 2);
    temp = temp ^ (temp >> 1);
    return temp & 1;
}

int guesstimate_alpha(unsigned char cp)
{
    if ((cp > 0 && cp < 32) || cp == 127)
        return -5; // Non printable characters are uncommon
    else if ((cp > 32 && cp < 48)
             || (cp > 57 && cp < 65)
             || (cp > 90 && cp < 97)
             || (cp > 122 && cp < 127))
        return -2; // Penalize special characters
    else
        return 1;
}

int guesstimate_numeric(unsigned char cp, int pos)
{
    if (cp == 'U')
        return -10;
    else if (cp == '[' || cp == ']')
        return -5;
    else if (cp == ' ' || cp == '.' || cp == '-')
        return -2;
    else if (pos < 10) // Penalize long messages
        return 5;
    else
        return 0;
}

// returns the n-th seven bit word
unsigned char get7(const unsigned char* buf, int n)
{
    return (unsigned char)(((buf[(n * 7) / 8] << 8) | buf[(n * 7 + 6) / 8]) >> ((n + 1) % 8));
}

// reverses the bit order of a seven bit word
unsigned char rev7(unsigned char b)
{
    return (unsigned char)(((b << 6) & 64) | ((b >> 6) & 1) |
                           ((b << 4) & 32) | ((b >> 4) & 2) |
                           ((b << 2) & 16) | ((b >> 2) & 4) |
                           ((b << 0) & 8));
}

} // namespace

/* ---------------------------------------------------------------------- */

PocsagDecoder::PocsagDecoder()
{
    bch_init();
    std::memset(buffer_, 0, sizeof(buffer_));
}

void PocsagDecoder::setCharset(const char* cs)
{
    if (!cs) { charset_idx_ = CS_US; return; }
    if (!std::strcmp(cs, "DE"))      charset_idx_ = CS_DE;
    else if (!std::strcmp(cs, "DK")) charset_idx_ = CS_DK;
    else if (!std::strcmp(cs, "SE")) charset_idx_ = CS_SE;
    else if (!std::strcmp(cs, "FR")) charset_idx_ = CS_FR;
    else if (!std::strcmp(cs, "SI")) charset_idx_ = CS_SI;
    else                             charset_idx_ = CS_US;
}

void PocsagDecoder::setErrorCorrection(int n)
{
    if (n < 0) n = 0;
    if (n > 2) n = 2;
    error_correction_ = n;
}

const char* PocsagDecoder::translate_alpha(unsigned char chr) const
{
    unsigned char c = chr & 0x7f;
    switch (charset_idx_) {
    case CS_DE:
        switch (c) {
        case 0x5b: return "\xC3\x84"; // Ä
        case 0x5c: return "\xC3\x96"; // Ö
        case 0x5d: return "\xC3\x9C"; // Ü
        case 0x7b: return "\xC3\xA4"; // ä
        case 0x7c: return "\xC3\xB6"; // ö
        case 0x7d: return "\xC3\xBC"; // ü
        case 0x7e: return "\xC3\x9F"; // ß
        default: break;
        }
        break;
    case CS_DK:
        switch (c) {
        case 0x5b: return "\xC3\x86"; // Æ
        case 0x5c: return "\xC3\x98"; // Ø
        case 0x5d: return "\xC3\x85"; // Å
        case 0x7b: return "\xC3\xA6"; // æ
        case 0x7c: return "\xC3\xB8"; // ø
        case 0x7d: return "\xC3\xA5"; // å
        default: break;
        }
        break;
    case CS_SE:
        switch (c) {
        case 0x5b: return "\xC3\x84"; // Ä
        case 0x5c: return "\xC3\x96"; // Ö
        case 0x5d: return "\xC3\x85"; // Å
        case 0x7b: return "\xC3\xA4"; // ä
        case 0x7c: return "\xC3\xB6"; // ö
        case 0x7d: return "\xC3\xA5"; // å
        default: break;
        }
        break;
    case CS_FR:
        switch (c) {
        case 0x24: return "\xC2\xA3"; // £
        case 0x40: return "\xC3\xA0"; // à
        case 0x5b: return "\xC2\xB0"; // °
        case 0x5c: return "\xC3\xA7"; // ç
        case 0x5d: return "\xC2\xA7"; // §
        case 0x5e: return "^";
        case 0x5f: return "_";
        case 0x60: return "\xC2\xB5"; // µ
        case 0x7b: return "\xC3\xA9"; // é
        case 0x7c: return "\xC3\xB9"; // ù
        case 0x7d: return "\xC3\xA8"; // è
        case 0x7e: return "\xC2\xA8"; // ¨
        default: break;
        }
        break;
    case CS_SI:
        switch (c) {
        case 0x40: return "\xC5\xBD"; // Ž
        case 0x5b: return "\xC5\xA0"; // Š
        case 0x5c: return "\xC4\x90"; // Đ
        case 0x5d: return "\xC4\x86"; // Ć
        case 0x5e: return "\xC4\x8C"; // Č
        case 0x60: return "\xC5\xBE"; // ž
        case 0x7b: return "\xC5\xA1"; // š
        case 0x7c: return "\xC4\x91"; // đ
        case 0x7d: return "\xC4\x87"; // ć
        case 0x7e: return "\xC4\x8D"; // č
        default: break;
        }
        break;
    default:
        break;
    }
    return kTrtabUS[c];
}

int PocsagDecoder::print_msg_numeric(std::string& out) const
{
    static const char* conv_table = "084 2.6]195-3U7[";
    const unsigned char* bp = buffer_;
    int len = (int)numnibbles_;
    out.clear();
    for (; len > 0; ++bp, len -= 2) {
        out.push_back(conv_table[(*bp >> 4) & 0xf]);
        if (len > 1)
            out.push_back(conv_table[*bp & 0xf]);
    }

    int guesstimate = 0;
    for (size_t i = 0; i < out.size(); ++i)
        guesstimate += guesstimate_numeric((unsigned char)out[i], (int)i);
    return guesstimate;
}

int PocsagDecoder::print_msg_alpha(std::string& out, int caesar) const
{
    int len = (int)(numnibbles_ * 4 / 7);
    out.clear();
    int guesstimate = 0;

    for (int i = 0; i < len; ++i) {
        unsigned char curchr = (unsigned char)(rev7(get7(buffer_, i)) - caesar);
        guesstimate += guesstimate_alpha(curchr);

        const char* tstr = translate_alpha(curchr);
        if (tstr)
            out += tstr;
        else
            out.push_back((char)curchr);
    }
    return guesstimate;
}

/* ---------------------------------------------------------------------- */

void PocsagDecoder::printmessage(bool sync)
{
    // Hide partial decodes (unknown address/function or not in sync).
    if ((address_ == -2) || (function_ == -2) || !sync)
        return;
    if (address_ == -1 && function_ == -1)
        return;

    if (numnibbles_ == 0) {
        // Tone-only / address-only page.
        if (onMessage)
            onMessage((uint32_t)address_, function_, std::string(), std::string(), sync);
        return;
    }

    std::string num_string, alpha_string, skyper_string;
    print_msg_numeric(num_string);
    int guess_alpha  = print_msg_alpha(alpha_string, CAESAR_ALPHA);
    int guess_skyper = print_msg_alpha(skyper_string, CAESAR_SKYPER);

    // Pick the more plausible alpha rendering (plain vs SKYPER ROT-1).
    const std::string& best_alpha =
        (guess_alpha >= guess_skyper) ? alpha_string : skyper_string;

    if (onMessage)
        onMessage((uint32_t)address_, function_, best_alpha, num_string, sync);
}

/* ---------------------------------------------------------------------- */

int PocsagDecoder::brute_repair(uint32_t* data)
{
    int result = bch_pocsag_correct(data);

    if (result == 0)
        return 0; // No errors

    total_errors_++;

    if (error_correction_ == 0) {
        uncorrected_errors_++;
        return 1;
    }

    if (result < 0) {
        // Uncorrectable error
        uncorrected_errors_++;
        return 1;
    }

    // Refuse to accept more corrections than the user permits.
    if (result > error_correction_) {
        uncorrected_errors_++;
        return 1;
    }

    corrected_errors_++;
    if (result == 1)
        corrected_1bit_++;
    else
        corrected_2bit_++;

    return 0;
}

bool PocsagDecoder::word_complete()
{
    // Do nothing for 31 bits; when the 32nd arrives the word is complete.
    rx_bit_ = (unsigned char)((rx_bit_ + 1) % 32);
    return rx_bit_ == 0;
}

/* ---------------------------------------------------------------------- */

void PocsagDecoder::do_one_bit(uint32_t rx_data)
{
    total_bits_++;

    switch (state_ & SYNC) {
    case NO_SYNC: {
        uint32_t rx_data_try;

        bits_not_synced_++;

        // Try normal polarity with error correction.
        rx_data_try = rx_data;
        brute_repair(&rx_data_try);
        if (rx_data_try == POCSAG_SYNC) {
            state_ = SYNC;
            inverted_ = 0;
            if (onSync) onSync();
            return;
        }

        // Try inverted polarity with error correction.
        rx_data_try = ~rx_data;
        brute_repair(&rx_data_try);
        if (rx_data_try == POCSAG_SYNC) {
            state_ = SYNC;
            inverted_ = 1;
            if (onSync) onSync();
            return;
        }
        return;
    }

    case SYNC: {
        bits_in_sync_++;

        // Apply inversion if auto-detected at sync acquisition.
        if (inverted_)
            rx_data = ~rx_data;

        if (!word_complete())
            return; // Wait for more bits to arrive.

        // 17 words per frame: position 0 is sync, positions 1-16 are data.
        unsigned char rxword = rx_word_; // for address calculation
        rx_word_ = (unsigned char)((rx_word_ + 1) % 17);

        if (state_ == SYNC)
            state_ = ADDRESS; // We're in sync, move on.

        if (brute_repair(&rx_data)) {
            // Arbitration lost
            if (state_ != LOST_SYNC)
                state_ = LOSING_SYNC;
        } else {
            if (state_ == LOST_SYNC) {
                state_ = ADDRESS;
                if (onRecoveredSync) onRecoveredSync();
            }
        }

        if (rx_data == POCSAG_SYNC)
            return; // Already sync'ed.

        while (true) {
            switch (state_) {
            case LOSING_SYNC: {
                // Output what we've received so far.
                printmessage(false);
                numnibbles_ = 0;
                address_ = -1;
                function_ = -1;
                state_ = LOST_SYNC;
                return;
            }

            case LOST_SYNC: {
                state_ = NO_SYNC;
                rx_word_ = 0;
                if (onLostSync) onLostSync();
                return;
            }

            case ADDRESS: {
                if (rx_data == POCSAG_IDLE) // Idle codewords have a magic address
                    return;

                if (rx_data & MESSAGE_FLAG) {
                    function_ = -2;
                    address_ = -2;
                    state_ = MESSAGE;
                    break; // Performing partial decode
                }

                function_ = (int)((rx_data >> 11) & 3);
                address_ = (int)(((rx_data >> 10) & 0x1ffff8) | ((rxword >> 1) & 7));
                state_ = MESSAGE;
                return;
            }

            case MESSAGE: {
                if (rx_data & MESSAGE_FLAG) {
                    // Continuation message word.
                } else {
                    // Address/idle signals end of message.
                    state_ = END_OF_MESSAGE;
                    break;
                }

                if (numnibbles_ > sizeof(buffer_) * 2 - 5) {
                    // Message too long indicates we're decoding garbage.
                    state_ = LOSING_SYNC;
                    break;
                }

                uint32_t data;
                unsigned char* bp = buffer_ + (numnibbles_ >> 1);
                data = (rx_data >> 11);
                if (numnibbles_ & 1) {
                    bp[0] = (unsigned char)((bp[0] & 0xf0) | ((data >> 16) & 0xf));
                    bp[1] = (unsigned char)(data >> 8);
                    bp[2] = (unsigned char)(data);
                } else {
                    bp[0] = (unsigned char)(data >> 12);
                    bp[1] = (unsigned char)(data >> 4);
                    bp[2] = (unsigned char)(data << 4);
                }
                numnibbles_ += 5;
                return;
            }

            case END_OF_MESSAGE: {
                printmessage(true);
                numnibbles_ = 0;
                address_ = -1;
                function_ = -1;
                state_ = ADDRESS;
                break;
            }

            default:
                return;
            }
        }
    }

    default:
        break;
    }
}

/* ---------------------------------------------------------------------- */

void PocsagDecoder::feedBit(int bit)
{
    rx_data_ <<= 1;
    rx_data_ |= (uint32_t)(!bit);
    if (invert_input_)
        do_one_bit(~rx_data_); // try the inverted signal
    else
        do_one_bit(rx_data_);
}

void PocsagDecoder::reset()
{
    rx_data_ = 0;
    state_ = NO_SYNC;
    rx_bit_ = 0;
    rx_word_ = 0;
    inverted_ = 0;
    function_ = -1;
    address_ = -1;
    numnibbles_ = 0;
    std::memset(buffer_, 0, sizeof(buffer_));
}

/* ---------------------------------------------------------------------- */

uint32_t PocsagDecoder::totalBits() const        { return total_bits_; }
uint32_t PocsagDecoder::bitsInSync() const        { return bits_in_sync_; }
uint32_t PocsagDecoder::totalErrors() const        { return total_errors_; }
uint32_t PocsagDecoder::correctedErrors() const    { return corrected_errors_; }
uint32_t PocsagDecoder::uncorrectedErrors() const  { return uncorrected_errors_; }

bool PocsagDecoder::inSync() const
{
    return state_ == SYNC || state_ == ADDRESS ||
           state_ == MESSAGE || state_ == END_OF_MESSAGE;
}
