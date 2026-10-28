# Troubleshooting

Japanese: [../TROUBLESHOOTING.md](../TROUBLESHOOTING.md)

## MMD fails to start after install

- Run `tools\uninstall_from_mmd.ps1`.  
- Ensure `d3d9_mme.dll` exists.  
- Check `mmd_vr_renderer.log`.

## MMEffect menu missing

- Proxy must load `d3d9_mme.dll` (`Loaded d3d9_mme.dll` in log).  
- Reinstall MME; rename its `d3d9.dll` → `d3d9_mme.dll`, then reinstall this proxy.

## GetRenderTargetData failed

- Current path StretchRect-resolves then SYSTEMMEM readback. Update to latest build.

## Camera probe / multi-view little change

- Do not block the MMD UI thread waiting for Present.  
- Use Camera Override Probe or the render worker.

## Seams / SSR / SSAO on panorama

- Expected for screen-space effects across cube faces. Do not edit ray-mmd.

## Broken draw after render start; cancel does not restore

- Latest builds restore camera and clear hooks on cancel.  
- If still broken: turn off cubemap faces and report with log.  
- `MMDVR_ISOLATE=1..3` for capture bisect.

## Output stuck on the same pose (frame 0)

- Frame seek failed; check `MmdTimeline` lines in the log.

## No audio / YouTube not VR

- Empty “Audio (WAV)” → no mux. Browse manually if auto-detect fails.  
- Spherical UUID is injected when cubemap is ON. Wait for YouTube processing.

## FFmpeg encode fails

- Provide ffmpeg or disable encode. Use a build with `-start_number` for resumed PNG sequences.

## Job resume

- Delete `job.json` in the output folder to start clean.
