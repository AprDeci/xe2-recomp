/* nn::settings: the available-language list, natively.
 *
 * GetAvailableLanguageCodeCount / GetAvailableLanguageCodes are SDK exports that talk to the
 * set:sys service. Our set:sys is a stub that answers "success" without output, so the SDK
 * client reads an uninitialised reply as the returned count. Xenoblade 2's locale setup then
 * iterates that (garbage, often huge) count over a 15-entry stack buffer, walking past it and
 * passing stack garbage to strcmp("...", "zh-Hans"/"zh-Hant") -> read of a bogus pointer.
 *
 * Supply the list directly instead of going through the service. The values match the console's
 * nn::settings language codes (same table as hle_offline.c). The count is the real 18; the
 * codes call fills at most the caller's `max` and returns how many it wrote (the game loops
 * over that written count, so the buffer is never overrun).
 */
#include "host.h"

static const char *const LANGS[] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl",
    "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR",
};
#define NLANGS ((int)(sizeof LANGS / sizeof *LANGS))

HLE(_ZN2nn8settings29GetAvailableLanguageCodeCountEv) { c->x[0] = (u64)NLANGS; }

HLE(_ZN2nn8settings25GetAvailableLanguageCodesEPNS0_12LanguageCodeEi) {
    u64 out = c->x[0];
    int max = (int)c->x[1];
    int n = 0;
    for (int i = 0; i < NLANGS && n < max; i++) {
        u64 code = 0;
        size_t len = strlen(LANGS[i]);
        if (len > 8) len = 8;
        memcpy(&code, LANGS[i], len);   /* LanguageCode is 8 bytes; codes are <= 7 chars */
        if (out) st64(out + (u64)n * 8, code);
        n++;
    }
    c->x[0] = (u64)n;
}
