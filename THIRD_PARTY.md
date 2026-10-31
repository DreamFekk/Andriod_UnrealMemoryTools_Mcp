# Attribution and retained dependencies

Original UnrealMemoryTools: 曦曦 / DreamFekk. The original [MIT license](LICENSE) and output attribution are retained. Third-party components retain their own terms and notices.

[MobileDumper-7](https://github.com/MJx0/MobileDumper-7), by MJx0, was inspected at commit `595655004a8e1d56fd0e708fe64649f1864d7066`. It is an architectural reference for memory abstraction, bounded caching and windowed scanning. This project also adapts its PUBG manager/descriptor resolution, New State decoding algorithms (including droidxrx credit), and UE5 name/field/enum handling concepts. See the retained [MIT notice](src/UE/UEGameProfiles/LICENSE.MobileDumper-7) and [profile compatibility documentation](docs/ue5-profile-support.md).

## Bundled components

| Component | Retained license / notice |
|---|---|
| KittyMemoryEx | [MIT](KittyMemoryEx/LICENSE) |
| Dear ImGui | [MIT](src/GUI/ImGui/LICENSE.txt); bundled headers identify `1.92.0 WIP` |
| FreeType | [License overview](src/GUI/ImGui/misc/git_freetype/LICENSE.TXT), [FreeType License](src/GUI/ImGui/misc/git_freetype/FTL.TXT), [GPLv2 alternative](src/GUI/ImGui/misc/git_freetype/GPLv2.TXT); bundled headers identify `2.13.3` |
| fmt | [License](deps/fmt/LICENSE) |
| nlohmann/json | [MIT](deps/nlohmann/LICENSE.MIT) |
| utfcpp | [License](deps/utfcpp/LICENSE) |
| stb_image | MIT / public-domain alternatives are embedded at the end of [stb_image.cpp](src/GUI/My_Utils/stb_image.cpp) |
| zip / miniz | [Unlicense](KittyMemoryEx/zip/UNLICENSE) and embedded notices in [miniz.h](KittyMemoryEx/zip/miniz.h) |
| Android native-surface integration | AFan4724's MIT notice is embedded in [ANativeWindowCreator.h](src/GUI/native_surface/ANativeWindowCreator.h) |
| Android Vulkan wrappers | Google copyright notices in `vulkan_wrapper.h` / `vulkan_wrapper.cpp` and [Apache License 2.0](src/GUI/Android_Graphics/LICENSE.Apache-2.0) |

FreeType static libraries are required by the native UI build and remain bundled with their headers. Existing source-level notices in Android surface, drawing and touch integrations are preserved. The Web UI does not require online JavaScript, CSS, fonts or CDN dependencies.

Source distributions retain these files alongside their components. Binary release packages include a `licenses/` directory containing dependency license texts and relevant embedded notices. Python, ADB, IDA and model credentials are not bundled; obtain optional tools from their respective publishers.
