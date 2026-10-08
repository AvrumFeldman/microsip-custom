// Copyright (C) 2026 AvrumFeldman contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Independent implementation of the grammar documented at
// https://www.microsip.org/help (Dial Plan), without ATL regex sources.
#include "DialPlan.h"

#include <climits>
#include <regex>
#include <vector>

namespace {
void AppendLiteral(std::wstring& expression, wchar_t value)
{
    if (std::wstring(L"\\.^$|()[]{}*+?").find(value) != std::wstring::npos)
        expression += L'\\';
    expression += value;
}

bool Translate(const std::wstring& fragment, std::wstring& expression)
{
    bool repeatable = false;
    for (size_t i = 0; i < fragment.size(); ++i) {
        wchar_t value = fragment[i];
        if (value == L'x' || value == L'X') {
            expression += L'.';
        }
        else if (value == L'.') {
            if (!repeatable) return false;
            expression += L'*';
            repeatable = false;
            continue;
        }
        else if (value == L'[') {
            const size_t end = fragment.find(L']', i + 1);
            if (end == std::wstring::npos || end == i + 1) return false;
            expression += L'[';
            for (++i; i < end; ++i) {
                // Preserve digit ranges. Other set members are literal;
                // an x within a set does not mean "any character".
                const wchar_t member = fragment[i];
                if (member == L'\\' || member == L'^' || member == L'[')
                    expression += L'\\';
                expression += member;
            }
            expression += L']';
        }
        else if (value == L'<' || value == L'>' || value == L']') {
            return false;
        }
        else {
            AppendLiteral(expression, value);
        }
        repeatable = true;
    }
    return true;
}

bool MatchRule(const std::wstring& rule, const std::wstring& number,
               std::wstring& output)
{
    std::wstring expression;
    std::vector<std::wstring> replacements;
    size_t start = 0;
    while (start < rule.size()) {
        const size_t group = rule.find(L'<', start);
        if (!Translate(rule.substr(start, group == std::wstring::npos
            ? std::wstring::npos : group - start), expression)) return false;
        if (group == std::wstring::npos) break;
        const size_t end = rule.find(L'>', group + 1);
        if (end == std::wstring::npos) return false;
        const std::wstring content = rule.substr(group + 1, end - group - 1);
        const size_t colon = content.find(L':');
        if (colon == std::wstring::npos) {
            if (!Translate(content, expression)) return false;
        }
        else {
            expression += L'(';
            if (!Translate(content.substr(0, colon), expression)) return false;
            expression += L')';
            replacements.push_back(content.substr(colon + 1));
        }
        start = end + 1;
    }

    try {
        const std::wregex pattern(expression, std::regex_constants::ECMAScript);
        std::wsmatch match;
        if (!std::regex_match(number, match, pattern)) return false;
        output.clear();
        size_t copied = 0;
        for (size_t i = 0; i < replacements.size(); ++i) {
            const size_t position = static_cast<size_t>(match.position(i + 1));
            output.append(number, copied, position - copied);
            output += replacements[i];
            copied = position + static_cast<size_t>(match.length(i + 1));
        }
        output.append(number, copied, std::wstring::npos);
        return true;
    }
    catch (const std::regex_error&) {
        // An invalid alternative must not allow an otherwise blocked number.
        return false;
    }
}
}

namespace CustomDialPlan {
std::wstring Apply(const std::wstring& plan, const std::wstring& number)
{
    if (plan.empty()) return number;
    const size_t first = plan.find_first_not_of(L" ()\t\r\n");
    if (first == std::wstring::npos) return L"";
    const size_t last = plan.find_last_not_of(L" ()\t\r\n");
    const std::wstring rules = plan.substr(first, last - first + 1);
    size_t start = 0;
    while (start < rules.size()) {
        const size_t end = rules.find(L'|', start);
        const std::wstring rule = rules.substr(start, end == std::wstring::npos
            ? std::wstring::npos : end - start);
        std::wstring output;
        if (!rule.empty() && MatchRule(rule, number, output)) return output;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return L"";
}

bool ParseAnswerAfter(const std::wstring& header, int& seconds)
{
    const std::wstring key = L"answer-after=";
    size_t start = header.find(key);
    if (start == std::wstring::npos) return false;
    start += key.size();
    if (start == header.size() || header[start] < L'0' || header[start] > L'9')
        return false;
    int value = 0;
    for (size_t i = start; i < header.size() && header[i] >= L'0' && header[i] <= L'9'; ++i) {
        const int digit = header[i] - L'0';
        // The caller creates a Windows timer using seconds * 1000.
        if (value > (INT_MAX / 1000 - digit) / 10) return false;
        value = value * 10 + digit;
    }
    seconds = value;
    return true;
}
}
