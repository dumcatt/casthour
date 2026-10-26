#pragma once

// Taps the game's audio output (ASIO bufferSwitch or WASAPI ReleaseBuffer), copies each block into
// a lock-free ring, and encodes AAC on our own thread. Nothing the game or driver sees is changed.
void audio_install();   // call on an STA thread (ASIO drivers are apartment-threaded COM objects)
void audio_start();
