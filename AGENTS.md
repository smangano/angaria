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

## Configuration

Configuration files use TOML. Implement TOML support with [cpptoml](https://github.com/skystrife/cpptoml). Provide YAML, JSON, and XML configuration support through converters that emit TOML for the common loader and validation path.

## Milestone branches and review

- Create the staging branch `milestone-<n>` from `main` for each milestone.
- Create `milestone-<n>-<s>` from its milestone staging branch for one focused step.
- Integrate each step through a PR targeting `milestone-<n>`.
- Integrate the completed milestone through a PR targeting `main`.
- Identify the roadmap objective, scope, and validation results in each step PR.
