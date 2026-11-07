# MLT dependency fixes for the experimental MCP build

These source patches target MLT 7.43.0, commit
`eca4e32d28e2c588a9c3b7c23e2276d9cd1f4edb` from
<https://github.com/mltframework/mlt>. They are separate from Kdenlive changes.
Use an isolated dependency build/prefix, then build Kdenlive against that prefix.
Do not copy a new dependency DLL into an editor process that is already running.

- `mlt-playlist-entry-info.patch` adds the read-only bulk playlist query used by
  the MCP state scanner. See its patch description and native playlist tests.
- `mlt-sdl2-audio-device-lifetime.patch` changes
  `src/modules/sdl2/consumer_sdl2_audio.c`: retain each consumer's audio device,
  close it before reinitialization or after worker shutdown, and serialize audio
  subsystem shutdown with initialization. The callback must be finished before
  its buffers and userdata are freed. The existing Windows timeout workaround
  that retains a subsystem reference is preserved.
- `mlt-openfx-factory-export.patch` changes `src/modules/openfx/factory.c`: use
  the module's existing generated export header for `mlt_register`. Without this
  export the Windows module builds but MLT cannot load its registration function.

- `mlt-xml-windows-getcwd.patch` adds the MSVC `<direct.h>` declaration in
  `src/modules/xml/producer_xml.c` and a native XML regression. The missing
  declaration made 64-bit Windows treat the returned current-directory pointer
  as an integer and crash when opening an XML file through a relative path.
- `mlt-rotoscoping-bounds.patch` replaces the fixed scanline intersection buffer
  with checked allocation, bounds Bezier subdivision and propagates geometry
  failures before applying a partial mask. Its native regression covers 4,096
  intersections, coincident-anchor loops, offscreen coordinates and rejected
  non-finite/excessive geometry through the real frame callback.
- `mlt-metadata-validation.patch` repairs duplicate YAML keys in three pinned
  module descriptions and a missing folded-scalar marker in RGBLUT metadata.
  The source-schema and runtime-query validation suites check these files.

- `mlt-private-cjson.patch` hides the bundled legacy cJSON implementation inside
  the PlusGPL module. Kdenlive links modern `libcjson` through its host libraries;
  the two versions use incompatible JSON type constants. ELF symbol interposition
  previously let the module parse with the other library, silently ignoring valid
  roto splines. Only `cJSON.c` gets hidden visibility; `mlt_register` stays exported.
  The editor's native roto validation/pixel/undo case exercises this loaded-module
  interaction, beyond the standalone MLT geometry regression.

Apply each patch from the MLT source root with `git apply --check <patch>` and
then `git apply <patch>`. Apply the documented sequence; test registration files
can be shared by patches. They are not applied automatically by Kdenlive's build;
the Linux helper explicitly checks and applies this complete list.
Record the exact dependency source revision and preserve its local changes.

Build MLT with its normal CMake workflow and platform dependencies. Enable
`MOD_XML=ON`, `MOD_SDL2=ON` and `MOD_OPENFX=ON`; the latter also requires GPL components and
GLib. For example, in an initialized compiler environment:

```text
cmake -S <mlt-source> -B <mlt-build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWINDOWS_DEPLOY=OFF -DCMAKE_INSTALL_PREFIX=<isolated-prefix> -DCMAKE_PREFIX_PATH=<dependency-prefix> -DMOD_XML=ON -DMOD_SDL2=ON -DMOD_OPENFX=ON
cmake --build <mlt-build> --target mlt mlt++ mltxml mltsdl2 mltopenfx
```

Normal installation also requires other enabled targets; build those before
`cmake --install <mlt-build>`. Keep the runtime modules, core DLLs and headers
from a compatible MLT build. On Windows, pass the private module directory to
`Mlt::Factory::init()`/`mlt_factory_init()` explicitly; setting an environment
variable alone is not sufficient for every caller.

## SDL lifetime regression

`tests/test_mlt_sdl2_audio_lifetime.py` is a Python-standard-library Windows regression
using the real MLT and SDL libraries, dummy audio and generated color producers.
Run it without Python optimization (`-O` would disable assertions):

```text
python -B tests/test_mlt_sdl2_audio_lifetime.py --prefix <compatible-mlt-prefix> --old-module <original-mltsdl2.dll> --new-module <patched-mltsdl2.dll> --work-dir <task-work-directory>
```

The old module is a calibration: with a second SDL audio initialization reference,
its stopped consumer leaves a live callback device. The diagnostic closes that
owned device before freeing userdata. The patched module must leave no live
owned device in the tested ID range. Ten isolated child cases cover normal
stop, a retained external subsystem reference, 30 restarts of one consumer,
30 replacements across three profiles (both with and without the reference),
forced audio timeout/reinitialization and initialization-failure cleanup. The
failure case explicitly calls stop before close, as in the editor lifecycle.
Each child has a deadline and writes its own log; the parent writes a JSON report.
This is a bounded diagnostic for its generated devices, not general enumeration
of arbitrary SDL devices.

The focused patch passed these checks and independent source review. A direct
close after failed startup, without an explicit stop, crashed with both original
and patched modules in a separate diagnostic; that pre-existing core lifecycle
case is outside this focused patch. The preserved Windows timeout workaround
can retain an SDL subsystem reference, but must no longer retain the callback
device. Do not claim the original editor profile-change crash resolved solely
from this native probe: also execute the actual GUI replacement/profile-change
workflow with the patched module and retain its process/output evidence.

## OpenFX verification

With the original module, a Windows export lookup for `mlt_register` failed.
The patched module exported the function and `mlt_factory_init()` succeeded in
an isolated repository. No OpenFX provider was installed in that test, so those
checks establish module registration only; they do not verify third-party filter
execution or availability.

The SDL callback-close guarantee is documented by
[SDL_CloseAudioDevice](https://wiki.libsdl.org/SDL2/SDL_CloseAudioDevice).

## XML relative-path regression

Enable `BUILD_TESTING=ON` and the project's normal Qt test prerequisites in the
MLT configure step. Build `test_xml`, then execute the native XML suite:

```text
cmake --build <mlt-build> --target mltxml test_xml
ctest --test-dir <mlt-build> -R "^QtTest:xml$" --output-on-failure --no-tests=error
```

`RelativeXmlPathUsesCurrentDirectory` writes a generated ten-frame color playlist
in a temporary subdirectory and opens it through `relative/project.mlt`. Its
working-directory guard restores the original directory even if an assertion
fails. The original Windows module was also tested in a contained native child:
XML registration succeeded, followed by an access violation on the relative-path
load. The correction supplies the same MSVC header already used by the XML writer.
