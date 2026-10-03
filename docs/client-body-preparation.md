# Native body preparation

Body preparation runs on the main thread with the simulation owner parked or
stopped. It proves that removing visual weapon models preserves the actual
native aim/controller tag bindings, dependency bits, duplicate links, parent
links, and animation-name bindings for each supported full weapon composition.

The dependency-name check previously called `DObjGetBoneName`, then
`SL_FindString`. Native `DObjGetBoneName` walks the object's models in order and
returns `SL_ConvertToString(model->boneNames[localBone])`. The preparation check
now performs that same walk and reads the original `boneNames` ID directly.
Both paths identify the same interned native asset name. The native
`DObjGetBoneIndex` checks on the reduced and full objects are unchanged. No
string-table lock or hash lookup is needed for this conversion.

Each successfully prepared body records the loaded weapon count against which
the complete proof ran. Refresh skips a second identical proof for those new
bodies. When the registered weapon count changes, every body whose recorded
count differs runs the complete proof again and updates its count only after
success. Map/asset teardown releases the bodies and clears this metadata.

These changes remove redundant work; they do not prove that a reported sound
restart stall was caused by a native string lock. Native compilation and live
lifecycle verification remain the build owner's responsibility.
