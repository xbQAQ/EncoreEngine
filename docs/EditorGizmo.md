# Editor Gizmo

The viewport toolbar provides translation, rotation and local-axis scaling. World and Local select the translation/rotation frame. Scale stays local because the ECS Transform stores translation, XYZ Euler rotation and diagonal scale, without shear.

Click visible geometry to select its owning entity. The World tab and viewport share selection. A light-owned ceiling mesh selects its analytic light; generated rectangle and disk emitters also select their light. Light scaling is disabled because the renderer consumes Width and Height from the Light component. The active view camera has no viewport transform handles.

Drag an axis, plane or center handle to manipulate the selection. Q selects without a transform widget; W, E and R choose translation, rotation and scale while the viewport has focus. Space cycles the transform tools; lights skip scale. Holding the right mouse button retains camera navigation, and Alt reserves mouse gestures for orbit, pan and dolly. Snap applies the displayed increment, and Ctrl temporarily enables it. These are increment snaps, not vertex snapping.

One complete Gizmo drag creates one undo transaction. Escape restores the pose at the beginning of a drag and creates no transaction; Ctrl+Z during an active drag cancels that drag without replaying an older command. Losing viewport focus ends the interaction and commits its last valid pose. Import, deletion and scene replacement release outstanding interactions. Tool shortcuts, camera controls, duplicate/delete and history behavior are documented in [EditorControls.md](EditorControls.md).

## Coordinate Contract

EditorGizmo.cpp converts the engine's right-handed X-then-Y-then-Z Euler rotation directly into column-major matrices. It does not use ImGuizmo's Euler decomposition helpers. The view faces positive Z and uses the same vertical FOV, aspect and zero-to-one perspective depth as the renderer. Picking and manipulation use the displayed image rectangle after toolbar layout and aspect-preserving letterboxing.

Mesh handles pivot around the immutable source AABB center. Matrix translation is the transformed center. Conversion back to ECS compensates the original vertex-space origin, so rotating or scaling an imported object leaves its visible center fixed. Reflected scale signs are retained; singular transforms, shear and nonfinite output are rejected.

Picking reuses scene extraction and Bvh::Trace on an unsorted leaf. ExtractRenderScene optionally returns an entity-owner vector aligned with its output triangles: the first mesh instance occupies its original primitive range, shared-resource clones append triangles, and generated emitters identify their analytic light directly. Picking resolves the exact output owner rather than guessing from immutable source ranges, so moved or mirrored clones remain independently selectable. The ownership vector is published only after successful extraction and is not a GPU ABI field. Analytic-light tags in c.w still serve renderer light identity independently of editor picking.

Surface acceptance follows SceneQueries: opaque single-sided backfaces are skipped, double-sided and transmissive backfaces remain pickable, and MASK coverage multiplies material opacity by the base texture alpha. UV interpolation, texture scale/rotation/offset, U/V wrap and bilinear filtering follow MaterialSampling. Clone triangles use their private transformed attributes, including mirrored UV corner associations. Rejected candidates leave accepted coplanar surfaces available for selection. Point, spot and directional lights can be selected in the World tab; they do not gain pickable viewport geometry from this feature.

This is a single-selection perspective editor. Orthographic views, vertex snapping, editable temporary pivots and Alt-dragging a transform widget to duplicate are not implemented. Alt+LMB is reserved for orbit, and duplication uses Ctrl+D.

## Verification

EditorGizmoTests checks matrix roundtrips against real scene extraction, signed nonuniform scaling, center-pivot compensation at gimbal lock, invalid matrices, rolled-camera projection, letterbox rejection, nearest visible mesh selection, analytic emitter ownership, sidedness/transmission, masked material opacity, coplanar candidates, transformed/wrapped/bilinear texture-alpha coverage, and exact clone ownership with mirrored alpha coverage.

EditorGizmoInteractionTests creates a hidden native window and feeds synthetic mouse and keyboard events into the production EditorUi and ImGuizmo. It checks selection, real press/drag/release behavior, atomic history and native text undo, scene/history invalidation, camera capture exclusion, Q/W/F and RMB/Alt navigation, duplicate/delete followed by real scene extraction, protected cameras and scene replacement. It does not move the operating-system cursor. The interaction test requires the optional DirectX 12 test configuration. Mathematical navigation and sparse history transactions also have standalone CPU tests; actual execution results remain in [Validation.md](Validation.md).

ImGuizmo is fetched from the official repository at revision 105e4fde45e4e74aae8973d98750820ef7aca7e2. It is a CPU editor dependency; GPU composition remains in the final Editor overlay RDG pass.
