// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// The single doctest entry point, shared by every test layer.
//
// This file is listed in the SOURCES of each test executable rather than
// compiled into ExyokiOfficeTestSupport: main() inside a static library is
// pulled in only as a side effect of the CRT referencing it, which is more
// fragile than simply naming the translation unit in every target.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
