// Copyright (c) 2026 Jakub Melka and Collaborators
// SPDX-License-Identifier: MIT
// See LICENSE file in the project root for full license text.

// Entry point of the ExyokiOffice container image.
//
// The image is distroless: it has no shell, so this program is where a bare
// `docker run` lands. With no arguments it prints the only documentation a user
// of the image gets without opening a browser; with a command name it replaces
// itself with one of the four installed binaries.
//
// It uses execv rather than fork, so the program named on the command line
// keeps the standard streams docker handed over and stays PID 1 with its
// signals intact. That matters for the MCP servers, which speak JSON-RPC over
// those streams. Nothing is written to standard output on the path that ends in
// execv, so the protocol stream is never polluted by this dispatcher.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef EXYOKI_VERSION
#define EXYOKI_VERSION "0.0.0"
#endif

#define EXYOKI_BIN_DIR "/opt/exyokioffice/bin/"

// Both the short name and the installed file name are accepted, so a command
// line copied from the manual of the binaries themselves also works here.
static const struct
{
    const char *alias;
    const char *binary;
} kCommands[] = {
    { "exyoki", "exyoki" },
    { "cli", "exyoki" },
    { "word", "exyoki-mcp-word" },
    { "exyoki-mcp-word", "exyoki-mcp-word" },
    { "excel", "exyoki-mcp-excel" },
    { "exyoki-mcp-excel", "exyoki-mcp-excel" },
    { "powerpoint", "exyoki-mcp-power-point" },
    { "power-point", "exyoki-mcp-power-point" },
    { "exyoki-mcp-power-point", "exyoki-mcp-power-point" },
};

static const size_t kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

static void PrintUsage(FILE *out)
{
    fprintf(out,
            "ExyokiOffice " EXYOKI_VERSION
            " - Office documents for command lines and AI agents\n"
            "\n"
            "This image carries the ExyokiOffice shared library and four programs. Name\n"
            "one as the first argument to docker run; everything after it is passed\n"
            "through to that program unchanged.\n"
            "\n"
            "  exyoki        command-line tool      (exyoki --help)\n"
            "  word          MCP server for .docx   (exyoki-mcp-word)\n"
            "  excel         MCP server for .xlsx   (exyoki-mcp-excel)\n"
            "  powerpoint    MCP server for .pptx   (exyoki-mcp-power-point)\n"
            "\n"
            "/work is the working directory and the MCP workspace root - the only place\n"
            "the servers may read or write. Mount your documents there.\n"
            "\n"
            "Command-line tool\n"
            "\n"
            "  docker run --rm -v \"$PWD:/work\" IMAGE exyoki --help\n"
            "  docker run --rm -v \"$PWD:/work\" IMAGE exyoki validate report.docx\n"
            "  docker run --rm -v \"$PWD:/work\" IMAGE exyoki convert report.docx report.md\n"
            "\n"
            "MCP servers\n"
            "\n"
            "They speak JSON-RPC over standard input and output, so -i is required and\n"
            "-t must not be used. Register one per document family with your client:\n"
            "\n"
            "  {\n"
            "    \"mcpServers\": {\n"
            "      \"word\": {\n"
            "        \"command\": \"docker\",\n"
            "        \"args\": [\"run\", \"--rm\", \"-i\",\n"
            "                 \"-v\", \"/path/to/documents:/work\",\n"
            "                 \"IMAGE\", \"word\"]\n"
            "      }\n"
            "    }\n"
            "  }\n"
            "\n"
            "Repeat that entry with \"excel\" and \"powerpoint\". Server options follow\n"
            "the command name:\n"
            "\n"
            "  docker run --rm -i -v \"$PWD:/work\" IMAGE word --read-only\n"
            "  docker run --rm    -v \"$PWD:/work\" IMAGE word --print-tools\n"
            "\n"
            "Writing as yourself\n"
            "\n"
            "The image runs as uid 65532. Add --user \"$(id -u):$(id -g)\" so documents\n"
            "saved into the mounted directory belong to you.\n"
            "\n"
            "IMAGE above is this image, exyokioffice:" EXYOKI_VERSION
            ". The manual pages are\n"
            "docs/tools/docker.md and docs/tools/mcp-servers.md in the repository.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0)
    {
        PrintUsage(stdout);
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0)
    {
        printf("exyokioffice image " EXYOKI_VERSION "\n");
        return 0;
    }

    for (size_t i = 0; i < kCommandCount; ++i)
    {
        if (strcmp(argv[1], kCommands[i].alias) != 0)
        {
            continue;
        }

        char path[sizeof(EXYOKI_BIN_DIR) + 64];
        const int written = snprintf(path, sizeof(path), EXYOKI_BIN_DIR "%s", kCommands[i].binary);
        if (written < 0 || (size_t)written >= sizeof(path))
        {
            fprintf(stderr, "exyokioffice: the path of '%s' does not fit\n", kCommands[i].binary);
            return 70;
        }

        // argv is NULL-terminated, so handing over argv + 1 keeps every argument
        // after the command name and terminates correctly.
        execv(path, argv + 1);
        fprintf(stderr, "exyokioffice: cannot execute %s: %s\n", path, strerror(errno));
        return 126;
    }

    fprintf(stderr,
            "exyokioffice: unknown command '%s'.\n"
            "Expected exyoki, word, excel or powerpoint. Run this image with no\n"
            "arguments for the full usage page.\n",
            argv[1]);
    return 2;
}
