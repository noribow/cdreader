// JNI bridge for the Android app (io.github.noribow.cdreader.NativeCd).
//
// The Kotlin side finds the USB mass storage interface, obtains permission
// and passes the file descriptor of its UsbDeviceConnection; everything from
// the Bulk-Only Transport up to the audio files runs here on the shared core
// through RipSession (rip_session.h), which holds the actual logic. Online
// lookups call back into Kotlin (HttpGet) for HTTP.
// All calls for one handle must come from one thread at a time, except
// nativeCancel().

#include <jni.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "bot_transport.h"
#include "cdreader/audio_writer.h"
#include "cdreader/cd_drive.h"
#include "cdreader/cddb.h"
#include "cdreader/http.h"
#include "cdreader/offset_detect.h"
#include "rip_session.h"
#include "usbdevfs_endpoints.h"

#ifndef CDREADER_VERSION
#define CDREADER_VERSION "0.1.0"
#endif

namespace {

struct Session {
    Session(int fd, int interfaceNumber, uint8_t endpointIn, uint8_t endpointOut)
        : endpoints(fd, interfaceNumber, endpointIn, endpointOut),
          transport(endpoints, uint8_t(interfaceNumber)),
          drive(transport),
          rip(drive) {}

    cdr::usb::UsbDevfsEndpoints endpoints;
    cdr::usb::BulkOnlyTransport transport;
    cdr::CdDrive drive;
    cdr::RipSession rip;
};

// Unwinds the rip when a Java callback throws.
struct JavaExceptionPending {};

Session* session(jlong handle) { return reinterpret_cast<Session*>(static_cast<intptr_t>(handle)); }

void throwJava(JNIEnv* env, const char* className, const std::string& message) {
    if (env->ExceptionCheck()) return;  // keep the first exception
    jclass cls = env->FindClass(className);
    if (cls != nullptr) env->ThrowNew(cls, message.c_str());
}

void throwIo(JNIEnv* env, const std::string& message) { throwJava(env, "java/io/IOException", message); }

// NewStringUTF() takes "modified UTF-8", which differs from real UTF-8 for
// characters outside the BMP (e.g. emoji in CDDB titles), so strings go
// through UTF-16. Invalid sequences become U+FFFD.
jstring toJava(JNIEnv* env, const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if (c >= 0xC2 && c <= 0xF4) {
            const size_t need = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (i + need <= s.size()) {
                uint32_t v = c & (0xFFu >> (need + 1));
                bool ok = true;
                for (size_t k = 1; k < need; ++k) {
                    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                    if ((cc & 0xC0) != 0x80) {
                        ok = false;
                        break;
                    }
                    v = (v << 6) | (cc & 0x3Fu);
                }
                const uint32_t min = need == 2 ? 0x80 : need == 3 ? 0x800 : 0x10000;
                if (ok && v >= min && v <= 0x10FFFF && !(v >= 0xD800 && v <= 0xDFFF)) {
                    cp = v;
                    len = need;
                }
            }
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(char16_t(0xD800 + (cp >> 10)));
            out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(char16_t(cp));
        }
        i += len;
    }
    return env->NewString(reinterpret_cast<const jchar*>(out.data()), jsize(out.size()));
}

// Java String -> UTF-8 (via UTF-16, see toJava()). Returns false with an
// exception pending on failure.
bool fromJava(JNIEnv* env, jstring s, std::string& out) {
    out.clear();
    if (s == nullptr) return true;
    const jsize length = env->GetStringLength(s);
    const jchar* chars = env->GetStringChars(s, nullptr);
    if (chars == nullptr) return false;
    for (jsize i = 0; i < length; ++i) {
        uint32_t cp = chars[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length && chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t(chars[i + 1]) - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }
    env->ReleaseStringChars(s, chars);
    return true;
}

jobjectArray toJavaArray(JNIEnv* env, const std::vector<std::string>& values) {
    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr) return nullptr;
    jobjectArray array = env->NewObjectArray(jsize(values.size()), stringClass, nullptr);
    if (array == nullptr) return nullptr;
    for (size_t i = 0; i < values.size(); ++i) {
        jstring s = toJava(env, values[i]);
        if (s == nullptr) return nullptr;
        env->SetObjectArrayElement(array, jsize(i), s);
        env->DeleteLocalRef(s);
    }
    return array;
}

jintArray toJavaArray(JNIEnv* env, const std::vector<jint>& values) {
    jintArray array = env->NewIntArray(jsize(values.size()));
    if (array != nullptr) env->SetIntArrayRegion(array, 0, jsize(values.size()), values.data());
    return array;
}

// Describes and clears the pending Java exception.
std::string takeJavaException(JNIEnv* env) {
    jthrowable t = env->ExceptionOccurred();
    env->ExceptionClear();
    if (t == nullptr) return "Java exception";
    std::string text = "Java exception";
    jclass cls = env->GetObjectClass(t);
    jmethodID toString = env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
    if (toString != nullptr) {
        auto s = static_cast<jstring>(env->CallObjectMethod(t, toString));
        if (!env->ExceptionCheck() && s != nullptr) fromJava(env, s, text);
    }
    env->ExceptionClear();
    return text;
}

// cdr::HttpClient on top of the Kotlin HttpGet object:
//   fun get(url: String): HttpResult   (fields status: Int, body: ByteArray?, error: String?)
// Runs on the thread that called into native code (the app's worker thread,
// never the UI thread).
class JavaHttpClient : public cdr::HttpClient {
public:
    JavaHttpClient(JNIEnv* env, jobject httpGet) : env_(env), httpGet_(httpGet) {}

    cdr::HttpResponse get(const std::string& url) override {
        cdr::HttpResponse response;
        JNIEnv* env = env_;
        if (httpGet_ == nullptr) {
            response.error = "no HTTP client";
            return response;
        }
        if (env->PushLocalFrame(16) != 0) {
            response.error = takeJavaException(env);
            return response;
        }
        jobject result = nullptr;
        jclass cls = env->GetObjectClass(httpGet_);
        jmethodID get = env->GetMethodID(cls, "get", "(Ljava/lang/String;)Lio/github/noribow/cdreader/HttpResult;");
        jstring jurl = get != nullptr ? toJava(env, url) : nullptr;
        if (jurl != nullptr) result = env->CallObjectMethod(httpGet_, get, jurl);
        if (env->ExceptionCheck()) {
            response.error = takeJavaException(env);
        } else if (result == nullptr) {
            response.error = "no HTTP result";
        } else {
            readResult(env, result, response);
        }
        env->PopLocalFrame(nullptr);
        return response;
    }

private:
    static void readResult(JNIEnv* env, jobject result, cdr::HttpResponse& response) {
        jclass cls = env->GetObjectClass(result);
        jfieldID statusField = env->GetFieldID(cls, "status", "I");
        jfieldID bodyField = statusField ? env->GetFieldID(cls, "body", "[B") : nullptr;
        jfieldID errorField = bodyField ? env->GetFieldID(cls, "error", "Ljava/lang/String;") : nullptr;
        if (errorField == nullptr) {
            response.error = takeJavaException(env);
            return;
        }
        auto error = static_cast<jstring>(env->GetObjectField(result, errorField));
        if (error != nullptr) {
            if (!fromJava(env, error, response.error)) response.error = takeJavaException(env);
            if (response.error.empty()) response.error = "request failed";
            return;
        }
        response.status = env->GetIntField(result, statusField);
        auto body = static_cast<jbyteArray>(env->GetObjectField(result, bodyField));
        if (body != nullptr) {
            const jsize length = env->GetArrayLength(body);
            response.body.resize(size_t(length));
            if (length > 0) env->GetByteArrayRegion(body, 0, length, reinterpret_cast<jbyte*>(&response.body[0]));
        }
        response.ok = true;
    }

    JNIEnv* env_;
    jobject httpGet_;
};

}  // namespace

extern "C" {

// Output formats built into the library, comma separated ("wav,flac,oggflac,alac,opus,vorbis,mka,...").
JNIEXPORT jstring JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeFormats(JNIEnv* env, jclass) {
    std::string list;
    for (const std::string& f : cdr::audioFormats()) list += (list.empty() ? "" : ",") + f;
    return toJava(env, list);
}

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
    if (handle != 0) session(handle)->rip.cancel();
}

JNIEXPORT jstring JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeInquiry(JNIEnv* env, jclass,
                                                                                 jlong handle) {
    try {
        return toJava(env, session(handle)->rip.driveName());
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

// [vendor, product, revision, display name, key for the saved read offsets (#37)].
JNIEXPORT jobjectArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeDriveInfo(JNIEnv* env, jclass,
                                                                                       jlong handle) {
    try {
        cdr::RipSession& rip = session(handle)->rip;
        const cdr::DriveInfo& info = rip.driveInfo();
        return toJavaArray(env, std::vector<std::string>{info.vendor, info.product, info.revision, rip.driveName(),
                                                         rip.driveOffsetKey()});
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
// Forgets the metadata and rip results of the previous disc.
JNIEXPORT jintArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeReadToc(JNIEnv* env, jclass,
                                                                                   jlong handle) {
    try {
        const cdr::Toc& toc = session(handle)->rip.readToc();
        std::vector<jint> v = {toc.firstTrack, toc.lastTrack, jint(toc.leadOutLba), jint(toc.cddbId())};
        for (const cdr::Track& t : toc.tracks) {
            v.push_back(t.number);
            v.push_back(jint(t.startLba));
            v.push_back(jint(t.lengthSectors));
            v.push_back((t.isAudio ? 1 : 0) | (t.preEmphasis ? 2 : 0));
        }
        return toJavaArray(env, v);
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

// Looks the disc up on CDDB (or clears the metadata when !enabled); an empty
// `server` means the default server. Never fails for network problems.
// Returns [found ("1"/"0"), message (error, or "exact"/"inexact"), artist,
// album, year, genre, folder name, AccurateRip disc id, chosen match index,
// match count N, N x "category/discid  Artist / Album", then one label per
// TOC track ("Title" or "Artist / Title", "" if unknown)].
JNIEXPORT jobjectArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeLookupCddb(
    JNIEnv* env, jclass, jlong handle, jboolean enabled, jstring server, jint matchIndex, jobject httpGet) {
    try {
        cdr::RipSession& rip = session(handle)->rip;
        cdr::CddbSettings settings;
        settings.enabled = enabled == JNI_TRUE && httpGet != nullptr;
        std::string url;
        if (!fromJava(env, server, url)) return nullptr;
        if (!url.empty()) settings.options.server = url;
        settings.options.matchIndex = matchIndex > 0 ? size_t(matchIndex) : 0;
        settings.options.client.version = CDREADER_VERSION;

        JavaHttpClient http(env, httpGet);
        const cdr::CddbLookupResult& r = rip.lookupCddb(settings.enabled ? &http : nullptr, settings);
        const cdr::AlbumMetadata& album = rip.album();
        std::vector<std::string> v = {r.found ? "1" : "0",
                                      r.found ? (r.exact ? "exact" : "inexact") : r.error,
                                      album.artist,
                                      album.title,
                                      album.year,
                                      album.genre,
                                      rip.albumDirectoryName(),
                                      cdr::AccurateRipDiscId::fromToc(rip.toc()).toString(),
                                      std::to_string(r.chosen),
                                      std::to_string(r.matches.size())};
        for (const cdr::CddbMatch& m : r.matches) v.push_back(m.category + "/" + m.discId + "  " + m.title);
        for (const cdr::Track& t : rip.toc().tracks) v.push_back(rip.trackLabel(t.number));
        return toJavaArray(env, v);
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

// Output folder name: "Artist - Album" from CDDB, otherwise "cd_<CDDB id>".
JNIEXPORT jstring JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeAlbumFolderName(JNIEnv* env, jclass,
                                                                                         jlong handle) {
    try {
        cdr::RipSession& rip = session(handle)->rip;
        rip.toc();  // the disc id names the folder without metadata
        return toJava(env, rip.albumDirectoryName());
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

// File name of a track for `format` ("wav" / "flac" / "oggflac" / "opus" / "vorbis" / "mka"), from the CDDB metadata.
JNIEXPORT jstring JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeTrackFileName(JNIEnv* env, jclass,
                                                                                       jlong handle, jint track,
                                                                                       jstring format) {
    try {
        std::string f;
        if (!fromJava(env, format, f)) return nullptr;
        return toJava(env, session(handle)->rip.trackFileName(track, f));
    } catch (const std::exception& e) {
        throwJava(env, "java/lang/IllegalArgumentException", e.what());
        return nullptr;
    }
}

// Starts a rip with these settings: forgets earlier results, clears a cancel.
// Lossy formats use their default settings (Opus VBR 160 kbit/s, Vorbis q5).
// `useC2`: read with C2 error pointers if the drive supports them (#33).
// `cacheMode`: drive cache defeat for re-reads (#34): 0 auto (timing test,
// once per disc), 1 FUA, 2 flush, 3 none.
// `offsetSource` (#37): where readOffset came from, for rip.log: 0 manual,
// 1 saved for this drive, 2 auto-detected; `offsetDetail` the note
// ("HL-DT-ST BD-RE BP71N (1.03); auto-detected: ..." / "2 of 2 tracks agreed, v2").
JNIEXPORT void JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeBeginRip(
    JNIEnv* env, jclass, jlong handle, jstring format, jint readOffset, jint maxRetries, jboolean verify,
    jboolean useC2, jint cacheMode, jint offsetSource, jstring offsetDetail) {
    try {
        cdr::RipSettings settings;
        if (!fromJava(env, format, settings.format)) return;
        if (!fromJava(env, offsetDetail, settings.offsetSource.detail)) return;
        switch (offsetSource) {
            case 1: settings.offsetSource.kind = cdr::ReadOffsetSource::Kind::Saved; break;
            case 2: settings.offsetSource.kind = cdr::ReadOffsetSource::Kind::Detected; break;
            default: settings.offsetSource.kind = cdr::ReadOffsetSource::Kind::Manual; break;
        }
        settings.options.readOffsetSamples = readOffset;
        settings.options.maxRetries = maxRetries;
        settings.options.verify = verify == JNI_TRUE;
        settings.useC2 = useC2 == JNI_TRUE;
        switch (cacheMode) {
            case 0: settings.cache = cdr::CacheSetting::Auto; break;
            case 1: settings.cache = cdr::CacheSetting::Fua; break;
            case 2: settings.cache = cdr::CacheSetting::Flush; break;
            case 3: settings.cache = cdr::CacheSetting::None; break;
            default: throw std::invalid_argument("invalid cache mode " + std::to_string(cacheMode));
        }
        session(handle)->rip.beginRip(settings);
    } catch (const std::exception& e) {
        throwJava(env, "java/lang/IllegalArgumentException", e.what());
    }
}

// Rips one track to a local file at `path` (in the format given to
// nativeBeginRip), calling listener.onProgress(done, total) in sectors.
// Returns [sectors, unreadableSectors, retries, paddedSamples, crc32,
// accurateRipV1, accurateRipV2, c2 (1: read with C2 pointers), C2 error
// sectors, C2 re-reads, unresolved C2 sectors, suspicious sectors, cache
// defeat operations (FUA commands or flushes before re-reads, #34)]. Throws
// java.util.concurrent.CancellationException after nativeCancel() and
// IOException on errors; the partial file is left for the caller to delete.
JNIEXPORT jintArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeRipTrack(JNIEnv* env, jclass,
                                                                                    jlong handle, jint trackNumber,
                                                                                    jstring path, jobject listener) {
    Session& s = *session(handle);
    std::string outputPath;
    if (!fromJava(env, path, outputPath)) return nullptr;

    jmethodID onProgress = nullptr;
    if (listener != nullptr) {
        jclass cls = env->GetObjectClass(listener);
        onProgress = env->GetMethodID(cls, "onProgress", "(II)V");
        if (onProgress == nullptr) return nullptr;  // NoSuchMethodError pending
    }

    try {
        const cdr::RippedTrack& r =
            s.rip.ripTrack(trackNumber, std::filesystem::u8path(outputPath), [&](uint32_t done, uint32_t total) {
                if (onProgress == nullptr) return;
                env->CallVoidMethod(listener, onProgress, jint(done), jint(total));
                if (env->ExceptionCheck()) throw JavaExceptionPending{};
            });
        return toJavaArray(env, std::vector<jint>{jint(r.result.sectors), jint(r.result.unreadableSectors),
                                                  jint(r.result.retries), jint(r.result.paddedSamples),
                                                  jint(r.result.crc32), jint(r.accurateRipV1), jint(r.accurateRipV2),
                                                  jint(r.result.c2 ? 1 : 0), jint(r.result.c2ErrorSectors),
                                                  jint(r.result.c2Rereads), jint(r.result.c2Unresolved),
                                                  jint(r.result.suspiciousSectors.size()),
                                                  jint(r.result.cacheDefeats)});
    } catch (const cdr::RipCancelled&) {
        throwJava(env, "java/util/concurrent/CancellationException", "rip cancelled");
    } catch (const JavaExceptionPending&) {
        // propagate the listener's exception
    } catch (const std::exception& e) {
        throwIo(env, e.what());
    }
    return nullptr;
}

// Detects the read offset with AccurateRip (#37), calling
// listener.onProgress(step, steps, track, doneSectors, totalSectors).
// Returns [status (0 detected, 1 disc not in database, 2 lookup failed,
// 3 no track with database entries, 4 no match, 5 too few tracks agree,
// 6 tracks disagree, 7 cancelled), offset (the best candidate), agreeing
// tracks, tracks read, confidence, matched version ("v1" / "v2" / "v1+v2"),
// single track ("1" / "0"), usable tracks, competing offsets
// ("+6,-1164"), English summary, agreement ("2 of 2 tracks agreed, v2"),
// error]. Throws IOException on errors outside the reads.
JNIEXPORT jobjectArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeDetectOffset(JNIEnv* env, jclass,
                                                                                          jlong handle,
                                                                                          jobject httpGet,
                                                                                          jobject listener) {
    jmethodID onProgress = nullptr;
    if (listener != nullptr) {
        jclass cls = env->GetObjectClass(listener);
        onProgress = env->GetMethodID(cls, "onProgress", "(IIIII)V");
        if (onProgress == nullptr) return nullptr;  // NoSuchMethodError pending
    }
    try {
        JavaHttpClient http(env, httpGet);
        cdr::OffsetDetectOptions options;
        options.progress = [&](const cdr::OffsetDetectProgress& p) {
            if (onProgress == nullptr) return;
            env->CallVoidMethod(listener, onProgress, jint(p.step), jint(p.steps), jint(p.track),
                                jint(p.doneSectors), jint(p.totalSectors));
            if (env->ExceptionCheck()) throw JavaExceptionPending{};
        };
        const cdr::OffsetDetection& d =
            session(handle)->rip.detectReadOffset(httpGet != nullptr ? &http : nullptr, options);
        using Status = cdr::OffsetDetection::Status;
        int status = 4;
        switch (d.status) {
            case Status::Detected: status = 0; break;
            case Status::NotInDatabase: status = 1; break;
            case Status::LookupFailed: status = 2; break;
            case Status::NoUsableTracks: status = 3; break;
            case Status::NoMatch: status = 4; break;
            case Status::NotEnough: status = 5; break;
            case Status::Conflict: status = 6; break;
            case Status::Cancelled: status = 7; break;
        }
        std::string candidates;
        for (int o : d.candidates) candidates += (candidates.empty() ? "" : ",") + std::to_string(o);
        return toJavaArray(env, std::vector<std::string>{
                                    std::to_string(status), std::to_string(d.offset), std::to_string(d.agreeingTracks),
                                    std::to_string(d.testedTracks()), std::to_string(d.confidence()),
                                    d.matchedVersion(), d.singleTrack ? "1" : "0", std::to_string(d.usableTracks),
                                    candidates, d.summary(), d.agreement(), d.error});
    } catch (const JavaExceptionPending&) {
        // propagate the listener's exception
    } catch (const std::exception& e) {
        throwIo(env, e.what());
    }
    return nullptr;
}

// Looks the tracks ripped since nativeBeginRip up in the AccurateRip database
// (httpGet null or !enabled: disabled). Never fails for network problems.
// Returns [status (0 found, 1 not in database, 2 error, 3 disabled),
// pressings, accurate tracks, tracks in database, then per ripped track:
// number, matched versions (bit 0: v1, bit 1: v2), v1 confidence,
// v2 confidence, total confidence, matching pressings, pressings with an
// entry for the track]. See nativeAccurateRipError() for the error text.
JNIEXPORT jintArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeCheckAccurateRip(JNIEnv* env, jclass,
                                                                                            jlong handle,
                                                                                            jboolean enabled,
                                                                                            jobject httpGet) {
    try {
        JavaHttpClient http(env, httpGet);
        const bool on = enabled == JNI_TRUE && httpGet != nullptr;
        const cdr::AccurateRipReport& report = session(handle)->rip.checkAccurateRip(on ? &http : nullptr);
        using Status = cdr::AccurateRipReport::Status;
        const jint status = report.status == Status::Found      ? 0
                            : report.status == Status::NotFound ? 1
                            : report.status == Status::Error    ? 2
                                                                : 3;
        std::vector<jint> v = {status, jint(report.pressings), report.accurateTracks(), report.tracksInDatabase()};
        for (const cdr::AccurateRipTrackResult& t : report.tracks) {
            v.push_back(t.track);
            v.push_back((t.v1Confidence > 0 ? 1 : 0) | (t.v2Confidence > 0 ? 2 : 0));
            v.push_back(t.v1Confidence);
            v.push_back(t.v2Confidence);
            v.push_back(t.totalConfidence);
            v.push_back(t.matchingPressings());
            v.push_back(jint(t.pressings.size()));
        }
        return toJavaArray(env, v);
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

JNIEXPORT jstring JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeAccurateRipError(JNIEnv* env, jclass,
                                                                                          jlong handle) {
    return toJava(env, session(handle)->rip.accurateRipReport().error);
}

// C2 error pointers of the current rip: 0 disabled, 1 not supported by the
// drive, 2 used, 3 supported but given up (the drive rejected C2 reads).
JNIEXPORT jint JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeC2Status(JNIEnv*, jclass, jlong handle) {
    const cdr::RipSession& rip = session(handle)->rip;
    switch (rip.c2Availability().mode) {
        case cdr::C2Availability::Mode::Disabled: return 0;
        case cdr::C2Availability::Mode::NotSupported: return 1;
        case cdr::C2Availability::Mode::Supported: break;
    }
    return rip.c2Fallback().empty() ? 2 : 3;
}

// Drive cache check of the current rip (#34): [result (0 not tested, 1 no
// cache, 2 cache and FUA works, 3 FUA ignored, 4 FUA rejected, 5 test
// inconclusive), method in use (0 none, 1 FUA, 2 flush), reported cache
// size in KB (0: unknown), 1 if FUA was given up during the rip].
JNIEXPORT jintArray JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeCacheStatus(JNIEnv* env, jclass,
                                                                                       jlong handle) {
    const cdr::RipSession& rip = session(handle)->rip;
    const cdr::DriveCacheCheck& c = rip.cacheCheck();
    using R = cdr::DriveCacheCheck::Result;
    jint result = 0;
    switch (c.result) {
        case R::NotRun: result = 0; break;
        case R::NoCache: result = 1; break;
        case R::FuaWorks: result = 2; break;
        case R::FuaIgnored: result = 3; break;
        case R::FuaRejected: result = 4; break;
        case R::Unknown: result = 5; break;
    }
    const cdr::CacheDefeat method = rip.ripSettings().options.cacheDefeat;
    return toJavaArray(env, std::vector<jint>{result,
                                              method == cdr::CacheDefeat::Fua     ? 1
                                              : method == cdr::CacheDefeat::Flush ? 2
                                                                                  : 0,
                                              jint(c.cacheKB), rip.cacheFallback().empty() ? 0 : 1});
}

// rip.log text for the current rip.
JNIEXPORT jstring JNICALL Java_io_github_noribow_cdreader_NativeCd_nativeRipLog(JNIEnv* env, jclass, jlong handle) {
    try {
        return toJava(env, session(handle)->rip.ripLog());
    } catch (const std::exception& e) {
        throwIo(env, e.what());
        return nullptr;
    }
}

}  // extern "C"
