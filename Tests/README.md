Run `./Tests/Run-LightRegression.ps1 -EngineRoot <engine-checkout>` from a Visual Studio developer PowerShell.
It builds the editor plugin and inspection tool, then checks the actual document component mapping, unchanged
read-back, edited intensity, emitter resizing, both stage up axes, area-light emission direction, child transforms,
Unreal's point-radius clamp and spotlight cone conversion.

The blue fixture panel has 2 nits over 2 square metres: 12.56637 lumens. Its source nit value stays 2.
Unnormalized area lights store the USD nit value directly in the component's Nits unit, including Unreal-authored
stages. Normalized area lights retain their lumen conversion. Resizing an unnormalized light preserves its nits.

Local lights import attenuation range from these optional extension attributes, in stage units, in priority order:
`plasma:light:range`, `inputs:attenuationRadius`, `unreal:attenuationRadius`, `inputs:range`.
USD `inputs:radius` remains emitter size. Missing, negative or non-finite ranges fall back to Auto; explicit zero
also selects Auto. Edited ranges round-trip through `plasma:light:range` in the participant layer. The range fixture
checks point, spot, rectangle, disk and cylinder lights, centimetre conversion, precedence and saved edits.
The current Bazaar export contains emitter sizes but no attenuation-range attribute, so its ranges remain Auto.

Add `-BazaarStage <Historic_Bazaar_Vol1.usd>` to check RectLight13 and RectLight16 from the real stage through
the document properties, runtime photometric calculation, and scene/prefab save/reload with type migrations.

The runner also compares optimized MikkTSpace results against the engine on ordinary/mirrored/degenerate UVs,
quads and vertex attributes. Streaming tests verify coarse previews arrive before final parts, retain material
groups and source bounds, leave final part bytes unchanged, release serialized buffers, propagate write failures,
and save/reload completed meshes before a prefab exists.

The material regression applies generated version-8 material permutations after an unrelated FULLBRIGHT,
transparent, two-sided draw. It checks opaque, two-sided, transparent, masked and soft-mask USD surfaces. These
flattened materials explicitly select lit metallic/roughness shading, blend mode, sidedness, weather and SSS state;
the engine does not inherit the referenced base material for format version 8. Without those values, persistent
render-context state made their appearance depend on draw order. Run `Bin/Tests/Streaming.exe --material-test`
for this check alone. The test fails against the preceding package build and passes with explicit permutations.

`plUsdInspect <stage> --mesh-profile <prim-path>` compares tangent generation on one mesh, including per-corner
equivalence. `Bin/Tests/Streaming.exe <absolute-test-manifest-path> <Historic_Bazaar_Vol1.usd>` additionally times
capture, the early coarse mesh, the first final part and the complete mesh for the dense clay plate.

Early meshes target 32,000 triangles across material groups and stay visible until all final parts are ready.
The target is flexible: topology-preserving simplification locks surface and material boundaries, retaining more
triangles for thin or disconnected geometry rather than punching holes in it. Previews use the source materials
and swap directly to the finished mesh; no dithered LOD fade is added. Preview version 2 rejects earlier sparse
previews while keeping completed full-quality geometry cached.

World Partition from USD has a **Select all / Select none** toggle beside **Prims**. When all enabled importable
prims are checked it clears them; otherwise it checks them all, including collapsed branches. The label follows
individual checkbox edits. It preserves excluded prims and updates parent checks and variant choices together.
The action is disabled until the stage is read and while a scan or import runs.
Preview jobs run before refinement jobs, so a dense mesh's tangent and meshlet work cannot hold up the next
mesh's first preview. The queue periodically prioritizes object origins near and in front of the active camera;
this is a proximity heuristic, not occlusion or bounds-based visibility. Authored USD LOD variants are not switched.
Final meshes retain full tangent/meshlet/LOD processing.

New mesh files are shared by geometry content, coordinate basis, material partition, conversion version and SDK
version. They contain default material slots; mesh and instancer components carry the actual bindings, including
in baked prefabs. Tangents are always generated for final shared geometry, so adding a normal map does not rebuild
it. Existing completed per-link files remain usable. Geometry-spec identities avoid recapturing and hashing the
same source for different material bindings. Transform, light and material-value changes keep their geometry.

Shared final resources are in `AssetCache/Generated/UsdLive/Geometry`; persistent previews are in
`AssetCache/UsdLivePreviews/Geometry`, outside export. Atomic completion markers include expected file sizes;
missing/truncated parts fall back to the preview and rebuild. Per-link manifests checkpoint every two seconds and
on close, including preview-only entries. Keep these directories to resume an interrupted load. Shared files are
not removed by one link's cleanup; automatic garbage collection of orphaned shared files is not implemented.

Admission checks estimated points before capturing topology, normals and UVs. A process-wide lease prevents
duplicate builds across links and bounds concurrent source points; memory pressure reduces mesh and texture
workers. An oversized mesh runs alone. Superseded jobs stop between build stages or parts. Application of mesh
components is budgeted per editor update and indexed by geometry users. Generated objects are explicitly destroyed
after direct removal, rather than leaking detached components when previews, links or prefabs are replaced.

The runner also executes `Streaming.exe --scheduler <StreamingRegression.usda> <test-output-folder>` in an isolated
project. It checks actual Live Link preview/final transitions, distinct material bindings sharing one resource,
cached reopen, cross-link reuse, transform/material edits without geometry jobs, geometry variant replacement,
cached adoption while the build budget is exhausted, and clean object destruction. Cache tests cover admission, oversized progress,
cancellation before tessellation, checkpoint recovery and truncated files. These tests do not measure Bazaar's
end-to-end viewport load time or GPU residency; repeat that measurement after restarting the editor with the new DLL.

Profiler scopes begin with `USD`: admission, capture/hash, tessellation, preview simplification, tangents, vertex
streams, meshlet LODs/proxies, serialization, file writes and document application. Completion logs report capture
and total task times. The live panel reports visible mesh objects and refined objects rather than just a remaining
queue count. These counters distinguish placements from active geometry builds.

For transform write-back, run `plUsdInspect <fixture> --roundtrip /World/Panel`; this writes a test session beside
the fixture and checks the saved and reverted transform.

`plUsdInspect <stage> --profile 1 tangents` profiles stage reading, geometry capture, content hashing, the first
distinct mesh, tangents, meshlet LODs and proxy generation. This probe holds all captured sources, so its memory
usage differs from Live Link's bounded worker queue. Measure total editor import time separately.
