// SPDX-License-Identifier: GPL-3.0-or-later
#include "GpxReader.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

const char* localName(const char* name) {
    const char* local = name;
    for (const char* p = name; *p != '\0'; p++) {
        if (*p == ':') {
            local = p + 1;
        }
    }
    return local;
}

bool parseDouble(const char* text, double& out) {
    char* end = nullptr;
    const double value = strtod(text, &end);
    if (end == text) {
        return false;
    }
    while (isSpace(*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    out = value;
    return true;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const int adjustedMonth = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned dayOfYear = static_cast<unsigned>((153 * adjustedMonth + 2) / 5 + static_cast<int>(day) - 1);
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

bool readDigits(const char*& text, int count, int& value) {
    value = 0;
    for (int i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    text += count;
    return true;
}

bool parseTimeText(const char* text, int64_t& out) {
    const char* p = text;
    while (isSpace(*p)) {
        p++;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!readDigits(p, 4, year) || *p++ != '-') {
        return false;
    }
    if (!readDigits(p, 2, month) || *p++ != '-') {
        return false;
    }
    if (!readDigits(p, 2, day) || (*p != 'T' && *p != 't')) {
        return false;
    }
    p++;
    if (!readDigits(p, 2, hour) || *p++ != ':') {
        return false;
    }
    if (!readDigits(p, 2, minute) || *p++ != ':') {
        return false;
    }
    if (!readDigits(p, 2, second)) {
        return false;
    }
    if (*p == '.') {
        p++;
        if (*p < '0' || *p > '9') {
            return false;
        }
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    }
    int offset = 0;
    if (*p == 'Z' || *p == 'z') {
        p++;
    } else if (*p == '+' || *p == '-') {
        const int sign = *p == '+' ? 1 : -1;
        p++;
        int offsetHour = 0;
        int offsetMinute = 0;
        if (!readDigits(p, 2, offsetHour) || *p++ != ':') {
            return false;
        }
        if (!readDigits(p, 2, offsetMinute)) {
            return false;
        }
        offset = sign * (offsetHour * 3600 + offsetMinute * 60);
    } else {
        return false;
    }
    while (isSpace(*p)) {
        p++;
    }
    if (*p != '\0') {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    if (hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    const int64_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    out = days * 86400 + hour * 3600 + minute * 60 + second - offset;
    return true;
}

void appendCodePoint(char* out, size_t size, size_t& used, uint32_t code) {
    if (code < 0x80) {
        if (used + 1 < size) {
            out[used++] = static_cast<char>(code);
        }
    } else if (code < 0x800) {
        if (used + 2 < size) {
            out[used++] = static_cast<char>(0xC0 | (code >> 6));
            out[used++] = static_cast<char>(0x80 | (code & 0x3F));
        }
    } else if (code < 0x10000) {
        if (used + 3 < size) {
            out[used++] = static_cast<char>(0xE0 | (code >> 12));
            out[used++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out[used++] = static_cast<char>(0x80 | (code & 0x3F));
        }
    } else if (code <= 0x10FFFF) {
        if (used + 4 < size) {
            out[used++] = static_cast<char>(0xF0 | (code >> 18));
            out[used++] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out[used++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out[used++] = static_cast<char>(0x80 | (code & 0x3F));
        }
    }
}

void decodeEntities(const char* in, char* out, size_t size) {
    if (size == 0) {
        return;
    }
    size_t used = 0;
    for (size_t index = 0; in[index] != '\0';) {
        if (in[index] == '&') {
            size_t semi = index + 1;
            while (semi < index + 12 && in[semi] != '\0' && in[semi] != ';') {
                semi++;
            }
            if (in[semi] == ';') {
                const char* name = in + index + 1;
                const size_t length = semi - index - 1;
                char decoded = '\0';
                if (length == 3 && strncmp(name, "amp", 3) == 0) {
                    decoded = '&';
                } else if (length == 2 && strncmp(name, "lt", 2) == 0) {
                    decoded = '<';
                } else if (length == 2 && strncmp(name, "gt", 2) == 0) {
                    decoded = '>';
                } else if (length == 4 && strncmp(name, "quot", 4) == 0) {
                    decoded = '"';
                } else if (length == 4 && strncmp(name, "apos", 4) == 0) {
                    decoded = '\'';
                }
                if (decoded != '\0') {
                    if (used + 1 < size) {
                        out[used++] = decoded;
                    }
                    index = semi + 1;
                    continue;
                }
                if (length > 1 && name[0] == '#') {
                    bool hex = false;
                    size_t digit = 1;
                    if (name[1] == 'x' || name[1] == 'X') {
                        hex = true;
                        digit = 2;
                    }
                    uint32_t code = 0;
                    bool valid = digit < length;
                    for (; digit < length; digit++) {
                        const char c = name[digit];
                        int value = -1;
                        if (c >= '0' && c <= '9') {
                            value = c - '0';
                        } else if (hex && c >= 'a' && c <= 'f') {
                            value = c - 'a' + 10;
                        } else if (hex && c >= 'A' && c <= 'F') {
                            value = c - 'A' + 10;
                        }
                        if (value < 0) {
                            valid = false;
                            break;
                        }
                        code = code * (hex ? 16u : 10u) + static_cast<uint32_t>(value);
                        if (code > 0x10FFFF) {
                            valid = false;
                            break;
                        }
                    }
                    if (valid && code != 0) {
                        appendCodePoint(out, size, used, code);
                    }
                    index = semi + 1;
                    continue;
                }
            }
        }
        if (used + 1 < size) {
            out[used++] = in[index];
        }
        index++;
    }
    out[used] = '\0';
}

} // namespace

GpxReader::GpxReader(GpxListener& listener) : listener_(listener) {}

bool GpxReader::feed(const char* data, size_t size) {
    if (failed_) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        handleByte(data[i]);
        if (failed_) {
            return false;
        }
    }
    return true;
}

bool GpxReader::finish() {
    if (failed_) {
        return false;
    }
    if (!rootSeen_) {
        fail("Not a GPX file");
        return false;
    }
    if (!rootClosed_ || state_ != State::Content) {
        fail("The file is incomplete");
        return false;
    }
    return true;
}

int GpxReader::pointCount() const {
    return pointCount_;
}

int GpxReader::skippedPoints() const {
    return skippedPoints_;
}

const char* GpxReader::error() const {
    return error_;
}

void GpxReader::fail(const char* message) {
    if (!failed_) {
        failed_ = true;
        error_ = message;
    }
}

void GpxReader::resetTag() {
    tagName_[0] = '\0';
    tagLen_ = 0;
    attrName_[0] = '\0';
    attrLen_ = 0;
    attrValue_[0] = '\0';
    attrValueLen_ = 0;
    tagLat_ = 0.0;
    tagLon_ = 0.0;
    tagHasLat_ = false;
    tagHasLon_ = false;
}

void GpxReader::appendText(char c) {
    if (depth_ <= 0) {
        return;
    }
    const Role role = stack_[depth_ - 1].role;
    if (role == Role::LineName || role == Role::PointName || role == Role::Ele || role == Role::Time) {
        if (textLen_ + 1 < sizeof(text_)) {
            text_[textLen_++] = c;
            text_[textLen_] = '\0';
        }
    }
}

void GpxReader::appendTagName(char c) {
    if (tagLen_ + 1 < sizeof(tagName_)) {
        tagName_[tagLen_++] = c;
        tagName_[tagLen_] = '\0';
    }
}

void GpxReader::appendAttrName(char c) {
    if (attrLen_ + 1 < sizeof(attrName_)) {
        attrName_[attrLen_++] = c;
        attrName_[attrLen_] = '\0';
    }
}

void GpxReader::appendAttrValue(char c) {
    if (attrValueLen_ + 1 < sizeof(attrValue_)) {
        attrValue_[attrValueLen_++] = c;
        attrValue_[attrValueLen_] = '\0';
    }
}

void GpxReader::applyAttribute(bool hasValue) {
    if (!hasValue) {
        return;
    }
    attrName_[attrLen_] = '\0';
    attrValue_[attrValueLen_] = '\0';
    const char* name = localName(attrName_);
    double value = 0.0;
    if (strcmp(name, "lat") == 0) {
        if (parseDouble(attrValue_, value)) {
            tagLat_ = value;
            tagHasLat_ = true;
        }
    } else if (strcmp(name, "lon") == 0) {
        if (parseDouble(attrValue_, value)) {
            tagLon_ = value;
            tagHasLon_ = true;
        }
    }
}

void GpxReader::handleByte(char c) {
    switch (state_) {
    case State::Content:
        if (c == '<') {
            sawTag_ = true;
            resetTag();
            state_ = State::TagStart;
        } else {
            if (!sawTag_ && !isSpace(c)) {
                nonSpace_++;
                if (nonSpace_ >= 256) {
                    fail("Not a GPX file");
                    return;
                }
            }
            appendText(c);
        }
        break;
    case State::TagStart:
        if (c == '/') {
            tagName_[0] = '\0';
            tagLen_ = 0;
            state_ = State::EndTagName;
        } else if (c == '!') {
            state_ = State::Bang;
            commentDashes_ = 0;
        } else if (c == '?') {
            state_ = State::Pi;
            piQuestion_ = false;
        } else if (isSpace(c)) {
            break;
        } else {
            appendTagName(c);
            state_ = State::TagName;
        }
        break;
    case State::Bang:
        if (c == '-') {
            state_ = State::BangDash;
        } else if (c == '[') {
            cdataMatch_ = 0;
            state_ = State::CDataOpen;
        } else if (c == '>') {
            state_ = State::Content;
        } else {
            state_ = State::Doctype;
        }
        break;
    case State::BangDash:
        if (c == '-') {
            commentDashes_ = 0;
            state_ = State::Comment;
        } else {
            state_ = State::Doctype;
        }
        break;
    case State::CDataOpen:
        if (c == "CDATA["[cdataMatch_]) {
            cdataMatch_++;
            if (cdataMatch_ == 6) {
                cdataBrackets_ = 0;
                state_ = State::CData;
            }
        } else {
            state_ = State::Doctype;
        }
        break;
    case State::Comment:
        if (c == '-') {
            commentDashes_++;
        } else if (c == '>' && commentDashes_ >= 2) {
            commentDashes_ = 0;
            state_ = State::Content;
        } else {
            commentDashes_ = 0;
        }
        break;
    case State::CData:
        if (c == ']') {
            cdataBrackets_++;
        } else if (c == '>' && cdataBrackets_ >= 2) {
            cdataBrackets_ = 0;
            state_ = State::Content;
        } else {
            cdataBrackets_ = 0;
        }
        break;
    case State::Pi:
        if (c == '>' && piQuestion_) {
            piQuestion_ = false;
            state_ = State::Content;
        } else {
            piQuestion_ = c == '?';
        }
        break;
    case State::Doctype:
        if (c == '>') {
            state_ = State::Content;
        }
        break;
    case State::TagName:
        if (c == '>') {
            finishStartTag(false);
        } else if (c == '/') {
            state_ = State::SelfClose;
        } else if (isSpace(c)) {
            state_ = State::BeforeAttr;
        } else {
            appendTagName(c);
        }
        break;
    case State::EndTagName:
        if (c == '>') {
            finishEndTag();
            state_ = State::Content;
        } else if (!isSpace(c)) {
            appendTagName(c);
        }
        break;
    case State::BeforeAttr:
        if (isSpace(c)) {
            break;
        }
        if (c == '>') {
            finishStartTag(false);
        } else if (c == '/') {
            state_ = State::SelfClose;
        } else {
            attrName_[0] = '\0';
            attrLen_ = 0;
            appendAttrName(c);
            state_ = State::AttrName;
        }
        break;
    case State::AttrName:
        if (c == '=') {
            state_ = State::BeforeValue;
        } else if (isSpace(c)) {
            state_ = State::AfterAttrName;
        } else if (c == '>') {
            applyAttribute(false);
            finishStartTag(false);
        } else if (c == '/') {
            applyAttribute(false);
            state_ = State::SelfClose;
        } else {
            appendAttrName(c);
        }
        break;
    case State::AfterAttrName:
        if (isSpace(c)) {
            break;
        }
        if (c == '=') {
            state_ = State::BeforeValue;
        } else if (c == '>') {
            applyAttribute(false);
            finishStartTag(false);
        } else if (c == '/') {
            applyAttribute(false);
            state_ = State::SelfClose;
        } else {
            applyAttribute(false);
            attrName_[0] = '\0';
            attrLen_ = 0;
            appendAttrName(c);
            state_ = State::AttrName;
        }
        break;
    case State::BeforeValue:
        if (isSpace(c)) {
            break;
        }
        if (c == '"' || c == '\'') {
            quote_ = c;
            attrValue_[0] = '\0';
            attrValueLen_ = 0;
            state_ = State::Value;
        } else if (c == '>') {
            applyAttribute(false);
            finishStartTag(false);
        } else if (c == '/') {
            applyAttribute(false);
            state_ = State::SelfClose;
        } else {
            quote_ = '\0';
            attrValue_[0] = '\0';
            attrValueLen_ = 0;
            appendAttrValue(c);
            state_ = State::UnquotedValue;
        }
        break;
    case State::Value:
        if (c == quote_) {
            applyAttribute(true);
            state_ = State::BeforeAttr;
        } else {
            appendAttrValue(c);
        }
        break;
    case State::UnquotedValue:
        if (isSpace(c)) {
            applyAttribute(true);
            state_ = State::BeforeAttr;
        } else if (c == '>') {
            applyAttribute(true);
            finishStartTag(false);
        } else if (c == '/') {
            applyAttribute(true);
            state_ = State::SelfClose;
        } else {
            appendAttrValue(c);
        }
        break;
    case State::SelfClose:
        if (c == '>') {
            finishStartTag(true);
        }
        break;
    }
}

void GpxReader::finishStartTag(bool selfClosing) {
    tagName_[tagLen_] = '\0';
    if (depth_ == 0) {
        if (strcmp(localName(tagName_), "gpx") != 0) {
            fail("Not a GPX file");
            return;
        }
        rootSeen_ = true;
    }
    if (depth_ >= kMaxDepth) {
        fail("The file is incomplete");
        return;
    }
    const Role parent = depth_ > 0 ? stack_[depth_ - 1].role : Role::None;
    const char* name = localName(tagName_);
    Role role = Role::Other;
    if (parent == Role::None) {
        role = strcmp(name, "gpx") == 0 ? Role::Gpx : Role::Other;
    } else if (parent == Role::Gpx) {
        if (strcmp(name, "wpt") == 0) {
            role = Role::Wpt;
        } else if (strcmp(name, "rte") == 0) {
            role = Role::Rte;
        } else if (strcmp(name, "trk") == 0) {
            role = Role::Trk;
        } else if (strcmp(name, "metadata") == 0) {
            role = Role::Metadata;
        }
    } else if (parent == Role::Trk) {
        if (strcmp(name, "name") == 0) {
            role = Role::LineName;
        } else if (strcmp(name, "trkseg") == 0) {
            role = Role::Trkseg;
        }
    } else if (parent == Role::Rte) {
        if (strcmp(name, "name") == 0) {
            role = Role::LineName;
        } else if (strcmp(name, "rtept") == 0) {
            role = Role::Rtept;
        }
    } else if (parent == Role::Trkseg) {
        if (strcmp(name, "trkpt") == 0) {
            role = Role::Trkpt;
        }
    } else if (parent == Role::Wpt || parent == Role::Rtept || parent == Role::Trkpt) {
        if (strcmp(name, "name") == 0) {
            role = Role::PointName;
        } else if (strcmp(name, "ele") == 0) {
            role = Role::Ele;
        } else if (strcmp(name, "time") == 0) {
            role = Role::Time;
        }
    }
    Element& element = stack_[depth_];
    size_t length = strlen(name);
    if (length >= sizeof(element.name)) {
        length = sizeof(element.name) - 1;
    }
    memcpy(element.name, name, length);
    element.name[length] = '\0';
    element.role = role;
    depth_++;
    startRole(role);
    if (selfClosing) {
        finishEndTag();
    }
    state_ = State::Content;
}

void GpxReader::finishEndTag() {
    if (depth_ == 0) {
        fail("Not a GPX file");
        return;
    }
    depth_--;
    const Role role = stack_[depth_].role;
    switch (role) {
    case Role::Gpx:
        rootClosed_ = true;
        break;
    case Role::Trk:
    case Role::Rte:
        ensureLineStarted();
        listener_.endLine();
        lineActive_ = false;
        lineStarted_ = false;
        break;
    case Role::LineName:
        decodeText(lineName_, sizeof(lineName_));
        break;
    case Role::PointName:
        decodeText(currentPoint_.name, sizeof(currentPoint_.name));
        break;
    case Role::Ele: {
        double value = 0.0;
        currentPoint_.elevation = parseDouble(text_, value) ? static_cast<float>(value) : NAN;
        break;
    }
    case Role::Time: {
        int64_t value = -1;
        if (parseTimeText(text_, value)) {
            currentPoint_.time = value;
        }
        break;
    }
    case Role::Wpt:
    case Role::Rtept:
    case Role::Trkpt:
        emitPoint();
        break;
    default:
        break;
    }
}

void GpxReader::startRole(Role role) {
    switch (role) {
    case Role::Trk:
        lineActive_ = true;
        lineStarted_ = false;
        lineKind_ = GpxKind::TrackPoint;
        lineName_[0] = '\0';
        break;
    case Role::Rte:
        lineActive_ = true;
        lineStarted_ = false;
        lineKind_ = GpxKind::RoutePoint;
        lineName_[0] = '\0';
        break;
    case Role::Trkseg:
        ensureLineStarted();
        listener_.beginSegment();
        break;
    case Role::Wpt:
    case Role::Rtept:
    case Role::Trkpt:
        ensureLineStarted();
        currentPoint_ = {};
        currentPoint_.kind = role == Role::Wpt ? GpxKind::Waypoint
            : (role == Role::Rtept ? GpxKind::RoutePoint : GpxKind::TrackPoint);
        currentPoint_.elevation = NAN;
        currentPoint_.time = -1;
        currentPoint_.lat = tagLat_;
        currentPoint_.lon = tagLon_;
        pointHasLat_ = tagHasLat_;
        pointHasLon_ = tagHasLon_;
        break;
    case Role::LineName:
    case Role::PointName:
    case Role::Ele:
    case Role::Time:
        textLen_ = 0;
        text_[0] = '\0';
        break;
    default:
        break;
    }
}

void GpxReader::ensureLineStarted() {
    if (lineActive_ && !lineStarted_) {
        listener_.beginLine(lineKind_, lineName_);
        lineStarted_ = true;
    }
}

void GpxReader::emitPoint() {
    if (!pointHasLat_ || !pointHasLon_) {
        skippedPoints_++;
        return;
    }
    if (!(currentPoint_.lat >= -90.0 && currentPoint_.lat <= 90.0)) {
        skippedPoints_++;
        return;
    }
    if (!(currentPoint_.lon >= -180.0 && currentPoint_.lon <= 180.0)) {
        skippedPoints_++;
        return;
    }
    pointCount_++;
    listener_.point(currentPoint_);
}

void GpxReader::decodeText(char* out, size_t size) {
    size_t begin = 0;
    size_t end = textLen_;
    while (begin < end && isSpace(text_[begin])) {
        begin++;
    }
    while (end > begin && isSpace(text_[end - 1])) {
        end--;
    }
    text_[end] = '\0';
    decodeEntities(text_ + begin, out, size);
}

} // namespace maps
