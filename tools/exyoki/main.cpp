// Copyright (c) 2026 Jakub Melka and Contributors
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

#include "CommandLine.hpp"

// The entry point and nothing else. Everything the tool does lives in
// exyoki_core, where tests/cli reaches it directly: what a command line means
// is decided by BuildCommandLine, and what a command does by its Run(). A
// main() that held any of that would be the one part of the tool no test could
// call.
int main(int argc, char** argv)
{
    return exyoki::RunCommandLine(argc, argv);
}
