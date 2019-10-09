CemuVR
=======
CemuVR is a fork of ReShade, a generic post-processing injector for games and video software, which aims to add VR rendering to Cemu by modifying and displaying OpenGL pipelines.

*Note: EXPERIMENTAL. Currently only works for win64 platforms*

## Building

You'll need Visual Studio 2017 or higher to build ReShade and a Python 2.7.9 or later installation (Python 3 is supported as well) for the `gl3w` dependency.

1. Clone this repository including all Git submodules (`git clone <repo> --recurse-submodules`)
2. Open the Visual Studio solution. Do not upgrade if using VS2019.
3. Select either the "32-bit" or "64-bit" target platform and build the solution (this will build ReShade and all dependencies).

After the first build, a `version.h` file will show up in the [res](/res) directory. Change the `VERSION_FULL` definition inside to something matching the current release version and rebuild so that shaders from the official repository at https://github.com/crosire/reshade-shaders won't cause a version mismatch error during compilation.

In subsequent builds, build in the following order:

1. Release | 32-bit
2. Release | 64-bit
3. Release Setup | 64-bit

## Installing

1. Run `CemuVR Setup.exe` from `/bin/AnyCPU/Release`
2. Extract archive `CemuVR.zip`.
3. Place `openvr_api.dll` in root Cemu directory, i.e. `/cemu-1.15.15b/`.
4. Create directory `cemu-vr` in root Cemu directory
5. Place `SuperDepth3D_2.0.6_VR.fx` into `cemu-vr` directory.

For best experience, the following graphics packs are needed:
* FPS++ (set FPS limit to 75)
* VR Aspect Ratio ([download](https://www.youtube.com/redirect?q=https%3A%2F%2Fcdn.discordapp.com%2Fattachments%2F356187763139280896%2F572156657350606860%2FBreathOfTheWild_VRAspectRatio.zip&redir_token=mxGwKxg0MGcwq9eWXeYw4anFSuZ8MTU3MDY2NzU5NUAxNTcwNTgxMTk1&v=CvrjNLsGQZI&event=video_description))
* Reshade Compatibility

## Contributing

Any contributions to the project are welcomed, it's recommended to use GitHub [pull requests](https://help.github.com/articles/using-pull-requests/).

## License

All source code in this repository is licensed under a [BSD 3-clause license](LICENSE.md).
