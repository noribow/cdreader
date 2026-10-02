// JNI bridge for the Android app (io.github.noribow.cdreader.NativeCd).
//
// The Kotlin side finds the USB mass storage interface, obtains permission
// and passes the file descriptor of its UsbDeviceConnection; everything from
// the Bulk-Only Transport up to WAV writing runs here on the shared core.
// All calls for one handle must come from one thread at a time, except
// nativeCancel().

#include <jni.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "bot_transport.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/metadata.h"
#include "cdreader/ripper.h"
#include "cdreader/toc.h"
#include "usbdevfs_endpoints.h"

namespace {

struct Session {
    Session(int fd, int interfaceNumber, uint8_t endpointIn, uint8_t endpointOut)
        : endpoints(fd, interfaceNumber, endpointIn, endpointOut),
          transport(endpoints, uint8_t(interfaceNumber)),
          drive(transport) {}

    cdr::usb::UsbDevfsEndpoints endpoints;
    cdr::usb::BulkOnlyTransport transport;
    cdr::CdDrive drive;
    std::optional<cdr::Toc> toc;
    std::atomic<bool> cancelled{false};
};

// Unwinds the rip when the user cancels or the Java listener throws.
struct Cancelled {};
struct JavaExceptionPending {};

Session* session(jlong handle) { return reinterpret_cast<Session*>(static_cast<intptr_t>(handle)); }

void throwJava(JNIEnv* env, const char* className, const std::string& message) {
    if (env->ExceptionCheck()) return;  // keep the first exception
    jclass cls = env->FindClass(className);
    if (cls != nullptr) env->ThrowNew(cls, message.c_str());
}

void throwIo(JNIEnv* env, const std::string& message) { throwJava(env, "java/io/IOException", message); }

const cdr::Toc& tocOf(Session& s) {
    if (!s.toc) s.toc = s.drive.readToc();
    return *s.toc;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeOpen(
    JNIEnv* env, jclass, jint fd, jint interfaceNumber, jint endpointIn, jint endpointOut) {
    try {
        auto s = std::make_unique<Session>(fd, interfaceNumber, uint8_t(endpointIn), uint8_t(endpointOut));
        std::string error;
        if (!s->endpoints.claimInterface(error)) {
            throwIo(env, error);
            return 0;
        }
        return static_cast<jlong>(reinterpret_cast<intptr_t>(s.release()));
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return 0;
    }
}

JNIEXPORT void JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeClose(JNIEnv*, jclass, jlong handle) {
    delete session(handle);
}

JNIEXPORT void JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeCancel(JNIEnv*, jclass, jlong handle) {
    if (handle != 0) session(handle)->cancelled = true;
}

JNIEXPORT jstring JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeInquiry(JNIEnv* env, jclass,
                                                                                 jlong handle) {
    try {
        return env->NewStringUTF(session(handle)->drive.inquiry().displayName().c_str());
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

JNIEXPORT jboolean JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeIsReady(JNIEnv*, jclass,
                                                                                  jlong handle) {
    return session(handle)->drive.isReady() ? JNI_TRUE : JNI_FALSE;
}

// Returns [firstTrack, lastTrack, leadOutLba, cddbId, then per track:
// number, startLba, lengthSectors, flags (1 = audio, 2 = pre-emphasis)].
JNIEXPORT jintArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeReadToc(JNIEnv* env, jclass,
                                                                                   jlong handle) {
    try {
        Session& s = *session(handle);
        s.toc.reset();  // the disc may have changed
        const cdr::Toc& toc = tocOf(s);
        std::vector<jint> v = {toc.firstTrack, toc.lastTrack, jint(toc.leadOutLba), jint(toc.cddbId())};
        for (const cdr::Track& t : toc.tracks) {
            v.push_back(t.number);
            v.push_back(jint(t.startLba));
            v.push_back(jint(t.lengthSectors));
            v.push_back((t.isAudio ? 1 : 0) | (t.preEmphasis ? 2 : 0));
        }
        jintArray array = env->NewIntArray(jsize(v.size()));
        if (array != nullptr) env->SetIntArrayRegion(array, 0, jsize(v.size()), v.data());
        return array;
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

// Rips one track to a WAV file at `path`, calling listener.onProgress(done,
// total) in sectors. Returns [sectors, unreadableSectors, retries,
// paddedSamples, crc32]. Throws java.util.concurrent.CancellationException
// after nativeCancel() and IOException on errors; the partial file is left
// for the caller to delete.
JNIEXPORT jintArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeRipTrack(
    JNIEnv* env, jclass, jlong handle, jint trackNumber, jstring path, jint readOffset, jint maxRetries,
    jboolean verify, jobject listener) {
    Session& s = *session(handle);
    s.cancelled = false;

    const char* chars = env->GetStringUTFChars(path, nullptr);
    if (chars == nullptr) return nullptr;
    const std::string outputPath(chars);
    env->ReleaseStringUTFChars(path, chars);

    jmethodID onProgress = nullptr;
    if (listener != nullptr) {
        jclass cls = env->GetObjectClass(listener);
        onProgress = env->GetMethodID(cls, "onProgress", "(II)V");
        if (onProgress == nullptr) return nullptr;  // NoSuchMethodError pending
    }

    try {
        const cdr::Toc& toc = tocOf(s);
        const cdr::Track* track = toc.findTrack(trackNumber);
        if (track == nullptr || !track->isAudio) throw std::runtime_error("not an audio track");

        cdr::RipOptions options;
        options.readOffsetSamples = readOffset;
        options.maxRetries = maxRetries;
        options.verify = verify == JNI_TRUE;

        cdr::AlbumMetadata album;
        char discId[9];
        std::snprintf(discId, sizeof discId, "%08x", toc.cddbId());
        album.discId = discId;

        std::unique_ptr<cdr::AudioWriter> writer = cdr::createAudioWriter("wav");
        writer->open(std::filesystem::u8path(outputPath),
                     album.forTrack(track->number, int(toc.audioTrackCount())));

        cdr::Ripper ripper(s.drive, toc, options);
        const cdr::TrackRipResult r = ripper.ripTrack(
            *track,
            [&](const uint8_t* pcm, size_t bytes) {
                if (s.cancelled) throw Cancelled{};
                writer->write(pcm, bytes);
            },
            [&](uint32_t done, uint32_t total) {
                if (s.cancelled) throw Cancelled{};
                if (onProgress == nullptr) return;
                env->CallVoidMethod(listener, onProgress, jint(done), jint(total));
                if (env->ExceptionCheck()) throw JavaExceptionPending{};
            });
        writer->close();

        const jint v[5] = {jint(r.sectors), jint(r.unreadableSectors), jint(r.retries), jint(r.paddedSamples),
                           jint(r.crc32)};
        jintArray array = env->NewIntArray(5);
        if (array != nullptr) env->SetIntArrayRegion(array, 0, 5, v);
        return array;
    } catch (const Cancelled&) {
        throwJava(env, "java/util/concurrent/CancellationException", "rip cancelled");
    } catch (const JavaExceptionPending&) {
        // propagate the listener's exception
    } catch (const std::exception& e) {
        throwIo(env, e.what());
    }
    return nullptr;
}

}  // extern "C"
