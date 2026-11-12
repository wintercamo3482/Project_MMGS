# Meeting Minutes Generation System

**MMGS (Meeting Minutes Generation System)** is a Windows desktop utility designed to process media files, with the long-term goal of automatically generating meeting minutes.

## Current Features (v1.1.0)

- Convert MP4 audio to MP3 or WAV using Windows Media Foundation.
- Select the output audio format (MP3 / WAV).
- Customize output file names and save locations.
- Monitor conversion progress and logs within the application.
- Optionally open the output folder and highlight the converted file.
- Display success and error notifications.
- Run without a separate FFmpeg installation.

## Build

- Visual Studio 2022 with C++ desktop development and MFC components
- Windows SDK
- C++17

Open `MMGS.sln`, select **Release | x64**, and build the solution.

## Notes

- Supported input codecs depend on the Media Foundation components available on Windows.
- Meeting-minute generation is a planned feature and is not included in v1.1.0.