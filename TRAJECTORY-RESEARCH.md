# Trajectory integration research notes

## Verified against 2.2081 Geode bindings

The official `bindings/2.2081/GeometryDash.bro` declares `GJBaseGameLayer::m_objects` and `m_collisionBlocks`, `GJBaseGameLayer::checkCollisions(PlayerObject*, float, bool)`, `collisionCheckObjects(...)`, and player collision methods. `GameObject` exposes `getObjectRect()`, `getOrientedBox()`, `getObjectRotation()`, `m_objectType`, `m_isDisabled`, `m_isGroupDisabled`, `m_isTrigger`, and `m_slopeIsHazard`. `PlayerObject` exposes `m_yVelocity`, `m_yStart`, `m_gravity`, `m_gravityMod`, `m_speedMultiplier`, `m_playerSpeed`, `m_isUpsideDown`, ground/mode flags, and `m_gameLayer`. `GameObjectType` values identify hazards, animated hazards, solids, slopes, orb/ring types, pads, and gravity portals.

Relevant callable declarations include `PlayerObject::getCurrentXVelocity()`, `getYVelocity()`, `getObjectRect()`, `getObjectRotation()`, `updateJump(float)`, `pushButton(PlayerButton)`, and `releaseButton(PlayerButton)`. The bindings prove signatures/availability for iOS 2.2081; they do not prove the safety of calling mutating collision routines on a shadow player.

## Physics source reference and applicability

The public `camila314/gdp` decompilation has a `PlayerObject::updateJump.cpp` reconstructed from Geometry Dash 2.2-era code. It shows the cube/robot ground jump is gated on grounded + buffered input and uses `m_yStart` (halved for robot; scaled for mini), while gravity and robot hold-boost depend on several mode/state variables. Its `GJBaseGameLayer_update.cpp` shows simulation steps interleave trigger processing, player updates, collision checks, and post-collision actions. This is useful for implementation structure, but it is not proof of exact 2.2081 iOS behavior.

## Integration constraints

- Native `checkCollisions` and `collisionCheckObjects` may mutate player fields, game objects, trigger state, player death state, and world state. Do not call them speculatively on the live player.
- `getObjectRect()` and `GameObjectType` can support read-only snapshots, but static AABB approximations are not enough for exact slopes, rotated geometry, dynamic groups, moving objects, or every hazard hitbox.
- Interaction categories are available via `GameObjectType`, but exact orb/pad impulses, portal transitions, activation ordering, and all modes still need verified rules before predictions can control inputs.
- The user-supplied iPad diagnostic screenshot confirms the current tested build has active `pushButton` / `releaseButton` / `PlayerObject::update` hooks. It also shows 9,999 command samples (`dt=0.041667`), 8,245 player update samples (`dt=0.25`), and 8,267 `updateJump` samples. This supports using the native player-update path for further instrumentation but shows update and command callback counts are not identical.

## Sources

- Official bindings: https://github.com/geode-sdk/bindings/blob/main/bindings/2.2081/GeometryDash.bro
- Official enum declarations: https://github.com/geode-sdk/bindings/blob/main/bindings/include/Geode/Enums.hpp
- Community 2.2 decompilation: https://github.com/camila314/gdp/tree/main
- Reverse-engineering handbook: https://docs.geode-sdk.org/handbook/vol2/chap2_1/
