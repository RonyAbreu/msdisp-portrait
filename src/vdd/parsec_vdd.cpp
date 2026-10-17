// STUB — track B replaces this file.
#include "msdisp/vdd.h"
namespace msdisp {
std::unique_ptr<IVirtualDisplay> createParsecVdd() { return nullptr; }
bool parsecVddSetCustomMode(int, int, int, int) { return false; }
bool setDisplayMode(const std::wstring&, int, int, int) { return false; }
}  // namespace msdisp
