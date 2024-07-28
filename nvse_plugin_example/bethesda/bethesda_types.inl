#pragma once
#include "bethesda_types.h"
#include "utility.h"

#define BSNew New
#define BSFree Delete

// 0x4037F0
template<typename T>
bool BSStringT<T>::Set(const T* apText, UInt32 auiLength) {
	UInt32 uiLength = 0;
	if (apText) {
		uiLength = strlen(apText);
	}

	UInt32 uiAllocLength = max(uiLength, auiLength);
	if (uiAllocLength > GetMaxLength()) {
		T* pOrgStr = pString;
		pString = BSNew<T>(uiAllocLength + sizeof(T));
		if (apText)
			memcpy(pString, apText, uiLength + sizeof(T));
		else
			pString[0] = 0;

		if (pOrgStr)
			BSFree(pOrgStr);
		
		SetMaxLength(uiAllocLength);
	}
	else if (uiAllocLength) {
		if (apText)
			memcpy(pString, apText, uiLength + sizeof(T));
		else
			pString[0] = 0;
	}
	else {
		if (pString)
			BSFree(pString);

		pString = nullptr;
		SetMaxLength(0);
	}
	SetLength(uiLength);
	return uiLength != 0;
}

// 0x404820
template<typename T>
BSStringT<T>* BSStringT<T>::operator+=(const T* apText) {
	if (!apText)
		return this;

	if (pString) {
		UInt32 uiAppendLength = strlen(apText);
		UInt32 uiNewLength = GetLength() + uiAppendLength;
		if (uiNewLength > GetMaxLength()) {
			Set(pString, uiNewLength);
		}
		UInt32 uiLength = GetLength();
		memcpy(&pString[uiLength], apText, uiAppendLength + sizeof(T));
		SetLength(uiNewLength);
	}
	else {
		Set(apText);
	}
	return this;
}

// 0x406F60
template<typename T>
void BSStringT<T>::Format(const T* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	ApplyFormat(fmt, args);
	va_end(args);
}

// 0x406F90
template<typename T>
void BSStringT<T>::ApplyFormat(const T* fmt, va_list args) {
	char cBuffer[1024] = {};
	sprintf_s(cBuffer, fmt, args);
	Set(cBuffer);
}

template<typename T>
bool BSStringT<T>::Includes(const char* toFind) const {
	if (!pString || !toFind)
		return false;

	return strstr(pString, toFind) != nullptr;
}

template<typename T>
bool BSStringT<T>::Replace(const char* _toReplace, const char* _replaceWith) {
	if (!pString || !_toReplace || !_replaceWith)
		return false;

	std::string toReplace(_toReplace);
	std::string replaceWith(_replaceWith);
	std::string str(pString, sLen);

	size_t pos = str.find(toReplace);
	if (pos == std::string::npos)
		return false;

	str.replace(pos, toReplace.length(), replaceWith);
	Set(str.c_str());
	return true;
}

template<typename T>
double BSStringT<T>::Compare(const BSStringT& compareTo, bool caseSensitive) const {
	if (!pString)
		return -2;		//signal value if comparison could not be made

	std::string first(pString, sLen);
	std::string second(compareTo.pString, compareTo.sLen);

	if (!caseSensitive)
	{
		std::transform(first.begin(), first.end(), first.begin(), tolower);
		std::transform(second.begin(), second.end(), second.begin(), tolower);
	}

	double comp = 0;
	if (first < second)
		comp = -1;
	else if (first > second)
		comp = 1;

	return comp;
}

template<typename T>
BSStringT<T>::~BSStringT() {
	if (pString) {
		BSFree(pString);
		pString = nullptr;
	}
	sMaxLen = sLen = 0;
}

// 0x4048E0
template<typename T>
UInt32 BSStringT<T>::GetLength() const {
	return sLen == 0xFFFF ? strlen(pString) : sLen;
}

template<typename T>
void BSStringT<T>::SetLength(UInt32 auiLen) {
	sLen = auiLen > 0xFFFF ? 0xFFFF : auiLen;
}

template<typename T>
const T* BSStringT<T>::CStr() const {
	return pString;
}