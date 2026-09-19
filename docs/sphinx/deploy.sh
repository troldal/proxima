#!/usr/bin/env bash
# Publishes the built documentation at https://docs.kinetiq.dev/proxima/,
# replacing what is there.
#
#     docs/sphinx/deploy.sh [<html directory>]
#
# The directory defaults to build/docs/html; build it first, with the docs
# target or `python -m sphinx -b html docs/sphinx <directory>`. CI runs this
# after a release tag has passed every job, and the docs-deploy target runs it
# from a build tree, CLion's included.
#
# Over SSH, with the key ssh finds for the host: your own, or CI's deploy key.
# Needs only tar and a shell on the server. The pages are unpacked beside the
# live copy and then swapped in, so a reader never sees half a site.
#
# Overridable from the environment, for another host or a trial run:
#     PROXIMA_DOCS_HOST  user@host        (kinetiq.dev@ssh.simply.com)
#     PROXIMA_DOCS_DIR   directory there  (/var/www/kinetiq.dev/docs/proxima)
#     PROXIMA_DOCS_SSH   options for ssh  (none)

set -euo pipefail

# Under Git for Windows' bash, an argument that looks like a POSIX path is
# rewritten as a Windows one when a Windows program is started: were ssh
# Windows' own OpenSSH, the server would be sent C:/Program Files/Git/var/www/…
# for the directory. Nothing here wants that. (Ignored everywhere else.)
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

html=${1:-build/docs/html}
host=${PROXIMA_DOCS_HOST:-kinetiq.dev@ssh.simply.com}
dir=${PROXIMA_DOCS_DIR:-/var/www/kinetiq.dev/docs/proxima}
read -r -a ssh_options <<< "${PROXIMA_DOCS_SSH:-}"

if [ ! -f "$html/index.html" ]; then
    echo "deploy.sh: no documentation in $html; build it first" >&2
    exit 1
fi
case $dir in
    /*/*) ;;
    *) echo "deploy.sh: refusing to replace '$dir', which is not an absolute path two levels deep" >&2
       exit 1 ;;
esac

echo "Publishing $html to $host:$dir"

# What runs on the server, with the pages arriving on its stdin as a tar
# stream. The directory is its argument, $1, rather than spliced into it. One
# line, because printf %q quotes a newline in a form only bash reads, and the
# server's login shell parses it first.
remote='set -eu; dir=$1; new="$dir.new"; old="$dir.old";'
remote+=' rm -rf "$new" "$old"; mkdir -p "$new"; tar -xzf - -C "$new";'
remote+=' if [ -d "$dir" ]; then mv "$dir" "$old"; fi;'
remote+=' mv "$new" "$dir"; rm -rf "$old";'
remote+=' echo "Published: $(find "$dir" -type f | wc -l) files"'

# Sphinx's own bookkeeping stays behind: the doctree cache and .buildinfo.
# ssh hands the server one command line, so both words are quoted for its
# shell.
tar -C "$html" --exclude=./.doctrees --exclude=./.buildinfo -czf - . |
    ssh "${ssh_options[@]}" "$host" \
        "sh -c $(printf '%q' "$remote") deploy $(printf '%q' "$dir")"
