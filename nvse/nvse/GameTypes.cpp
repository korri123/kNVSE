#include "GameTypes.h"
#include "GameAPI.h"

#include <string>
#include <algorithm>

/*** String ***/

BSString::BSString() : m_data(NULL), m_dataLen(0), m_bufLen(0) {}

BSString::BSString(char* str)
{
	this->m_data = str;
	this->m_dataLen = strlen(str);
	this->m_bufLen = this->m_dataLen + 1;
}

BSString::~BSString()
{
	if (m_data)
	{
		FormHeap_Free(m_data);
		m_data = nullptr;
	}
	m_bufLen = m_dataLen = 0;
}

BSString::BSString(BSString&& other) noexcept
{
	m_data = other.m_data;
	m_dataLen = other.m_dataLen;
	m_bufLen = other.m_bufLen;
	other.m_data = nullptr;
	other.m_dataLen = 0;
	other.m_bufLen = 0;
}

BSString& BSString::operator=(BSString&& other) noexcept
{
	if (this != &other)
	{
		if (m_data)
		{
			FormHeap_Free(m_data);
			m_data = nullptr;
		}
		m_bufLen = m_dataLen = 0;
		m_data = other.m_data;
		m_dataLen = other.m_dataLen;
		m_bufLen = other.m_bufLen;
		other.m_data = nullptr;
		other.m_dataLen = 0;
		other.m_bufLen = 0;
	}
	return *this;
}

bool BSString::Set(const char * src)
{
	if (!src) {
		FormHeap_Free(m_data);
		m_data = 0;
		m_bufLen = 0;
		m_dataLen = 0;
		return true;
	}
	
	UInt32	srcLen = strlen(src);

	// realloc if needed
	if(srcLen > m_bufLen)
	{
		FormHeap_Free(m_data);
		m_data = (char *)FormHeap_Allocate(srcLen + 1);
		m_bufLen = m_data ? srcLen : 0;
	}

	if(m_data)
	{
		strcpy_s(m_data, m_bufLen + 1, src);
		m_dataLen = srcLen;
	}
	else
	{
		m_dataLen = 0;
	}

	return m_data != NULL;
}

bool BSString::Includes(const char *toFind) const
{
	if (!m_data || !toFind)		//passing null ptr to std::string c'tor = CRASH
		return false;
	std::string curr(m_data, m_dataLen);
	std::string str2(toFind);
	std::string::iterator currEnd = curr.end();
	return (std::search(curr.begin(), currEnd, str2.begin(), str2.end(), ci_equal) != currEnd);
}

bool BSString::Replace(const char *_toReplace, const char *_replaceWith)
{
	if (!m_data || !_toReplace)
		return false;

	std::string curr(m_data, m_dataLen);
	std::string toReplace(_toReplace);

	std::string::iterator currBegin = curr.begin();
	std::string::iterator currEnd = curr.end();
	std::string::iterator replaceIt = std::search(currBegin, currEnd, toReplace.begin(), toReplace.end(), ci_equal);
	if (replaceIt != currEnd) {
		std::string replaceWith(_replaceWith);
		// we found the substring, now we need to do the modification
		std::string::size_type replacePos = distance(currBegin, replaceIt);
		curr.replace(replacePos, toReplace.size(), replaceWith);
		Set(curr.c_str());
		return true;
	}
	return false;
}

bool BSString::Append(const char* toAppend)
{
	std::string curr("");
	if (m_data)
		curr = std::string(m_data, m_dataLen);

	curr += toAppend;
	Set(curr.c_str());
	return true;
}

double BSString::Compare(const BSString& compareTo, bool caseSensitive)
{
	if (!m_data)
		return -2;		//signal value if comparison could not be made

	std::string first(m_data, m_dataLen);
	std::string second(compareTo.m_data, compareTo.m_dataLen);

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

const char* BSString::CStr(void) const
{

	return m_data ? m_data : "";
}
