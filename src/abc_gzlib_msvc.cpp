// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: abc's zlib gzlib.c for the MSVC build
//
// abc is compiled as C++ inside namespace abc (ABC_USE_NAMESPACE).  Its
// bundled zlib keeps the gzopen path as a const void* so that gzopen_w can
// hand over a wide string, and passes it to wcstombs and _wopen: valid C,
// an error in C++, where MSVC has no permissive mode for it (g++ has
// -fpermissive).  This unit stands in for gzlib.c in the CMake build on
// MSVC and adds the casts through macros; the CRT headers that declare the
// two functions are included first, so their declarations are not touched.
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Jonathan Kimmitt
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include <cstdlib>
#include <cwchar>
#include <io.h>

#define wcstombs(dst, src, n) wcstombs((dst), static_cast<const wchar_t*>(src), (n))
#define _wopen(path, flags, mode) _wopen(static_cast<const wchar_t*>(path), (flags), (mode))

#include "misc/zlib/gzlib.c"
