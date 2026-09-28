# RT64 overlay provenance

`game_frame_matching.delta.json` applies only to RT64 commit
`6f1c2d99a4ea571c139f449c326fd176ba8f3496`, licensed under the MIT License
by the RT64 contributors. The recipe records the normalized upstream file and
span SHA-256 values and stores only the project-authored replacement span.

The overlay independently deduplicates transform spans, tile spans, and
resolved look-at indices inside each RT64 match hash bucket. It preserves the
existing ordered numeric pair sets and all downstream matching code. This is a
performance candidate pending measurement in the Terrydactyland reproduction;
the synthetic test demonstrates set-union equivalence and synthetic work
reduction, not game-frame-rate improvement.

Revision 5 additionally prevents first-workload projection, transform, tile
and look-at matching from using indices originating in another workload. It
rejects scene pairs whose first current workload lacks a sized previous map or
whose previous-workload index disagrees with the scene candidate before scene
distance ranking, so invalid pairs cannot consume scene slots. The inner
`matchScene` guard is retained. For
automatically identified transforms whose vertices are not interpolated, it
admits a previous transform
only when both nonempty local position sequences have equal length and hash.
Those cached geometry hashes are identity guards against pairing different
animated model parts that share a draw-call hash; dynamic local geometry falls
back to the current transform. The player-facing deformation report has not
yet been verified against this candidate.

`transform_upload_guard.delta.json` targets the same immutable pin and its
`src/render/rt64_transform_processor.cpp`. It makes upload select interpolated
world-transform buffers only for workloads that `process` mapped to a previous
workload. Unmapped workloads use their original transform buffer; otherwise
their cleared, empty interpolation buffers could be selected. This does not
establish a causal link to the reported character deformation.

## Projection preservation

`projection_preservation.delta.json` targets the same immutable RT64 pin and
MIT-licensed source, `src/render/rt64_projection_processor.cpp`. Its input and
replacement spans are hash checked by the existing materializer. The dependency
checkout remains unchanged.

When no previous projection is mapped and no debugger camera is applied, retain
the original combined view-projection matrix; apply any aspect adjustment directly
to its X column. Reconstructing a fixed-point combined matrix through its heuristic
view/projection decomposition can change depth enough to clip distant geometry.
The later mapped-projection correction preserves each source combined matrix's
decomposition residual through RT64's interpolation weights when camera and
projection interpolation are enabled. When RT64 skips projection interpolation,
it uses the current source residual for both composed matrices. This keeps the
existing interpolated view/projection motion while restoring the source
combined matrix at the endpoints. The debugger camera path remains pinned.
This addresses a demonstrated numeric gap in the mapped path; the separately
reported high-rate character deformation still needs a paired gameplay
observation before assigning its cause.

The CPU-only regression exercises the real projection processor with observed
numeric matrices, including aspect changes and the unaffected paths. This proves
matrix/depth preservation, not visual acceptance of the Jinjo Village artifact.
