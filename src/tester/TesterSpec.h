#pragma once
#include "TesterTypes.h"

namespace Lx::Tester 
{

// 查询接口
const Lx::Tester::TestModeDef* FindMode(TestMode mode);
std::vector<Lx::Tester::TestModeDef> GetAllModes();

}