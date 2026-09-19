"""Sphinx configuration for Proxima's documentation.

Doxygen reads the public headers into XML, Breathe turns that XML into
Sphinx pages, and Furo lays them out. Doxygen runs from here, before the
build, so one command builds everything:

    pip install -r docs/sphinx/requirements.txt
    sphinx-build -b html docs/sphinx build/docs/html

or, from a configured build tree, `cmake --build <dir> --target docs`.

The environment variable DOXYGEN names the doxygen executable when the one
on PATH is not the one to use.
"""

import os
import pathlib
import re
import shutil
import subprocess

from sphinx.util import logging

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
_logger = logging.getLogger(__name__)


def _project_version() -> str:
    """The version in CMakeLists.txt's project(), the one source of it."""
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(\s*proxima\s+VERSION\s+([0-9.]+)", text)
    return match.group(1) if match else "unknown"


# -- Project ------------------------------------------------------------------

project = "Proxima"
author = "Kenneth Balslev"
copyright = f"2026, {author}"
release = _project_version()
version = release

# -- General ------------------------------------------------------------------

extensions = [
    "breathe",
    "myst_parser",
]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}
exclude_patterns = ["_build"]

primary_domain = "cpp"
# Untagged blocks are ASCII diagrams in the Markdown pages, not C++: every C++
# example is tagged.
highlight_language = "none"

# Overloads that differ only in their requires-clauses, such as Integer's
# constructor templates, are one declaration to Sphinx's C++ domain, which
# does not compare constraints. Both are shown; the warning is not about
# anything in the headers.
suppress_warnings = ["duplicate_declaration.cpp"]

# The guide's own links point at its headings: [x](#failure-is-an-outcome...).
myst_heading_anchors = 3
myst_enable_extensions = ["colon_fence"]

# -- Breathe ------------------------------------------------------------------

# Set for real in _run_doxygen, once the output directory is known.
breathe_projects = {"proxima": ""}
breathe_default_project = "proxima"
breathe_default_members = ("members", "undoc-members")
breathe_show_include = True

# -- HTML ---------------------------------------------------------------------

html_theme = "furo"
html_title = f"Proxima {release}"
html_theme_options = {
    "source_repository": "https://github.com/troldal/proxima",
    "source_branch": "master",
    "source_directory": "docs/sphinx/",
}


# -- The Markdown documents outside docs/sphinx --------------------------------

# Each is shown on a page of its own, read in as that page's source. Their
# links are written for GitHub, relative to where each file lives, so as each
# is read: a link to another of these becomes a link to its page, and a link
# to anything else in the repository goes to it on GitHub.
_MARKDOWN_PAGES = {
    "guide": ROOT / "docs" / "guide.md",
    "architecture": ROOT / "ARCH.md",
    "design": ROOT / "DESIGN.md",
}
_REPOSITORY = "https://github.com/troldal/proxima/blob/master/"
_LOCAL_LINK = re.compile(r"\]\((?!https?:|mailto:|#)([^)\s]+)\)")


def _resolve_link(target: str, source: pathlib.Path) -> str:
    path_part, hash_, anchor = target.partition("#")
    suffix = hash_ + anchor
    path = (source.parent / path_part).resolve()
    if not path.exists():
        # Broken on GitHub too; the -W build turns this into a failure.
        _logger.warning("%s links to %s, which does not exist", source.name, target)
    for page, document in _MARKDOWN_PAGES.items():
        if path == document.resolve():
            return f"{page}.md{suffix}"
    try:
        return _REPOSITORY + path.relative_to(ROOT).as_posix() + suffix
    except ValueError:
        return target  # Outside the repository: leave it as written.


def _with_links_resolved(text: str, source: pathlib.Path) -> str:
    out = []
    fenced = False
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("```"):
            fenced = not fenced
        if not fenced:
            line = _LOCAL_LINK.sub(
                lambda m: "](" + _resolve_link(m.group(1), source) + ")", line
            )
        out.append(line)
    return "".join(out)


def _read_markdown_page(app, docname: str, source: list) -> None:
    document = _MARKDOWN_PAGES.get(docname)
    if document is None:
        return
    app.env.note_dependency(str(document))
    source[0] = _with_links_resolved(document.read_text(encoding="utf-8"), document)


# -- Doxygen, before the build --------------------------------------------------


def _run_doxygen(app) -> None:
    """Runs Doxygen into a directory beside Sphinx's output, and points
    Breathe at the XML it writes."""
    doxygen = os.environ.get("DOXYGEN") or shutil.which("doxygen")
    if not doxygen:
        raise RuntimeError(
            "doxygen was not found: install it, or set DOXYGEN to its path"
        )
    out = pathlib.Path(app.outdir).parent / "doxygen"
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PROXIMA_ROOT=ROOT.as_posix(), PROXIMA_DOXYGEN_OUT=out.as_posix())
    subprocess.run([doxygen, str(HERE / "Doxyfile")], cwd=HERE, env=env, check=True)
    app.config.breathe_projects = {"proxima": str(out / "xml")}


def setup(app):
    app.connect("builder-inited", _run_doxygen)
    app.connect("source-read", _read_markdown_page)
