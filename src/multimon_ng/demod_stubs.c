// Stub demod_param definitions for non-FLEX decoders.
// These exist only to satisfy linker references from multimon_lib.c.
// Only the FLEX decoder (demod_flex.c) is actually linked with real code.
#include "multimon.h"

const struct demod_param demod_afsk1200   = { "AFSK1200", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_afsk2400   = { "AFSK2400", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_afsk2400_2 = { "AFSK2400_2", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_afsk2400_3 = { "AFSK2400_3", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_poc5       = { "POCSAG512", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_poc12      = { "POCSAG1200", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_poc24      = { "POCSAG2400", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_eas        = { "EAS", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_ufsk1200   = { "UFSK1200", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_clipfsk    = { "CLIPFSK", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_fmsfsk     = { "FMSFSK", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_dtmf       = { "DTMF", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_zvei1      = { "ZVEI1", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_zvei2      = { "ZVEI2", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_zvei3      = { "ZVEI3", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_dzvei      = { "DZVEI", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_pzvei      = { "PZVEI", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_eea        = { "EEA", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_eia        = { "EIA", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_ccir       = { "CCIR", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_hapn4800   = { "HAPN4800", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_fsk9600    = { "FSK9600", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_morse      = { "MORSE", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_x10        = { "X10", 0, 0, 0, 0, 0, 0, 0 };
const struct demod_param demod_scope      = { "SCOPE", 0, 0, 0, 0, 0, 0, 0 };
