// Modified 2026-10-08 for MicroSIP Custom: independent contact XML adapter interface.
// SPDX-License-Identifier: GPL-2.0-or-later
// Independent MSXML 6 adapter implementing only MicroSIP's contact XML API.
// Contains none of the original First Objective CMarkup implementation.
#pragma once
#include <afxwin.h>
#include <atlbase.h>
#include <msxml6.h>
#include <vector>

class CMarkup {
public:
    CMarkup();
    ~CMarkup();
    CMarkup(const CMarkup&) = delete;
    CMarkup& operator=(const CMarkup&) = delete;
    bool SetDoc(LPCTSTR text);
    CString GetDoc() const;
    bool AddElem(LPCTSTR name);
    bool AddAttrib(LPCTSTR name, LPCTSTR value);
    bool FindElem(LPCTSTR name);
    bool FindChildElem(LPCTSTR name);
    bool IntoElem();
    bool OutOfElem();
    bool FindAttrib(LPCTSTR name) const;
    CString GetAttrib(LPCTSTR name) const;
    CString GetChildData() const;
    void ResetChildPos() { position_.child.Release(); }
private:
    struct Position {
        CComPtr<IXMLDOMNode> parent;
        CComPtr<IXMLDOMNode> current;
        CComPtr<IXMLDOMNode> child;
    } position_;
    std::vector<Position> ancestors_;
    CComPtr<IXMLDOMDocument2> document_;
    bool uninitialize_ = false;
    static CComPtr<IXMLDOMNode> FindNext(IXMLDOMNode* parent, IXMLDOMNode* after, LPCTSTR name);
};
