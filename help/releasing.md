# Releasing SavvyCAN

Tagged releases are built by `.github/workflows/build.yml`. A tag beginning
with `v` builds and publishes these GitHub Release assets:

- Linux x86-64 AppImage
- Windows x86-64 ZIP
- macOS x86-64 DMG
- macOS ARM64 DMG
- Build information and SHA-256 checksums

Normal pushes to `master` continue to update the `continuous` development
prerelease. They do not create a numbered release.

## Prepare a release

1. Switch to `master` and bring it up to date.
2. Update `VERSION` in `config.h` when the application version changes.
3. Commit and locally test the release candidate. The normal local build stays
   in `build/qt515/`.
4. Make sure `git status` reports a clean worktree.
5. Validate the intended tag without changing Git:

```bash
./scripts/release.sh v222
```

The numeric part of the tag must match `VERSION` in `config.h`.

## Publish

Run the guarded release command:

```bash
./scripts/release.sh v222 --push
```

The script asks you to type the tag before it does anything. It then:

1. Pushes the current `master` commits to `origin`.
2. Creates an annotated tag at the current commit.
3. Pushes that tag, which starts the GitHub Actions release pipeline.

Open the repository's **Actions** page to watch all four builds. The release is
created only after every platform succeeds.

On a new fork, first open **Actions** and enable workflows if GitHub displays an
enable button. A tag still appears under **Tags** when Actions is disabled, but
it only has GitHub's automatic source archives and no corresponding Release.

## Retry an existing tag

The workflow can rebuild an existing tag without moving or recreating it:

1. Open **Actions**, select **Build**, then choose **Run workflow**.
2. Select `master`, enter the existing tag (for example `v222`) in
   **Existing v* tag to build and publish**, and run it.
3. Wait for validation and all four platform builds to complete. The workflow
   then creates the Release and uploads the binaries and checksums.

Running it again for a published tag replaces its assets while preserving the
tag and Release page.

## Manual Git equivalent

```bash
git switch master
git status
git push origin master
git tag -a v222 -m "SavvyCAN v222"
git push origin v222
```

## Correct a tag before release

If the tag has not been pushed, remove it locally:

```bash
git tag -d v222
```

Do not move or overwrite a published release tag. Correct the problem and use a
new version tag instead, preserving the original release history.
