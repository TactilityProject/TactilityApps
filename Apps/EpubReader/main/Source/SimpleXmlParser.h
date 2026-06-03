#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

/**
 * SimpleXmlParser - A buffered XML parser for reading attributes
 * Adapted from microreader for Tactility.
 */
class SimpleXmlParser {
public:
    typedef std::function<int(char* buffer, size_t maxSize)> StreamCallback;

    SimpleXmlParser();
    ~SimpleXmlParser();
    SimpleXmlParser(const SimpleXmlParser&) = delete;
    SimpleXmlParser& operator=(const SimpleXmlParser&) = delete;
    SimpleXmlParser(SimpleXmlParser&&) = delete;
    SimpleXmlParser& operator=(SimpleXmlParser&&) = delete;

    bool openFromMemory(const char* data, size_t dataSize);
    bool openFromStream(StreamCallback callback);
    void close();

    enum class NodeType {
        None = 0,
        Element,
        Text,
        EndElement,
        Comment,
        ProcessingInstruction,
        CDATA,
        EndOfFile
    };

    bool read();
    NodeType getNodeType() const { return currentNodeType_; }
    const std::string& getName() const { return currentName_; }
    bool isEmptyElement() const { return isEmptyElement_; }
    std::string getAttribute(const char* name) const;
    
    // For text nodes
    const std::string& getText() const { return currentValue_; }

private:
    const char* memoryData_ = nullptr;
    size_t memorySize_ = 0;
    bool usingMemory_ = false;

    StreamCallback streamCallback_;
    bool usingStream_ = false;
    size_t streamPosition_ = 0;
    bool streamEOF_ = false;

    static constexpr size_t BUFFER_SIZE = 4096;
    uint8_t* buffer_ = nullptr;
    size_t bufferStartPos_ = 0;
    size_t bufferLen_ = 0;
    size_t filePos_ = 0;

    char getByteAt(size_t pos);
    bool loadBufferAround(size_t pos);
    bool skipWhitespace();
    bool matchString(const char* str);
    char readChar();
    char peekChar();

    struct Attribute {
        std::string name;
        std::string value;
    };

    NodeType currentNodeType_ = NodeType::None;
    std::string currentName_;
    std::string currentValue_;
    bool isEmptyElement_ = false;
    std::vector<Attribute> attributes_;

    bool readElement();
    bool readEndElement();
    bool readText();
    bool readComment();
    bool readCDATA();
    bool readProcessingInstruction();
    void parseAttributes();
    std::string readElementName();
    void skipToEndOfTag();
};
