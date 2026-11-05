# rf-packer

A small window to make an `.rfx` file from captures, or to open one, read what it holds and change its
tracks: add, remove, reorder, code again, and edit the metadata. It is in the style of rf-analyse
(and of ld-analyse and ld-stacker-gui), with the same **View** menu: Theme and Style.

```bash
rf-packer                                  # an empty list
rf-packer disc.rfx                         # the tracks of disc.rfx
rf-packer disc.rfx capture.s16             # an archive and a capture to add to it
```

Files can also be dropped on the window: an `.rfx` file opens, captures are added at the end.

## The window

| Part | What it does |
| --- | --- |
| **Create RFX…** / **Process…** | At the top. **Create RFX…** is what the button is called while the list is new (no archive opened): it writes the list. **Process…** is the same for an opened archive: it asks whether to **write a copy** or **overwrite** the file (overwriting is for RFX only) |
| **RFX / FLAC / Raw** | Next to them: how the list is written. **RFX** uses the predictor (the smallest file; the tracks keep their notes and metadata). **FLAC** writes an `.rfx` file that audio programs open (larger, needs ffmpeg). **Raw** is the same Matroska file with the samples stored as they are, as 16-bit PCM (as large as the captures, nothing to decode, needs ffmpeg; the names, notes, rate, bits and files that go with the tracks are kept, as in the predictor variant). Both are the *variants* of the format that the specification (section 9) describes |
| **Sample rate** and **Bit depth** | Above the list, for the whole list: what the samples contain. They are told once when the list is new (a capture's `.yaml` gives the rate), and are written in the metadata (`rate_hz`, `bits`). In an opened file they are shown and cannot be changed |
| **Open RFX…** | Loads the tracks of an `.rfx` file into the list, of the predictor variant or of the FLAC or raw one (the codec menu then shows which, and choosing another converts the list: a file of the predictor is coded again) (it asks first if the list has changes that were not written) |
| **Add track…** | Adds captures (`.s16 .r16 .s8 .r8 .lds .flac .ldf`) at the end of the list |
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
| **Notes** | A free text, stored in the metadata of the track (`notes`) and shown by rf-analyse too. It can be edited, and **Delete note** takes it away. Nothing is written until the list is processed |

Changing only the name, the rate, the files or the notes copies the compressed blocks of the track as
they are: nothing is coded again.

## Writing

**Create RFX…** and **Process…** ask for the file (and, for an archive in RFX, a copy or an overwrite), the
level of the predictor (1 to 4, for all the tracks), the device (processor or graphics card), the threads, whether every segment is decoded
again and compared with its samples, and the rate of the captures that say none. When captures are added
to other tracks, the lines of the captures are located first, from their RF, by the program itself (no ld-decode is needed): it is the pass 0 of ld-decode, ported to C++, and gives the same positions.

The work is done by the program itself, started again with `--job`: the window shows its messages and a
progress bar, and **Cancel** stops it and removes what it had begun. The file is written aside and takes
its name when it is complete, so overwriting an archive never leaves half a file. What was written is then
what the list shows.

The tracks are written in the order of the list. The references of a track are the tracks before it that it
is predicted from; a track whose references change is coded again.
