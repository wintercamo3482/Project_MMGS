# Meeting Minutes Generation System

**MMGS (Meeting Minutes Generation System)** is a Windows desktop utility being developed to process media files and eventually generate meeting minutes.

## Current Features (v1.0.0)

- Convert MP4 audio to MP3 using Windows Media Foundation.
- Select an input file and output directory.
- View conversion progress and logs in the application window.
- Display success and error notifications.
- Run without a separate FFmpeg installation.

## Build

- Visual Studio 2022 with C++ desktop development and MFC components
- Windows SDK
- C++17

Open `MMGS.sln`, select **Release | x64**, and build the solution.

## Notes

- Supported input codecs depend on the Media Foundation components available on Windows.
- Meeting-minute generation is a planned feature and is not included in v1.0.0.