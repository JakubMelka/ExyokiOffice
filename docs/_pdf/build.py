#!/usr/bin/env python3
"""Assemble docs/ into one pandoc-ready Markdown file for the PDF manual.

Reads the chapter order from chapters.txt, concatenates the chapters, and
rewrites links so the result works as a single document:

- A link to another chapter (``word/tables.md``) becomes an internal link to
  that chapter's title anchor.
- A link to another chapter's section (``exyoki.md#exit-codes``) keeps the
  section fragment, which pandoc resolves globally.
- A link out of docs/ (``../examples/...``) becomes an absolute repository
  URL, so it stays clickable in the PDF.
- Every chapter title gets an explicit pandoc identifier the rewritten links
  point at.

Usage: python3 docs/_pdf/build.py [--out build/docs-pdf]
The pandoc invocation itself lives in the docs-pdf workflow and uses
defaults.yaml next to this script.

``--print-version`` reports the release number from the repository root
VERSION.txt and exits; the workflow uses it to name the rendered PDF.
"""

from __future__ import annotations

import argparse
import datetime
import os
import posixpath
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DOCS_DIR = os.path.dirname(SCRIPT_DIR)
REPO_DIR = os.path.dirname(DOCS_DIR)

LINK_PATTERN = re.compile(r"(?<!\!)\[(?P<text>[^\]]*)\]\((?P<target>[^)\s]+)\)")
EXTERNAL_SCHEMES = ("http://", "https://", "mailto:")


def read_version() -> str:
    """Release number from the repository root VERSION.txt."""
    # utf-8-sig: an editor on Windows may leave a byte order mark behind.
    with open(os.path.join(REPO_DIR, "VERSION.txt"), encoding="utf-8-sig") as handle:
        return handle.read().strip()


# Symbols the docs use that the main font (TeX Gyre Pagella) has no glyph for:
# the compatibility matrix markers and the small triangle bullet. Each one is
# typeset from DejaVu Sans instead; without this xelatex drops the character
# and warns. \symbol with the code point is used rather than the literal
# character, which newunicodechar has just made active.
FALLBACK_SYMBOLS = ("2714", "2716", "25D0", "25B8")


def title_page_header() -> str:
    """LaTeX preamble putting the repository logo above the title.

    The path is absolute because pandoc runs the PDF engine in a temporary
    directory, where a repository-relative path would not resolve. titling's
    \\pretitle hook is used instead of an image in the document body so the
    logo lands on the title page itself, whatever the pandoc template does
    with include-before.
    """
    # The repository file is LOGO.png; the case matters on the Linux runner.
    logo = os.path.join(REPO_DIR, "LOGO.png")
    if not os.path.isfile(logo):
        raise SystemExit(f"error: the title page logo is missing: {logo}")
    fallbacks = "".join(
        "  \\newunicodechar{"
        + chr(int(code_point, 16))
        + "}{{\\symbolfont\\symbol{\"" + code_point + "}}}\n"
        for code_point in FALLBACK_SYMBOLS
    )
    return (
        "header-includes: |\n"
        "  \\usepackage{graphicx}\n"
        "  \\usepackage{titling}\n"
        "  \\usepackage{newunicodechar}\n"
        "  \\newfontfamily\\symbolfont{DejaVu Sans}\n"
        + fallbacks
        + "  \\pretitle{\\begin{center}\\includegraphics[width=140mm]{"
        + logo.replace("\\", "/")
        + "}\\\\[2em]\\LARGE}\n"
        "  \\posttitle{\\par\\end{center}}\n"
    )


def chapter_anchor(docs_relative_path: str) -> str:
    """Stable pandoc identifier for a chapter's title heading."""
    slug = re.sub(r"[^a-z0-9]+", "-", docs_relative_path.lower()).strip("-")
    return f"doc-{slug}"


def read_chapter_list(path: str) -> list[tuple[str, str]]:
    """Returns (kind, value) entries where kind is 'part' or 'chapter'."""
    entries: list[tuple[str, str]] = []
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith(":: "):
                entries.append(("part", line[3:].strip()))
            else:
                entries.append(("chapter", line.replace("\\", "/")))
    return entries


def repository_url(repo_relative_path: str) -> str:
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    reference = os.environ.get("GITHUB_SHA", "master")
    if not repository:
        return repo_relative_path
    return f"{server}/{repository}/blob/{reference}/{repo_relative_path}"


def rewrite_links(markdown: str, chapter_dir: str, chapters: set[str]) -> str:
    def resolve(target: str) -> str | None:
        """Docs-relative normalized path, or None when it leaves docs/."""
        joined = posixpath.normpath(posixpath.join(chapter_dir, target))
        return None if joined.startswith("..") else joined

    def replace(match: re.Match[str]) -> str:
        text, target = match.group("text"), match.group("target")
        if target.startswith(EXTERNAL_SCHEMES) or target.startswith("#"):
            return match.group(0)

        path, fragment = (target.split("#", 1) + [""])[:2]
        resolved = resolve(path)

        if resolved is not None and resolved in chapters:
            anchor = fragment if fragment else chapter_anchor(resolved)
            return f"[{text}](#{anchor})"

        if resolved is not None:
            # A docs/ file that is not part of the manual: link to the repo.
            return f"[{text}]({repository_url(posixpath.join('docs', resolved))})"

        # Out of docs/: examples, headers, the repository README.
        repo_relative = posixpath.normpath(
            posixpath.join("docs", chapter_dir, path))
        return f"[{text}]({repository_url(repo_relative)})"

    return LINK_PATTERN.sub(replace, markdown)


def tag_title(markdown: str, anchor: str, chapter_path: str) -> str:
    lines = markdown.split("\n")
    for index, line in enumerate(lines):
        if line.startswith("# "):
            lines[index] = f"{line.rstrip()} {{#{anchor}}}"
            return "\n".join(lines)
    raise SystemExit(f"error: {chapter_path} has no '# ' title heading")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="build/docs-pdf",
                        help="output directory (default: build/docs-pdf)")
    parser.add_argument("--print-version", action="store_true",
                        help="print the release number from VERSION.txt and exit")
    arguments = parser.parse_args()

    version = read_version()
    if arguments.print_version:
        print(version)
        return

    entries = read_chapter_list(os.path.join(SCRIPT_DIR, "chapters.txt"))
    chapters = {value for kind, value in entries if kind == "chapter"}

    missing = [chapter for chapter in sorted(chapters)
               if not os.path.isfile(os.path.join(DOCS_DIR, chapter))]
    if missing:
        raise SystemExit("error: chapters.txt lists missing files: "
                         + ", ".join(missing))

    today = datetime.date.today().isoformat()
    pieces = [
        "---\n"
        "title: ExyokiOffice\n"
        f"subtitle: User manual for version {version}\n"
        f"date: {today}\n"
        + title_page_header()
        + "---\n"
    ]

    for kind, value in entries:
        if kind == "part":
            pieces.append(f"\\part{{{value}}}\n")
            continue
        with open(os.path.join(DOCS_DIR, value), encoding="utf-8") as handle:
            markdown = handle.read().rstrip() + "\n"
        markdown = tag_title(markdown, chapter_anchor(value), value)
        markdown = rewrite_links(markdown, posixpath.dirname(value), chapters)
        pieces.append(markdown)

    out_dir = os.path.join(REPO_DIR, arguments.out)
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, "manual.md")
    with open(out_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n\n".join(pieces))

    chapter_count = len(chapters)
    print(f"wrote {out_path} ({chapter_count} chapters, version {version})")


if __name__ == "__main__":
    sys.exit(main())
