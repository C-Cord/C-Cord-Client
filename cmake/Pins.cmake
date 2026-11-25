# Every pinned third-party version and hash lives here.
set(CCORD_PIN_LUA_VERSION "5.5.1")
set(CCORD_PIN_LUA_URL "https://www.lua.org/ftp/lua-5.5.1.tar.gz")
set(CCORD_PIN_LUA_SHA256 "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce")

# SDL3 and SDL3_ttf (system mode requires exactly these versions; fetch mode downloads these tarballs).
set(CCORD_PIN_SDL3_VERSION "3.4.20")
set(CCORD_PIN_SDL3_URL "https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz")
set(CCORD_PIN_SDL3_SHA256 "f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f")
set(CCORD_PIN_SDL3_TTF_VERSION "3.2.2")
set(CCORD_PIN_SDL3_TTF_URL "https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-3.2.2.tar.gz")
set(CCORD_PIN_SDL3_TTF_SHA256 "63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d")

# Discord DAVE E2EE: discord/libdave (C API in cpp/includes/dave/dave.h) and its MLS dependency cisco/mlspp.
# GitHub commit archives, vendored in third_party/{libdave,mlspp}/. The mlspp commit is the one libdave's
# vcpkg overlay port pins (its sha512 5d37631e2c47daae... matches this tarball).
set(CCORD_PIN_LIBDAVE_COMMIT "6396c90aedbb9a6ee117ea9ce1ce9832af6a3375")
set(CCORD_PIN_LIBDAVE_URL "https://github.com/discord/libdave/archive/${CCORD_PIN_LIBDAVE_COMMIT}.tar.gz")
set(CCORD_PIN_LIBDAVE_SHA256 "37784dd0c1476738426f0968a6420dc8590e4b40cb8aae42e36df3c663e18cc5")
set(CCORD_PIN_MLSPP_COMMIT "1cc50a124a3bc4e143a787ec934280dc70c1034d")
set(CCORD_PIN_MLSPP_URL "https://github.com/cisco/mlspp/archive/${CCORD_PIN_MLSPP_COMMIT}.tar.gz")
set(CCORD_PIN_MLSPP_SHA256 "7a9d6318627e548903bc65c3dc5a4de4a90290983efea9a49af8561ed3f999f5")

# Native C mini web runtime (experimental CAPTCHA renderer): QuickJS-ng, Lexbor, wasm3.
set(CCORD_PIN_QUICKJS_VERSION "0.18.0")
set(CCORD_PIN_QUICKJS_URL "https://github.com/quickjs-ng/quickjs/archive/refs/tags/v${CCORD_PIN_QUICKJS_VERSION}.tar.gz")
set(CCORD_PIN_QUICKJS_SHA256 "28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd")

set(CCORD_PIN_LEXBOR_VERSION "3.0.0")
set(CCORD_PIN_LEXBOR_URL "https://github.com/lexbor/lexbor/archive/refs/tags/v${CCORD_PIN_LEXBOR_VERSION}.tar.gz")
set(CCORD_PIN_LEXBOR_SHA256 "eafaa79ef9871f0bbb1978eda8677d184f7ecdcaa203d7cd25b3f86e32c014c2")

set(CCORD_PIN_WASM3_COMMIT "28ecb9af6d2040e474a70f7cb7f43666740141fb")
set(CCORD_PIN_WASM3_URL "https://github.com/wasm3/wasm3/archive/${CCORD_PIN_WASM3_COMMIT}.tar.gz")
set(CCORD_PIN_WASM3_SHA256 "5ecbb8851bd9acb54a83be10f0536167cb0da13b007406578761672b0bd94b18")
