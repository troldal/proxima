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
author = "Kenneth Troldal Balslev"
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


# -- Markdown pages, and their links -----------------------------------------

# Two documents live outside docs/sphinx, at the repository root, and are
# shown as pages of the User guide: read in as those pages' source.
_INCLUDED_PAGES = {
    "guide/architecture": ROOT / "ARCH.md",
    "guide/design": ROOT / "DESIGN.md",
}
_REPOSITORY = "https://github.com/troldal/proxima/blob/master/"
_LOCAL_LINK = re.compile(r"\]\((?!https?:|mailto:|#)([^)\s]+)\)")


def _resolve_link(target: str, source: pathlib.Path, docname: str) -> str:
    """A link from `source`, shown as page `docname`, as the site needs it.

    Links are written to work on GitHub, relative to the file they are in. On
    the site: a link to an included document goes to its page; one to a file
    inside docs/sphinx stays relative, but to the page it appears on; and one
    to anything else in the repository goes to that file on GitHub.
    """
    path_part, hash_, anchor = target.partition("#")
    suffix = hash_ + anchor
    path = (source.parent / path_part).resolve()
    if not path.exists():
        # Broken on GitHub too; the -W build turns this into a failure.
        _logger.warning("%s links to %s, which does not exist", source.name, target)
    here = (HERE / docname).parent
    for page, document in _INCLUDED_PAGES.items():
        if path == document.resolve():
            return os.path.relpath(HERE / f"{page}.md", here).replace(os.sep, "/") + suffix
    if path.is_relative_to(HERE):
        return os.path.relpath(path, here).replace(os.sep, "/") + suffix
    if path.is_relative_to(ROOT):
        return _REPOSITORY + path.relative_to(ROOT).as_posix() + suffix
    return target  # Outside the repository: leave it as written.


def _with_links_resolved(text: str, source: pathlib.Path, docname: str) -> str:
    out = []
    fenced = False
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith("```"):
            fenced = not fenced
        if not fenced:
            line = _LOCAL_LINK.sub(
                lambda m: "](" + _resolve_link(m.group(1), source, docname) + ")", line
            )
        out.append(line)
    return "".join(out)


def _read_markdown(app, docname: str, source: list) -> None:
    """Resolves the links of every Markdown page, and reads the included
    documents in as their pages' source."""
    included = _INCLUDED_PAGES.get(docname)
    if included is not None:
        app.env.note_dependency(str(included))
        source[0] = _with_links_resolved(included.read_text(encoding="utf-8"), included, docname)
        return
    path = HERE / f"{docname}.md"
    if path.exists():
        source[0] = _with_links_resolved(source[0], path, docname)


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
    app.connect("source-read", _read_markdown)
