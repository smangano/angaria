# Angaria C++ conventions

- Use CamelCase for class and struct names, such as `MessageQueue`.
- Prefix member variables with an underscore and use camelCase, such as `_messageCount`.
- Use underscore-separated words for functions and member functions, such as `receive_message`.
- Use Allman formatting, with braces on their own lines aligned with the declaration or control statement.
- Include project headers with `#include "..."`.
- Include standard-library and third-party headers with `#include <...>`.

Boost and Intel TBB are permitted dependencies when warranted by the implementation.

## Performance and correctness

- Treat performance and correctness as equally important requirements. Consider latency, throughput, allocation, synchronization, and scaling costs throughout design and review.
- Every implementation step must include either a focused performance unit test or a full matched benchmark against nanomsg, as appropriate to the change's scope. Run the selected check and report the baseline, result, expected direction and any regression in the same PR so problems are caught early. Keep focused checks for isolated costs; use end-to-end latency and verified throughput for transport/policy changes. Design-only steps specify the upcoming checks and make no measured speedup claim.
- Assess a precaution's runtime cost against the likelihood and impact of the failure it prevents, given the supported operating conditions and assumptions.
- After that risk assessment, a justified tradeoff may favor performance over overly defensive or overengineered precautions. Existing checks and tests are open to reassessment; their presence alone does not justify retaining a costly precaution.
- Record the rationale, assumptions, accepted risks, and supporting measurements in the design or PR. Make any changes to documented guarantees explicit, and update affected documentation and tests accordingly.
- Keep benchmark reports, plots, summaries, provenance and archive checksums in Git. Store bulky raw samples, compressed attempt archives and profiler dumps in the maintainer benchmark archive instead of committing them. See [benchmark archive policy and restoration](benchmarks/results/README.md).
- Preserve every original attempt, including failures, timeouts and unfavorable results. Verify the external copy before removing tracked payloads; expiring CI artifacts are temporary transfer copies, not permanent retention.
- Restore the recorded raw inputs before strict verification or figure regeneration. Do not replace raw-evidence checks with summary-only checks. Archiving does not authorize rewriting Git history.

## Shared helpers

- Reuse the focused helper headers listed in `docs/helper-audit.md` before adding a local utility. Keep related helpers together; avoid a general-purpose catch-all header.
- Pure byte operations, arithmetic, identity checks, wire-code mappings, and result unwrapping belong in their shared inline/constexpr headers. Keep operation-specific limits, diagnostics, and ownership/recovery policy with the operation.
- Ordinary byte helpers operate on local copies. Live mapped synchronization words still require the native atomic adapter.
- Test plumbing may share test-only headers. Keep golden-byte construction independent of production encoders, with expected fields/codes/values explicit in each scenario.

## Documentation

- Define specialized and project-specific terms at first use, or link to their canonical definition.
- Keep shared terminology in `docs/glossary.md` consistent with the format/API contracts and code comments.

## Living programmer primer

- Maintain `docs/primer.md` and its runnable programs in `examples/primer/` throughout all milestones. The reader is a junior programmer: use simple language, define new terms, and progress from small complete programs to larger applications.
- Every feature/milestone PR must review the primer. Incorporate newly implemented public behavior and update affected commands, examples, expected results, ownership/lifetime explanations, and error handling in the same PR. An unrelated change need not manufacture a primer edit.
- Treat the compiled example sources as canonical. Keep linked explanations and snippets consistent with them; add new example targets and extend `test/primer_examples.py` when the learning sequence grows.
- Clearly separate implemented, runnable features from planned chapters and the legacy prototype. Do not demonstrate undeveloped API methods as working examples.
- Primer programs use public Angaria headers and calls directly. Show configuration values or an explicit TOML load, and check each fallible result before extracting its value. Do not hide these steps in generic result/configuration wrappers, macros, or private implementation helpers. Application-specific functions may return `Result<T>` or `OperationResult` and explicitly propagate an `Error`.
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

- Before `1.0.0`, backward compatibility is not required. Prefer one current runtime over maintaining superseded engines, old-format readers, API adapters or migration paths. Document breaking changes and require fresh development Sessions where needed. Unsupported versions must still be rejected before access; never reinterpret old bytes under a new protocol.
- `VERSION` is the single source of the framework's `major.minor.build` version; CMake reads it.
- During roadmap development, major is `0` and minor is the milestone number.
- Starting with milestone 3, reset build to `1` at each new milestone: its first step is `0.<milestone>.1`. Assign the next unused build number within that milestone once for every new PR, including fixes, documentation, and milestone integration PRs.
- Review fixes, additional commits, and CI reruns within the same PR retain that PR's version. Include the assigned version in its description and check previously assigned versions within the same milestone to avoid reusing a build number.
- Milestone 2 retains its previously issued versions through `0.2.14`; earlier unversioned PRs remain unversioned. The first milestone-3 step uses `0.3.1`.
- Set the completed roadmap release to `1.0.0`. Framework versions do not change configuration, mapped-format, schema, or synchronization protocol versions automatically.
- See [the roadmap's versioning policy](ROADMAP.md#versioning) for examples.
