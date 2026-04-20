# DAWN Satellite (Tier 1)

Voice-controlled satellite client for the DAWN voice assistant. Runs on
Raspberry Pi with local ASR (Vosk or Whisper), TTS (Piper), VAD (Silero),
and wake word detection. Connects to the central DAWN daemon via WebSocket
using the DAP2 protocol.

## Install

From the repository root on a fresh Raspberry Pi OS Lite (64-bit):

```bash
git clone --recursive https://github.com/The-OASIS-Project/dawn.git
cd dawn
./scripts/install.sh --target satellite
```

The installer handles apt packages, voice-processing dependencies (ONNX
Runtime, libvosk, espeak-ng rhasspy fork, piper-phonemize), model
downloads with memory-aware recommendations, the satellite binary build,
and systemd service deployment.

## Documentation

- **Install, configure, troubleshoot** &rarr; [`docs/DAP2_SATELLITE.md`](../docs/DAP2_SATELLITE.md)
- **Wire protocol reference** &rarr; [`docs/WEBSOCKET_PROTOCOL.md`](../docs/WEBSOCKET_PROTOCOL.md)
- **Service paths, logrotate, manual install** &rarr; [`services/dawn-satellite/README.md`](../services/dawn-satellite/README.md)
- **Tier 2 ESP32 variant** &rarr; [`dawn_satellite_arduino/README.md`](../dawn_satellite_arduino/README.md)

## License

GPLv3 or later. See `LICENSE` in the repository root.
