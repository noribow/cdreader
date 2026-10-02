#pragma once

// Minimal EBML / Matroska reader for the tests: walks the element tree of a
// file written by mkv::Muxer, strictly (every size must be known and fit in
// its parent). Independent of the writer.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace ebml {

struct Element {
    uint32_t id = 0;
    size_t offset = 0;      // of the ID
    size_t dataOffset = 0;  // of the body
    uint64_t size = 0;      // of the body
    size_t end() const { return dataOffset + size_t(size); }
};

// Reads a VINT at `pos`; with `keepMarker` the marker bit stays (element IDs).
inline uint64_t readVint(const std::vector<uint8_t>& d, size_t& pos, bool keepMarker, bool* unknown = nullptr) {
    if (pos >= d.size()) throw std::runtime_error("EBML: truncated");
    const uint8_t first = d[pos];
    unsigned length = 1;
    while (length <= 8 && !(first & (0x80 >> (length - 1)))) ++length;
    if (length > 8) throw std::runtime_error("EBML: invalid VINT");
    if (pos + length > d.size()) throw std::runtime_error("EBML: truncated VINT");
    uint64_t value = keepMarker ? first : (first & (0xFF >> length));
    for (unsigned i = 1; i < length; ++i) value = value << 8 | d[pos + i];
    if (unknown) *unknown = !keepMarker && value == (uint64_t(1) << (7 * length)) - 1;
    pos += length;
    return value;
}

inline Element readElement(const std::vector<uint8_t>& d, size_t pos, size_t limit) {
    Element e;
    e.offset = pos;
    e.id = uint32_t(readVint(d, pos, true));
    bool unknown = false;
    e.size = readVint(d, pos, false, &unknown);
    if (unknown) throw std::runtime_error("EBML: element of unknown size");
    e.dataOffset = pos;
    if (e.end() > limit) throw std::runtime_error("EBML: element exceeds its parent");
    return e;
}

// The elements in [begin, end).
inline std::vector<Element> children(const std::vector<uint8_t>& d, size_t begin, size_t end) {
    std::vector<Element> v;
    for (size_t pos = begin; pos < end;) {
        v.push_back(readElement(d, pos, end));
        pos = v.back().end();
    }
    return v;
}
inline std::vector<Element> children(const std::vector<uint8_t>& d, const Element& parent) {
    return children(d, parent.dataOffset, parent.end());
}

inline std::vector<Element> all(const std::vector<uint8_t>& d, const Element& parent, uint32_t id) {
    std::vector<Element> v;
    for (const Element& e : children(d, parent))
        if (e.id == id) v.push_back(e);
    return v;
}

inline Element find(const std::vector<uint8_t>& d, const Element& parent, uint32_t id) {
    const std::vector<Element> v = all(d, parent, id);
    if (v.size() != 1) throw std::runtime_error("EBML: expected exactly one element " + std::to_string(id));
    return v[0];
}

inline bool has(const std::vector<uint8_t>& d, const Element& parent, uint32_t id) {
    return !all(d, parent, id).empty();
}

inline uint64_t uintValue(const std::vector<uint8_t>& d, const Element& e) {
    if (e.size > 8) throw std::runtime_error("EBML: uint too long");
    uint64_t v = 0;
    for (size_t i = 0; i < e.size; ++i) v = v << 8 | d[e.dataOffset + i];
    return v;
}

inline int64_t intValue(const std::vector<uint8_t>& d, const Element& e) {
    if (e.size == 0 || e.size > 8) throw std::runtime_error("EBML: bad int size");
    uint64_t v = (d[e.dataOffset] & 0x80) ? ~uint64_t(0) : 0;
    for (size_t i = 0; i < e.size; ++i) v = v << 8 | d[e.dataOffset + i];
    return int64_t(v);
}

inline double floatValue(const std::vector<uint8_t>& d, const Element& e) {
    if (e.size != 8) throw std::runtime_error("EBML: expected an 8-byte float");
    const uint64_t bits = uintValue(d, e);
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}

inline std::string stringValue(const std::vector<uint8_t>& d, const Element& e) {
    return std::string(d.begin() + std::ptrdiff_t(e.dataOffset), d.begin() + std::ptrdiff_t(e.end()));
}

inline std::vector<uint8_t> binaryValue(const std::vector<uint8_t>& d, const Element& e) {
    return std::vector<uint8_t>(d.begin() + std::ptrdiff_t(e.dataOffset), d.begin() + std::ptrdiff_t(e.end()));
}

}  // namespace ebml
