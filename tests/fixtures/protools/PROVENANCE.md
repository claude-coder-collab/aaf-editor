# Pro Tools multichannel exports

Synthetic test sessions exported from Pro Tools 26.4.1 (macOS) for issue #25 by the project owner and
contributed for testing. They contain no real programme material. The referenced WAV media is not included.

Session: 24 fps, 48 kHz / 24-bit, with mono, stereo, 5.1 and 7.1 tracks, a split multichannel clip, crossfades,
a fade-in and clip gain. Exported with File → Export → Selected Tracks as New AAF/OMF:

| File | Enforce Media Composer compatibility | Stereo/5.1/7.1 as multi-channel | Media | Notes |
|---|---|---|---|---|
| `multichannel_wav.aaf` | on | on | WAV, consolidated, 1000 ms handles | cuts not frame-aligned: "Sample accurate edit" clips |
| `split_mono_wav.aaf` | on | off | WAV, consolidated, 1000 ms handles | each channel a mono track |
| `split_mono_linked.aaf` | off | (unavailable) | linked to source media | each channel a mono track |
| `multichannel_embedded.aaf` | on | on | embedded | cuts not frame-aligned |
| `multichannel_frame_aligned.aaf` | on | on | WAV, consolidated, 1000 ms handles | frame-aligned edits: fades as "Fade " clips, no "Sample accurate edit" clips |

How Pro Tools encodes these is described in SPEC §6 ("Multichannel audio").
