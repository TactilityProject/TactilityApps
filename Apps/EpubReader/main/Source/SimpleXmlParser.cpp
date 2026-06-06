#include "SimpleXmlParser.h"
#include <tactility/log.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "SimpleXmlParser";

SimpleXmlParser::SimpleXmlParser() {
    buffer_ = (uint8_t*)malloc(BUFFER_SIZE);
    if (!buffer_) {
        LOG_E(TAG, "Failed to allocate buffer");
        abort();
    }
}

SimpleXmlParser::~SimpleXmlParser() {
    close();
    if (buffer_) {
        free(buffer_);
        buffer_ = nullptr;
    }
}

bool SimpleXmlParser::openFromMemory(const char* data, size_t dataSize) {
    close();
    if (!buffer_ || !data) return false;

    memoryData_ = data;
    memorySize_ = dataSize;
    usingMemory_ = true;

    bufferStartPos_ = 0;
    bufferLen_ = 0;
    filePos_ = 0;
    currentNodeType_ = NodeType::None;
    return true;
}

bool SimpleXmlParser::openFromStream(StreamCallback callback) {
    close();
    if (!buffer_ || !callback) return false;

    streamCallback_ = callback;
    usingStream_ = true;
    streamPosition_ = 0;
    streamEOF_ = false;

    bufferStartPos_ = 0;
    bufferLen_ = 0;
    filePos_ = 0;
    currentNodeType_ = NodeType::None;
    return true;
}

void SimpleXmlParser::close() {
    memoryData_ = nullptr;
    memorySize_ = 0;
    usingMemory_ = false;
    streamCallback_ = nullptr;
    usingStream_ = false;
    streamPosition_ = 0;
    streamEOF_ = false;
    bufferStartPos_ = 0;
    bufferLen_ = 0;
    filePos_ = 0;
    currentNodeType_ = NodeType::None;
}

bool SimpleXmlParser::loadBufferAround(size_t pos) {
    if (usingStream_) {
        // Simple streaming: we can only move forward
        if (pos < bufferStartPos_) return false;
        
        while (pos >= bufferStartPos_ + bufferLen_ && !streamEOF_) {
            int bytesRead = streamCallback_((char*)buffer_, BUFFER_SIZE);
            if (bytesRead <= 0) {
                streamEOF_ = true;
                return false;
            }
            bufferStartPos_ = streamPosition_;
            bufferLen_ = bytesRead;
            streamPosition_ += bytesRead;
        }
        return pos >= bufferStartPos_ && pos < bufferStartPos_ + bufferLen_;
    }

    if (usingMemory_) {
        if (pos >= memorySize_) return false;
        
        bufferStartPos_ = pos;
        bufferLen_ = (memorySize_ - pos > BUFFER_SIZE) ? BUFFER_SIZE : (memorySize_ - pos);
        memcpy(buffer_, memoryData_ + pos, bufferLen_);
        return bufferLen_ > 0;
    }

    return false;
}

char SimpleXmlParser::getByteAt(size_t pos) {
    if (usingMemory_) {
        return (pos < memorySize_) ? memoryData_[pos] : '\0';
    }
    if (bufferLen_ > 0 && pos >= bufferStartPos_ && pos < bufferStartPos_ + bufferLen_) {
        return (char)buffer_[pos - bufferStartPos_];
    }
    if (loadBufferAround(pos)) {
        return (char)buffer_[pos - bufferStartPos_];
    }
    return '\0';
}

char SimpleXmlParser::peekChar() {
    return getByteAt(filePos_);
}

char SimpleXmlParser::readChar() {
    char c = getByteAt(filePos_);
    if (c != '\0') filePos_++;
    return c;
}

bool SimpleXmlParser::skipWhitespace() {
    while (true) {
        char c = peekChar();
        if (c == '\0') return false;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            readChar();
        } else {
            return true;
        }
    }
}

bool SimpleXmlParser::matchString(const char* str) {
    size_t len = strlen(str);
    size_t savedFilePos = filePos_;
    for (size_t i = 0; i < len; i++) {
        if (readChar() != str[i]) {
            filePos_ = savedFilePos;
            return false;
        }
    }
    return true;
}

bool SimpleXmlParser::read() {
    if (!usingMemory_ && !usingStream_) return false;

    currentName_.clear();
    currentValue_.clear();
    isEmptyElement_ = false;
    attributes_.clear();

    while (true) {
        char c = peekChar();
        if (c == '\0') {
            currentNodeType_ = NodeType::EndOfFile;
            return false;
        }

        if (c == '<') {
            readChar(); // consume '<'
            char next = peekChar();
            if (next == '/') {
                return readEndElement();
            } else if (next == '!') {
                readChar(); // consume '!'
                if (peekChar() == '-') return readComment();
                if (matchString("[CDATA[")) return readCDATA();
                skipToEndOfTag();
                continue;
            } else if (next == '?') {
                return readProcessingInstruction();
            } else {
                return readElement();
            }
        } else {
            if (readText()) return true;
            // whitespace-only text node - continue outer loop
        }
    }
}

bool SimpleXmlParser::readElement() {
    currentNodeType_ = NodeType::Element;
    currentName_ = readElementName();
    parseAttributes();
    skipWhitespace();
    if (peekChar() == '/') {
        readChar();
        isEmptyElement_ = true;
    }
    skipToEndOfTag();
    return true;
}

bool SimpleXmlParser::readEndElement() {
    currentNodeType_ = NodeType::EndElement;
    readChar(); // consume '/'
    currentName_ = readElementName();
    skipToEndOfTag();
    return true;
}

bool SimpleXmlParser::readText() {
    currentNodeType_ = NodeType::Text;
    currentValue_.clear();
    bool hasNonWhitespace = false;
    while (true) {
        char c = peekChar();
        if (c == '\0' || c == '<') break;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') hasNonWhitespace = true;
        currentValue_ += readChar();
    }
    return hasNonWhitespace; // false = whitespace-only; caller (read()) continues looping
}

bool SimpleXmlParser::readComment() {
    currentNodeType_ = NodeType::Comment;
    readChar(); // consume '-'
    if (readChar() != '-') {
        skipToEndOfTag();
        return false;
    }
    while (true) {
        char c = readChar();
        if (c == '\0') break;
        if (c == '-' && peekChar() == '-') {
            readChar();
            if (peekChar() == '>') {
                readChar();
                break;
            }
        }
    }
    return true;
}

bool SimpleXmlParser::readCDATA() {
    currentNodeType_ = NodeType::CDATA;
    while (true) {
        char c = readChar();
        if (c == '\0') break;
        if (c == ']' && peekChar() == ']') {
            size_t savedPos = filePos_;
            readChar(); // tentatively consume second ']'
            if (peekChar() == '>') {
                readChar(); // consume '>'
                break;
            }
            filePos_ = savedPos; // not end of CDATA - backtrack second ']'
        }
        currentValue_ += c;
    }
    return true;
}

bool SimpleXmlParser::readProcessingInstruction() {
    currentNodeType_ = NodeType::ProcessingInstruction;
    readChar(); // consume '?'
    currentName_ = readElementName();
    while (true) {
        char c = readChar();
        if (c == '\0') break;
        if (c == '?' && peekChar() == '>') {
            readChar();
            break;
        }
    }
    return true;
}

std::string SimpleXmlParser::readElementName() {
    std::string name;
    while (true) {
        char c = peekChar();
        if (c == '\0' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == '/' || c == '=') break;
        name += readChar();
    }
    return name;
}

void SimpleXmlParser::parseAttributes() {
    while (true) {
        skipWhitespace();
        char c = peekChar();
        if (c == '>' || c == '/' || c == '\0') break;
        std::string name = readElementName();
        if (name.empty()) break;
        skipWhitespace();
        if (readChar() != '=') break;
        skipWhitespace();
        char quote = readChar();
        if (quote != '"' && quote != '\'') break;
        std::string value;
        while (true) {
            char vc = readChar();
            if (vc == '\0' || vc == quote) break;
            value += vc;
        }
        attributes_.push_back({name, value});
    }
}

void SimpleXmlParser::skipToEndOfTag() {
    while (true) {
        char c = readChar();
        if (c == '>' || c == '\0') break;
    }
}

std::string SimpleXmlParser::getAttribute(const char* name) const {
    for (const auto& attr : attributes_) {
        if (attr.name == name) return attr.value;
    }
    return "";
}
