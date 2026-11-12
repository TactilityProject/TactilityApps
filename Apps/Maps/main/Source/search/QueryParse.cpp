// SPDX-License-Identifier: GPL-3.0-or-later
#include "QueryParse.h"

#include "TextFold.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cctype>

namespace maps {

static uint32_t decode_utf8(const char* s, size_t len, size_t* out_len) {
    const unsigned char* u = (const unsigned char*)s;
    if (len == 0) { *out_len = 0; return 0; }
    if (u[0] < 0x80) { *out_len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && len >= 2) { *out_len = 2; return ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); }
    if ((u[0] & 0xF0) == 0xE0 && len >= 3) { *out_len = 3; return ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); }
    if ((u[0] & 0xF8) == 0xF0 && len >= 4) { *out_len = 4; return ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); }
    *out_len = 1; return u[0];
}

static bool is_letter(uint32_t cp) {
    if (cp < 128) return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    return true;
}

/** The suffix letter as the study stores it (data.py _sfx): the first letter of its Russian-profile transliteration. */
static char translit(uint32_t cp) {
    char letter[5];
    const size_t used = utf8Append(cp, letter, sizeof(letter), 0);
    if (used == 0) return 0;
    char folded[16];
    transliterate(letter, Translit::Russian, folded, sizeof(folded));
    const char first = folded[0];
    return (first >= 'a' && first <= 'z') ? first : 0;
}

static void trim(char* s) {
    size_t len = strlen(s);
    while (len > 0 && std::isspace((unsigned char)s[len - 1])) s[--len] = 0;
    size_t start = 0;
    while (s[start] && std::isspace((unsigned char)s[start])) start++;
    if (start > 0) memmove(s, s + start, len - start + 1);
}

bool parseHouseNumber(const char* text, size_t length, HouseNumber& out) {
    char buf[32];
    if (length >= sizeof(buf)) return false;
    memcpy(buf, text, length);
    buf[length] = 0;
    trim(buf);

    const char* s = buf;
    uint32_t num = 0;
    int digits = 0;
    while (*s && std::isdigit((unsigned char)*s) && digits < 5) {
        num = num * 10 + (*s - '0');
        s++; digits++;
    }
    if (digits == 0 || num > 65535) return false;

    while (*s && std::isspace((unsigned char)*s)) s++;

    if (*s == 0) {
        out.number = static_cast<uint16_t>(num); out.aux = 0; out.flags = 0;
        out.suffix = 0;
        return true;
    }

    if (*s == '/') {
        s++;
        while (*s && std::isspace((unsigned char)*s)) s++;
        if (std::isdigit((unsigned char)*s)) {
            uint32_t aux = 0; int aux_digits = 0;
            while (*s && std::isdigit((unsigned char)*s) && aux_digits < 5) {
                aux = aux * 10 + (*s - '0');
                s++; aux_digits++;
            }
            if (aux > 65535) return false;
            while (*s && std::isspace((unsigned char)*s)) s++;
            size_t l; uint32_t cp = decode_utf8(s, strlen(s), &l);
            char sfx = (l > 0 && is_letter(cp)) ? translit(cp) : 0;
            if (s[l] == 0) {
                out.number = static_cast<uint16_t>(num); out.aux = static_cast<uint16_t>(aux); out.suffix = sfx; out.flags = HOUSE_SLASH;
                return true;
            }
        } else {
            size_t l; uint32_t cp = decode_utf8(s, strlen(s), &l);
            if (l > 0 && is_letter(cp) && s[l] == 0) {
                out.number = static_cast<uint16_t>(num); out.aux = 0; out.suffix = translit(cp); out.flags = HOUSE_SLASH;
                return true;
            }
        }
    } else if (*s == '-') {
        s++;
        while (*s && std::isspace((unsigned char)*s)) s++;
        uint32_t aux = 0; int aux_digits = 0;
        while (*s && std::isdigit((unsigned char)*s) && aux_digits < 5) {
            aux = aux * 10 + (*s - '0');
            s++; aux_digits++;
        }
        if (aux_digits > 0 && *s == 0 && aux <= 65535) {
            out.number = static_cast<uint16_t>(num); out.aux = static_cast<uint16_t>(aux); out.suffix = 0; out.flags = HOUSE_RANGE;
            return true;
        }
    } else {
        size_t l; uint32_t cp = decode_utf8(s, strlen(s), &l);
        if (l > 0 && is_letter(cp) && s[l] == 0) {
            out.number = static_cast<uint16_t>(num); out.aux = 0; out.suffix = translit(cp); out.flags = 0;
            return true;
        }
    }
    return false;
}

static bool is_numtok(const char* s) {
    if (!*s) return false;
    int digits = 0;
    while (*s && std::isdigit((unsigned char)*s) && digits < 5) {
        s++; digits++;
    }
    if (digits == 0) return false;
    if (*s == 0) return true;
    if (*s == '/') {
        s++;
        if (std::isdigit((unsigned char)*s)) {
            int aux_digits = 0;
            while (*s && std::isdigit((unsigned char)*s) && aux_digits < 5) {
                s++; aux_digits++;
            }
            if (aux_digits == 0) return false;
            size_t l; uint32_t cp = decode_utf8(s, strlen(s), &l);
            if (l == 0 || (l > 0 && is_letter(cp) && s[l] == 0)) return true;
        } else {
            size_t l; uint32_t cp = decode_utf8(s, strlen(s), &l);
            if (l > 0 && is_letter(cp) && s[l] == 0) return true;
        }
    } else {
        size_t l; uint32_t cp = decode_utf8(s, strlen(s), &l);
        if (l > 0 && is_letter(cp) && s[l] == 0) return true;
    }
    return false;
}

int parseQuery(const char* query, QueryReading out[3]) {
    char q[256];
    strncpy(q, query, sizeof(q) - 1);
    q[sizeof(q) - 1] = 0;
    trim(q);

    // re.sub(r'\s*/\s*', '/', q)
    char q2[256]; int j = 0;
    for (int i = 0; q[i]; i++) {
        if (q[i] == '/') {
            while (i > 0 && std::isspace((unsigned char)q[i - 1])) q2[--j] = 0;
            q2[j++] = '/';
            while (q[i + 1] && std::isspace((unsigned char)q[i + 1])) i++;
        } else {
            q2[j++] = q[i];
        }
    }
    q2[j] = 0;

    // re.sub(r'(\d)\s+([A-Za-z])$', r'\1\2', q)
    int len = strlen(q2);
    if (len >= 2 && std::isalpha((unsigned char)q2[len - 1])) {
        int k = len - 2;
        while (k >= 0 && std::isspace((unsigned char)q2[k])) k--;
        if (k >= 0 && std::isdigit((unsigned char)q2[k])) {
            char q3[256]; int m = 0;
            for (int n = 0; n <= k; n++) q3[m++] = q2[n];
            q3[m++] = q2[len - 1];
            q3[m] = 0;
            strcpy(q2, q3);
        }
    }

    char toks[16][32]; int tok_count = 0;
    char* cur = q2;
    while (*cur && tok_count < 16) {
        while (*cur && std::isspace((unsigned char)*cur)) cur++;
        if (!*cur) break;
        int start = 0;
        while (*cur && !std::isspace((unsigned char)*cur) && start < 31) {
            toks[tok_count][start++] = *cur++;
        }
        toks[tok_count][start] = 0;
        tok_count++;
    }

    int readings = 0;
    if (tok_count >= 2) {
        char last[32]; strcpy(last, toks[tok_count - 1]);
        size_t last_len = strlen(last);
        if (last_len > 0 && last[last_len - 1] == '.') last[last_len - 1] = 0;
        if (is_numtok(last)) {
            int s_len = 0;
            for (int i = 0; i < tok_count - 1; i++) {
                int t_len = strlen(toks[i]);
                if (s_len + t_len + 1 >= 96) break;
                if (s_len > 0) out[readings].street[s_len++] = ' ';
                memcpy(out[readings].street + s_len, toks[i], t_len);
                s_len += t_len;
            }
            out[readings].street[s_len] = 0;
            strncpy(out[readings].number, last, 23);
            out[readings].number[23] = 0;
            readings++;
        }
        if (readings < 3 && is_numtok(toks[0])) {
            int s_len = 0;
            for (int i = 1; i < tok_count; i++) {
                int t_len = strlen(toks[i]);
                if (s_len + t_len + 1 >= 96) break;
                if (s_len > 0) out[readings].street[s_len++] = ' ';
                memcpy(out[readings].street + s_len, toks[i], t_len);
                s_len += t_len;
            }
            out[readings].street[s_len] = 0;
            strncpy(out[readings].number, toks[0], 23);
            out[readings].number[23] = 0;
            readings++;
        }
    }

    if (readings < 3) {
        strncpy(out[readings].street, q2, 95);
        out[readings].street[95] = 0;
        out[readings].number[0] = 0;
        readings++;
    }

    return readings;
}

} // namespace maps
