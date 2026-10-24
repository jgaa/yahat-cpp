# Install yahat-mcp in a Codex project

Copy the complete skill directory into the consuming repository's
`.agents/skills/yahat-mcp`. It contains instructions and UI metadata only;
installing it does not install yahat, enable MCP in a library build, or configure
an MCP client/server connection.

## From a local yahat checkout

Use the checkout containing this skill and the implemented MCP API. From the
consumer project's root, replace the example source path:

```sh
yahat_checkout=/path/to/yahat-cpp
mkdir -p .agents/skills
test ! -e .agents/skills/yahat-mcp &&
    cp -R "$yahat_checkout/.agents/skills/yahat-mcp" .agents/skills/
```

If the destination exists, compare/review it before updating rather than merging
copies blindly. Track the skill in the consumer repository if the team should
share it. Preserve `references/` and `agents/` alongside `SKILL.md`; there are no
links that depend on the skill staying inside the yahat checkout.

Alternatively, ask Codex in the target project:

> Copy the complete yahat-mcp skill from /path/to/yahat-cpp/.agents/skills/yahat-mcp
> into this project's .agents/skills/yahat-mcp. Inspect any existing destination
> before replacing it. Then use $yahat-mcp to add the requested tools using this
> project's yahat dependency and authentication model.

Codex discovers repository skills under `.agents/skills`. Invoke this skill with
`$yahat-mcp`, or select it from the skill picker. Changes are detected
automatically; restart Codex if it does not appear. See the
[official skill discovery documentation](https://learn.chatgpt.com/docs/build-skills#where-codex-loads-local-skills).

Example request after installation:

> Use $yahat-mcp to expose our existing read-only status API as typed coroutine
> tools. Reuse our authenticator and provide detailed examples through help.

## Locate the dependency after copying

The source-path table in `SKILL.md` refers to the **yahat dependency source**,
not files expected in the consumer repository. Locate it through that project's
CMake/submodule/package configuration. With a binary installation, inspect the
installed headers and generated config, then obtain matching source when the
examples or tests are needed. Use the existing project dependency rather than
adding a second yahat checkout by default.

The [upstream repository](https://github.com/jgaa/yahat-cpp) may lag a local MCP
checkout. Do not replace a working local dependency with a public tag/branch
until you have checked it contains `include/yahat/Mcp.h` and the required API.
If matching documentation/source cannot be located, report that gap rather than
inventing an API from the feature proposal.

## Check the copied skill

Confirm that Codex can select `yahat-mcp` and that `references/install.md` resolves
relative to its `SKILL.md`. If the bundled `skill-creator` validator is available,
run its `scripts/quick_validate.py` against the copied skill directory; its
location depends on the Codex installation. Also check that every source-path
entry resolves in the selected yahat checkout. Updating the dependency is a
reason to recheck these paths and the guidance against its public API.
