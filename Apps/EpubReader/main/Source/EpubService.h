#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>

// Forward-declare the C epub reader handle so we don't need to pull in the C header
struct epub_reader_c;

/**
 * EpubService - handles opening and reading EPUB archive files.
 * Parses the OPF manifest, spine, and NCX table of contents.
 * Use open() to create an instance; check isValid() before use.
 */
class EpubService {
public:
    struct SpineItem {
        std::string idref;
        std::string href;
    };

    struct TocItem {
        std::string title;
        std::string href;
    };

    /**
     * Open an EPUB file. Returns nullptr on failure.
     */
    static std::shared_ptr<EpubService> open(const std::string& path);
    ~EpubService();

    bool isValid() const { return reader_ != nullptr && !spine_.empty(); }

    const std::vector<SpineItem>& getSpine() const { return spine_; }
    const std::vector<TocItem>& getToc() const { return toc_; }

    /** Path of the cover image inside the EPUB archive, or "" if not found.
     *  Typically a .jpg or .png. Use readFile() to get the raw bytes. */
    const std::string& getCoverPath() const { return coverPath_; }

    /** Read a file from the EPUB archive into a string. Returns "" on error.
     *  If maxBytes > 0, reading stops after that many bytes (useful for large chapter HTML). */
    std::string readFile(const std::string& filename, size_t maxBytes = 0) const;

    /** Get OPF metadata value by key (e.g. "title", "creator"). */
    std::string getMetadata(const std::string& key) const;

private:
    EpubService() = default;
    EpubService(const EpubService&) = delete;
    EpubService& operator=(const EpubService&) = delete;
    EpubService(EpubService&&) = delete;
    EpubService& operator=(EpubService&&) = delete;

    epub_reader_c* reader_ = nullptr;
    std::vector<SpineItem> spine_;
    std::vector<TocItem> toc_;
    std::map<std::string, std::string> metadata_;
    std::string contentOpfPath_;
    std::string coverPath_;   // resolved path of cover image within the archive, or ""

    void parseContainer();
    void parseContentOpf();
    void parseTocNcx(const std::string& tocPath);

    static std::string pathParent(const std::string& path);
};
