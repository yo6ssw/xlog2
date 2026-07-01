// opus_jni — a thin JNI wrapper over the vendored libopus decoder, used by the
// Kotlin OpusDecoder. The rig-audio stream client (AudioStreamClient.kt) does the
// UDP receive + AudioTrack playback in Kotlin (like QRZ moved to OkHttp) and only
// crosses JNI to decode each Opus packet — so the native surface stays tiny and
// the desktop's PipeWire-coupled AudioStreamClient stays untouched.
//
// Wire form matches cwsd's audio_stream_server / the desktop client: raw Opus
// packets, decoded to interleaved int16 PCM.

#include <jni.h>
#include <opus/opus.h>

#include <cstdint>

extern "C" {

JNIEXPORT jlong JNICALL
Java_ro_scripca_xlog2_OpusDecoder_nativeCreate(JNIEnv*, jobject, jint rate, jint channels) {
    int err = 0;
    OpusDecoder* dec = opus_decoder_create(rate, channels, &err);
    if (err != OPUS_OK || dec == nullptr)
        return 0;
    return reinterpret_cast<jlong>(dec);
}

// Decode one Opus packet into `pcm` (interleaved int16). Returns the number of
// samples per channel decoded, or a negative Opus error code.
JNIEXPORT jint JNICALL
Java_ro_scripca_xlog2_OpusDecoder_nativeDecode(JNIEnv* env, jobject, jlong handle,
        jbyteArray packet, jint packetLen, jshortArray pcm, jint frameSize) {
    auto* dec = reinterpret_cast<OpusDecoder*>(handle);
    if (dec == nullptr)
        return -1;
    jbyte*  pkt = env->GetByteArrayElements(packet, nullptr);
    jshort* out = env->GetShortArrayElements(pcm, nullptr);
    const int n = opus_decode(dec, reinterpret_cast<const unsigned char*>(pkt), packetLen,
                              reinterpret_cast<opus_int16*>(out), frameSize, 0);
    env->ReleaseByteArrayElements(packet, pkt, JNI_ABORT);  // input untouched
    env->ReleaseShortArrayElements(pcm, out, 0);            // commit decoded PCM
    return n;
}

// Packet-loss concealment: synthesise one frame from the decoder's state.
JNIEXPORT jint JNICALL
Java_ro_scripca_xlog2_OpusDecoder_nativeDecodePlc(JNIEnv* env, jobject, jlong handle,
        jshortArray pcm, jint frameSize) {
    auto* dec = reinterpret_cast<OpusDecoder*>(handle);
    if (dec == nullptr)
        return -1;
    jshort* out = env->GetShortArrayElements(pcm, nullptr);
    const int n = opus_decode(dec, nullptr, 0, reinterpret_cast<opus_int16*>(out), frameSize, 0);
    env->ReleaseShortArrayElements(pcm, out, 0);
    return n;
}

JNIEXPORT void JNICALL
Java_ro_scripca_xlog2_OpusDecoder_nativeDestroy(JNIEnv*, jobject, jlong handle) {
    auto* dec = reinterpret_cast<OpusDecoder*>(handle);
    if (dec != nullptr)
        opus_decoder_destroy(dec);
}

}  // extern "C"
