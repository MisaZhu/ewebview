#pragma once

#include <string.h>
#include <ctype.h>
#if !defined( WIN32 ) && !defined( WINCE )
#include <strings.h>
#endif

#include <stdint.h>
#include <time.h>
static inline uint64_t sys_tic_ms(uint32_t)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

namespace litehtml
{
#if !defined(WIN32) && !defined(WINCE)
inline int litehtml_strncasecmp(const char* lhs, const char* rhs, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		unsigned char lc = (unsigned char) tolower((unsigned char) lhs[i]);
		unsigned char rc = (unsigned char) tolower((unsigned char) rhs[i]);
		if (lc != rc || lc == '\0' || rc == '\0') return (int) lc - (int) rc;
	}
	return 0;
}

inline int litehtml_strcasecmp(const char* lhs, const char* rhs)
{
	while (*lhs || *rhs) {
		unsigned char lc = (unsigned char) tolower((unsigned char) *lhs++);
		unsigned char rc = (unsigned char) tolower((unsigned char) *rhs++);
		if (lc != rc) return (int) lc - (int) rc;
	}
	return 0;
}
#endif

#if defined( WIN32 ) || defined( WINCE )

#ifndef LITEHTML_UTF8

	typedef std::wstring		tstring;
	typedef wchar_t				tchar_t;
	typedef std::wstringstream	tstringstream;

	#define _t(quote)			L##quote

	#define t_strlen			wcslen
	#define t_strcmp			wcscmp
	#define t_strncmp			wcsncmp
	#define t_strcasecmp		_wcsicmp
	#define t_strncasecmp		_wcsnicmp
	#define t_strtol			wcstol
	#define t_atoi				_wtoi
	#define t_strtod			wcstod
	#define t_itoa(value, buffer, size, radix)	_itow_s(value, buffer, size, radix)
	#define t_strstr			wcsstr
	#define t_tolower			towlower
	#define t_isdigit			iswdigit

#else

	typedef std::string			tstring;
	typedef char				tchar_t;
	typedef std::stringstream	tstringstream;

	#define _t(quote)			quote

	#define t_strlen			strlen
	#define t_strcmp			strcmp
	#define t_strncmp			strncmp
	#define t_strcasecmp		_stricmp
	#define t_strncasecmp		_strnicmp
	#define t_strtol			strtol
	#define t_atoi				atoi
	#define t_strtod			strtod
	#define t_itoa(value, buffer, size, radix)	_itoa_s(value, buffer, size, radix)
	#define t_strstr			strstr
	#define t_tolower			tolower
	#define t_isdigit			isdigit

#endif

	#ifdef _WIN64
		typedef unsigned __int64 uint_ptr;
	#else
		typedef unsigned int	uint_ptr;
	#endif

#else
	#define LITEHTML_UTF8

	typedef std::string			tstring;
	typedef char				tchar_t;
	typedef void*				uint_ptr;
	typedef std::stringstream	tstringstream;

	#define _t(quote)			quote

	#define t_strlen			strlen
	#define t_strcmp			strcmp
	#define t_strncmp			strncmp

	// CSS keyword / colour-name matching is case-insensitive per spec;
	// a plain strcmp here silently dropped every lower-case named color.
	#define t_strcasecmp		::litehtml::litehtml_strcasecmp
	#define t_strncasecmp		::litehtml::litehtml_strncasecmp
	#define t_itoa(value, buffer, size, radix)	snprintf(buffer, size, "%d", value)

	#define t_strtol			strtol
	#define t_atoi				atoi
	#define t_strtod			strtod
	#define t_strstr			strstr
	#define t_tolower			tolower
	#define t_isdigit			isdigit

#endif
}
