#pragma once

#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <iosfwd>

#include "d3dx12.h"

using namespace Microsoft::WRL;

class HrException : public std::runtime_error
{
	inline std::string HrToString(HRESULT hr)
	{
		char s_str[64] = {};
		sprintf_s(s_str, "HRESULT of 0x%08X", static_cast<UINT>(hr));
		return std::string(s_str);
	}
public:
	HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), m_hr(hr) {}
	HRESULT Error() const { return m_hr; }
private:
	const HRESULT m_hr;
};

inline void ThrowIfFailed(HRESULT hr, const wchar_t *msg)
{
	if (FAILED(hr))
	{
		OutputDebugString(msg);
		throw HrException(hr);
	}
}


