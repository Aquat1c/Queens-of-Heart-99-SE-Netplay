#include "input/KeyboardScanCodec.h"

namespace qinput {

DikResolution dikFromRawScan(const RawKeyboardScan& scan) {
    DikResolution result{};
    if (scan.makeCode == 0 || scan.makeCode > 0xFF) return result;

    if (scan.e1) {
        // The only supported E1 sequence is Pause (E1 1D 45): Raw Input reports makeCode 0x1D with E1 set, and DIK_PAUSE is 0xC5.
        if (scan.makeCode == 0x1D) {
            result.valid = true;
            result.dik = 0xC5;
        }
        return result;
    }

    if (scan.e0) {
        if (scan.makeCode == 0x2A) return result;
        switch (scan.makeCode) {
            case 0x1C: result.dik = 0x9C; break;
            case 0x1D: result.dik = 0x9D; break;
            case 0x35: result.dik = 0xB5; break;
            case 0x37: result.dik = 0xB7; break;
            case 0x38: result.dik = 0xB8; break;
            case 0x45: result.dik = 0xC5; break;
            case 0x47: result.dik = 0xC7; break;
            case 0x48: result.dik = 0xC8; break;
            case 0x49: result.dik = 0xC9; break;
            case 0x4B: result.dik = 0xCB; break;
            case 0x4D: result.dik = 0xCD; break;
            case 0x4F: result.dik = 0xCF; break;
            case 0x50: result.dik = 0xD0; break;
            case 0x51: result.dik = 0xD1; break;
            case 0x52: result.dik = 0xD2; break;
            case 0x53: result.dik = 0xD3; break;
            case 0x5B: result.dik = 0xDB; break;
            case 0x5C: result.dik = 0xDC; break;
            case 0x5D: result.dik = 0xDD; break;
            default:
                return result;
        }
        result.valid = true;
        return result;
    }

    if (scan.makeCode <= 0x53 || scan.makeCode == 0x56
        || scan.makeCode == 0x57 || scan.makeCode == 0x58) {
        result.valid = true;
        result.dik = static_cast<uint8_t>(scan.makeCode);
        return result;
    }
    return result;
}

bool dikIsExtended(uint8_t dik) { return dik >= 0x80; }

const char* dikDisplayLabel(uint8_t dik) {
    switch (dik) {
        case 0x01: return "Esc";
        case 0x02: return "1"; case 0x03: return "2";
        case 0x04: return "3"; case 0x05: return "4";
        case 0x06: return "5"; case 0x07: return "6";
        case 0x08: return "7"; case 0x09: return "8";
        case 0x0A: return "9"; case 0x0B: return "0";
        case 0x0C: return "-"; case 0x0D: return "=";
        case 0x0E: return "Backspace"; case 0x0F: return "Tab";
        case 0x10: return "Q"; case 0x11: return "W";
        case 0x12: return "E"; case 0x13: return "R";
        case 0x14: return "T"; case 0x15: return "Y";
        case 0x16: return "U"; case 0x17: return "I";
        case 0x18: return "O"; case 0x19: return "P";
        case 0x1A: return "["; case 0x1B: return "]";
        case 0x1C: return "Enter"; case 0x1D: return "LCtrl";
        case 0x1E: return "A"; case 0x1F: return "S";
        case 0x20: return "D"; case 0x21: return "F";
        case 0x22: return "G"; case 0x23: return "H";
        case 0x24: return "J"; case 0x25: return "K";
        case 0x26: return "L"; case 0x27: return ";";
        case 0x28: return "'"; case 0x29: return "`";
        case 0x2A: return "LShift"; case 0x2B: return "\\";
        case 0x2C: return "Z"; case 0x2D: return "X";
        case 0x2E: return "C"; case 0x2F: return "V";
        case 0x30: return "B"; case 0x31: return "N";
        case 0x32: return "M"; case 0x33: return ",";
        case 0x34: return "."; case 0x35: return "/";
        case 0x36: return "RShift"; case 0x37: return "Numpad*";
        case 0x38: return "LAlt"; case 0x39: return "Space";
        case 0x3A: return "CapsLock";
        case 0x3B: return "F1"; case 0x3C: return "F2";
        case 0x3D: return "F3"; case 0x3E: return "F4";
        case 0x3F: return "F5"; case 0x40: return "F6";
        case 0x41: return "F7"; case 0x42: return "F8";
        case 0x43: return "F9"; case 0x44: return "F10";
        case 0x45: return "NumLock"; case 0x46: return "ScrollLock";
        case 0x47: return "Numpad7"; case 0x48: return "Numpad8";
        case 0x49: return "Numpad9"; case 0x4A: return "Numpad-";
        case 0x4B: return "Numpad4"; case 0x4C: return "Numpad5";
        case 0x4D: return "Numpad6"; case 0x4E: return "Numpad+";
        case 0x4F: return "Numpad1"; case 0x50: return "Numpad2";
        case 0x51: return "Numpad3"; case 0x52: return "Numpad0";
        case 0x53: return "Numpad."; case 0x56: return "OEM102";
        case 0x57: return "F11"; case 0x58: return "F12";
        case 0x9C: return "NumpadEnter"; case 0x9D: return "RCtrl";
        case 0xB5: return "Numpad/"; case 0xB7: return "PrintScreen";
        case 0xB8: return "RAlt"; case 0xC5: return "Pause";
        case 0xC7: return "Home"; case 0xC8: return "Up";
        case 0xC9: return "PageUp"; case 0xCB: return "Left";
        case 0xCD: return "Right"; case 0xCF: return "End";
        case 0xD0: return "Down"; case 0xD1: return "PageDown";
        case 0xD2: return "Insert"; case 0xD3: return "Delete";
        case 0xDB: return "LWin"; case 0xDC: return "RWin";
        case 0xDD: return "Apps";
        default: return "?";
    }
}

}
