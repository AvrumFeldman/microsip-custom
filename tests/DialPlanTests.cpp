// SPDX-License-Identifier: GPL-2.0-or-later
#include "../DialPlan.h"
#include <iostream>
#include <cstdlib>

static void Expect(const wchar_t* plan, const wchar_t* number, const wchar_t* expected)
{
    const std::wstring actual = CustomDialPlan::Apply(plan, number);
    if (actual != expected) {
        std::wcerr << L"FAIL plan=" << plan << L" number=" << number
                   << L" expected=" << expected << L" actual=" << actual << L'\n';
        std::exit(1);
    }
}

int main()
{
    Expect(L"<8:1555>xxxxxxx", L"81234567", L"15551234567");
    Expect(L"<:1>xxxxxxxxxx", L"1234567890", L"11234567890");
    Expect(L"<+:00>x.|x.", L"+15551234", L"0015551234");
    Expect(L"<+:00>x.|x.", L"15551234", L"15551234");
    Expect(L"(911|[2-9]xxxxxx)", L"911", L"911");
    Expect(L"(911|[2-9]xxxxxx)", L"1123456", L"");
    Expect(L"[16-9*]xx", L"*98", L"*98");
    Expect(L"[16-9*]xx", L"698", L"698");
    Expect(L"[16-9*]xx", L"298", L"");
    Expect(L"01.", L"0", L"0");
    Expect(L"01.", L"01111", L"01111");
    Expect(L"01.", L"010", L"");
    Expect(L"<9:>xxxx<:99>", L"91234", L"123499");
    Expect(L"<1:2>xxx<4:5>", L"11234", L"21235");
    Expect(L"<x.:123>", L"abcd", L"123");
    Expect(L"*8x.", L"*81234", L"*81234");
    Expect(L"**x.", L"**1234", L"**1234");
    Expect(L"<123>XX", L"123ab", L"123ab");
    Expect(L"x.", L"sip:user@example.test", L"sip:user@example.test");
    Expect(L"", L"555", L"555");
    Expect(L"[9-2]x|911", L"911", L"911");
    Expect(L"<9:123", L"9", L"");
    Expect(L".x", L"1", L"");
    Expect(L"[2-9", L"2", L"");
    Expect(L"xxx", L"1234", L"");
    int seconds = -1;
    if (!CustomDialPlan::ParseAnswerAfter(L"<sip:test>;answer-after=0", seconds) || seconds != 0 ||
        !CustomDialPlan::ParseAnswerAfter(L"answer-after=15;foo=bar", seconds) || seconds != 15 ||
        CustomDialPlan::ParseAnswerAfter(L"answer-after=-1", seconds) ||
        CustomDialPlan::ParseAnswerAfter(L"answer-after=", seconds) ||
        CustomDialPlan::ParseAnswerAfter(L"answer-after=999999999999999", seconds) ||
        CustomDialPlan::ParseAnswerAfter(L"answer-after=2147484", seconds)) {
        std::cerr << "FAIL answer-after parsing\n";
        return 1;
    }
    std::cout << "PASS: documented dial-plan rewrites, alternatives, ranges, repetition, literal pickup codes, blocking, invalid patterns and answer-after parsing\n";
}
