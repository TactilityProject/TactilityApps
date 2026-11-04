// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

enum class GpxKind : uint8_t { Waypoint, RoutePoint, TrackPoint };

struct GpxPoint {
    GpxKind kind;
    double lat;
    double lon;
    float elevation;
    int64_t time;
    char name[48];
};

class GpxListener {
public:
    virtual ~GpxListener() = default;
    /**
     * A <trk> or <rte> starts; name may be empty (it is reported when known: the name comes after the start, so
     * call this at the first point or at the end of the element, whichever is first, with the name read so far).
     */
    virtual void beginLine(GpxKind kind, const char* name) {
        (void)kind;
        (void)name;
    }
    /** A new <trkseg> inside a track. */
    virtual void beginSegment() {}
    /** A point; for waypoints and route points name/elevation/time are complete, for track points too. */
    virtual void point(const GpxPoint& point) {
        (void)point;
    }
    virtual void endLine() {}
};

class GpxReader {
public:
    explicit GpxReader(GpxListener& listener);
    /** Feeds the next bytes of the file, any chunk size from 1 byte up. Returns false once the file is known to be bad. */
    bool feed(const char* data, size_t size);
    /** Call after the last chunk. True when the document was a complete GPX file (root <gpx>, closed). */
    bool finish();
    /** Counts, and why a file was refused (a short English sentence, "" when fine). */
    int pointCount() const;
    int skippedPoints() const;
    const char* error() const;

private:
    enum class State : uint8_t {
        Content,
        TagStart,
        Bang,
        BangDash,
        CDataOpen,
        Comment,
        CData,
        Pi,
        Doctype,
        TagName,
        EndTagName,
        BeforeAttr,
        AttrName,
        AfterAttrName,
        BeforeValue,
        Value,
        UnquotedValue,
        SelfClose,
    };

    enum class Role : uint8_t {
        None,
        Gpx,
        Metadata,
        Trk,
        Trkseg,
        Rte,
        Wpt,
        Rtept,
        Trkpt,
        LineName,
        PointName,
        Ele,
        Time,
        Other,
    };

    struct Element {
        char name[32];
        Role role;
    };

    static constexpr int kMaxDepth = 32;

    GpxListener& listener_;
    bool failed_ = false;
    const char* error_ = "";
    int pointCount_ = 0;
    int skippedPoints_ = 0;
    bool rootSeen_ = false;
    bool rootClosed_ = false;
    bool sawTag_ = false;
    int nonSpace_ = 0;

    State state_ = State::Content;
    char tagName_[32] = {};
    size_t tagLen_ = 0;
    char attrName_[32] = {};
    size_t attrLen_ = 0;
    char attrValue_[64] = {};
    size_t attrValueLen_ = 0;
    char quote_ = '"';
    char text_[64] = {};
    size_t textLen_ = 0;
    int commentDashes_ = 0;
    int cdataMatch_ = 0;
    int cdataBrackets_ = 0;
    bool piQuestion_ = false;

    double tagLat_ = 0.0;
    double tagLon_ = 0.0;
    bool tagHasLat_ = false;
    bool tagHasLon_ = false;

    Element stack_[kMaxDepth] = {};
    int depth_ = 0;

    bool lineActive_ = false;
    bool lineStarted_ = false;
    GpxKind lineKind_ = GpxKind::TrackPoint;
    char lineName_[64] = {};

    GpxPoint currentPoint_ = {};
    bool pointHasLat_ = false;
    bool pointHasLon_ = false;

    void fail(const char* message);
    void handleByte(char c);
    void resetTag();
    void appendText(char c);
    void appendTagName(char c);
    void appendAttrName(char c);
    void appendAttrValue(char c);
    void applyAttribute(bool hasValue);
    void finishStartTag(bool selfClosing);
    void finishEndTag();
    void startRole(Role role);
    void ensureLineStarted();
    void emitPoint();
    void decodeText(char* out, size_t size);
};

} // namespace maps
