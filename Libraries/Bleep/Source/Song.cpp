#include "Bleep/Song.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bleep {

namespace {

const char* const CHANNEL_NAMES[CHANNEL_COUNT] = { "lead", "harmony", "bass", "drums" };
const char* const NOTE_NAMES[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };

/** @return the cell of a token, or -1 when it's not valid in the channel */
int parseCell(Channel channel, const std::string& token) {
    if (token == "...") return CellRest;
    if (token == "---") return CellHold;
    if (channel == Drums) {
        if (token == "K..") return CellKick;
        if (token == "S..") return CellSnare;
        if (token == "H..") return CellHat;
        return -1;
    }
    if (token.size() != 3 || token[0] < 'A' || token[0] > 'G' || (token[1] != '-' && token[1] != '#') || token[2] < '0' || token[2] > '8') {
        return -1;
    }
    static const int semitones[7] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
    int note = semitones[token[0] - 'A'] + (token[1] == '#' ? 1 : 0);
    if (token[1] == '#' && (token[0] == 'E' || token[0] == 'B')) {
        return -1; // no E# or B#
    }
    note += (token[2] - '0' + 1) * 12;
    return note;
}

} // namespace

const char* channelName(Channel channel) {
    return channel < CHANNEL_COUNT ? CHANNEL_NAMES[channel] : "?";
}

bool cellValid(Channel channel, uint8_t cell) {
    if (cell == CellRest || cell == CellHold) return true;
    if (channel == Drums) return cell >= CellKick && cell <= CellHat;
    return cell >= CellFirstNote && cell <= CellLastNote;
}

void cellText(Channel channel, uint8_t cell, char* out) {
    if (cell == CellRest || !cellValid(channel, cell)) {
        strcpy(out, "...");
    } else if (cell == CellHold) {
        strcpy(out, "---");
    } else if (channel == Drums) {
        strcpy(out, cell == CellKick ? "K.." : cell == CellSnare ? "S.." : "H..");
    } else {
        const int octave = cell / 12 - 1;
        out[0] = NOTE_NAMES[cell % 12][0];
        out[1] = NOTE_NAMES[cell % 12][1];
        out[2] = (char)('0' + octave);
        out[3] = '\0';
    }
}

bool parseSong(const std::string& text, Song& out, std::string& error) {
    out = Song();
    int lineNumber = 0;
    size_t lineStart = 0;
    char message[96];
    auto fail = [&](const char* what) {
        snprintf(message, sizeof(message), "line %d: %s", lineNumber, what);
        error = message;
        return false;
    };
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        std::string line = text.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        lineNumber++;
        // Comments: '#' at the start of a line, or '#' followed by whitespace (sharps like F#5 aren't)
        const size_t firstText = line.find_first_not_of(" \t");
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '#' && (i == firstText || i + 1 == line.size() || line[i + 1] == ' ' || line[i + 1] == '\t')) {
                line.resize(i);
                break;
            }
        }

        std::vector<std::string> words;
        size_t p = 0;
        while (p < line.size()) {
            while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r')) p++;
            const size_t start = p;
            while (p < line.size() && line[p] != ' ' && line[p] != '\t' && line[p] != '\r') p++;
            if (p > start) words.push_back(line.substr(start, p - start));
        }
        if (words.empty()) continue;

        const std::string& key = words[0];
        if (key == "bpm") {
            const int bpm = words.size() > 1 ? atoi(words[1].c_str()) : 0;
            if (bpm < MIN_BPM || bpm > MAX_BPM) return fail("bpm needs a number from 40 to 300");
            out.bpm = bpm;
            continue;
        }
        if (key == "bar") {
            out.bars.emplace_back();
            continue;
        }
        int channel = -1;
        for (int c = 0; c < CHANNEL_COUNT; c++) {
            if (key == CHANNEL_NAMES[c]) channel = c;
        }
        if (channel < 0) return fail("unknown keyword");
        if (out.bars.empty()) return fail("expected 'bar' first");
        if (words.size() != ROWS_PER_BAR + 1) return fail("a channel needs 16 tokens");
        Bar& bar = out.bars.back();
        for (int row = 0; row < ROWS_PER_BAR; row++) {
            const int cell = parseCell((Channel)channel, words[row + 1]);
            if (cell < 0) return fail(channel == Drums ? "invalid drum (K.. S.. H.. --- ...)" : "invalid note");
            bar.cells[channel][row] = (uint8_t)cell;
        }
    }
    if (out.bars.empty()) {
        error = "no bars";
        return false;
    }
    return true;
}

std::string formatSong(const Song& song, const std::string& comment) {
    std::string out;
    size_t start = 0;
    while (start < comment.size()) {
        size_t end = comment.find('\n', start);
        if (end == std::string::npos) end = comment.size();
        out += "# " + comment.substr(start, end - start) + "\n";
        start = end + 1;
    }
    if (!comment.empty()) out += "\n";
    char line[32];
    snprintf(line, sizeof(line), "bpm %d\n", song.bpm);
    out += line;
    char cell[4];
    for (const Bar& bar : song.bars) {
        out += "\nbar\n";
        for (int c = 0; c < CHANNEL_COUNT; c++) {
            snprintf(line, sizeof(line), "%-8s", CHANNEL_NAMES[c]);
            out += line;
            for (int row = 0; row < ROWS_PER_BAR; row++) {
                cellText((Channel)c, bar.cells[c][row], cell);
                if (row > 0) out += ' ';
                out += cell;
            }
            out += '\n';
        }
    }
    return out;
}

} // namespace bleep
