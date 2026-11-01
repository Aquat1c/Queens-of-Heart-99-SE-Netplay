#include "qoh99/VanillaNetProtocol.hpp"

namespace qoh99 { namespace vnet {

static_assert(qohPackGameplayInputForNet(0, 0, 0) == 0, "zero packs to zero");
static_assert(qohPackGameplayInputForNet(0xF, 0, 0) == 0xF, "buttons b0-3");
static_assert(qohPackGameplayInputForNet(0xF0000, 0, 0) == 0xF00, "dirs b8-11");
static_assert(qohPackGameplayInputForNet(0, 0x300, 0) == 0x3000, "sys b12-13");
static_assert(qohPackGameplayInputForNet(0, 0, 7) == 0x70000, "seq b16+");
static_assert(qohUnpackGameplayInputFromNet(0xF0F) == 0xF000F, "unpack");

}}
