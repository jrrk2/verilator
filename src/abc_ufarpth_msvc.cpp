// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: abc's opt/ufar/UfarPth.cpp for the MSVC build
//
// Compiled in place of UfarPth.cpp by the CMake build on MSVC, with the
// pthread calls provided by abc_pthread_msvc.h rather than abc's
// pthreads-win32 DLL (see that header).
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

#include "abc_pthread_msvc.h"

#include "opt/ufar/UfarPth.cpp"
