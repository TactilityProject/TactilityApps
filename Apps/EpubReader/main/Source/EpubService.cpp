#include "EpubService.h"
#include "SimpleXmlParser.h"
#include <tactility/log.h>
#include <map>

extern "C" {
#include "epub_parser.h"
}

static const char* TAG = "EpubService";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string EpubService::pathParent(const std::string& path) {
    size_t pos = path.rfind('/');
    return (pos == std::string::npos) ? "" : path.substr(0, pos);
}

// Join basePath + href and resolve any ".." or "." segments so EPUBs that use
// relative paths like "../../Text/chapter1.xhtml" resolve correctly.
static std::string normalizePath(const std::string& basePath, const std::string& href) {
    if (!href.empty() && href[0] == '/') return href; // already absolute

    std::string combined = basePath.empty() ? href : basePath + "/" + href;

    // Walk each slash-delimited segment and resolve ".." / "."
    std::string result;
    result.reserve(combined.size());
    size_t i = 0;
    while (i <= combined.size()) {
        size_t slash = combined.find('/', i);
        bool atEnd = (slash == std::string::npos);
        std::string seg = atEnd ? combined.substr(i) : combined.substr(i, slash - i);

        if (seg == "..") {
            size_t last = result.rfind('/');
            if (last != std::string::npos) result.erase(last);
            else result.clear();
        } else if (!seg.empty() && seg != ".") {
            if (!result.empty()) result += '/';
            result += seg;
        }

        if (atEnd) break;
        i = slash + 1;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::shared_ptr<EpubService> EpubService::open(const std::string& path) {
    epub_reader_c* reader = nullptr;
    epub_parser_error err = epub_open_c(path.c_str(), &reader);
    if (err != EPUB_OK) {
        LOG_E(TAG, "Failed to open epub: %s (err=%d)", path.c_str(), (int)err);
        return nullptr;
    }
    auto service = std::shared_ptr<EpubService>(new EpubService());
    service->reader_ = reader;
    service->parseContainer();
    service->parseContentOpf();

    // Return nullptr if essential parsing failed
    if (service->spine_.empty()) {
        LOG_E(TAG, "EPUB initialization failed: no spine items");
        return nullptr;
    }
    return service;
}

EpubService::~EpubService() {
    if (reader_) {
        epub_close_c(reader_);
        reader_ = nullptr;
    }
}

std::string EpubService::readFile(const std::string& filename, size_t maxBytes) const {
    if (!reader_) return "";
    epub_stream_context_c* stream = epub_stream_open_c(reader_, filename.c_str());
    if (!stream) {
        LOG_W(TAG, "File not found in epub: %s", filename.c_str());
        return "";
    }
    std::string result;
    // Small stack buffer - readFile sits deep in the LVGL call chain and 4 KB
    // eaten by a local array is enough to cause a stack overflow on some targets.
    char buffer[512];
    size_t readBytes;
    while ((readBytes = epub_stream_read_c(stream, buffer, sizeof(buffer))) > 0) {
        if (maxBytes > 0 && result.size() + readBytes > maxBytes) {
            result.append(buffer, maxBytes - result.size());
            break;
        }
        result.append(buffer, readBytes);
    }
    epub_stream_close_c(stream);

    // If truncated at maxBytes, ensure we didn't split a UTF-8 multi-byte sequence.
    if (maxBytes > 0 && result.size() >= maxBytes) {
        size_t len = result.size();
        while (len > 0 && ((unsigned char)result[len - 1] & 0xC0) == 0x80) --len;
        if (len > 0) {
            unsigned char lead = (unsigned char)result[len - 1];
            size_t seqLen = (lead < 0x80) ? 1 : (lead < 0xE0) ? 2 : (lead < 0xF0) ? 3 : 4;
            if (len - 1 + seqLen > result.size()) result.resize(len - 1);
        }
    }
    return result;
}

std::string EpubService::getMetadata(const std::string& key) const {
    auto it = metadata_.find(key);
    return (it != metadata_.end()) ? it->second : "";
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

void EpubService::parseContainer() {
    std::string xml = readFile("META-INF/container.xml", 65536);  // 64 KB cap
    if (xml.empty()) {
        LOG_E(TAG, "container.xml missing");
        return;
    }
    SimpleXmlParser parser;
    if (!parser.openFromMemory(xml.c_str(), xml.size())) return;

    while (parser.read()) {
        if (parser.getNodeType() == SimpleXmlParser::NodeType::Element &&
            parser.getName() == "rootfile") {
            contentOpfPath_ = parser.getAttribute("full-path");
            LOG_I(TAG, "OPF path: %s", contentOpfPath_.c_str());
            break;
        }
    }
}

void EpubService::parseContentOpf() {
    if (contentOpfPath_.empty()) return;

    std::string xml = readFile(contentOpfPath_, 1048576);  // 1 MB cap
    if (xml.empty()) {
        LOG_E(TAG, "OPF file empty: %s", contentOpfPath_.c_str());
        return;
    }

    SimpleXmlParser parser;
    if (!parser.openFromMemory(xml.c_str(), xml.size())) return;

    std::map<std::string, std::string> manifest;
    bool inMetadata = false, inManifest = false, inSpine = false;
    std::string lastTag;
    std::string tocId;
    std::string coverMetaId;  // id from <meta name="cover" content="ID"/>

    while (parser.read()) {
        auto nodeType = parser.getNodeType();
        auto name = parser.getName();

        if (nodeType == SimpleXmlParser::NodeType::Element) {
            lastTag = name;
            if (name == "metadata") {
                inMetadata = true;
            } else if (name == "manifest") {
                inManifest = true;
            } else if (name == "spine") {
                inSpine = true;
                tocId = parser.getAttribute("toc");
            } else if (inMetadata && name == "meta") {
                // EPUB2: <meta name="cover" content="cover-item-id"/>
                if (parser.getAttribute("name") == "cover") {
                    coverMetaId = parser.getAttribute("content");
                }
            } else if (inManifest && name == "item") {
                std::string id   = parser.getAttribute("id");
                std::string href = parser.getAttribute("href");
                if (!id.empty() && !href.empty()) {
                    manifest[id] = href;
                    // EPUB3: <item properties="cover-image" .../>
                    if (coverPath_.empty()) {
                        std::string props = parser.getAttribute("properties");
                        if (props.find("cover-image") != std::string::npos) {
                            coverPath_ = href; // resolved below after baseDir is known
                        }
                    }
                    // Common fallback ids
                    if (coverPath_.empty() && (id == "cover" || id == "cover-image" || id == "cover-img")) {
                        std::string mt = parser.getAttribute("media-type");
                        if (mt.find("image") != std::string::npos) {
                            coverPath_ = href;
                        }
                    }
                }
            } else if (inSpine && name == "itemref") {
                std::string idref = parser.getAttribute("idref");
                if (!idref.empty()) {
                    spine_.push_back({idref, ""});
                }
            }
        } else if (nodeType == SimpleXmlParser::NodeType::EndElement) {
            if (name == "metadata") inMetadata = false;
            else if (name == "manifest") inManifest = false;
            else if (name == "spine") inSpine = false;
            lastTag = "";
        } else if (nodeType == SimpleXmlParser::NodeType::Text && inMetadata) {
            if (lastTag == "dc:title") metadata_["title"] = parser.getText();
            else if (lastTag == "dc:creator") metadata_["creator"] = parser.getText();
        }
    }

    // Resolve spine hrefs from manifest (normalizePath handles "../" segments)
    std::string baseDir = pathParent(contentOpfPath_);
    for (auto& si : spine_) {
        auto it = manifest.find(si.idref);
        if (it != manifest.end()) {
            si.href = normalizePath(baseDir, it->second);
        }
    }

    // Resolve cover path: EPUB2 <meta name="cover"> takes priority
    if (!coverMetaId.empty()) {
        auto it = manifest.find(coverMetaId);
        if (it != manifest.end()) coverPath_ = it->second;
    }
    if (!coverPath_.empty()) {
        coverPath_ = normalizePath(baseDir, coverPath_);
        LOG_I(TAG, "Cover: %s", coverPath_.c_str());
    }

    LOG_I(TAG, "Spine items: %d", (int)spine_.size());

    // Parse NCX table of contents
    if (!tocId.empty()) {
        auto it = manifest.find(tocId);
        if (it != manifest.end()) {
            parseTocNcx(normalizePath(baseDir, it->second));
        }
    }
}

void EpubService::parseTocNcx(const std::string& tocPath) {
    std::string xml = readFile(tocPath, 524288);  // 512 KB cap
    if (xml.empty()) return;

    SimpleXmlParser parser;
    if (!parser.openFromMemory(xml.c_str(), xml.size())) return;

    std::string baseDir = pathParent(tocPath);
    bool inNavPoint = false, inNavLabel = false;
    std::string currentTitle, currentHref;

    while (parser.read()) {
        auto nodeType = parser.getNodeType();
        auto name = parser.getName();

        if (nodeType == SimpleXmlParser::NodeType::Element) {
            if (name == "navPoint") {
                inNavPoint = true;
                currentTitle.clear();
                currentHref.clear();
            } else if (name == "navLabel") {
                inNavLabel = true;
            } else if (name == "content" && inNavPoint) {
                currentHref = parser.getAttribute("src");
            }
        } else if (nodeType == SimpleXmlParser::NodeType::Text && inNavLabel) {
            currentTitle = parser.getText();
        } else if (nodeType == SimpleXmlParser::NodeType::EndElement) {
            if (name == "navPoint") {
                if (!currentTitle.empty() && !currentHref.empty()) {
                    // Strip fragment identifier before normalizing
                    std::string href = currentHref;
                    size_t hashPos = href.find('#');
                    if (hashPos != std::string::npos) href = href.substr(0, hashPos);
                    if (!href.empty()) {
                        toc_.push_back({currentTitle, normalizePath(baseDir, href)});
                    }
                }
                inNavPoint = false;
            } else if (name == "navLabel") {
                inNavLabel = false;
            }
        }
    }

    LOG_I(TAG, "TOC items: %d", (int)toc_.size());
}
