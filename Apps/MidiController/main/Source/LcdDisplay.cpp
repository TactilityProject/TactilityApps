#include "LcdDisplay.h"
#include "GroveLookup.h"

#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

static constexpr auto* TAG = "LcdDisplay";

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

static UnitPaHub* s_hub = nullptr;
static UnitLcd    s_lcd;
static Device*    s_i2cDev = nullptr;
// Discovered in lcdInit() by probing every PaHub channel (0-5), the LCD unit can be plugged into any port on the hub.
static uint8_t    s_lcdChannel = UnitPaHub::NO_CHANNEL;

// ---------------------------------------------------------------------------
// State cache - only redraw when something actually changed
// ---------------------------------------------------------------------------

static MidiMode  s_mode     = MidiMode::HOME;
static bool      s_dirty    = true;  // force full redraw on first update

// Per-mode cached values (simple structs with sentinels so first draw always fires)
struct RagaCache  { int raga=-1, tala=-1, root=-1, bpm=-1; bool playing=false, drone=false; };
struct DimCache   { int a=-1, b=-1, c=-1, d=-1, eq=-1, bpm=-1, octave=INT_MIN, rnd_cached=INT_MIN; bool playing=false; };
struct Tb3Cache   { int step=-1, key=-1, scale=-1, bpm=-1, density=-1; int patScale=-1, patDensity=-1; uint32_t seed=0; bool playing=false; };
struct Baby8Cache { int note[8]={-1,-1,-1,-1,-1,-1,-1,-1}; bool gate[8]={}; int step=-1; bool playing=false; };
struct EuclidCache{ int voice=-1, steps=-1, events=-1, rot=-1, note=-1, bpm=-1; bool playing=false; };

static RagaCache   s_raga;
static DimCache    s_dim;
static Tb3Cache    s_tb3;
static Baby8Cache  s_b8;
static EuclidCache s_euc;

// ---------------------------------------------------------------------------
// Colours (RGB565)
// ---------------------------------------------------------------------------

static constexpr uint16_t C_BG       = 0x0000;  // black
static constexpr uint16_t C_WHITE    = 0xFFFF;
static constexpr uint16_t C_GRAY     = 0x4A69;
static constexpr uint16_t C_DGRAY    = 0x2104;
static constexpr uint16_t C_RED      = 0xF800;
static constexpr uint16_t C_GREEN    = 0x07E0;
static constexpr uint16_t C_BLUE     = 0x001F;
static constexpr uint16_t C_ORANGE   = 0xFD20;
static constexpr uint16_t C_YELLOW   = 0xFFE0;
static constexpr uint16_t C_CYAN     = 0x07FF;
static constexpr uint16_t C_MAGENTA  = 0xF81F;
static constexpr uint16_t C_PURPLE   = 0x801F;

// Voice colours matching EUCLID_COLORS in EncoderInput
static constexpr uint16_t VOICE_COL[8] = {
    0xF800, 0xFD20, 0x47E0, 0x45FF,
    0xF81F, 0xFFE0, 0x07FE, 0xFC11
};

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

static bool selectLcd() {
    return s_hub && s_lcdChannel != UnitPaHub::NO_CHANNEL && s_hub->select(s_lcdChannel);
}

static void fillRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t col) {
    if (w == 0 || h == 0) return;
    s_lcd.fillRect(x, y, (uint8_t)(x + w - 1), (uint8_t)(y + h - 1), col);
}

static void hline(uint8_t x, uint8_t y, uint8_t w, uint16_t col) {
    fillRect(x, y, w, 1, col);
}

// ---------------------------------------------------------------------------
// Minimal 5×7 bitmap font (printable ASCII 0x20-0x7E)
// Each character is 5 columns × 7 rows, packed into 5 bytes (LSB = top row).
// ---------------------------------------------------------------------------

static const uint8_t FONT5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x00,0x08,0x14,0x22,0x41}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x41,0x22,0x14,0x08,0x00}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
    {0x00,0x01,0x02,0x04,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'
    {0x08,0x14,0x54,0x54,0x3C}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 't'
    {0x3C,0x40,0x40,0x40,0x7C}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'
    {0x00,0x08,0x36,0x41,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00}, // '}'
    {0x08,0x04,0x08,0x10,0x08}, // '~'
};

// Draw one character at pixel position (x,y), scale 1 or 2, colour/bg.
static void drawChar(uint8_t x, uint8_t y, char c, uint16_t col, uint16_t bg, uint8_t scale = 1) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = FONT5x7[c - 0x20];
    for (int col_i = 0; col_i < 5; col_i++) {
        uint8_t col_data = glyph[col_i];
        for (int row_i = 0; row_i < 7; row_i++) {
            uint16_t px_col = (col_data & (1 << row_i)) ? col : bg;
            uint8_t px = (uint8_t)(x + col_i * scale);
            uint8_t py = (uint8_t)(y + row_i * scale);
            if (scale == 1)
                s_lcd.drawPixel(px, py, px_col);
            else
                fillRect(px, py, scale, scale, px_col);
        }
    }
}

// Draw a string. Returns x after last character.
static uint8_t drawStr(uint8_t x, uint8_t y, const char* str, uint16_t col,
                       uint16_t bg = C_BG, uint8_t scale = 1) {
    uint8_t cw = (uint8_t)(6 * scale);  // 5px + 1px gap
    while (*str) {
        drawChar(x, y, *str++, col, bg, scale);
        x = (uint8_t)(x + cw);
    }
    return x;
}

// Draw a string centred in a width-w region starting at x.
static void drawStrCentred(uint8_t x, uint8_t y, uint8_t w, const char* str,
                           uint16_t col, uint16_t bg = C_BG, uint8_t scale = 1) {
    int len    = (int)strlen(str);
    int sw     = len * 6 * scale - scale;  // total pixel width (no trailing gap)
    int offset = ((int)w - sw) / 2;
    if (offset < 0) offset = 0;
    drawStr((uint8_t)(x + offset), y, str, col, bg, scale);
}

// Erase a text row (fill with background).
static void clearRow(uint8_t y, uint8_t h) {
    fillRect(0, y, 135, h, C_BG);
}

// ---------------------------------------------------------------------------
// Mode header (top 18px)
// ---------------------------------------------------------------------------

static const char* modeName(MidiMode m) {
    switch (m) {
        case MidiMode::RAGA:       return "RAGA";
        case MidiMode::DIMENSIONS: return "DIM";
        case MidiMode::TB3PO:      return "TB-3PO";
        case MidiMode::BABY8:      return "BABY8";
        case MidiMode::EUCLID:     return "EUCLID";
        default:                   return "MIDI";
    }
}

static uint16_t modeAccent(MidiMode m) {
    switch (m) {
        case MidiMode::RAGA:       return C_ORANGE;
        case MidiMode::DIMENSIONS: return C_CYAN;
        case MidiMode::TB3PO:      return C_GREEN;
        case MidiMode::BABY8:      return C_MAGENTA;
        case MidiMode::EUCLID:     return C_RED;
        default:                   return C_GRAY;
    }
}

static void drawHeader(MidiMode m) {
    uint16_t acc = modeAccent(m);
    fillRect(0, 0, 135, 16, acc);
    drawStrCentred(0, 4, 135, modeName(m), C_BG, acc, 2);
    hline(0, 16, 135, C_DGRAY);
}

// ---------------------------------------------------------------------------
// Note name helper
// ---------------------------------------------------------------------------

static const char* NOTE_NAMES[] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

static void noteName(int midi, char* buf, int bufsz) {
    int n = midi % 12;
    int oct = midi / 12 - 1;
    snprintf(buf, (size_t)bufsz, "%s%d", NOTE_NAMES[n], oct);
}

// ---------------------------------------------------------------------------
// Per-mode draw functions
// ---------------------------------------------------------------------------

// -- RAGA ---------------------------------------------------------------
// Layout (y=0..239, portrait):
//  0-16  header
//  18-65 raga name (large, 2×scale)
//  68-79 tala
//  82-93 root + BPM on same row
//  96-   play/drone status bars

static void drawRaga(const MidiController::RagaState& rs) {
    static const char* RAGA_NAMES[] = {
        "Bhairavi","Lalit","Bhupali","Todi",
        "Madhuvanti","Meghmalhar","Yaman","Malkauns"
    };
    static const char* TALA_NAMES[] = {"Teental","Rupak","Jhaptal"};

    auto& c = s_raga;
    bool fullRedraw = s_dirty;

    if (fullRedraw) {
        fillRect(0, 0, 135, 240, C_BG);
        drawHeader(MidiMode::RAGA);
        // Static labels
        drawStr(2, 70, "TALA", C_GRAY);
        drawStr(2, 84, "ROOT", C_GRAY);
        drawStr(70, 84, "BPM",  C_GRAY);
        hline(0, 98, 135, C_DGRAY);
    }

    static constexpr int RAGA_NAMES_COUNT = (int)(sizeof(RAGA_NAMES) / sizeof(RAGA_NAMES[0]));
    static constexpr int TALA_NAMES_COUNT = (int)(sizeof(TALA_NAMES) / sizeof(TALA_NAMES[0]));

    // Raga name (big, 2× scale)
    if (fullRedraw || c.raga != rs.raga) {
        clearRow(20, 46);
        if (rs.raga >= 0 && rs.raga < RAGA_NAMES_COUNT) {
            drawStrCentred(0, 22, 135, RAGA_NAMES[rs.raga], C_ORANGE, C_BG, 2);
        } else {
            drawStrCentred(0, 22, 135, "--", C_ORANGE, C_BG, 2);
        }
        c.raga = rs.raga;
    }

    // Tala
    if (fullRedraw || c.tala != rs.tala) {
        fillRect(36, 68, 99, 10, C_BG);
        if (rs.tala >= 0 && rs.tala < TALA_NAMES_COUNT) {
            drawStr(36, 68, TALA_NAMES[rs.tala], C_YELLOW);
        } else {
            drawStr(36, 68, "--", C_YELLOW);
        }
        c.tala = rs.tala;
    }

    // Root
    if (fullRedraw || c.root != rs.rootNote) {
        fillRect(36, 82, 32, 10, C_BG);
        char buf[8]; noteName(rs.rootNote, buf, sizeof(buf));
        drawStr(36, 82, buf, C_CYAN);
        c.root = rs.rootNote;
    }

    // BPM
    if (fullRedraw || c.bpm != rs.bpm) {
        fillRect(96, 82, 39, 10, C_BG);
        char buf[6]; snprintf(buf, sizeof(buf), "%d", rs.bpm);
        drawStr(96, 82, buf, C_WHITE);
        c.bpm = rs.bpm;
    }

    // Play status bar
    if (fullRedraw || c.playing != rs.playing) {
        uint16_t col = rs.playing ? C_RED : C_DGRAY;
        fillRect(0, 100, 135, 28, col);
        drawStrCentred(0, 107, 135, rs.playing ? "PLAYING" : "STOPPED", C_WHITE, col, 2);
        c.playing = rs.playing;
    }

    // Drone bar
    if (fullRedraw || c.drone != rs.droneOn) {
        uint16_t col = rs.droneOn ? C_BLUE : C_DGRAY;
        fillRect(0, 132, 135, 20, col);
        drawStrCentred(0, 137, 135, rs.droneOn ? "DRONE ON" : "DRONE OFF", C_WHITE, col);
        c.drone = rs.droneOn;
    }

    // Knob legend at bottom
    if (fullRedraw) {
        uint8_t ly = 170;
        struct { const char* k; const char* v; uint16_t col; } rows[] = {
            {"K1","RAGA", C_ORANGE},
            {"K2","TALA", C_YELLOW},
            {"K3","ROOT", C_CYAN},
            {"K4","BPM",  C_WHITE},
            {"B1","PLAY", C_RED},
            {"B2","DRON", C_BLUE},
        };
        for (auto& r : rows) {
            fillRect(0, ly, 135, 10, C_BG);
            drawStr(2,  ly, r.k, C_GRAY);
            drawStr(22, ly, r.v, r.col);
            ly = (uint8_t)(ly + 12);
        }
    }
}

// -- DIMENSIONS ---------------------------------------------------------------
// Layout:
//  0-16   header
//  18-77  A/B top row, C/D bottom row - coloured value bars
//  80-91  Eq + BPM
//  94-105 Oct + Rnd
//  108-   play status + knob legend

static void drawBar(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                    float val, float maxVal, uint16_t col) {
    if (maxVal <= 0.0f) { fillRect(x, y, w, h, C_DGRAY); return; }
    uint8_t filled = (uint8_t)((val / maxVal) * w);
    if (filled > w) filled = w;
    fillRect(x, y, filled, h, col);
    if (filled < w) fillRect((uint8_t)(x + filled), y, (uint8_t)(w - filled), h, C_DGRAY);
}

static void drawDimensions(const MidiController::DimensionsState& ds) {
    auto& c = s_dim;
    bool fullRedraw = s_dirty;

    if (fullRedraw) {
        fillRect(0, 0, 135, 240, C_BG);
        drawHeader(MidiMode::DIMENSIONS);
        // Static labels
        drawStr(2,  20, "A", UnitLcd::rgb888to565(0x88DDFF));
        drawStr(69, 20, "B", UnitLcd::rgb888to565(0x88FFCC));
        drawStr(2,  50, "C", UnitLcd::rgb888to565(0xFFCC88));
        drawStr(69, 50, "D", UnitLcd::rgb888to565(0xFF88CC));
        drawStr(2,  82, "EQ", C_ORANGE);
        drawStr(70, 82, "BPM", C_YELLOW);
        drawStr(2,  96, "OCT", UnitLcd::rgb888to565(0xAAFF88));
        drawStr(70, 96, "RND", UnitLcd::rgb888to565(0xFFAA44));
        hline(0, 110, 135, C_DGRAY);
    }

    static const uint16_t BC[4] = {
        UnitLcd::rgb888to565(0x88DDFF), UnitLcd::rgb888to565(0x88FFCC),
        UnitLcd::rgb888to565(0xFFCC88), UnitLcd::rgb888to565(0xFF88CC)
    };
    float vals[4] = { ds.a, ds.b, ds.c, ds.d };
    int   cvals[4]= { (int)ds.a, (int)ds.b, (int)ds.c, (int)ds.d };
    int*  cprev[4]= { &c.a, &c.b, &c.c, &c.d };

    for (int i = 0; i < 4; i++) {
        if (fullRedraw || *cprev[i] != cvals[i]) {
            uint8_t bx = (i & 1) ? 67 : 2;
            uint8_t by = (i < 2) ? 30 : 60;
            // value text
            fillRect(bx, by - 10, 60, 10, C_BG);
            char buf[8]; snprintf(buf, sizeof(buf), "%.0f", vals[i]);
            drawStr((uint8_t)(bx + 12), (uint8_t)(by - 10), buf, BC[i]);
            // bar
            drawBar(bx, by, 60, 8, vals[i], 100.0f, BC[i]);
            *cprev[i] = cvals[i];
        }
    }

    if (fullRedraw || c.eq != ds.equation) {
        fillRect(20, 82, 46, 10, C_BG);
        char buf[8]; snprintf(buf, sizeof(buf), "%d", ds.equation);
        drawStr(20, 82, buf, C_ORANGE);
        c.eq = ds.equation;
    }
    if (fullRedraw || c.bpm != ds.bpm) {
        fillRect(90, 82, 45, 10, C_BG);
        char buf[6]; snprintf(buf, sizeof(buf), "%d", ds.bpm);
        drawStr(90, 82, buf, C_YELLOW);
        c.bpm = ds.bpm;
    }
    if (fullRedraw || c.octave != ds.octave) {
        fillRect(22, 96, 46, 10, C_BG);
        char buf[8]; snprintf(buf, sizeof(buf), "%+d", ds.octave);
        drawStr(22, 96, buf, UnitLcd::rgb888to565(0xAAFF88));
        c.octave = ds.octave;
    }
    int rndRounded = (int)ds.rnd;
    if (fullRedraw || c.rnd_cached != rndRounded) {
        fillRect(90, 96, 45, 10, C_BG);
        char buf[8]; snprintf(buf, sizeof(buf), "%.0f", ds.rnd);
        drawStr(90, 96, buf, UnitLcd::rgb888to565(0xFFAA44));
        c.rnd_cached = rndRounded;
    }

    if (fullRedraw || c.playing != ds.playing) {
        uint16_t col = ds.playing ? C_GREEN : C_DGRAY;
        fillRect(0, 112, 135, 20, col);
        drawStrCentred(0, 117, 135, ds.playing ? "PLAYING" : "STOPPED", C_WHITE, col);
        c.playing = ds.playing;
    }
}

// -- TB3PO ---------------------------------------------------------------
// Layout:
//  0-16   header
//  18-49  32-step mini piano roll (6×12 px cells at an 8 px pitch, 2 rows of 16)
//  52-63  key + scale
//  66-77  density + BPM
//  80-91  play status

static constexpr const char* TB3_SCALE_NAMES[] = {
    "Major","Minor","Dorian","Penta","Blues","Chrome"
};
static constexpr int TB3_SCALE_NAMES_COUNT = (int)(sizeof(TB3_SCALE_NAMES) / sizeof(TB3_SCALE_NAMES[0]));
static void drawTb3po(const MidiController::Tb3poState& ts) {
    auto& c = s_tb3;
    bool fullRedraw = s_dirty;

    if (fullRedraw) {
        fillRect(0, 0, 135, 240, C_BG);
        drawHeader(MidiMode::TB3PO);
        drawStr(2,  54, "KEY",   C_GREEN);
        drawStr(70, 54, "SCALE", C_CYAN);
        drawStr(2,  68, "DEN",   C_YELLOW);
        drawStr(70, 68, "BPM",   C_WHITE);
        hline(0, 82, 135, C_DGRAY);
    }

    // 32-step piano roll - two rows of 16 cells (6×12 px, 2 px gaps)
    // Row 0: steps 0-15, Row 1: steps 16-31
    // The pattern itself changes on regen, density, or scale edits even when the
    // playback position does not, so those also invalidate the step cells.
    if (fullRedraw || c.step != ts.currentStep || c.seed != ts.seed ||
        c.playing != ts.playing || c.patDensity != ts.density || c.patScale != ts.scale) {
        for (int i = 0; i < 32; i++) {
            int row = i / 16;
            int col_i = i % 16;
            uint8_t x = (uint8_t)(2 + col_i * 8);
            uint8_t y = (uint8_t)(20 + row * 15);
            bool active = ts.playing && (i == ts.currentStep);
            bool hasNote = ts.steps[i].gate;
            uint16_t col;
            if (active)       col = C_WHITE;
            else if (hasNote) col = C_GREEN;
            else              col = C_DGRAY;
            fillRect(x, y, 6, 12, col);
            // Accent dot
            if (ts.steps[i].accent && !active)
                fillRect((uint8_t)(x+2), (uint8_t)(y+1), 2, 2, C_YELLOW);
        }
        c.step = ts.currentStep;
        c.seed = ts.seed;
        c.patDensity = ts.density;
        c.patScale = ts.scale;
    }

    if (fullRedraw || c.key != ts.key) {
        fillRect(22, 54, 46, 10, C_BG);
        int safeKey = ((ts.key % 12) + 12) % 12;
        drawStr(22, 54, KEY_NAMES[safeKey], C_GREEN);
        c.key = ts.key;
    }
    if (fullRedraw || c.scale != ts.scale) {
        fillRect(96, 54, 39, 10, C_BG);
        drawStr(96, 54, TB3_SCALE_NAMES[ts.scale % TB3_SCALE_NAMES_COUNT], C_CYAN);
        c.scale = ts.scale;
    }
    if (fullRedraw || c.density != ts.density) {
        fillRect(22, 68, 46, 10, C_BG);
        char buf[4]; snprintf(buf, sizeof(buf), "%d", ts.density);
        drawStr(22, 68, buf, C_YELLOW);
        c.density = ts.density;
    }
    if (fullRedraw || c.bpm != ts.bpm) {
        fillRect(96, 68, 39, 10, C_BG);
        char buf[6]; snprintf(buf, sizeof(buf), "%d", ts.bpm);
        drawStr(96, 68, buf, C_WHITE);
        c.bpm = ts.bpm;
    }
    if (fullRedraw || c.playing != ts.playing) {
        uint16_t col = ts.playing ? C_GREEN : C_DGRAY;
        fillRect(0, 84, 135, 20, col);
        drawStrCentred(0, 89, 135, ts.playing ? "PLAYING" : "STOPPED", C_WHITE, col);
        c.playing = ts.playing;
    }
}

// -- BABY8 ---------------------------------------------------------------
// Layout:
//  0-16   header
//  18-    8 step cells, each 28px tall:
//           note name, gate indicator, current step highlight

static void drawBaby8(const MidiController::Baby8State& bs) {
    auto& c = s_b8;
    bool fullRedraw = s_dirty;

    if (fullRedraw) {
        fillRect(0, 0, 135, 240, C_BG);
        drawHeader(MidiMode::BABY8);
    }

    for (int i = 0; i < 8; i++) {
        bool stepChanged = fullRedraw || c.step != bs.currentStep || c.playing != bs.playing;
        bool noteChanged = fullRedraw || c.note[i] != bs.steps[i].note;
        bool gateChanged = fullRedraw || c.gate[i] != bs.steps[i].gate;

        if (!stepChanged && !noteChanged && !gateChanged) continue;

        uint8_t y = (uint8_t)(18 + i * 27);
        bool isCurrent = bs.playing && (i == bs.currentStep);
        bool gateOn    = bs.steps[i].gate;

        uint16_t bgcol = isCurrent ? C_WHITE : (gateOn ? C_DGRAY : C_BG);
        uint16_t fgcol = isCurrent ? C_BG    : (gateOn ? C_GREEN : C_GRAY);

        fillRect(0, y, 135, 25, bgcol);

        // Step number
        char sbuf[4]; snprintf(sbuf, sizeof(sbuf), "%d", i + 1);
        drawStr(4, (uint8_t)(y + 8), sbuf, isCurrent ? C_BG : C_GRAY, bgcol);

        // Note name
        char nbuf[8]; noteName(bs.steps[i].note, nbuf, sizeof(nbuf));
        drawStr(24, (uint8_t)(y + 4), nbuf, fgcol, bgcol, 2);

        // Gate dot
        uint16_t dotcol = gateOn ? (isCurrent ? C_BG : C_GREEN) : C_DGRAY;
        fillRect(115, (uint8_t)(y + 8), 12, 8, dotcol);

        hline(0, (uint8_t)(y + 25), 135, C_DGRAY);

        c.note[i] = bs.steps[i].note;
        c.gate[i] = bs.steps[i].gate;
    }
    c.step = bs.currentStep;
    c.playing = bs.playing;
}

// -- EUCLID ---------------------------------------------------------------
// Layout:
//  0-16   header
//  18-33  voice tabs (8 mini boxes)
//  38-53  selected voice name (2x scale)
//  80-150 steps / events / rotation / note / BPM
//  156-174 play status, 176-236 knob legend

static void drawEuclid(const MidiController::EuclidState& es) {
    auto& c = s_euc;
    bool fullRedraw = s_dirty;

    if (fullRedraw) {
        fillRect(0, 0, 135, 240, C_BG);
        drawHeader(MidiMode::EUCLID);
        drawStr(2,  80, "STEPS",  C_GRAY);
        drawStr(2,  94, "EVENTS", C_GRAY);
        drawStr(2, 108, "ROT",    C_GRAY);
        drawStr(2, 122, "NOTE",   C_GRAY);
        drawStr(2, 140, "BPM",    C_GRAY);
        hline(0, 154, 135, C_DGRAY);
    }

    // Capture voice-changed flag before updating the cache
    bool voiceChanged = (c.voice != es.selectedVoice);

    // Selected voice index
    int idx = es.selectedVoice & 7;

    // Voice tabs
    if (fullRedraw || voiceChanged) {
        for (int i = 0; i < 8; i++) {
            uint8_t tx = (uint8_t)(i * 17);
            uint16_t col = VOICE_COL[i];
            uint16_t bg  = (i == idx) ? col : C_DGRAY;
            fillRect(tx, 18, 15, 15, bg);
            char vb[2] = { (char)('1' + i), 0 };
            drawChar((uint8_t)(tx + 4), 22, vb[0],
                     (i == idx) ? C_BG : C_GRAY, bg);
        }
        c.voice = es.selectedVoice;
    }

    // Selected voice name
    if (fullRedraw || voiceChanged) {
        clearRow(36, 18);
        char vbuf[16]; snprintf(vbuf, sizeof(vbuf), "VOICE %d", idx + 1);
        drawStrCentred(0, 38, 135, vbuf, VOICE_COL[idx], C_BG, 2);
    }

    const auto& sv = es.voice[idx];

    auto drawVal = [&](uint8_t y, int val, int& cached, uint16_t col) {
        if (fullRedraw || cached != val || voiceChanged) {
            fillRect(54, y, 81, 10, C_BG);
            char b[8]; snprintf(b, sizeof(b), "%d", val);
            drawStr(54, y, b, col);
            cached = val;
        }
    };

    drawVal(80,  sv.steps,    c.steps,  C_YELLOW);
    drawVal(94,  sv.events,   c.events, C_CYAN);
    drawVal(108, sv.rotation, c.rot,    UnitLcd::rgb888to565(0xAAFF88));

    if (fullRedraw || c.note != sv.note || voiceChanged) {
        fillRect(54, 122, 81, 10, C_BG);
        char nb[8]; noteName(sv.note, nb, sizeof(nb));
        drawStr(54, 122, nb, UnitLcd::rgb888to565(0xFF88CC));
        c.note = sv.note;
    }

    if (fullRedraw || c.bpm != es.bpm) {
        fillRect(54, 140, 81, 10, C_BG);
        char b[8]; snprintf(b, sizeof(b), "%d", es.bpm);
        drawStr(54, 140, b, C_WHITE);
        c.bpm = es.bpm;
    }

    if (fullRedraw || c.playing != es.playing) {
        uint16_t col = es.playing ? C_RED : C_DGRAY;
        fillRect(0, 156, 135, 18, col);
        drawStrCentred(0, 160, 135, es.playing ? "PLAYING" : "STOPPED", C_WHITE, col);
        c.playing = es.playing;
    }

    // Knob legend
    if (fullRedraw || voiceChanged) {
        uint8_t ly = 176;
        struct { const char* k; const char* v; uint16_t col; } rows[] = {
            {"K1","STEPS",  C_YELLOW},
            {"K2","EVENTS", C_CYAN},
            {"K3","ROT",    UnitLcd::rgb888to565(0xAAFF88)},
            {"K4","NOTE",   UnitLcd::rgb888to565(0xFF88CC)},
            {"K5","BPM",    C_WHITE},
            {"B", "VOICE",  VOICE_COL[idx]},
        };
        for (auto& r : rows) {
            fillRect(0, ly, 135, 10, C_BG);
            drawStr(2,  ly, r.k, C_GRAY);
            drawStr(22, ly, r.v, r.col);
            ly = (uint8_t)(ly + 10);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool lcdInit(UnitPaHub* hub) {
    s_hub = hub;
    if (!s_hub) return false;

    // findGroveI2cDevice() (GroveLookup.h), not a hardcoded "i2c1" name - the grove port device
    // is a mode-switchable I2C/UART router, not an I2C controller itself, so device_get_by_name
    // on the router's own name crashes (wrong driver data layout for i2c_controller_*() calls).
    Device* i2cDev = findGroveI2cDevice();
    if (!i2cDev) {
        ESP_LOGE(TAG, "grove0_i2c not found");
        return false;
    }
    s_i2cDev = i2cDev;

    // Find the LCD by probing every PaHub channel - UnitLcd::begin()
    // itself probes the I2C address and returns false if nothing answers, so trying each channel
    // in turn and keeping the first hit is safe/cheap (6 channels, worst case).
    s_lcdChannel = UnitPaHub::NO_CHANNEL;
    for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS; ch++) {
        if (!s_hub->select(ch)) continue;
        if (s_lcd.begin(i2cDev, UnitLcd::DEFAULT_ADDR)) {
            s_lcdChannel = ch;
            break;
        }
    }
    if (s_lcdChannel == UnitPaHub::NO_CHANNEL) {
        ESP_LOGW(TAG, "LCD unit not found on any PaHub channel");
        s_i2cDev = nullptr;
        return false;
    }
    s_lcd.setRotation(0);   // portrait 135×240
    s_lcd.setBrightness(180);
    s_lcd.fillScreen(C_BG);
    ESP_LOGI(TAG, "LCD ready on PaHub CH%d", (int)s_lcdChannel);
    return true;
}

void lcdStop() {
    if (s_lcd.isPresent() && selectLcd()) {
        s_lcd.setBrightness(0);
        s_lcd.fillScreen(C_BG);
    }
    s_i2cDev = nullptr;
    s_lcdChannel = UnitPaHub::NO_CHANNEL;
}

bool lcdIsPresent() {
    return s_lcd.isPresent();
}

void lcdSetMode(MidiMode mode) {
    s_mode  = mode;
    s_dirty = true;
    // Reset all caches so next lcdUpdate does a full redraw
    s_raga = {};  s_raga.raga = -1;
    s_dim  = {};  s_dim.a = -1;
    s_tb3  = {};  s_tb3.step = -1;
    s_b8   = {};  for (auto& n : s_b8.note) n = -1;
    s_euc  = {};  s_euc.voice = -1;
}

void lcdUpdate(MidiController* mc) {
    if (!s_lcd.isPresent() || !s_hub) return;
    if (!selectLcd()) return;

    switch (s_mode) {
        case MidiMode::RAGA:       drawRaga      (mc->raga_);  break;
        case MidiMode::DIMENSIONS: drawDimensions(mc->dim_);   break;
        case MidiMode::TB3PO:      drawTb3po     (mc->tb3po_); break;
        case MidiMode::BABY8:      drawBaby8     (mc->baby8_); break;
        case MidiMode::EUCLID:     drawEuclid    (mc->euclid_);break;
        default: {
            if (s_dirty) {
                fillRect(0, 0, 135, 240, C_BG);
                drawHeader(s_mode);
            }
            break;
        }
    }
    s_dirty = false;
}
