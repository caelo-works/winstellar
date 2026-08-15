# WinStellar — support knowledge base

**This is written for a support agent, not for a user.** Quote it, do not
paraphrase it: the facts here are checked against the shipped code, a paraphrase
is not.

Applies to **0.8.0**. **The viewer's title bar does not show the version** — it
shows the file name and the zoom level. To find out what a user is running, ask
them for one of these:

- **Settings → Apps → Installed apps → WinStellar** — the version is listed there.
- Right-click `C:\Program Files\WinStellar\WinStellar.exe` → **Properties →
  Details → File version**. The **Comments** field on that same tab additionally
  carries the exact build — commit and date, e.g. `0.7.0 (a7048bb1, 2026-08-15)`
  — which is what to ask for when a bug report needs pinning to a build.

**The interface is in English. On 0.6.10 and earlier a few labels in the
inspection windows were in French** — *« Carte du fond »*, *« Visuel »*,
*« Inspecté »*, *« Courbure »*, *« Analyse PSF… »*. **They are English from 0.7.0
onward.** There is no language setting and never was: on those older versions
every user saw the same mixed interface, whatever their Windows language. So a
user quoting a French label is not on a "French version" — they are on 0.6.10 or
earlier. Both wordings are given throughout this document; quote the one matching
their version.

**Never invent a figure, a path, a menu name, a supported format or a
compatibility claim.** If the answer is not in this document, the correct answer
is *"I don't know, I'm passing this to the team."*

- Website: https://winstellar.fr
- Repository and issue tracker: https://github.com/caelo-works/winstellar
- Releases: https://github.com/caelo-works/winstellar/releases

---

## The product card — what WinStellar is

WinStellar is a **native Windows application** for looking at astrophotography
frames — **FITS**, **XISF** and **camera RAW** — without launching a heavy
processing suite.

It is **two things in one install**, and users conflate them constantly:

1. **Three Windows Explorer extensions** — thumbnails, the preview pane, and
   sortable metadata + quality **columns**. They work inside Explorer; there is
   no window to open.
2. **A standalone viewer** — zoom / pan / rotate, auto-stretch, histogram, and an
   inspection suite (star markers, tilt, aberration, background map).

| | |
|---|---|
| Version | 0.8.0 |
| Licence | GPL-3.0 — free and open source |
| Requires | **Windows 10 or 11, 64-bit**. Nothing else — no .NET, no runtime, no separate download. |
| Installs to | `C:\Program Files\WinStellar` |
| Code signing | **Not signed yet.** Windows shows a "publisher unknown" warning on first run. |
| Website | https://winstellar.fr |

---

## Installation — installing WinStellar on Windows

Download **`WinStellarSetup-<version>.exe`** from https://winstellar.fr or from
the releases page, and run it. **It asks for administrator rights** — a
system-wide Explorer extension cannot be installed without them.

A **`SHA256SUMS.txt`** is published next to the installer on every release from
v0.6.5 onward, for users who want to verify the download.

The installer is available in **English and French**. It:

- copies everything to **`C:\Program Files\WinStellar`**;
- registers the Explorer extensions;
- **restarts Windows Explorer** so they load immediately (the screen flickers and
  the taskbar blinks — this is normal and expected);
- adds a Start-menu shortcut, and a desktop icon only if the user ticks the box
  (it is **unticked** by default);
- adds an entry to **Apps & features** for a clean uninstall.

### "Windows says the publisher is unknown / SmartScreen blocks it"

**Expected, and harmless.** WinStellar is not code-signed yet. Tell the user to
click **More info → Run anyway**. It is not a virus warning; it means the
executable has no purchased signing certificate. This is the single most
frequently reported "problem" and it is not a bug.

### "The installer refuses to run"

The installer shows **"Windows 10 or later is required."** on anything older.
WinStellar is **Windows 10 / 11, 64-bit only**. There is no 32-bit build, no
Windows 7/8 build, no macOS or Linux build.

### "I installed it and I don't see anything in Explorer"

The installer restarts Explorer for you, so this is rare — but when it happens:

1. **Sign out of Windows and sign back in** (or reboot). This fixes it in almost
   every case.
2. If thumbnails specifically are still missing, the **thumbnail cache** is
   holding stale entries: run **Disk Cleanup**, tick **Thumbnails**, clean, then
   reopen the folder.
3. Remind them **where** to look: thumbnails need a thumbnail view (Large icons
   or bigger), the preview pane is toggled with **Alt+P**, and the columns must
   be **added by hand** — see the columns section.

### Updating to a newer version

There is no in-app updater and no update notification. The user downloads the
newer installer from https://winstellar.fr and **runs it over the existing
install** — there is no need to uninstall first, and the analysis cache is kept.

**Running the installer is what registers the Explorer extensions.** So when a
release adds support for new file types, those files get thumbnails, preview and
columns **only after the new installer has been run**. If someone updates and a
newly supported format still looks inert in Explorer, ask first whether they ran
the installer, then have them sign out and back in.

### Uninstalling

**Settings → Apps → Installed apps → WinStellar → Uninstall.** It unregisters the
Explorer extensions, restarts Explorer, removes the program files, and **deletes
the analysis cache** at `%LOCALAPPDATA%\WinStellar`. Nothing is left behind, and
no user image file is ever touched.

---

## Windows Explorer — thumbnails, preview pane and columns

WinStellar adds three things to Explorer. They apply to the file extensions
listed in the "Supported formats" section, and to no others.

### Thumbnails

Open any folder of frames and switch Explorer to a thumbnail view (**Large
icons**, **Extra large icons**…). Each frame is rendered with an **automatic
stretch**, so a linear sub that would otherwise be a black rectangle shows its
actual content.

If thumbnails do not appear: sign out and back in, then clear the thumbnail cache
with **Disk Cleanup → Thumbnails**. Windows caches aggressively.

### The preview pane (Alt+P)

Select a frame and press **Alt+P** — that is Windows' own shortcut for the
preview pane, not WinStellar's. The frame is rendered with the same automatic
stretch. FITS, XISF and camera RAW all preview.

The preview pane is **read-only**: no zoom, no controls, no stretch sliders. For
those, the user must open the standalone viewer.

### The metadata and quality columns

**The columns are not shown by default. Windows never shows a new column
automatically — the user must add it.** This is the number-one "it doesn't work"
report, and the answer is always the same:

1. Switch Explorer to **Details** view.
2. **Right-click any column header → More…**
3. Search the list for the column: *Object*, *HFR (px)*, *Stars*, *Exposure (s)*,
   *Filter*, *Focal length (mm)*…, tick it, **OK**.
4. Click the column header to **sort** — sorting a night's lights by **HFR (px)**
   brings the sharpest frames to the top.

The columns fall into **two families that behave very differently**, and the
difference is the subject of its own section below. Read it before answering any
"my HFR column is empty" question.

---

## The columns, one by one — which ones exist

Two families. **Metadata columns** are read from the file's header and are cheap.
**Measurement columns** require reading and analysing the whole image, and they
do not always fill immediately — see the next section.

### Metadata columns (from the header)

Available for FITS, XISF and camera RAW. They fill as soon as Explorer looks at
the file.

- **Object** — the FITS `OBJECT` keyword (target name). Also shown as the file's
  *Title*.
- **Exposure (s)** — `EXPTIME` or `EXPOSURE`.
- **Filter** — `FILTER`.
- **Date observed** — `DATE-OBS`, in UTC. Also shown as *Date taken*.
- **Gain** — `GAIN`.
- **Offset** — `OFFSET`.
- **Sensor temp (°C)** — `CCD-TEMP` or `CCDTEMP`.
- **Telescope** — `TELESCOP`.
- **Instrument** — `INSTRUME`.
- **Bayer pattern** — `BAYERPAT`.
- **Focal length (mm)** — `FOCALLEN`.
- **Focal ratio (f/)** — `FOCRATIO`.
- **Focuser position** — `FOCPOS` or `FOCUSPOS`.
- **Image type** — `IMAGETYP` (LIGHT / DARK / FLAT / BIAS).
- **Software** — `SWCREATE`.
- **Dimensions**, **Width**, **Height** — the image geometry.

**A column stays empty when the keyword is simply not in the file.** If a user's
*Gain* column is blank, their capture software very likely did not write a `GAIN`
keyword. That is not a WinStellar bug, and the fix is on the acquisition side.

### Measurement columns (computed from the pixels)

- **Stars** — number of stars detected.
- **HFR (px)** — median half-flux radius. The focus metric.
- **HFR SD** — standard deviation of HFR across stars (focus / tilt uniformity).
- **FWHM (px)** — median full width at half maximum.
- **Eccentricity** — median star eccentricity (**0 = round, 1 = a streak**).
- **Pixel mean**, **Pixel SD**, **Pixel median**, **Pixel MAD** — image-wide pixel
  statistics.
- **Min**, **Max** — extreme pixel value with its count, e.g. `65535 (51x)`. A
  large count on **Max** flags **saturation**.

**Stars, HFR, HFR SD, FWHM and Eccentricity only appear when stars were actually
detected.** On a dark, a bias or a flat there are no stars: the pixel statistics
still fill, the star columns stay empty. That is correct behaviour.

---

## Why the HFR / Stars / pixel columns are empty — read this before answering ⚠️

This is the most common real question, and the answer is **not** "it's broken".

The measurement columns need the **whole image**. Explorer asks for column values
while the user scrolls, dozens of files at a time — decoding every 50 MB frame on
the spot would freeze the folder for seconds. So WinStellar only computes them in
Explorer when it is cheap, and otherwise waits.

**The rule, per format:**

- **FITS of 32 MB or less** — computed **immediately, in Explorer**. Nothing to
  do. This is the common case for most FITS subs.
- **FITS larger than 32 MB** — **empty until the frame has been opened once in
  the WinStellar viewer.** A 26-megapixel 16-bit sub is about 52 MB, so this hits
  many modern CMOS cameras.
- **XISF** — **empty until the frame has been opened once in the viewer.**
  Explorer reads only the XISF header.
- **Camera RAW (NEF, CR2, ARW…)** — **empty until the frame has been opened once
  in the viewer.** Explorer reads only the first 2 MB, which is the EXIF, so that
  scrolling a folder of NEFs stays instant.

**The answer to give the user:**

> Open the frame once in the WinStellar viewer (double-click it, or right-click →
> Open with → WinStellar). Go back to Explorer, and the HFR / Stars / pixel
> columns are filled in. The result is cached, so it is a one-time cost per file
> — and the cache survives moving or renaming the file.

**This is by design, not a defect.** It is the trade-off that keeps Explorer from
freezing while scrolling. Do not tell the user to reinstall, to re-add the
column, or to clear anything: none of that will fill the column. **Only opening
the file in the viewer will.**

---

## The standalone viewer — opening files and moving around

### Opening a frame

- **Double-click** a `.fit`, `.fits` or `.xisf` file. WinStellar registers itself
  as the default application for those three, and only those three.
- **Camera RAW files keep the user's existing photo app as the default opener** —
  deliberately, so WinStellar does not hijack their photography workflow. To open
  a RAW in WinStellar: **right-click → Open with → WinStellar**.
- **Ctrl+O** inside the viewer opens a file dialog, with filters for *Astro
  images*, *FITS*, *XISF*, *Camera RAW* and *All files*.
- **Drag and drop** a file onto the viewer window.

**← and → move to the previous / next frame in the same folder** (so do PgUp and
PgDn). This is the fastest way to flip through a night's subs.

### The toolbar, button by button

The tooltips are the exact labels the user sees:

- **Open… (Ctrl+O)**
- **Export image (Ctrl+S)** — the floppy-disk button, second from the left.
  From **0.8.0** only; opens the format menu (see the Export section).
- **Previous image (← / PgUp)** · **Next image (→ / PgDn)**
- **Fit to window (F)** · **Actual size (1)** (the `1:1` button)
- **Zoom out (-)** · **Zoom in (+)**
- **Rotate 90° left (Shift+R)** · **Rotate 90° right (R)**
- **RAW** — *No stretch (linear, 0..max)*. The raw linear data. **A linear
  astronomy sub looks almost black in this mode — that is what the data actually
  is, not a bug.**
- **Auto** — *Auto stretch (PixInsight AutoSTF)*. The default, and what makes the
  image visible.
- **Histogram + stretch sliders (Ctrl+H)**
- **Inspection tools (stars / tilt / aberration)**
- **Show / hide measurements (A)**
- **Show / hide FITS headers (H)**

**The stretch is a display setting only: WinStellar never modifies the file it
opened.** From **0.8.0** it can WRITE a new file — see the Export section — but
it always writes a new one and never touches the original.

### Keyboard shortcuts, complete list

- **Ctrl+O** — open a file
- **← / PgUp**, **→ / PgDn** — previous / next frame in the folder
- **F** — fit to window · **1** — actual size (100 %)
- **+ / −** — zoom in / out (the **mouse wheel** zooms too)
- **R** — rotate 90° right · **Shift+R** — rotate 90° left
- **A** — show / hide the measurements panel
- **H** — show / hide the FITS headers panel
- **Ctrl+H** — show / hide the histogram window
- **Ctrl+S** — export the image (0.8.0 and later)

Dragging with the left mouse button **pans** the image.

### The side panels

- **Measurements panel (A)** — a *Metric / Value* table: **Stars**, **HFR (px)**,
  **HFR SD**, **FWHM (px)**, **Eccentricity**, **Pixel mean**, **Pixel SD**,
  **Pixel median**, **Pixel MAD**, **Min**, **Max**. If no star was found it says
  **`Stars   0 (no detection)`**; if the analysis itself failed it says
  **`Status   analysis failed`**.
- **Headers panel (H)** — the full *Keyword / Value* list of the FITS header.
- **Histogram window (Ctrl+H)** — a log-scaled histogram with **three draggable
  handles** (shadows, midtone, highlights) that reshape the stretch live, plus an
  **Auto** button (back to the automatic stretch) and a **RAW** button (back to
  linear). Again: display only — the opened file is never written to.

---

## The inspection suite — stars, tilt, aberration, background

Open it from the **Inspection tools** button in the toolbar. It is a menu with
four entries. **All four are greyed out until an image is loaded.**

### Star markers

Toggles the detected stars as overlays on the image. Nothing opens; the markers
are drawn on the frame itself.

### Tilt diagram…

A small **Tilt** window showing HFR across the field as a deformed square, with
the header **`Tilt xx.x %      Curvature xx.x %`** (**`Courbure`**, French for
*curvature*, on 0.6.10 and earlier — see the preamble). The legend reads
**`HFR (px) · green = sharp  red = soft`**, and
**`HFR (px) · vert = net  rouge = mou`** on 0.6.10 and earlier.

**If it says `No stars analysed`**, no star was detected in the frame. A tilt
diagram cannot be built without stars: a dark, a flat, a heavily defocused frame
or a cloudy one will show this. It is not a failure of the tool.

### Aberration inspector…

An **Aberration Inspector** window: a grid of PSF tiles sampled across the field,
so coma, astigmatism and tilt show up as distorted star shapes away from the
centre. Its toolbar has four buttons:

- **3×3** / **5×5** — the sampling grid.
- **Visual** (**Visuel** on 0.6.10 and earlier) — the actual pixels at each grid
  position.
- **Inspected** (**Inspecté** on 0.6.10 and earlier) — the synthesised PSF model,
  annotated with eccentricity, concentration and the dominant axis (**ref**, **radial**,
  **tang.**, **oblique**).

**`Analysing PSF…`** (**`Analyse PSF…`** on 0.6.10 and earlier) displayed in the
window means it is **computing**, not that it failed. It runs on a background thread and the
window fills in when it is done.

### Background map…

A **Background map** window (titled **Carte du fond** on 0.6.10 and earlier) — an
illumination / sky-background map that reveals vignetting, gradients and amp glow. The readout
line gives the radial drop, the gradient and its direction, the azimuthal
anisotropy and the corner glow.

**If it says `Image too small for the background map.`** (**`Image trop petite
pour la carte de fond.`** on 0.6.10 and earlier), the frame is **smaller than 800
pixels on its short side**. The map is computed on a 48 × 72 cell grid and a small image has too few
pixels per cell to be meaningful. This is a hard limit, not a bug, and there is
no setting to lower it.

---

## Exporting an image — JPG, PNG, TIFF, FITS

**Not available before 0.8.0.** From **0.8.0**, the toolbar has an
**Export** button (**Ctrl+S**) that opens a menu of four formats. **The original
file is never modified** — export always writes a new file, where the user chooses.

**The four formats are not interchangeable, and the menu says so.** Two of them
write what is on screen; two write the data underneath it:

- **JPEG — as displayed (stretched)** — the image exactly as shown, stretch
  applied, 8 bits per channel. For sharing.
- **PNG — as displayed (stretched)** — the same pixels, losslessly compressed.
- **TIFF — linear 16-bit (unstretched)** — the **linear** data, no stretch, 16
  bits per channel. For taking into another editor.
- **FITS — original data (unstretched)** — the original frame: original bit depth,
  original headers, and for one-shot-colour the **raw CFA mosaic**, not the
  debayered image. For putting back into a stacker.

**The rotation the user applied (R / Shift+R) is written into all four.**

**If a user says the TIFF "looks black"**, that is correct and expected: it is
linear data, exactly like the **RAW** button in the viewer. They want JPEG or PNG
if they want it to look like the screen.

**Exporting to FITS may ask a question first.** The exported FITS is re-read from
the original file so it carries the true mosaic and headers. When that is
impossible — the image came from a camera RAW or an XISF rather than a FITS, or
the original was moved, renamed or deleted since it was opened — WinStellar asks
whether to export the processed image instead. Answering yes produces a valid
FITS, but for a colour frame it holds the **debayered and white-balanced** image,
not the raw mosaic; the file records this in its HISTORY. Answering no cancels.

**An unrotated FITS export of a FITS file is a byte-for-byte copy** of the
original. That is intentional: nothing is more faithful than the original bytes.

**A failed export never leaves a damaged file.** Everything is written to a
temporary file next to the destination and moved into place only once it is
complete, so an interrupted export leaves the previous file untouched.

---

## Supported formats and file extensions

**Only these extensions are recognised.** A file whose extension is not on this
list gets no thumbnail, no preview, no column, and the viewer will not open it —
even if the format itself is a camera RAW.

- **FITS** — `.fit`, `.fits`. Mono, one-shot-colour (Bayer, debayered
  automatically from `BAYERPAT`), and 3-plane RGB cubes.
- **XISF** — `.xisf` (PixInsight).
- **Camera RAW** — `.nef` and `.nrw` (Nikon), `.cr2` (Canon), `.arw` and `.sr2`
  (Sony), `.dng` (Adobe), `.pef` (Pentax), `.srw` (Samsung), `.iiq` (Phase One).
- **From 0.7.0, also** — `.cr3` (recent Canon), `.raf` (Fujifilm), `.orf`
  (Olympus / OM System), `.rw2` (Panasonic). These four need a **0.7.0-or-later
  installer to have been run**, since that is what registers the Explorer
  extensions for them — updating by any other means will not do it.

**`.cr3`, `.raf`, `.orf` and `.rw2` do not work on 0.6.10 or earlier** — this
catches people out. If a user with a Canon R-series camera says "my RAW files
don't show up", **it is almost certainly a `.cr3`**. Ask which version they run:

- **On 0.6.10 or earlier** — confirm the limitation. **Updating to 0.7.0 is the
  fix.** If they cannot update, converting to **DNG** (Adobe DNG Converter) works,
  since every version reads DNG.
- **On 0.7.0 or later** — these formats work. If one does not open, check the
  0.7.0 installer was actually run; if it was, escalate with the file.

**Geometry limits:** an image is rejected above **100 000 pixels on either axis**
or **1 gigapixel in total**. No real astronomy frame comes close; a file that
trips this is corrupt.

---

## The analysis cache — where results are stored

Measurements (HFR, stars, pixel statistics) and file metadata are cached in a
**SQLite database**:

> `%LOCALAPPDATA%\WinStellar\analysis_cache.db`

- It is keyed by the **file's content**, not by its path. **Renaming or moving a
  frame keeps its cached measurements.** Modifying the frame invalidates them and
  they are recomputed.
- The **viewer writes it** every time it opens a frame. That is why opening a file
  once fills its Explorer columns.
- **Explorer also writes it** for FITS files of 32 MB or less.
- The cache is what makes revisiting a folder instant instead of re-reading every
  file.

**Deleting this file is safe.** Nothing is lost but time: the results are simply
recomputed the next time each frame is looked at. It never contains image data,
only measurements. Uninstalling WinStellar deletes it.

The viewer also remembers its window position and size under the registry key
`HKEY_CURRENT_USER\Software\WinStellar`.

---

## Error messages, word for word

The user will paste the message. This is what each one means.

### In the standalone viewer

**`Could not open FITS file:`** followed by a reason, in a message box titled
**WinStellar**.
The file could not be read. Genuine causes: the file is truncated or corrupt
(interrupted download, bad transfer, dying drive), or it is a format WinStellar
does not support (see the formats section — on 0.6.10 and earlier, **`.cr3` is
the usual culprit**). Ask for the exact reason shown after the colon.

**`Status   analysis failed`** in the measurements panel.
The image loaded and is displayed, but star detection and statistics could not
run on it. Escalate with the file if it is reproducible.

**`Stars   0 (no detection)`** in the measurements panel.
**Not an error.** No star was found — normal on a dark, a bias, a flat, a cloudy
frame or a badly defocused one.

**`No stars analysed`** in the Tilt window.
Same cause: the tilt diagram needs detected stars. Not a bug.

**`Image too small for the background map.`** in the background map window —
**`Image trop petite pour la carte de fond.`** on 0.6.10 and earlier.
The image is smaller than **800 pixels on its short side**. Hard limit.

**`Analysing PSF…`** (**`Analyse PSF…`** on 0.6.10 and earlier) in the Aberration
Inspector.
**Not an error** — it means the PSF plate is being computed. Wait for it.

### In the Explorer preview pane

**`No data`** — the preview pane got an empty file.

**`Failed to parse FITS`** — the file is not a valid FITS/XISF/RAW, or it is
truncated.

**`Malformed or unreadable file`** — the loader threw while reading it. The file
is corrupt.

**`Empty render`** / **`DIB allocation failed`** — rare. The image decoded but
could not be turned into a bitmap. Collect the file and escalate.

### In the installer

**`Windows 10 or later is required.`** — WinStellar is Windows 10 / 11, 64-bit
only. There is no build for anything older.

---

## Known bugs and limits — read before answering ⚠️

**Confirm these when a user reports them. Do not send them back to their settings
to hunt for a mistake they did not make.**

### The HFR / Stars / pixel columns are empty for XISF, camera RAW, and FITS over 32 MB

**Symptom:** "I added the HFR column and it's blank for all my files."

**Cause and answer:** by design. Explorer only computes those columns on the spot
for **FITS of 32 MB or less**. For **XISF**, for **camera RAW**, and for **FITS
over 32 MB** (a 26 Mpx 16-bit sub is ~52 MB), the frame must be **opened once in
the WinStellar viewer**; the result is cached and the column then fills in
Explorer permanently.

This is a deliberate trade-off — decoding every large frame while the user
scrolls would freeze the folder. Explain it, don't apologise for it, and don't
suggest a reinstall.

### CR3, RAF, ORF and RW2 camera RAW files are not supported on 0.6.10 or earlier

**Symptom:** "My Canon R6 / Fuji / Olympus / Panasonic RAW files show no
thumbnail and won't open."

**Cause:** those versions required the canonical TIFF marker, which those four
formats do not carry, so **`.cr3`, `.raf`, `.orf` and `.rw2` were not recognised
at all**. **Fixed in 0.7.0**, which reads all four.

**Answer:** **update to 0.7.0.** If they cannot, converting to **DNG** with the
free Adobe DNG Converter works on every version.

**On 0.7.0 these formats work.** If one still does not show up there, the usual
reason is that **a 0.7.0-or-later installer was never run** — registering the Explorer
extensions for the new extensions is what the installer does. Have them run it,
then sign out and back in. If it still fails after that, escalate with the file:
that is a new bug, not this one.

### Parts of the inspection windows are in French (0.6.10 and earlier only)

On **0.6.10 and earlier** the Tilt, Aberration and Background windows show a
handful of French labels — *« Courbure »*, *« Visuel »*, *« Inspecté »*,
*« Analyse PSF… »*, *« Carte du fond »*, *« Image trop petite pour la carte de
fond. »*. **Every user on those versions sees them, in every language**; there is
no language setting and no "English version" to switch to, so it is not something
they can fix. **The labels are English from 0.7.0 onward.** On an older version:
confirm it, say it is cosmetic and fixed in 0.7.0, translate the label, move on.

### Windows warns that the publisher is unknown

WinStellar is **not code-signed yet**. Every fresh install triggers SmartScreen's
*"publisher unknown"* warning: **More info → Run anyway**. It is not a malware
detection. This is by far the most reported non-bug.

### There is no automatic update

WinStellar **never checks for updates and never notifies the user** of a new
version. To update, the user downloads the latest installer from
https://winstellar.fr and runs it over the existing install. Automatic updating
is a known missing feature, not a broken one.

### The background map needs an image at least 800 px on its short side

Anything smaller shows **`Image too small for the background map.`** (*« Image trop
petite pour la carte de fond. »* on 0.6.10 and earlier). There is no setting to
lower the threshold.

### Tilt and aberration need detected stars

Both tools are built from the star field. On a dark, a bias, a flat, a cloudy or a
heavily defocused frame there is nothing to measure, and the Tilt window says
**`No stars analysed`**. Expected.

---

## Troubleshooting — symptom → cause → answer

### Troubleshooting — columns and thumbnails in Explorer

**"My HFR / Stars column is empty."**
Almost always the expected behaviour, not a bug. Explorer computes those columns
directly only for **FITS of 32 MB or less**. For **XISF**, for **camera RAW**, and
for **FITS over 32 MB**, the user must **open the frame once in the WinStellar
viewer** — the measurement is then cached and the column fills in Explorer for
good.

**"I don't see any WinStellar column at all."**
Windows never adds a column by itself. In **Details** view: **right-click the
column header → More… → search for *HFR (px)*, *Object*, *Stars*… → tick → OK.**

**"My Gain / Filter / Focal length column is blank on every file."**
The keyword is not in the file. WinStellar reads `GAIN`, `FILTER`, `FOCALLEN`…
straight from the header — if the capture software did not write it, there is
nothing to show. Not a WinStellar problem.

**"No thumbnails in Explorer."**
Sign out of Windows and back in, then clear the thumbnail cache: **Disk Cleanup →
tick Thumbnails → clean**. Windows caches thumbnails very aggressively. Also check
the folder is actually in a thumbnail view (Large icons or bigger), not Details.

### Troubleshooting — files that won't open, formats, associations

**"My RAW files don't open / have no thumbnail."**
Check the extension first. `.nef`, `.nrw`, `.cr2`, `.arw`, `.sr2`, `.dng`, `.pef`,
`.srw`, `.iiq` work on every version. **`.cr3`, `.raf`, `.orf`, `.rw2` need
0.7.0** — on 0.6.10 or earlier, update, or convert to DNG. On 0.7.0 they work,
provided its installer was actually run; if they still don't, escalate.

**"Double-clicking my NEF opens my photo app, not WinStellar."**
Deliberate. WinStellar claims `.fit`, `.fits` and `.xisf` as default, and leaves
camera RAW to the user's photo app. **Right-click → Open with → WinStellar.**

**"The image is black / almost black."**
The **RAW** button is active — that is the linear data, and a linear astronomy sub
really is nearly black. Click **Auto** (auto stretch) in the toolbar, or press
**Ctrl+H** and use the **Auto** button in the histogram window.

### Troubleshooting — installing, updating, SmartScreen

**"Windows says the publisher is unknown."**
Expected — WinStellar is not signed yet. **More info → Run anyway.** Safe.

**"How do I update?"**
By hand: download the latest installer from https://winstellar.fr and run it over
the existing install. WinStellar does not check for updates by itself. Running
the installer is also what registers the Explorer extensions, so it is the only
way a release's new file types start showing thumbnails and columns.

### Troubleshooting — the viewer, the inspection windows, speed

**"The tilt / aberration window is empty or says `No stars analysed`."**
No stars were detected in that frame. Normal on darks, biases, flats, clouded or
badly defocused frames. Both tools need stars.

**"Half the inspection window is in French."**
Known cosmetic issue on **0.6.10 and earlier**; every user on those sees it, it is
not their install. **Fixed in 0.7.0.** *« Carte du fond »* = background
map, *« Courbure »* = curvature, *« Visuel »* = visual, *« Inspecté »* =
inspected, *« Analyse PSF… »* = computing the PSF.

**"Can I save / export the image?"**
**Not before 0.8.0** — earlier versions have no export at all. **From 0.8.0**,
the toolbar has an **Export** button (**Ctrl+S**) offering four formats,
and which one they want depends on what they are doing:
- **JPEG** and **PNG** — what is on screen, with the stretch applied. For sharing
  and posting.
- **TIFF** — 16-bit **linear** data, unstretched. For taking into another editor.
- **FITS** — the **original data**, unstretched, keeping the original headers and,
  for one-shot-colour, the raw CFA mosaic. For putting back into a stacker.
The rotation applies to all four. The original file is never modified.

**"Explorer is slow when I scroll a folder of frames."**
It should not be from 0.6.10 onward — that release specifically fixed scroll
freezes on folders of camera RAW. Ask for the WinStellar version first (Settings →
Apps → Installed apps). If they are on **0.6.10 or later** and it still freezes,
that is a genuine regression: collect the details below and escalate.

**"Fujifilm thumbnails take longer to appear than my other RAW files."**
Expected. Fujifilm's X-Trans sensor data has to be decompressed before anything
can be drawn, and that step cannot be shortened the way the rest of the decode
can. A folder of `.raf` fills in several times slower than the same folder of
`.nef` or `.cr3` on the same machine. It is slower, not stuck — nothing to fix.

---

## Escalation — when to stop and hand over to a human

**Escalate, and do not improvise, when:**

- the user reports a crash — of the viewer, or of **Windows Explorer itself**.
  Explorer crashes are serious: WinStellar runs inside Explorer, so a bad frame
  can in principle take it down. Collect the file and hand over;
- a **file will not open** and it is a supported extension, or a file opens but
  shows **`Status analysis failed`**;
- the user needs a **format WinStellar does not support** (CR3, RAF, ORF, RW2) —
  confirm the limitation, then pass it on as a feature request; do not promise a
  date;
- the user reports something this document does not cover. Say *"I don't know,
  I'm passing this to the team."* A plausible-sounding guess about someone's data
  is worse than silence;
- anything about payment or licensing beyond *"it is free and GPL-3.0"*.

**Never tell a user to delete, move or overwrite an image file** to fix a
WinStellar problem. No WinStellar issue is ever fixed by touching their frames.
WinStellar only ever writes a file the user explicitly asked for through Export,
to a destination they chose; it never modifies the frame it opened.

**Collect these before escalating.** Without them the report is not actionable:

1. **The WinStellar version** — Settings → Apps → Installed apps → WinStellar.
2. **The Windows version** — 10 or 11, and 64-bit.
3. **The exact file extension** and, if possible, **the file itself** (or one that
   reproduces it). Almost every real bug is file-specific.
4. **The exact text of any message**, and a screenshot of the window.

Bugs can also be filed directly at
https://github.com/caelo-works/winstellar/issues
