# Angaria C++ conventions

- Use CamelCase for class and struct names, such as `MessageQueue`.
- Prefix member variables with an underscore and use camelCase, such as `_messageCount`.
- Use underscore-separated words for functions and member functions, such as `receive_message`.
- Use Allman formatting, with braces on their own lines aligned with the declaration or control statement.
- Include project headers with `#include "..."`.
- Include standard-library and third-party headers with `#include <...>`.

Boost and Intel TBB are permitted dependencies when warranted by the implementation.

## Documentation

- Define specialized and project-specific terms at first use, or link to their canonical definition.
- Keep shared terminology in `docs/glossary.md` consistent with the format/API contracts and code comments.

## Living programmer primer

- Maintain `docs/primer.md` and its runnable programs in `examples/primer/` throughout all milestones. The reader is a junior programmer: use simple language, define new terms, and progress from small complete programs to larger applications.
- Every feature/milestone PR must review the primer. Incorporate newly implemented public behavior and update affected commands, examples, expected results, ownership/lifetime explanations, and error handling in the same PR. An unrelated change need not manufacture a primer edit.
- Treat the compiled example sources as canonical. Keep linked explanations and snippets consistent with them; add new example targets and extend `test/primer_examples.py` when the learning sequence grows.
- Clearly separate implemented, runnable features from planned chapters and the legacy prototype. Do not demonstrate undeveloped API methods as working examples.
- Include a primer/example update in each milestone's completion review, preserving the beginner path as advanced features are added.

## Tests as usage documentation

- Treat tests as examples readers use to learn Angaria. Explain the scenario, relevant API sequence, and the guarantee or failure behavior being demonstrated.
- Add brief suite orientation and purposeful test comments; explain non-obvious setup and ownership/lifetime transitions instead of narrating each assertion.
- Clearly distinguish public API usage from private fixtures, deliberate corruption, fault injection, the legacy prototype, and models of future behavior. Do not imply a planned feature is implemented.

## Configuration

Configuration files use TOML. Implement TOML support with [cpptoml](https://github.com/skystrife/cpptoml). Provide YAML, JSON, and XML configuration support through converters that emit TOML for the common loader and validation path.

## Milestone branches and review

- Create the staging branch `milestone-<n>` from `main` for each milestone.
- Create `milestone-<n>-<s>` from its milestone staging branch for one focused step.
- Integrate each step through a PR targeting `milestone-<n>`.
- Integrate the completed milestone through a PR targeting `main`.
- Identify the roadmap objective, scope, and validation results in each step PR.

## Framework versioning

- `VERSION` is the single source of the framework's `major.minor.build` version; CMake reads it.
- During roadmap development, major is `0` and minor is the milestone number.
- Assign the next unused build number once for every new PR, including fixes, documentation, and milestone integration PRs. Keep the counter increasing across milestones; do not reset it when minor changes.
- Review fixes, additional commits, and CI reruns within the same PR retain that PR's version. Include the assigned version in its description and check previously assigned PR versions to avoid reusing a build number.
- Numbering starts with the versioning PR at `0.2.1`; earlier PRs remain unversioned.
- Set the completed roadmap release to `1.0.0`. Framework versions do not change configuration, mapped-format, schema, or synchronization protocol versions automatically.
- See [the roadmap's versioning policy](ROADMAP.md#versioning) for examples.
