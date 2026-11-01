#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace qplat {

struct PeImageInfo {
    uintptr_t base = 0;
    uint32_t sizeOfImage = 0;
    uint16_t machine = 0;
    uint32_t timestamp = 0;
    uint32_t entryPointRva = 0;
    bool isDll = false;
    uint16_t subsystem = 0;
    uint32_t checkSum = 0;
};

bool isReadableRange(const void* p, size_t len);

bool inspectLoadedImage(void* moduleBase, PeImageInfo& out, std::string& error);

bool validateHostImage(void* moduleBase, PeImageInfo& out, std::string& error);

struct IatSlot {
    void** slot = nullptr;
    void* original = nullptr;
};

bool findImportSlot(void* moduleBase, const char* dllName, const char* funcName,
                    IatSlot& out, std::string& error);

bool patchImportSlot(IatSlot& slot, void* replacement, std::string& error);
bool restoreImportSlot(const IatSlot& slot, std::string& error);

bool remoteModuleBase(void* process, const wchar_t* moduleBaseName,
                      uintptr_t& base, std::string& error);

// A CREATE_SUSPENDED process has an empty PEB module list.
bool remoteModuleBaseOrValidatedCandidate(void* process,
                                          const wchar_t* moduleBaseName,
                                          uintptr_t candidateBase,
                                          uint32_t expectedSizeOfImage,
                                          const char* requiredExport,
                                          uintptr_t& base,
                                          bool& usedCandidate,
                                          std::string& error);
bool remoteValidateModule(void* process, uintptr_t base, PeImageInfo& out,
                          std::string& error);

bool inspectFileImage(const uint8_t* bytes, size_t len, PeImageInfo& out,
                      std::string& error);

}
