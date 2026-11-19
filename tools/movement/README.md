# Movement test

A scripted check of how Kat moves for a given stick push, comparable across hosts (the Windows
PC port running the game natively, the Mac ARM64 host, the Switch 2).

- `install_save.py <decrypted save folder> <user dir>`: a decrypted PS4 save (data files plus
  `sce_sys/param.sfo`) as the runtime's save directory, `<user dir>/savedata/1/CUSA01113/<dir>`.
- `drive.py --log run.log -- <host command>`: runs the host with the movement log
  (`GOW3_MOVE_LOG=fast`) and scripted input (`GOW3_PAD_FILE`): cross on the title screen until
  the save's area loads, then up, down, left and right for 2 s each, twice, with no buttons.
- `diag.py run.log`: the share of running samples moving exactly along the world x = z
  diagonal. The ARM hosts show 44-63% (X and Z speeds equal: the movement bug); correct
  movement should show few.
- `dirs.py run.log`: each push, how far Kat moved and at what angle from the camera's forward
  (up should be near 0, down near 180, right and left near +-90).

The log's `Move:` lines come from the camera probe (`runtime_camera_probe_install`, in
`src/runtime_pad.c`): camera position, the point it looks at (Kat), and the stick values sent.
