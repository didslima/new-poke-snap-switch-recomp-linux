# Ultrawide (21:9, 32:9)

On a window wider than 16:9, the 3D view gets wider: the vertical field of view stays the same and the sides show more of the world. The interface, menus, videos and photos keep the original 16:9 layout in the middle.

## Setting

| Where | Values | Default |
|---|---|---|
| `SNAP_ULTRAWIDE` | `auto`, `off` | `auto` |
| Launcher and F1 > Graphics: "Ultrawide" (`gfx_ultrawide`) | Auto (wider 3D view), Off (16:9 with side bars) | Auto |

The setting applies with no restart since 2026-11-19: the render targets are made again at the next frame (`runtime/nvn/nvn_retarget.cpp`). The factor is k = window aspect / (16/9), capped at 4. It is read from the window's client size when the game starts (`host/boot.cpp`, log line `ultrawide: auto, window client WxH, factor k`) and again when the row changes; resizing the window alone does not change it mid-run. A window at 16:9 or narrower gets k = 1, and every path then runs exactly as before. Off keeps the 16:9 picture with black bars.

## How it works

Code: `runtime/nvn/nvn_wide.cpp` (the draw rules), `host/mods/lodanim.cpp` (`wide_camera`, the camera; `snap_wide_eval_copy`, the photo Evaluation), plus small hooks in `nvn_draw.cpp`, `nvn_entry.cpp`, `nvn_aa.cpp` and `nvn_fx.h`, each marked `[wide]`.

1. **Camera.** The ride and lab camera builder (`main+0x295170`) loads the aspect into `s9` just before its `tanf` call at `main+0x296400`. The `tanf` hook multiplies that register by k when it holds 16/9, so the projection keeps its vertical field of view and gets k times the width. The CPU camera is widened too, so culling and level of detail cover the new edges and nothing pops in at the sides. Other 16:9 cameras (cutscenes, some menus) are not widened.
2. **Render targets.** Every 16:9 render target (the scene, its depth, the post chain, the window textures) gets an image k times wider (`TextureImpl::wide`; `hostW = ceil(width * scale * k)`). The Evaluation depth target and the photo targets keep their size.
3. **Per draw.** Each draw into a wide target picks how its guest x maps onto the image (`ReplayCtx::wideMode`):
   - **full** (x times k): draws with the widened camera, post passes that sample a wide image, full-screen fills, and full-screen or screen-quarter overlays while the 3D view is wide (dimming, wipes, the viewfinder frame).
   - **narrow** (the middle 16:9): interface draws and 16:9 cameras that were not widened.
   - **effects**: world-space particles drawn with a 16:9 camera through the shared camera buffer (c3) get a widened copy of that buffer, so they line up with the widened world. Set `SNAP_WIDE_NO_C3=1` to keep them narrow.
4. **Photos and scoring.** A photo keeps the projection the ride camera had at the shutter, so at k > 1 it is k times wider than 16:9 (the 72 photo records, 0x550 bytes apart, hold it at aspect 16/9 x k). The Evaluation copies each photo's camera block (0x480 bytes at +0x50 of the shared photo camera) to its stack with `memcpy`: `f_710041c330` (return `main+0x41c438`), and `f_7100418350` on the Score screen (return `main+0x41b334`). It then crops that projection to its 3:2 photo camera (`f_71003975d0`). The host `memcpy` override (`host/hooks.cpp`) calls `snap_wide_eval_copy` first, which multiplies the x scale (m00) of each 16:9 x k projection in the block by k. The Evaluation then works from the 16:9 projection and builds the same 3:2 camera (aspect 1.5) as at 16:9, on the CPU and in its GPU passes. A draw with the widened camera into a 16:9-only target uses mode **unwiden**: the viewport is k times wider around its center, which gives back the original 16:9 projection exactly. A 16:9 target that samples a wide image reads its middle 16:9 at full resolution (`WideCropSubstitute`). Saved photos keep the original framing.
5. **Present.** A frame with a widened 3D draw is shown at full width. A frame with none (menus, photo select, movies, cutscenes) shows the middle 16:9 with black bars (`WidePresentCrop`). When the game sets its own present crop (it does on some loading and menu screens), that crop is mapped into the middle 16:9 too. The decision holds for 4 presents so the bars do not flicker on screens that draw their 3D backdrop every other frame.

## What to expect

- The ride, the lab hub and other widened 3D scenes fill the screen. The HUD stays 16:9 in the middle.
- The rounded viewfinder frame the game darkens around the view during camera moments is stretched with the view (the original draws it at about 12% to 88% of the width and height, and it keeps those fractions).
- Menus, photo select, the evaluation screens and videos show 16:9 with side bars.

## Owner follow-up: 32:9 transitions (2026-11-20)

Observed on a 5120x1440 display (`k = 2`). These are future investigation items; none is implemented or verified yet.

1. **Startup aspect transition.** The game initially renders at 16:9, then switches to 32:9. Investigate whether the widescreen window dimensions and ultrawide factor can be applied before the first guest frame, avoiding the visible switch.
2. **Gray panel after “Press any button to start.”** A gray panel appears centered in the 16:9 area and disappears after the switch to 32:9. Investigate extending its gray background across the remaining left and right screen areas while preserving its centered content. Reference: [`ultrawide-startup-gray-panel.png`](screenshots/ultrawide-startup-gray-panel.png).
3. **Course intro / Skip screen.** Before ride control begins, the screen with the Skip button renders at 16:9. Investigate widening that scene or transition to avoid the brief pillarboxed presentation.
4. **Professor photo evaluation.** The evaluation itself is expected to remain 16:9. However, a very brief 32:9 frame appears during the transition and causes a noticeable flash. Investigate keeping the present crop at 16:9 throughout the transition, without changing the evaluation view.

## Known limits

- **Photo records keep the widened projection.** Only the Evaluation's copy is changed; the records themselves still hold aspect 16/9 x k. Check (Florio, `score_time_a`, memory scans after the Score screen): at k = 2 without the fix, the Evaluation cameras were at aspect 3.0 (m00 1.02589, m11 3.07768). With the fix, no camera at aspect 1.5 x k is left. The Evaluation cameras read m00 2.05179, m11 3.07768, m8, m9 and m10 0, m14 0.01, the same values as an Off run (k = 1), and 5.84533 / 8.768 when zoomed. Scores were not compared on the same photo: a save state cannot change k, and rides are not repeatable.
- **What reads the records back (checked 2026-11-16, Florio `prog_resnap`, Auto at k = 2, run `0-20261116_050734`): nothing shows them stretched.** The album grid, the photo viewer and the Re-snap view draw at 16:9 (the `[wide]` stats count 0 `unwiden` draws for the whole run, so no widened camera drew into a 16:9-only target). The Re-snap photo scored 2,370 against 2,369 for the original shot. The HD photo export (`photos_hd`, 2160x1440) is a 3:2 crop of the 16:9 scene and looks normal. The Professor screen goes through the same Evaluation copy as photo select, so it gets the fix. The save files hold no projection matrix, so a later switch to Off cannot stretch older photos.
- Post effects that use a horizontal aspect constant (the edge focus blur, vignette and lens-distortion tonemap) keep the 16:9 constant over the wider image, so their shape is stretched by k.

## Developer switches

- `SNAP_WIDE_LOG=1`: one summary line per present change; `=2`: each distinct draw decision once (target, rule, camera kind, box, sampled sizes, fragment shader key); `=3`: widened-camera draws too. `SNAP_WIDE_LOG_FROM=<present>` starts the log later.
- `SNAP_WIDE_TRACE=1`: logs each caller that builds a camera at a widened site.
- `SNAP_WIDE_SITES=0xOFF:reg,...`: extra camera `tanf` sites to widen (main offset and the float register that holds the aspect).
- `SNAP_WIDE_NO_C3=1`: 16:9 effect cameras stay narrow.

## Tests

Florio, route `score_time_a`, at 5120x1440 (k = 2) and 2560x1080 (k = 1.3333). No Vulkan validation errors in any run.
