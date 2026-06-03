#include "HtmlStrip.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// ESC token constants  (0x1B = ASCII ESC, safe in UTF-8 text streams)
// ---------------------------------------------------------------------------
static const char ESC         = '\x1B';
static const char ALIGN_LEFT  = 'L';
static const char ALIGN_CENTER= 'C';
static const char ALIGN_RIGHT = 'R';
static const char BOLD_ON     = 'B';
static const char BOLD_OFF    = 'b';
static const char ITALIC_ON   = 'I';
static const char ITALIC_OFF  = 'i';

// ---------------------------------------------------------------------------
// Minimal inline attribute scanner
// Searches the raw attribute string for a named value, e.g.
//   findAttrValue("style", "text-align", ...) → "center"
// ---------------------------------------------------------------------------

// Returns true if `haystack` (lower-cased attribute region) contains `needle`.
static bool attrContains(const char* haystack, const char* needle) {
    return strstr(haystack, needle) != nullptr;
}

// ---------------------------------------------------------------------------
// Emit a UTF-8 codepoint (up to U+10FFFF) as its byte sequence into `out`.
// ---------------------------------------------------------------------------
static void emitCodepoint(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// ---------------------------------------------------------------------------
// Main function
// ---------------------------------------------------------------------------
void stripHtmlToText(const std::string& html, std::string& out) {
    out.clear();
    if (html.empty()) return;
    out.reserve(html.size() / 2);

    // ── Parser state ────────────────────────────────────────────────────────
    bool inTag          = false;
    bool skipBlock      = false;
    char tagName[8]     = {};
    int  tagNameLen     = 0;
    bool tagIsClose     = false;
    bool tagNameDone    = false;
    bool tagIsSelfClose = false;

    // Attribute buffer - collects text between tag name and '>'
    char attrBuf[192] = {};
    int  attrLen      = 0;

    // Inline style tracking (nesting depth)
    int boldDepth   = 0;
    int italicDepth = 0;
    // Per-span style tracking - a stack so nested spans each close only what they opened.
    struct SpanState { bool bold; bool italic; };
    std::vector<SpanState> spanStack;

    // Per-paragraph alignment (reset at each block element open)
    char pendingBlockAlign = ALIGN_LEFT;

    int  nlPending    = 0;
    bool lastWasSpace = false;
    bool outputEmpty  = true;

    // Word buffer for hyphenation
    std::string wordBuf;

    // ── Helpers ─────────────────────────────────────────────────────────────

    auto trimTrailingSpace = [&]() {
        if (lastWasSpace && !out.empty()) { out.pop_back(); lastWasSpace = false; }
    };

    auto reqNL = [&](int n) {
        if (outputEmpty) return;
        trimTrailingSpace();
        nlPending = std::max(nlPending, n);
    };

    auto addNL = [&](int n) {
        if (outputEmpty) return;
        trimTrailingSpace();
        nlPending = std::min(nlPending + n, 2);
    };

    // Flush any buffered word directly into the output.
    auto flushWord = [&]() {
        if (wordBuf.empty()) return;
        if (!outputEmpty && nlPending > 0) {
            for (int k = 0; k < nlPending; ++k) out += '\n';
            nlPending = 0;
            lastWasSpace = false;
        }
        if (outputEmpty) {
            out += ESC;
            out += pendingBlockAlign;
            outputEmpty = false;
        }
        out += wordBuf;
        lastWasSpace = false;
        wordBuf.clear();
    };

    // Emit a single ASCII character, with deferred-newline / space-collapse logic.
    // Characters that can be part of a word are buffered for hyphenation.
    auto emit = [&](char c) {
        if (c == ' ') {
            flushWord();
            if (outputEmpty || lastWasSpace || nlPending > 0) return;
            out += ' ';
            lastWasSpace = true;
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            wordBuf += c;
        } else {
            // Non-alphabetic: flush any buffered word first
            flushWord();
            if (!outputEmpty && nlPending > 0) {
                for (int k = 0; k < nlPending; ++k) out += '\n';
                nlPending = 0;
                lastWasSpace = false;
            }
            if (outputEmpty) {
                out += ESC;
                out += pendingBlockAlign;
                outputEmpty = false;
            }
            out += c;
            lastWasSpace = false;
        }
    };

    // Emit a raw ESC token (bold/italic on/off) - bypasses word buffering.
    auto emitToken = [&](char code) {
        flushWord();
        if (!outputEmpty) {
            out += ESC;
            out += code;
        }
        // If outputEmpty, defer - the token will be emitted after alignment marker
        // by the next emit() call.  For now, tokens before any content are dropped
        // (rare in practice: bold/italic before any paragraph text).
    };

    // Emit a multi-byte UTF-8 sequence directly (after flushing word and newlines).
    auto emitUtf8 = [&](const char* bytes, int len) {
        flushWord();
        if (!outputEmpty && nlPending > 0) {
            for (int k = 0; k < nlPending; ++k) out += '\n';
            nlPending = 0;
            lastWasSpace = false;
        }
        if (outputEmpty) {
            out += ESC;
            out += pendingBlockAlign;
            outputEmpty = false;
        }
        for (int k = 0; k < len; ++k) out += bytes[k];
        lastWasSpace = false;
    };

    // ── Main parse loop ──────────────────────────────────────────────────────
    for (size_t i = 0; i < html.size(); ++i) {
        unsigned char c = (unsigned char)html[i];

        if (c == '<') {
            // HTML comment <!-- ... -->
            if (i + 3 < html.size() &&
                    html[i+1]=='!' && html[i+2]=='-' && html[i+3]=='-') {
                size_t end = html.find("-->", i + 4);
                i = (end != std::string::npos) ? end + 2 : html.size() - 1;
                continue;
            }
            inTag          = true;
            tagNameLen     = 0;
            tagIsClose     = false;
            tagNameDone    = false;
            tagIsSelfClose = false;
            tagName[0]     = '\0';
            attrLen        = 0;
            attrBuf[0]     = '\0';

        } else if (c == '>') {
            if (inTag) {
                // Lower-case the attribute buffer for case-insensitive checks
                for (int k = 0; k < attrLen; ++k)
                    attrBuf[k] = (char)tolower((unsigned char)attrBuf[k]);

                bool isSS  = (strcmp(tagName, "style")  == 0 ||
                              strcmp(tagName, "script") == 0 ||
                              strcmp(tagName, "head")   == 0);
                bool isP   = (strcmp(tagName, "p")   == 0);
                bool isDiv = (strcmp(tagName, "div") == 0);
                bool isBr  = (strcmp(tagName, "br")  == 0);
                bool isLi  = (strcmp(tagName, "li")  == 0);
                bool isH   = (tagName[0] == 'h' && tagName[1] >= '1' &&
                              tagName[1] <= '6' && tagName[2] == '\0');
                bool isB   = (strcmp(tagName, "b")      == 0 ||
                              strcmp(tagName, "strong") == 0);
                bool isI   = (strcmp(tagName, "i")  == 0 ||
                              strcmp(tagName, "em") == 0);
                bool isSpan= (strcmp(tagName, "span") == 0);

                // Determine per-tag alignment and inline style from attributes
                char tagAlign = 0; // 0 = inherit / don't change
                bool attrBold   = attrContains(attrBuf, "font-weight:bold") ||
                                  attrContains(attrBuf, "font-weight: bold");
                bool attrItalic = attrContains(attrBuf, "font-style:italic") ||
                                  attrContains(attrBuf, "font-style: italic");
                if (attrContains(attrBuf, "text-align:center") ||
                    attrContains(attrBuf, "text-align: center"))
                    tagAlign = ALIGN_CENTER;
                else if (attrContains(attrBuf, "text-align:right") ||
                         attrContains(attrBuf, "text-align: right"))
                    tagAlign = ALIGN_RIGHT;

                if (isSS) {
                    if (!tagIsClose && !tagIsSelfClose) skipBlock = true;
                    else if (tagIsClose)                skipBlock = false;
                } else if (!skipBlock) {
                    if (isBr) {
                        flushWord();
                        addNL(1);
                    } else if (isH && !tagIsClose) {
                        // h1/h2 → centered, h3-h6 → left
                        pendingBlockAlign = (tagName[1] <= '2') ? ALIGN_CENTER : ALIGN_LEFT;
                        if (tagAlign) pendingBlockAlign = tagAlign;
                        reqNL(2);
                    } else if (isH && tagIsClose) {
                        pendingBlockAlign = ALIGN_LEFT;
                        reqNL(2);
                    } else if ((isP || isDiv) && !tagIsClose) {
                        pendingBlockAlign = tagAlign ? tagAlign : ALIGN_LEFT;
                        reqNL(2);
                    } else if ((isP || isDiv) && tagIsClose) {
                        pendingBlockAlign = ALIGN_LEFT;
                        reqNL(2);
                    } else if (isLi && !tagIsClose) {
                        flushWord();
                        if (!outputEmpty) {
                            trimTrailingSpace();
                            int n = std::max(nlPending, 1);
                            for (int k = 0; k < std::min(n, 2); ++k) out += '\n';
                            nlPending = 0; lastWasSpace = false;
                        }
                        if (outputEmpty) { out += ESC; out += pendingBlockAlign; outputEmpty = false; }
                        out += '-';
                        out += ' ';
                        lastWasSpace = true;
                    } else if (isB && !tagIsClose) {
                        boldDepth++;
                        if (boldDepth == 1) emitToken(BOLD_ON);
                    } else if (isB && tagIsClose) {
                        if (boldDepth > 0) {
                            boldDepth--;
                            if (boldDepth == 0) emitToken(BOLD_OFF);
                        }
                    } else if (isI && !tagIsClose) {
                        italicDepth++;
                        if (italicDepth == 1) emitToken(ITALIC_ON);
                    } else if (isI && tagIsClose) {
                        if (italicDepth > 0) {
                            italicDepth--;
                            if (italicDepth == 0) emitToken(ITALIC_OFF);
                        }
                    } else if (isSpan && !tagIsClose) {
                        SpanState ss = { false, false };
                        if (attrBold   && boldDepth == 0)   { boldDepth++;   ss.bold   = true; emitToken(BOLD_ON);   }
                        if (attrItalic && italicDepth == 0) { italicDepth++; ss.italic = true; emitToken(ITALIC_ON); }
                        spanStack.push_back(ss);
                    } else if (isSpan && tagIsClose) {
                        // Only close styles that this span opened - never touch depth set by <b>/<i>
                        if (!spanStack.empty()) {
                            SpanState ss = spanStack.back(); spanStack.pop_back();
                            if (ss.bold   && boldDepth > 0)   { boldDepth--;   if (boldDepth == 0) emitToken(BOLD_OFF);   }
                            if (ss.italic && italicDepth > 0) { italicDepth--; if (italicDepth == 0) emitToken(ITALIC_OFF); }
                        }
                    }
                }
            }
            inTag = false;

        } else if (inTag) {
            if (!tagNameDone) {
                if (tagNameLen == 0 && c == '/') {
                    tagIsClose = true;
                } else if (c == '/') {
                    tagIsSelfClose = true;
                    tagNameDone    = true;
                } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                    tagNameDone = true;
                } else if (tagNameLen < 6) {
                    tagName[tagNameLen++] = (char)tolower((int)c);
                    tagName[tagNameLen]   = '\0';
                } else {
                    tagNameDone = true;
                }
            } else {
                // Collect attribute content (limited buffer, lower-cased on '>')
                if (attrLen < (int)sizeof(attrBuf) - 1) {
                    attrBuf[attrLen++] = (char)c;
                    attrBuf[attrLen]   = '\0';
                }
            }

        } else if (!skipBlock) {
            // ── Text content ────────────────────────────────────────────────
            if (c == '\n' || c == '\r' || c == '\t') {
                emit(' ');
            } else if (c == '&') {
                // Entity decoding - output proper Unicode, not ASCII substitutes
                if      (html.compare(i, 5,  "&shy;")   == 0) {
                    i += 4;  // soft hyphen - discard
                }
                else if (html.compare(i, 6,  "&nbsp;")  == 0) { emit(' ');   i += 5; }
                else if (html.compare(i, 4,  "&lt;")    == 0) { emit('<');   i += 3; }
                else if (html.compare(i, 4,  "&gt;")    == 0) { emit('>');   i += 3; }
                else if (html.compare(i, 5,  "&amp;")   == 0) { emit('&');   i += 4; }
                else if (html.compare(i, 6,  "&quot;")  == 0) { emit('"');   i += 5; }
                else if (html.compare(i, 6,  "&apos;")  == 0) { emit('\'');  i += 5; }
                // Typographic punctuation - emit proper Unicode
                else if (html.compare(i, 7,  "&mdash;") == 0) {
                    const char s[] = "\xE2\x80\x94"; emitUtf8(s, 3); i += 6;
                }
                else if (html.compare(i, 7,  "&ndash;") == 0) {
                    const char s[] = "\xE2\x80\x93"; emitUtf8(s, 3); i += 6;
                }
                else if (html.compare(i, 8,  "&hellip;")== 0) {
                    const char s[] = "\xE2\x80\xA6"; emitUtf8(s, 3); i += 7;
                }
                else if (html.compare(i, 7,  "&lsquo;") == 0) {
                    const char s[] = "\xE2\x80\x98"; emitUtf8(s, 3); i += 6;
                }
                else if (html.compare(i, 7,  "&rsquo;") == 0) {
                    const char s[] = "\xE2\x80\x99"; emitUtf8(s, 3); i += 6;
                }
                else if (html.compare(i, 7,  "&ldquo;") == 0) {
                    const char s[] = "\xE2\x80\x9C"; emitUtf8(s, 3); i += 6;
                }
                else if (html.compare(i, 7,  "&rdquo;") == 0) {
                    const char s[] = "\xE2\x80\x9D"; emitUtf8(s, 3); i += 6;
                }
                else if (html.compare(i, 2, "&#") == 0) {
                    size_t j = i + 2;
                    bool isHex = (j < html.size() && (html[j]=='x' || html[j]=='X'));
                    if (isHex) ++j;
                    size_t numStart = j;
                    while (j < html.size() && html[j] != ';') ++j;
                    if (j < html.size() && j > numStart) {
                        char numBuf[10] = {};
                        size_t numLen = std::min(j - numStart, (size_t)(sizeof(numBuf)-1));
                        html.copy(numBuf, numLen, numStart);
                        unsigned long cp = isHex ? strtoul(numBuf, nullptr, 16)
                                                 : strtoul(numBuf, nullptr, 10);
                        if (cp == 0x00AD) {
                            // Soft hyphen - discard
                        } else if (cp == 0x00A0) {
                            emit(' ');
                        } else if (cp >= 0x20 && cp < 0x80) {
                            emit((char)cp);
                        } else if (cp >= 0x80) {
                            // Emit as UTF-8
                            std::string tmp;
                            emitCodepoint(tmp, cp);
                            emitUtf8(tmp.c_str(), (int)tmp.size());
                        }
                        i = j;
                    } else {
                        emit('&');
                    }
                } else {
                    emit('&');
                }

            } else if (c < 0x80) {
                emit((char)c);

            } else if ((c & 0xE0) == 0xC0) {
                // 2-byte UTF-8
                bool valid = (i + 1 < html.size() &&
                              ((unsigned char)html[i+1] & 0xC0) == 0x80);
                if (valid) {
                    unsigned char b2 = (unsigned char)html[i+1];
                    if (c == 0xC2 && b2 == 0xA0) {
                        emit(' '); // NBSP → regular space
                    } else if (c == 0xC2 && b2 == 0xAD) {
                        // U+00AD soft hyphen - discard
                    } else {
                        char seq[2] = { (char)c, (char)b2 };
                        emitUtf8(seq, 2);
                    }
                    i += 1;
                }

            } else if ((c & 0xF0) == 0xE0 && i + 2 < html.size()) {
                // 3-byte UTF-8
                unsigned char b2 = (unsigned char)html[i+1];
                unsigned char b3 = (unsigned char)html[i+2];
                bool valid = ((b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80);
                if (valid) {
                    char seq[3] = { (char)c, (char)b2, (char)b3 };
                    emitUtf8(seq, 3);
                    i += 2;
                }

            } else if (c >= 0xF0 && i + 3 < html.size()) {
                // 4-byte UTF-8 (emoji etc.) - drop
                i += 3;
            }
        }
    }

    flushWord();

    // Trim trailing whitespace / newlines
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
}
