# rf-packer

A small window to make an `.rfx` file from captures, or to open one, read what it holds and change its
tracks: add, remove, reorder, code again, and edit the metadata. It is in the style of rf-analyse
(and of ld-analyse and ld-stacker-gui), with the same **View** menu: Theme and Style.

```bash
rf-packer                                  # an empty list
rf-packer disc.rfx                         # the tracks of disc.rfx
rf-packer disc.rfx capture.s16             # an archive and a capture to add to it
```

Files can also be dropped on the window: the first `.rfx` file dropped on an empty list opens; the other `.rfx` files, and any dropped on a list that has tracks, have their tracks added at the end; captures are added at the end.

## The window

| Part | What it does |
| --- | --- |
| **Create RFX…** / **Process…** | At the top. **Create RFX…** is what the button is called while the list is new (no archive opened): it writes the list. **Process…** is the same for an opened archive: it asks whether to **write a copy** or **overwrite** the file (overwriting is for RFX only) |
| **RFX / FLAC / Raw** | Next to them: how the list is written. **RFX** uses the predictor (the smallest file; the tracks keep their notes and metadata). **FLAC** writes an `.rfx` file that audio programs open (larger, needs ffmpeg). **Raw** is the same Matroska file with the samples stored as they are, as 16-bit PCM (as large as the captures, nothing to decode, needs ffmpeg; the names, notes, rate, bits and files that go with the tracks are kept, as in the predictor variant). Both are the *variants* of the format that the specification (section 9) describes |
| **Sample rate** and **Bit depth** | Above the list, for the whole list: what the samples contain. They are told once when the list is new (a capture's `.yaml` gives the rate), and are written in the metadata (`rate_hz`, `bits`). In an opened file they are shown and cannot be changed |
| **Open RFX…** | Loads the tracks of an `.rfx` file into the list, of the predictor variant or of the FLAC or raw one (the codec menu then shows which, and choosing another converts the list: a file of the predictor is coded again) (it asks first if the list has changes that were not written) |
| **Add track…** | Adds captures (`.s16 .r16 .s8 .r8 .lds .flac .ldf`) at the end of the list |
| **Add the tracks of an .rfx file…** | In the File menu (and by *Add track…*, which takes `.rfx` files too, and by dropping them): adds the tracks of other `.rfx` files at the end of the list, so that several files, of one track each for instance, are written as one. A track is **copied as it is** wherever it can be, with no decoding and no coding: the blocks of a file of the predictor (when the tracks it is predicted from are in the list before it), and the streams of a file of the FLAC or raw variant (ffmpeg `-c copy`; only the description of the tracks is written again, with the names, notes and files the list says). The files of a list are all of the predictor (with one segment length) or all of the FLAC or raw variant; the panel of information says which file a track is from. A list of several files is written to a new file, never over one of them; the writing is `rfx merge` and `rfx add` of the command line |
| **Unpack all the tracks…** | In the File menu, for an opened file: asks for a folder and gives the captures back there as they were (checked by their SHA-256), whatever the variant of the file. The **⤓** button on a line of a track of the file does the same for that track alone. It unpacks the file on disk, not the changes of the list; the work runs in the program again, so it can be stopped |
| **Compute TBC curve** | The curve the speed of the disc and the time base correction are made from. **File > Compute TBC curve**, for an opened file of the predictor variant, works it out from the RF of every track (locating the lines, the pass 0 of ld-decode) and keeps it in the file, as a block added at its end (specification 3.1; nothing is coded again): rf-analyse then shows the speed and aligns the captures at once, with no minutes of work. The tracks are worked on together; the work runs in the program again, so it can be stopped. The same is asked for when a file is written, by the box *Compute TBC curve* of the dialog of the options (off to begin with, remembered). For **one track**, the **Compute TBC curve** button of the panel of information (beside *Check this track*) does the work for the track picked alone; the panel says, in its *TBC curve* line, whether the file keeps it for the track (and the system and the number of fields), and the curve a file keeps goes along when a track is copied into another file. For the whole file the tracks that have the curve are left as they are (the button of one track works it out again). Quitting asks whether to finish the work when it was asked for (the box of the options, or the menu) and was stopped before every track had it: the curve is kept in the file as soon as it is worked out, so there is nothing to save. When the lines found are not those of the standard said for a track, a box lists those tracks, each with a list of standards to choose from and a cross to leave it as it is; **Update** changes the standard of the tracks in the list, to be written with the file (nothing is asked when no standard is said or it agrees) |
| **Check** | The integrity of the file: **File > Check the file** and the **Check** button beside *Info* decode every track of the opened file and compare it with the SHA-256 of the samples the file keeps (and each stored sidecar with its own); **Check this track** in the panel of information does the same for the track picked. It works for the three variants of the file, the tracks are decoded together (a few minutes for a file of several gigabytes), the work runs in the program again so it can be stopped, and the result says that all is intact or which tracks are not and why. Nothing is written |
| The list | A line per track with its number, as in the list of ld-stacker-gui: a bar of the colour of the track, its name, what it takes, and how it stands: *kept*, *edited* (only its metadata changes), *recode* (it is predicted from tracks that are no longer before it, so it is coded again by itself), *new* or *removed*. The arrows move it, the cross removes it (a track of the archive that is removed stays in the list, struck out, until the file is written, and can be kept again) |
| **Info** | A panel on the right, which widens the window when it opens and narrows it when it closes (key `I`). It holds everything known of the selected track, and some of it can be changed |

### The Info panel

| Field | |
| --- | --- |
| Name | Editable for a track of the archive (the name of a new capture is the one of its file) |
| Rate | Shown only: it is the one of the list |
| Captured | When the capture was made, from the `.yaml` that sits beside it (`started`, `finished` and `duration_seconds` of the Domesday Duplicator) |
| File date | The date of the capture file when it was packed |
| Format, size, stored, samples, SHA-256, references | What the archive says of the track |
| Files that go with it | The sidecar files (the `.yaml`, the `.json`): **Add…** and **Remove** |
| **Standard, Reference, Disc copy, Capture no., Face, Signal** | What the person knows of the disc and of how the capture was made (nothing is guessed from the samples), for each track, in the panel of information: the **standard** (PAL, NTSC, PAL-M, SECAM, NTSC-VHD), a **reference** of the disc (a catalogue number, a title), the **number of the physical copy** of the disc (*Disc copy*, the exemplar), the **number of the capture** (*Capture no.*: one copy can be captured several times, so the two are not the same), the **face** (A, B, AB or BA) and the **signal** (CVBS or Y/C: two buttons, a click on the one that is on says nothing again). Each is "not said" to begin with. They are stored in the metadata of the track (specification 3) and shown by rf-analyse. Like the name and the note, changing them copies the blocks of the track as they are; a new capture gets what is said when it is coded. `rfx pack` and the editing commands say them with `--standard`, `--reference`, `--copy` (or `--disc-copy`), `--capture-number`, `--face` and `--signal`, and `rfx tag` changes them in a file |
| **Notes** | A free text, stored in the metadata of the track (`notes`) and shown by rf-analyse too. It can be edited, and **Delete note** takes it away. Nothing is written until the list is processed |

Changing only the name, the rate, the files or the notes copies the compressed blocks of the track as
they are: nothing is coded again.

## Writing

**Create RFX…** and **Process…** ask for the file (and, for an archive in RFX, a copy or an overwrite), the
level of the predictor (1 to 4, for all the tracks), the device (processor or graphics card), the threads, whether every segment is decoded
again and compared with its samples, and the rate of the captures that say none. When captures are added
to other tracks, the TBC curve of the captures is worked out first, from their RF, by the program itself (no ld-decode is needed): it locates the lines as the pass 0 of ld-decode does, ported to C++, and gives the same positions.

The work is done by the program itself, started again with `--job`: the window shows its messages and a
progress bar, and **Cancel** stops it and removes what it had begun. The file is written aside and takes
its name when it is complete, so overwriting an archive never leaves half a file. What was written is then
what the list shows.

The tracks are written in the order of the list. The references of a track are the tracks before it that it
is predicted from; a track whose references change is coded again.
