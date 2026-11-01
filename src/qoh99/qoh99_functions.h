#pragma once

#include "../RuntimeProfileRegistry.h"
#include "qoh99/generated/NativeSymbolCatalog.hpp"

#include <cstdint>

namespace qoh99::fn {

using ReadLogicalFn = int (__fastcall*)(int player);
using ApplyLogicalFn = int (__fastcall*)(int player, int mask);
using RecordFn = int (__thiscall*)(int fighter);
using SystemKeysFn = int (__cdecl*)(void);
using KeyTestFn = char (__fastcall*)(unsigned __int8 scancode);
using KeyInjectFn = int (__fastcall*)(unsigned __int8 scancode, int value);
using NetSendFn = int (__fastcall*)(int chan, int type, unsigned int value);
using RngDrawFn = unsigned (__cdecl*)(void);
using RngSeedFn = void (__thiscall*)(void* seed);
using RngGetFn = int (__cdecl*)(void);
using VoidFn = int (__cdecl*)(void);
using PairFn = int (__fastcall*)(int fA, int fB);

inline std::uintptr_t runtimeAddress(generated_model::FunctionId id) {
    return runtime_profile::selectedFunctionOrZero(id);
}

inline ReadLogicalFn readLogical() {
    return reinterpret_cast<ReadLogicalFn>(
        runtimeAddress(generated_model::FunctionId::ReadLogicalInput));
}

inline ApplyLogicalFn applyLogical() {
    return reinterpret_cast<ApplyLogicalFn>(
        runtimeAddress(generated_model::FunctionId::ApplyLogicalInput));
}

inline RecordFn recordInput() {
    return reinterpret_cast<RecordFn>(
        runtimeAddress(generated_model::FunctionId::FighterRecordInput));
}

inline NetSendFn netSend() {
    return reinterpret_cast<NetSendFn>(
        runtimeAddress(generated_model::FunctionId::NetSendMsg));
}

inline RngDrawFn rngDraw() {
    return reinterpret_cast<RngDrawFn>(
        runtimeAddress(generated_model::FunctionId::RngDraw));
}

inline RngGetFn rngGet() {
    return reinterpret_cast<RngGetFn>(
        runtimeAddress(generated_model::FunctionId::RngGetGameplaySeed));
}

inline void rngSeedBoth(std::uint32_t seed) {
    const auto target = reinterpret_cast<RngSeedFn>(
        runtimeAddress(generated_model::FunctionId::RngSrand));
    if (target) target(reinterpret_cast<void*>(seed));
}

}
