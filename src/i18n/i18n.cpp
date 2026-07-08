#include "i18n/i18n.h"

// English-only build: _L() is an identity pass-through. The Lang/i18n API is
// kept so call sites compile unchanged and translations can be reintroduced.

namespace {
Lang gLang = Lang::EN;
}

void i18nInit()
{
    gLang = Lang::EN;
}

void i18nSet(Lang lang)
{
    gLang = lang;
}

Lang i18nGet()
{
    return gLang;
}

const char* i18nName(Lang lang)
{
    switch (lang)
    {
    case Lang::EN: return "English";
    default: return "???";
    }
}

const char* _L(const char* en)
{
    return en;
}
