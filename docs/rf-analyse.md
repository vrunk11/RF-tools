# rf-analyse

A window to look at the RF captures stored in an `.rfx` file, or at a capture on its own: the
waveform, the spectrum, the noise and the drift of every track, without unpacking anything. It is in the style of
ld-analyse and ld-stacker-gui.

```bash
rf-analyse disc.rfx
rf-analyse disc.rfx other.rfx capture.s16 --rate 40     # several files together
```

Or open a file with **File > Open RF file…**, or drop it on the window. The file chooser offers
every kind of file it opens:

| File | What happens |
| --- | --- |
| `.rfx` | An archive of the predictor variant (what `rfx pack` writes) says what it holds: it opens at once. The FLAC and raw variants (what `rfx pack --flac` and `--raw` write) open as well: every track is a stream of the Matroska file, and the samples are read exactly where they are asked for (the raw one straight from the file, the FLAC one from the frames that hold them), so they have no reference and no stored lag; the file chooser, the metadata and the *From RF* speed work as for a capture |
| `.s16` with a `.yaml` | A capture of the Domesday Duplicator has a `.yaml` of the same name beside it that says its format ("signed 16-bit") and its sample rate: it is opened at once, with no question. The bits are found from the samples |
| `.s16 .r16 .s8 .r8 .lds` | A raw capture with nothing beside it that says its format, so a dialog asks for it: the **format** (chosen from the extension), the **sample rate** (from the `.yaml` that sits beside the capture when there is one, else what was used last, else 40 MHz) and the **bits** the samples carry (found from the samples, or given; fixed at ten for `.lds`) |
| `.flac .ldf` | Opened at once, without asking and without writing anything: the length and the rate come from the header of the FLAC (which holds the rate in kHz: 40000 for 40 MHz) or from the `.yaml` beside it, and the samples are decoded where they are looked at, by ffmpeg (or `FFMPEG=path`), which jumps to them (a few tens of milliseconds); the stretches decoded last are kept in memory (about 100 MB). While the spectrum at the bar is being worked out a rough one of the whole capture, made in the background from a short stretch at 128 places, is shown, marked as rough. The dialog only comes if the header says no rate |

A capture is one track with no reference: the *Source* panel shows its file and samples (no stored
size, hash or coding) and the speed deviation says there is nothing to compare it with.

**Several files can be selected at once** in the file chooser (Ctrl or Shift, or the files given on
the command line, or dropped together): their tracks come one after the other in the list, the
captures among them asked for in a single table (a row each: format, rate, bits), and everything
is shown together. An `.rfx` file keeps its references among its own tracks, and a capture has
none. The timeline is the one of the first rate and the longest track; the status bar says how
many files are open.

Only what is on the screen is decoded: the samples of a window, and of the references a track is
predicted from, come from the segments that hold them (see `TrackReader`), kept for the next
move. A file of many gigabytes opens at once.

## The window

| Where | What |
| --- | --- |
| Left | **Tracks**: a check box (show or hide the track in the views), the colour of the track, its name, and its duration and what it takes in the file. A click on a track selects it and shows its metadata; a click on the selected track hides them |
| Next to it | **Source**: the metadata of the selected track, in sections that fold: *File* (name, size, size stored in the `.rfx`, SHA-256, sidecar), *Capture* (when it was captured, from the `.yaml`; the date of the file when it was packed; and the notes, a free text that [rf-packer](rf-packer.md) can write), then a section for each part of the `.yaml` that has something in it (*Sidecar*: the version of the application; *Capture settings*: the input range, the decimation and the like; *Signal*: the minimum, maximum and rms, the clipped samples; *Requantization*, *Board*, *Device*, *Player*, *Disc*), *Samples* (duration, count, rate, format, shift and mean, bits carried) and *Coding* (order, references and weights, predictor, speed at the bar and its range, lag at the bar, origin) |
| Right, tabs | **Combined**: every track shown, superposed. **Individual**: *Selected* shows the track selected alone; *All lanes* one lane for each track shown |
| Zoom bar | Zoom out, zoom in, *Fit*; the width of the window in samples and microseconds; the vertical zoom of the waveform (×1 to ×8) |
| Bottom | The navigation bar, and the status bar with the duration and the size of the file against its captures |

### The views

- **RF waveform**: the samples of the window, in the units of the converter (±512 for ten bits).
  A window longer than the screen shows the extremes of each column.
- **Spectrum**: of the stretch the bar or the zone stands for, averaged over Hann-windowed frames
  that overlap by half (FFT of 1,024, 4,096 or 16,384). 0 dB is a sine that fills the range. The
  shaded band, 0 to 14 MHz (or 1.5 MHz short of the Nyquist limit), is the one the statistics are
  made over.
- **The pass 0 curve.** Two graphs need to know where the lines of a capture are: the speed of the disc, and the time base correction. Both
  come from the same thing, the **pass 0 curve** of the capture: pass 0 (the one of ld-decode, ported: the same positions to the bit)
  locates every line of the capture from its RF, with the frame code of each field. NTSC is tried first, then PAL. It takes minutes on a
  large capture (a track coded against others needs them decoded first), so it is **never done unasked**, and only for the tracks that are
  asked for: the box *From RF* of the speed graph and the box *Align capture* of a waveform both ask for it, and once a track has its
  curve neither works it out again. The tracks asked for are worked on together and share what they decode (`--rf-speed-all` and
  `--align` start every track on the command line). Meanwhile the speed graph says so in place of its curve, the list of tracks has a bar
  along the foot of each track being worked on (filled as far as it is done, dotted while it waits), and the status bar gives the number
  of tracks and how far they are. The curve is kept until the files change. A curve that is worked out can be kept in the file: **File >
  Save speed curve** (for an `.rfx` file of the predictor variant) adds the lines of pass 0 to the end of the file (a few megabytes at
  most, nothing else is rewritten; specification 3.1) and they are there at once when it is opened again; rf-packer does the same for a file
  (**File > Compute the pass 0 curve**, or the box of the options when it writes one).
- **Align capture** (a box in the heading of each waveform: of the combined view for the tracks shown, of a lane for its track): the
  time base of each capture corrected, as before stacking them. The signal is resampled (a cubic through four samples) so that each
  line is at its place at the nominal speed, and the place is the same in every capture: a field takes the place the field with the
  same frame code has in the first track, so that the captures are lined up with one another (on a test, the correlation of two captures
  over a window goes from 0.01 to 0.998). The waveform, the spectrum and the statistics are all those of the corrected signal. It needs the
  pass 0 curve of the track and of the first one (which gives the axis): the program asks whether to work out the ones that are missing,
  for the tracks shown only (the first one is worked out too, when it is not shown, and the message says so), and the box takes effect
  when it is done. The box stays on for the tracks that are shown afterwards: a track ticked in the list while it is on is aligned too, and
  asked for when it has no curve. The captures do not all begin at the same place of the disc, and on the corrected time one that began later has nothing before it: once the tracks shown have their time base, a window that lies before the place where all of them are is moved to it.
- **Speed deviation over time**: how fast or slow each track runs, in parts per million, over the whole capture, from its pass 0 curve: at
  each field, the spacing of the lines against the nominal one (63.556 us NTSC, 64 us PAL), averaged over a few hundredths of the
  capture. Above 0 is faster, below is slower. It is **not there until asked for**: the **From RF** box in the title bar of the block
  (off to begin with; `--rf-speed` on the command line, for the track of `--track`) asks for the curve. Any track can have one, the
  reference and a capture on its own included. The box of a lane (individual view, all lanes) is about its track; the one of the combined
  view is about every track shown (untick a track in the list to leave it out), stays on for the tracks ticked afterwards, and the
  combined view draws every track that was asked for. The lag tables stored in an .rfx file are relative to the reference (not to the
  nominal speed), so they are not drawn (the metadata panel still shows the lag). A click on the graph puts the bar there.

  **Range** (a menu in the title bar, or the wheel over the graph) sets the amplitude of the graph: auto fits the largest deviation, or a fixed
  range of 5 to 100000 ppm above and below 0 (the wheel forward zooms in, backward out). It is remembered.
- **Statistics**: for every track, over the same stretch as the spectrum:

  | | |
  | --- | --- |
  | Mean amplitude | The mean of the absolute value of the samples, once their mean is taken off, in LSB of the converter |
  | SNR | The mean power of the band over its noise floor, in dB |
  | Noise floor | The RMS of the white noise that has the power of the band's floor (the median of its bins, corrected for the frames averaged), in LSB |
  | Effective bits | (SNR − 1.76) / 6.02: the bits that carry something above the noise |

  The noise floor is estimated as the requantiser of [rf-requantize](rf-requantize.md) does, so
  the two can be compared: the bits effective here against the bits carried says how many the
  requantiser could cut.

### The bar and the zone

The **bar** is the vertical line in the waveform. The spectrum and the statistics are those of the
stretch of the FFT size around it. A **zone** (a drag) replaces it: they are then those of the zone,
and the headings say which (`bar 0.2048 ms · 4,096 samples` or `zone … to … · n samples`).

| | |
| --- | --- |
| Click on the waveform | Puts the bar there |
| Drag on the waveform | Selects a zone |
| Wheel | Scrolls in time, an eighth of the window a notch |
| Ctrl + wheel | Zooms, around the bar |
| Shift + drag | Pans the window |
| Navigation bar | The slider spans the window of the waveform and moves the bar inside it, one to one; the buttons go to the start of the capture, to the previous and next window, and to the end of the capture, taking the bar with them. Tracks of different lengths share the timeline of the longest: a shorter one is drawn up to its end (a dashed line marks it), and its statistics say *past its end* beyond. *off screen* says the bar is out of the window |
| Click on the deviation graph | Puts the bar there, and the waveform follows |
| `I` | Shows or hides the metadata |

## Command line

```
rf-analyse [options] [file...]
```

| Option | |
| --- | --- |
| `--format <format>` | The format of a raw capture: `s16`, `r16`, `s8`, `r8` or `lds` (from its extension when not given) |
| `--rate <MHz>` | The sample rate of the raw captures. With it they are opened without the table |
| `--bits <n>` | The bits a raw capture carries, 1 to 16; found from the samples when not given |
| `--view <view>` | The view to start with: `combined`, `individual` (the selected track) or `lanes` |
| `--screenshot <file.png>` | Write what the window shows to a PNG file and quit: for documentation and tests, also on a machine with no screen (`QT_QPA_PLATFORM=offscreen`) |
| `--size <WxH>` | The size of the window, such as `1280x820` |

## Limits

- The sample rate of an archive comes from the file (the `.yaml` the capture came with, or the rate
  given to `rfx pack`); of a capture, from what was asked. When an archive does not say, 75 MHz is
  assumed, and the status bar says so.
- The statistics are made over at most 4,194,304 samples; a longer zone is taken from its middle.
- A spectrum of a short stretch (a few thousand samples) is noisy, as any single FFT: choose a
  longer zone to smooth it.

## Look

The buttons use the icons of ld-analyse and ld-stacker-gui (drawn from their SVG, kept in `src/analyse/Graphics`, as PNG so that Qt SVG is not needed), and **View > Theme** and **View > Style** are those of the two programs: Auto, Light, Dark, Soft light, Dim, Dark grey and a Custom theme with its editor, and the styles of the machine. The choice is kept in the settings; `--force-dark-theme` overrides the theme for one run. The code of the themes comes from ld-decode-tools (GPL-3.0-or-later, Simon Inns and contributors).
