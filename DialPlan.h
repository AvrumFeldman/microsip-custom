// Copyright (C) 2026 AvrumFeldman contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace CustomDialPlan {
// Implements the documented MicroSIP dial-plan grammar. A configured plan
// blocks a number when none of its alternatives matches the entire number.
std::wstring Apply(const std::wstring& plan, const std::wstring& number);
bool ParseAnswerAfter(const std::wstring& header, int& seconds);
}
