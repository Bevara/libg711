# libg711
This filter decodes the G.711 / G.721 / G.723 family - Sun's public-domain
reference implementation of the CCITT voice compressions - from Sun/NeXT `.au`
files, to raw 16-bit PCM.

## What works, and what does not

**`audec` works.** It reads a `.au` file and covers the five encodings that make
up the family:

| `.au` encoding | Codec | Bits per sample |
|---|---|---|
| 1 | G.711 mu-law | 8 |
| 27 | G.711 A-law | 8 |
| 23 | G.721 ADPCM, 32 kbit/s | 4 |
| 25 | G.723 ADPCM, 24 kbit/s | 3 |
| 26 | G.723 ADPCM, 40 kbit/s | 5 |

The container is read rather than guessed at, and that is the point of it: a
bare G.72x bitstream does not say which member of the family it is, and 3, 4 and
5 bits per sample divide too many stream lengths for a guess to be safe. Linear
PCM `.au` files are refused - `rfpcm` is the filter for those.

The two companding laws are expanded here from their definition in the standard.
The three ADPCM rates go through Sun's reference implementation, as carried in
libsndfile's `src/G72x` with the block wrapper that handles the per-rate bit
packing.

Verified: mu-law and A-law match ffmpeg's independent decoders with **0 differing
samples out of 80000**; the three ADPCM rates match the same Sun code built
natively with **0 differing samples out of 79920**.

**`g711dec` and `rfg711` do not resolve in a graph.** They were written for the
other route into G.711 - a WAV file with format tag 6 or 7, demuxed by `rfpcm`
and decoded by `g711dec` as a chain link. That route is blocked upstream:
`rfpcm` does not connect to a `.wav` pid at all, not even a linear-PCM one, and
its own test is commented out in the player repository. The two filters are kept
because the decoding in them is correct and the blockage is not theirs, but
`audec` is the one to use.

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
