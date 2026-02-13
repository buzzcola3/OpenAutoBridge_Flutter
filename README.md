# openautoflutter

A new Flutter plugin project.

## Docker build environment

Build an isolated Linux toolchain with Flutter, clang-18, and libc++ using the provided `Dockerfile`:

```
docker build -t openautoflutter-builder .
```

Run builds inside the container while mounting the workspace (example builds the example app for Linux):

```
docker run --rm -it \
	-v "$PWD:/workspace" \
	-w /workspace/example \
	openautoflutter-builder \
	flutter build linux -v
```

The image precaches Linux artifacts and enables the Linux desktop target; no extra setup is required in the container.

## GitHub Actions CI

Two workflows automate builds:

### Builder Image (`docker-image.yml`)

Builds and pushes the Docker builder image to GitHub Container Registry (`ghcr.io/buzzcola3/openautoflutter-builder`). It runs automatically when `Dockerfile` or the workflow file itself changes on `main`, and can also be triggered manually from the Actions tab.

- Produces two platform tags: `amd64` and `arm64`
- Each architecture builds natively on its own runner (no QEMU emulation)
- Uses GitHub Actions cache for Docker layers to speed up rebuilds

### Release Builds (`release-build.yml`)

Triggered by pushing a version tag (e.g. `git tag v0.0.18 && git push origin v0.0.18`). It:

1. Pulls the pre-built builder image from GHCR (fast, no Docker build)
2. Builds the example app for Linux desktop (`flutter build linux`)
3. Builds the flutter-drm bundle using the release scripts
4. Packages and uploads both artifacts to the GitHub Release

Both `amd64` and `arm64` builds run in parallel on native runners.

### Releasing a new version

```bash
git tag v<next> && git push origin v<next>
```

If the Dockerfile has changed since the last image push, run the Builder Image workflow first (push to `main` or trigger manually) before tagging a release.

## Getting Started

This project is a starting point for a Flutter
[plug-in package](https://flutter.dev/to/develop-plugins),
a specialized package that includes platform-specific implementation code for
Android and/or iOS.

For help getting started with Flutter development, view the
[online documentation](https://docs.flutter.dev), which offers tutorials,
samples, guidance on mobile development, and a full API reference.

