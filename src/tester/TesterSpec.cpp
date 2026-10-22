#include "TesterSpec.h"

namespace Lx::Tester 
{
constexpr std::array<Lx::Tester::TestModeDef, 4> BuiltinModeTable = { {
    { TestMode::ModeA, "ModeA", TestItemMask::RfPacket | TestItemMask::FreqOffset },
    { TestMode::ModeB, "ModeB", TestItemMask::All },
    { TestMode::ModeC, "ModeC", TestItemMask::All },
    { TestMode::ModeD, "ModeD", TestItemMask::All },
} };

const Lx::Tester::TestModeDef* FindMode(TestMode mode)
{
    for (const auto& it : BuiltinModeTable)
    { 
        if (it.mode == mode) return &it;
    }
    return nullptr;
}

std::vector<Lx::Tester::TestModeDef> GetAllModes()
{
    return { BuiltinModeTable.begin(), BuiltinModeTable.end() };
}

}