# Releases and automated builds

## Make a GA release

Update `YAHAT_VERSION` in the root `CMakeLists.txt` and merge it, along with the
changes to release, into `main`. Then open **Actions → Release GA → Run workflow**
and select `main`. There are no version or artifact inputs.

The workflow checks out current `main`, reads the CMake version, and creates
`v<YAHAT_VERSION>` plus a published GitHub release. It fails if that tag already
exists, including if another process creates it during the run. Only a literal
`MAJOR.MINOR.PATCH` version is accepted for a GA release.

Release notes list commit subjects and short hashes, oldest first, since the
most recently published non-draft, non-prerelease release. The first release
includes the full commit history. API failures, missing previous tags and a
previous release outside main's ancestry stop publication rather than silently
choosing an incorrect changelog boundary.

There is no application build or asset upload. GitHub supplies its usual source
archives. The workflow uses `GITHUB_TOKEN` with `contents: write` only in the
release job, and publication runs are serialized.

Tag creation happens before release creation. If publication fails after creating
the tag, inspect the tag/release in GitHub before repairing it; rerunning refuses
to reuse that tag. The workflow never deletes or overwrites an existing tag.

## Which changes run builds?

| Workflow | Automatic trigger | Relevant inputs |
| --- | --- | --- |
| Ubuntu, macOS, Windows | Pushes to `main` and pull requests targeting `main` | Library sources/headers, CMake/package configuration, native C++ tests, built API/chat examples, and that platform's workflow |
| YahatChat container | Pushes to `main` | Library sources/headers, CMake/package configuration, chat example, Docker inputs, and its own workflow |
| Release GA | Manual only, from `main` | Current root CMake version and committed history |

Documentation, skills, feature proposals and the separate Python/Node conformance
tooling do not trigger platform builds. Changes to tests or API-example C++ code
also do not rebuild the chat container. The container script builds only the
`yahatchat` target. Releases do not automatically trigger container publication.

The existing platform build configurations are preserved; these changes do not
add the official MCP conformance runner to CI. See
[local MCP testing](mcp-testing.md) for that suite.

The Windows build uses the `windows-2025-vs2026` image and the
`Visual Studio 18 2026` generator, with CMake supplied by that image. Its vcpkg
cache is scoped to the runner architecture and compiler generation so a compiler
migration does not reuse the previous generation's installed libraries.
