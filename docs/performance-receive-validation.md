# Reusing receive selection validation during issuance

Milestone 4 step **4.18**, framework **0.4.20**, continues the [performance investigation](performance-diagnostics-findings.md) after [operation-entry validation reuse](performance-operation-validation.md). It removes duplicate topology construction and delivery-history validation between selecting a delivery and issuing it under the same mutation gate. Public application interfaces, MessageHeader/payload lifetimes, stored formats and native protocol versions are unchanged. Integration into `main` remains held for measured improvements and qualification.

## What changes

A **TopologySnapshot** is the private owning model built by `runtime_topology` within one recovered RuntimeAccess. Its construction checks definitions, current relationships and complete delivery history before selection. Previously receive built this model, selected a Source/Queue, then issuance built and validated the same model again while the gate was still held. The preceding first H=128 receive counted two topology builds, two delivery validations and 258 Publication checks (twice the 129 retained Publications).

Receive now passes its already validated model to a trusted private issuance overload. The general issuance function still builds a fresh model. Both paths share the same issuance body and retain native guard/capability checks, Session and consumer-claim checks, Thread-parent checks, fresh Source/Queue/receipt word reads, active-state/counter checks, exact delivery lookup, and journal execution.

The reuse applies to fresh issuance and an existing pin. Source reconciliation completes before selection validation. Allowed-loss evaluation retains its own generic validation: a successful empty result performs no transaction, so issuance can still use the earlier model; a committed gap or error returns from receive before issuance. Post-issuance mapping/binding failure releases through the generic progress path, validating freshly after the mutation.

## Risk assessment

The [project guidance](../AGENTS.md#performance-and-correctness) gives performance and correctness equal priority. Under the supported protocol there is no intervening writer while this unchanged receive operation continuously holds its mutation guard. A second full delivery-history pass repeats established facts at substantial cost.

- The trusted overload accepts only the complete, unmodified model from `runtime_topology(access)` under that same live access and continuously held guard, with no intervening Session mutation. An arbitrary model, a different access or an old model after progress violates this private precondition. It is not a public application API or a cache between receive calls.
- This changes validation frequency in a narrow internal interval. Unsupported raw metadata writes or arbitrary memory corruption between selection validation and issuance may no longer be caught by a duplicate pass. Fresh operations and generic progress functions still validate. The existing coordination model does not promise continuous detection of writers that bypass the gate.
- Fresh claim/parent, pin, Queue and receipt reads remain. Reuse removes complete-model reconstruction, not those checks. Existing pins preserve token, dequeue time and mapping identity; repeat receive still precedes fresh selection/counter checks.
- No validation is reused across a committed gap, issuance, release, acknowledgment, reconciliation mutation, or operation boundary. Allowed-loss null-result semantics are documented because that branch must remain free of transactions. Generic loss evaluation and post-issuance cleanup retain their validation paths.
- Existing deadline checks and Source close fences stay at their current boundaries. A faster validation pass does not create a hard response-time promise. Issuance still commits before payload mapping/binding; failed local preparation still performs exact cleanup or retains discoverable uncertain state.

No mapping or snapshot escapes the operation through this change. Payload views keep their existing independent owner and persisted pin. Generic primitives remain appropriate for qualification fixtures that deliberately modify storage. The accepted tradeoff is trusting the already validated model during this short, unchanged gate-held interval.

## Qualification and measurements

Focused private tests compare trusted and generic issuance/pin-repeat behavior, preserve participant/Queue rejection without writes, and verify generic progress still detects same-root corruption. Existing public receive, allowed-loss/gap, deadline, close, cleanup, recovery, ownership/fork, payload lifetime and full-log tests remain relevant. These private storage fixtures are separate from the primer's application examples.

The unchanged diagnostic workload compares preserved 0.4.19 executables with normal and enabled builds. Fresh Sessions, matched geometry/history/capacity/payload/Field controls, independent thread/process workloads, raw outcomes and source/executable hashes are retained. Normal builds supply timing comparisons; enabled counters explain which work disappeared. Adjacent alternating serial pairs complement sequential full matrices. Plots show observed per-run statistics, not confidence intervals or production-tail guarantees. CI records correctness and observations without timing thresholds.

Measured results, plots and exact provenance are recorded with the PR after qualification.
