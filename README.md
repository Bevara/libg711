# libg711
This filter decodes ITU-T G.711 audio, both A-law and mu-law, to raw 16-bit PCM.

## Status: builds, but does not yet resolve in a graph

Read this before using it. The module compiles, links and loads: it imports 28
symbols and every one of them is exported by `solver_1` and by
`solver_minimal_1` alike, so there is nothing missing. The decoding itself is
two companding tables expanded from the definitions in the standard, and it is
written here in full.

What does not work is the **caps resolution**. G.711 has no container of its
own; in practice it arrives inside a WAV, with format tag 6 (A-law) or 7
(mu-law). Neither route connects:

- `rfpcm`, the repository's raw-PCM reframer, was extended to map those two
  format tags to `GF_CODECID_ALAW` and `GF_CODECID_MULAW` and to declare them
  among its output codec ids - but its WAV path is broken independently of this
  work, and its own test is commented out in the repository;
- `rfg711`, the dedicated reframer in `reframe_g711.c` (modelled on the one in
  `libaiff`), does not connect to the pid the WAV produces either.

So the filter is published as it stands rather than quietly held back. Anyone
picking it up should start at the caps, not at the decoder.

## Requirements

[CMake](https://cmake.org/) is used as a build system. To install it, follow
[Debian build instructions](developing_in_debian.md).

[Emscripten SDK](https://emscripten.org/) is required for building
WebAssembly artifacts. To install it, follow the
[Download and Install](https://emscripten.org/docs/getting_started/downloads.html)
guide:

```bash
cd $OPT

# Get the emsdk repo.
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory.
cd emsdk

# Download and install the latest SDK tools.
./emsdk install latest

# Make the "latest" SDK "active" for the current user. (writes ~/.emscripten file)
./emsdk activate latest
```

## Building the accessor

```bash
# Setup EMSDK and other environment variables. In practice EMSDK is set to be
# $OPT/emsdk.
source $OPT/emsdk/emsdk_env.sh

# Assuming you are in the root level of the cloned repo :
emcmake cmake .
emmake make
```

Once built, you can use and distribute libg711_1.wasm with your universal tags.

## Documentation

For more details, please visit our documentation at https://bevara.com/documentation/develop/.
