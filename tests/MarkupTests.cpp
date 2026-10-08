// SPDX-License-Identifier: GPL-2.0-or-later
#include "../lib/Markup.h"
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int wmain()
{
    try {
        CMarkup writer;
        Check(writer.AddElem(L"contacts") && writer.IntoElem(), "create root");
        const CString name(L"Music & <friends> \"' \x05e9\x05dc\x05d5\x05dd");
        for (int i = 0; i < 2; ++i) {
            Check(writer.AddElem(L"contact"), "add contact");
            Check(writer.AddAttrib(L"name", name), "write unicode attribute");
            Check(writer.AddAttrib(L"number", i ? L"+123" : L"001"), "write number");
            Check(writer.AddAttrib(L"empty", L""), "write empty attribute");
        }
        CMarkup reader;
        Check(reader.SetDoc(L"<?xml version=\"1.0\"?>\r\n" + writer.GetDoc()), "parse export");
        Check(reader.FindElem(L"contacts"), "find root");
        int count = 0;
        while (reader.FindChildElem(L"contact")) {
            Check(reader.IntoElem(), "enter contact");
            Check(reader.GetAttrib(L"name") == name, "unicode and XML escaping round trip");
            Check(reader.GetAttrib(L"number") == (count ? L"+123" : L"001"), "ordered child iteration");
            Check(reader.FindAttrib(L"empty") && !reader.FindAttrib(L"absent"), "missing vs empty attribute");
            Check(reader.OutOfElem(), "return to root");
            ++count;
        }
        Check(count == 2, "contact count");
        Check(reader.SetDoc(L"<contacts refresh=\"60\" silent=\"1\"><!--x--><skip/><contact name=\"A\"/><contact name=\"B\"/></contacts>"), "directory parse");
        Check(reader.FindElem(L"contacts") && reader.GetAttrib(L"refresh") == L"60", "directory root attributes");
        Check(reader.FindChildElem(L"contact") && reader.IntoElem() && reader.GetAttrib(L"name") == L"A", "skip unrelated nodes");
        Check(reader.OutOfElem() && reader.FindChildElem(L"contact") && reader.IntoElem() && reader.GetAttrib(L"name") == L"B", "resume child iteration");
        Check(reader.SetDoc(L"<IPPhoneDirectory><DirectoryEntry><Name>A &amp; B</Name><Telephone>101</Telephone><Telephone>102</Telephone></DirectoryEntry></IPPhoneDirectory>"), "IP phone directory parse");
        Check(!reader.FindElem(L"contacts") && !reader.FindElem(L"YealinkIPPhoneBook"), "directory format alternatives");
        Check(reader.FindChildElem(L"DirectoryEntry") && reader.IntoElem(), "IP phone entry and implicit root");
        Check(reader.FindChildElem(L"Telephone") && reader.GetChildData() == L"101", "IP phone primary");
        Check(reader.FindChildElem(L"Telephone") && reader.GetChildData() == L"102", "IP phone secondary");
        reader.ResetChildPos();
        Check(reader.FindChildElem(L"Name") && reader.GetChildData() == L"A & B", "reset child and decode text");
        Check(!reader.SetDoc(L"<contacts><broken></contacts>"), "reject malformed document");
        Check(!reader.SetDoc(L"<!DOCTYPE contacts [<!ENTITY x SYSTEM 'file:///no-such-file'>]><contacts>&x;</contacts>"), "reject DTD and external entities");
        std::cout << "PASS: contact XML round trip, Unicode, directory iteration, missing attributes, malformed/DTD rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
