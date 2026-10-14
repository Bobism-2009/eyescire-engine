Eyescire UI backup — taken before the modern-UI experiment.

Snapshot of files the skin work is based on (production copies stayed in place):

  color.nxa     <- engine/color.nxa
  theme.nxa     <- engine/theme.nxa
  ui.nxa        <- engine/ui.nxa
  editor.nxa    <- examples/editor.nxa

To restore production from this backup if anything in engine/ or examples/
was overwritten (the modern experiment should NOT touch those):

  copy color.nxa  ..\..\engine\color.nxa
  copy theme.nxa  ..\..\engine\theme.nxa
  copy ui.nxa     ..\..\engine\ui.nxa
  copy editor.nxa ..\..\examples\editor.nxa

The modern skin lives only under test\modern-ui\ and builds a separate
editor_modern.exe. Deleting that folder (or ignoring that exe) leaves
the normal editor unchanged.
