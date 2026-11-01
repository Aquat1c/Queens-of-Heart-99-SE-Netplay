#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace qnetplay::test {

class PinnedImageBytes {
public:
    bool load(const std::string& path, std::string& error) {
        sections_.clear();
        image_.clear();
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) {
            error = "cannot open pinned image fixture: " + path;
            return false;
        }
        unsigned char chunk[64 * 1024];
        for (;;) {
            const std::size_t read = std::fread(chunk, 1, sizeof(chunk), file);
            if (read == 0) break;
            image_.insert(image_.end(), chunk, chunk + read);
        }
        std::fclose(file);
        return parse(error);
    }

    std::uint64_t size() const { return image_.size(); }

    bool readVa(std::uint32_t va, std::uint8_t* out, std::uint32_t count) const {
        if (out == nullptr || count == 0) return false;
        for (const Section& section : sections_) {
            const std::uint64_t span =
                section.virtualSize > section.rawSize ? section.virtualSize
                                                      : section.rawSize;
            if (va < section.va || va >= section.va + span) continue;
            const std::uint64_t start =
                static_cast<std::uint64_t>(section.rawOffset)
                + (va - section.va);
            if (start + count > image_.size()) return false;
            for (std::uint32_t i = 0; i < count; ++i)
                out[i] = image_[static_cast<std::size_t>(start + i)];
            return true;
        }
        return false;
    }

private:
    struct Section {
        std::uint32_t va = 0;
        std::uint32_t virtualSize = 0;
        std::uint32_t rawOffset = 0;
        std::uint32_t rawSize = 0;
    };

    std::uint32_t word(std::size_t offset) const {
        return static_cast<std::uint32_t>(image_[offset])
            | (static_cast<std::uint32_t>(image_[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(image_[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(image_[offset + 3]) << 24);
    }

    bool parse(std::string& error) {
        if (image_.size() < 0x40) {
            error = "pinned image fixture is too small for a DOS header";
            return false;
        }
        const std::uint32_t pe = word(0x3C);
        if (pe + 24 > image_.size() || image_[pe] != 'P' || image_[pe + 1] != 'E'
            || image_[pe + 2] != 0 || image_[pe + 3] != 0) {
            error = "pinned image fixture has no PE signature";
            return false;
        }
        const std::uint32_t sectionCount =
            static_cast<std::uint32_t>(image_[pe + 6])
            | (static_cast<std::uint32_t>(image_[pe + 7]) << 8);
        const std::uint32_t optionalSize =
            static_cast<std::uint32_t>(image_[pe + 20])
            | (static_cast<std::uint32_t>(image_[pe + 21]) << 8);
        imageBase_ = word(pe + 24 + 28);
        std::size_t offset = pe + 24 + optionalSize;
        for (std::uint32_t i = 0; i < sectionCount; ++i) {
            if (offset + 40 > image_.size()) {
                error = "pinned image fixture has a truncated section table";
                return false;
            }
            Section section;
            section.virtualSize = word(offset + 8);
            section.va = imageBase_ + word(offset + 12);
            section.rawSize = word(offset + 16);
            section.rawOffset = word(offset + 20);
            sections_.push_back(section);
            offset += 40;
        }
        if (sections_.empty()) {
            error = "pinned image fixture declares no sections";
            return false;
        }
        error.clear();
        return true;
    }

    std::vector<unsigned char> image_;
    std::vector<Section> sections_;
    std::uint32_t imageBase_ = 0;
};

}
