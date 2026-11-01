#include "qoh99/CommandHistory.hpp"

namespace qoh99 {

static_assert(cmd_field::kDirRing + 4 * cmd_field::kDirDepth == cmd_field::kDirHold,
              "dir hold array must directly follow the dir ring");
static_assert(cmd_field::kBtnRing + 4 * cmd_field::kBtnDepth == cmd_field::kBtnHold,
              "btn hold array must directly follow the btn ring");
static_assert(cmd_field::kMinSize <= cmd_field::kSaveSize,
              "save extent must cover the proven-referenced extent");

}
