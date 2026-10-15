Jolt Demo — 3D rigid-body playground
====================================

Open this folder in the Eyescire editor (File → Open Project), then press Play.

What you should see
-------------------
* Cubes, spheres, and capsules falling under gravity (Jolt, not Box2D)
* Stacking / bouncing collisions in full X/Y/Z
* A controllable player cube on the floor

Controls
--------
  WASD / arrows   move on the floor plane (X and Z)
  Space / W / Up  jump
  E               shove forward (impulse)
  R               rain more crates / balls from above
  G               blast nearby dynamics away from the player
  F               freeze / unfreeze the sim (engine pause if bound in editor)

Notes
-----
Scene is RENDER 3D at 3840×2160 with PPM 150. Physics uses Jolt while Play
is active; 2D projects still use Box2D.
