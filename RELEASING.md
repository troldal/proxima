# Releasing Proxima

A release is a commit on `master` that CI has passed, tagged `vX.Y.Z`. Pushing
the tag runs [the release workflow](.github/workflows/release.yml), which
publishes a GitHub release with the changelog's notes for that version.

The version is written in one place: `project()` in `CMakeLists.txt`. The
generated `<proxima/version.hpp>`, the package's version file and the
documentation's title all read it from there, and a test checks the header
agrees.

## Versions

Proxima uses [semantic versioning](https://semver.org).

- **Before 1.0**, a minor release (0.2 → 0.3) may change the API, and a
  patch release (0.2.0 → 0.2.1) only fixes. `find_package(proxima 0.2)`
  accepts 0.2.x only.
- **From 1.0**, only a major release may break the API.

A minor release completes a milestone in [ROADMAP.md](ROADMAP.md).

## Checklist

1. **Everything in the milestone is done,** or moved to a later one in
   `ROADMAP.md`.
2. **CI has passed on `master`,** every job.
3. **The version:** set it in `project()` in `CMakeLists.txt`. Nowhere else.
4. **The changelog:**
   1. In `CHANGELOG.md`, rename `[Unreleased]` to `[X.Y.Z] — YYYY-MM-DD`.
   2. Open a new, empty `[Unreleased]` above it.
   3. Update the links at the bottom:
      - `[Unreleased]` compares from `vX.Y.Z` to `HEAD`;
      - `[X.Y.Z]` compares from the previous tag to `vX.Y.Z`.
5. **The roadmap:** mark the milestone as released in `ROADMAP.md`.
6. **Commit** as `Release X.Y.Z`, push, and wait for CI to pass on that
   commit.
7. **Tag it and push the tag:**

   ```sh
   git tag -a vX.Y.Z -m "Proxima X.Y.Z"
   git push origin vX.Y.Z
   ```

   The release workflow checks two things, then publishes the release with
   the section as its notes. If either check fails, delete the tag
   (`git push --delete origin vX.Y.Z`), fix, and tag again.
   - The tag must match `project()`.
   - `CHANGELOG.md` must have a section for the version.
8. **Check the release** on GitHub: the notes, and the source archives.
9. **Check the documentation** at https://docs.kinetiq.dev/proxima/. CI
   publishes it once every job has passed on the tag; the title shows the
   version.

## The documentation site

The site is published at https://docs.kinetiq.dev/proxima/ by
[`docs/sphinx/deploy.sh`](docs/sphinx/deploy.sh). The script copies the built
HTML over SSH to `kinetiq.dev@ssh.simply.com`, into
`/var/www/kinetiq.dev/docs/proxima`, and replaces whatever is there. The new
copy is unpacked beside the old one and then swapped in.

**From CI.** The `deploy-docs` job publishes after every other job has
passed. It runs for a release tag, or when CI is started by hand: Actions →
CI → Run workflow, on `master`. It logs in with a key used for nothing else.
The key is set up once:

1. Make the key, with no passphrase, since CI cannot type one:

   ```sh
   ssh-keygen -t ed25519 -f proxima-deploy -C "proxima docs deploy"
   ```

2. Add `proxima-deploy.pub` to the SSH keys of the kinetiq.dev web hosting in
   simply.com's control panel.
3. Put the private key, the file `proxima-deploy`, in the repository's
   secrets as `DOCS_DEPLOY_KEY`: Settings → Secrets and variables → Actions.
   Then delete the file, or keep it somewhere safe.

To revoke CI's access, remove that key at simply.com; your own key is not
affected.

The job accepts only the server keys pinned in `.github/workflows/ci.yml`. If
simply.com changes its keys, the job fails until the pinned ones are updated
with `ssh-keyscan ssh.simply.com`.

**From your machine or CLion.** Build the `docs-deploy` target. It builds the
site, then runs `deploy.sh` with your own SSH key, the one `ssh
kinetiq.dev@ssh.simply.com` logs in with. On Windows it uses the bash that
comes with Git. From a shell:

```sh
cmake --build <build> --target docs-deploy
```
