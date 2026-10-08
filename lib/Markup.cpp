// Modified 2026-10-08 for MicroSIP Custom: independent contact XML adapter using Windows MSXML6.
// SPDX-License-Identifier: GPL-2.0-or-later
// Contact XML parsing/serialization using the Windows MSXML system component.
#include "Markup.h"
#pragma comment(lib, "msxml6.lib")

CMarkup::CMarkup()
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    uninitialize_ = SUCCEEDED(result);
    if (SUCCEEDED(result) || result == RPC_E_CHANGED_MODE) SetDoc(nullptr);
}

CMarkup::~CMarkup()
{
    ancestors_.clear();
    position_ = Position();
    document_.Release();
    if (uninitialize_) CoUninitialize();
}

bool CMarkup::SetDoc(LPCTSTR text)
{
    ancestors_.clear();
    position_ = Position();
    document_.Release();
    if (FAILED(document_.CoCreateInstance(__uuidof(DOMDocument60)))) return false;
    document_->put_async(VARIANT_FALSE);
    document_->put_validateOnParse(VARIANT_FALSE);
    document_->put_resolveExternals(VARIANT_FALSE);
    document_->setProperty(CComBSTR(L"ProhibitDTD"), CComVariant(true));
    if (text && *text) {
        VARIANT_BOOL loaded = VARIANT_FALSE;
        if (FAILED(document_->loadXML(CComBSTR(text), &loaded)) || loaded != VARIANT_TRUE) return false;
    }
    position_.parent = document_;
    return true;
}

CString CMarkup::GetDoc() const
{
    CComBSTR xml;
    if (document_) document_->get_xml(&xml);
    return xml ? CString(xml) : CString();
}

bool CMarkup::AddElem(LPCTSTR name)
{
    if (!document_ || !position_.parent) return false;
    CComPtr<IXMLDOMElement> element;
    CComPtr<IXMLDOMNode> added;
    if (FAILED(document_->createElement(CComBSTR(name), &element)) ||
        FAILED(position_.parent->appendChild(element, &added))) return false;
    position_.current = added;
    position_.child.Release();
    return true;
}

bool CMarkup::AddAttrib(LPCTSTR name, LPCTSTR value)
{
    CComQIPtr<IXMLDOMElement> element(position_.current);
    return element && SUCCEEDED(element->setAttribute(CComBSTR(name), CComVariant(value ? value : L"")));
}

CComPtr<IXMLDOMNode> CMarkup::FindNext(IXMLDOMNode* parent, IXMLDOMNode* after, LPCTSTR name)
{
    CComPtr<IXMLDOMNode> node;
    if (after) after->get_nextSibling(&node);
    else if (parent) parent->get_firstChild(&node);
    while (node) {
        DOMNodeType type = NODE_INVALID;
        CComBSTR tag;
        node->get_nodeType(&type);
        node->get_nodeName(&tag);
        if (type == NODE_ELEMENT && tag && (!name || !*name || CString(tag) == name)) return node;
        CComPtr<IXMLDOMNode> next;
        node->get_nextSibling(&next);
        node = next;
    }
    return nullptr;
}

bool CMarkup::FindElem(LPCTSTR name)
{
    CComPtr<IXMLDOMNode> found = FindNext(position_.parent, position_.current, name);
    if (!found) return false;
    position_.current = found;
    position_.child.Release();
    return true;
}

bool CMarkup::FindChildElem(LPCTSTR name)
{
    if (!position_.current) position_.current = FindNext(position_.parent, nullptr, nullptr);
    CComPtr<IXMLDOMNode> found = FindNext(position_.current, position_.child, name);
    if (!found) return false;
    position_.child = found;
    return true;
}

bool CMarkup::IntoElem()
{
    if (!position_.current) return false;
    ancestors_.push_back(position_);
    position_.parent = position_.current;
    position_.current = position_.child;
    position_.child.Release();
    return true;
}

bool CMarkup::OutOfElem()
{
    if (ancestors_.empty()) return false;
    position_ = ancestors_.back();
    ancestors_.pop_back();
    return true;
}

bool CMarkup::FindAttrib(LPCTSTR name) const
{
    CComQIPtr<IXMLDOMElement> element(position_.current);
    CComPtr<IXMLDOMAttribute> attribute;
    return element && SUCCEEDED(element->getAttributeNode(CComBSTR(name), &attribute)) && attribute;
}

CString CMarkup::GetAttrib(LPCTSTR name) const
{
    CComQIPtr<IXMLDOMElement> element(position_.current);
    CComVariant value;
    if (!element || FAILED(element->getAttribute(CComBSTR(name), &value)) || value.vt != VT_BSTR) return CString();
    return CString(value.bstrVal);
}

CString CMarkup::GetChildData() const
{
    CComBSTR text;
    if (position_.child) position_.child->get_text(&text);
    return text ? CString(text) : CString();
}
