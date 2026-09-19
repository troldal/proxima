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
